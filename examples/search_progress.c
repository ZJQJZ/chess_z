#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#else
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#endif
#include "search_progress.h"
#include "search_sample_report.h"

#include <inttypes.h>
#include <limits.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <pthread.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

#define CLI_SAMPLE_REQUEST_HISTORY 1024u
#ifndef CLI_SAMPLE_QUEUE_CAPACITY
#define CLI_SAMPLE_QUEUE_CAPACITY 65536u
#endif
#ifndef CLI_SAMPLE_BATCH_CAPACITY
#define CLI_SAMPLE_BATCH_CAPACITY 512u
#endif
_Static_assert(CLI_SAMPLE_QUEUE_CAPACITY > 0, "sampling queue must not be empty");
_Static_assert(CLI_SAMPLE_BATCH_CAPACITY > 0, "sampling batch must not be empty");

typedef enum CliTimelineKind
{
    CLI_TIMELINE_STATE, CLI_TIMELINE_SAMPLE, CLI_SAMPLE_BOUNDARY, CLI_SAMPLE_ALLOCATION_FAILURE
} CliTimelineKind;
typedef struct CliTimelineEvent
{
    XqSearchSample sample;
    uint64_t requested_at;
    CliTimelineKind kind;
    bool finished;
} CliTimelineEvent;

typedef struct CliSampleSlot
{
    uint64_t last_request, state_sequence;
    XqSearchThreadState state;
    unsigned state_depth;
    bool seen_request, seen_state;
} CliSampleSlot;

struct CliSearchProgress
{
    XqSearchProgress latest;
    unsigned max_depth;
    uint64_t budget_ms;
    uint64_t started_ms;
    bool clock_available;
    bool threaded;
    bool show_progress, terminal;
    bool timeline;
    XqSearchSampler *sampler;
    CliSampleReport *report;
    CliSampleSlot *slots;
    unsigned thread_capacity;
    uint64_t interval_ms, requested;
    uint64_t *request_times;
    uint64_t *request_ids;
    CliTimelineEvent *record_queue, *record_batch;
    size_t record_head, record_count;
    CliSampleCounters counters;
    CliSampleSummary summary;
    unsigned active_threads;
    bool stop, writer_stop, writer_started;
    size_t previous_width; /* Display-thread owned until joined. */
#ifdef _WIN32
    HANDLE thread;
    HANDLE writer;
    CRITICAL_SECTION mutex;
    CONDITION_VARIABLE changed, record_ready, record_space;
#else
    pthread_t thread;
    pthread_t writer;
    pthread_mutex_t mutex;
    pthread_cond_t changed, record_ready, record_space;
#endif
};

static bool monotonic_ms(uint64_t *value)
{
#ifdef _WIN32
    *value = GetTickCount64();
#else
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return false;
    *value = (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
#endif
    return true;
}

static bool stderr_is_terminal(void)
{
#ifdef _WIN32
    return _isatty(_fileno(stderr)) != 0;
#else
    return isatty(fileno(stderr)) != 0;
#endif
}

static unsigned terminal_width(void)
{
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO info;
    HANDLE output = (HANDLE)_get_osfhandle(_fileno(stderr));
    if (GetConsoleScreenBufferInfo(output, &info))
        return (unsigned)(info.srWindow.Right - info.srWindow.Left + 1);
#else
    struct winsize size;
    if (ioctl(fileno(stderr), TIOCGWINSZ, &size) == 0 && size.ws_col > 0)
        return size.ws_col;
#endif
    return 80;
}

static void lock_display(CliSearchProgress *display)
{
#ifdef _WIN32
    EnterCriticalSection(&display->mutex);
#else
    pthread_mutex_lock(&display->mutex);
#endif
}

static void unlock_display(CliSearchProgress *display)
{
#ifdef _WIN32
    LeaveCriticalSection(&display->mutex);
#else
    pthread_mutex_unlock(&display->mutex);
#endif
}

static void format_progress(const CliSearchProgress *display, XqSearchProgress snapshot,
                            unsigned width, char *text, size_t capacity)
{
    char elapsed[48], timing[80], bar[21];
    uint64_t now;
    unsigned percent = snapshot.total_moves == 0 ? 0 :
        (unsigned)(100 * (uint64_t)snapshot.completed_moves / snapshot.total_moves);
    unsigned i;
    if (display->clock_available && monotonic_ms(&now) && now >= display->started_ms)
        snprintf(elapsed, sizeof(elapsed), "%.1fs", (double)(now - display->started_ms) / 1000);
    else
        snprintf(elapsed, sizeof(elapsed), "unavailable");
    if (display->budget_ms > 0)
        snprintf(timing, sizeof(timing), "%s / %.1fs", elapsed, (double)display->budget_ms / 1000);
    else
        snprintf(timing, sizeof(timing), "%s", elapsed);
    for (i = 0; i < 20; ++i)
        bar[i] = i < percent / 5 ? '#' : '-';
    bar[20] = '\0';
    snprintf(text, capacity, "search depth %u/%u  [%s] %u%%  roots %u/%u  elapsed %s",
             snapshot.depth, display->max_depth, bar, percent,
             snapshot.completed_moves, snapshot.total_moves, timing);
    if (strlen(text) > width)
    {
        /* Reserve space for metadata first, then shorten the bar to fit the terminal. */
        char metadata[192];
        size_t metadata_width;
        unsigned bars;
        snprintf(metadata, sizeof(metadata), "depth %u/%u %u%% roots %u/%u elapsed %s",
                 snapshot.depth, display->max_depth, percent,
                 snapshot.completed_moves, snapshot.total_moves, timing);
        metadata_width = strlen(metadata);
        bars = width > metadata_width + 3 ? (unsigned)(width - metadata_width - 3) : 0;
        if (bars > 20) bars = 20;
        for (i = 0; i < bars; ++i)
            bar[i] = i < percent * bars / 100 ? '#' : '-';
        bar[bars] = '\0';
        if (bars > 0)
            snprintf(text, capacity, "%s [%s]", metadata, bar);
        else
            snprintf(text, capacity, "%s", metadata);
        if (strlen(text) > width)
            snprintf(text, capacity, "d%u %u/%u %s", snapshot.depth,
                     snapshot.completed_moves, snapshot.total_moves, elapsed);
    }
    if (width < capacity && strlen(text) > width)
        text[width] = '\0';
}

static void draw(CliSearchProgress *display, XqSearchProgress snapshot, bool overwrite)
{
    char text[512];
    unsigned columns = overwrite ? terminal_width() : (unsigned)sizeof(text);
    unsigned width = columns > 1 ? columns - 1 : 1;
    size_t length, clear_to;
    if (display->sampler)
    {
        CliSampleSummary summary;
        unsigned active;
        lock_display(display);
        summary = display->summary;
        active = display->active_threads;
        unlock_display(display);
        char base[256] = "";
        if (display->show_progress)
            format_progress(display, snapshot, sizeof(base) - 1, base, sizeof(base));
        else
            snprintf(base, sizeof(base), "search depth %u", snapshot.depth);
        /* Keep samples visible even on narrow terminals. The full prefix remains in reports. */
        snprintf(text, sizeof(text), "d%u samples/5s %" PRIu64 " active %u/%u maxQ %" PRIu64
                 "%s hot %s", snapshot.depth, summary.recent_samples,
                 active, display->thread_capacity,
                 summary.max_quiescence_ply, summary.incomplete ? " incomplete" : "",
                 summary.hot_path[0] ? summary.hot_path : "-");
        size_t summary_length = strlen(text);
        if (display->show_progress && strlen(base) + summary_length + 3 <= width)
        {
            char combined[512];
            snprintf(combined, sizeof(combined), "%s | %s", base, text);
            memcpy(text, combined, strlen(combined) + 1);
        }
        else if (display->show_progress)
        {
            char combined[512];
            snprintf(combined, sizeof(combined), "d%u roots %u/%u samp/5s %" PRIu64
                     " active %u/%u Q%" PRIu64 " %s", snapshot.depth,
                     snapshot.completed_moves, snapshot.total_moves,
                     summary.recent_samples, active,
                     display->thread_capacity, summary.max_quiescence_ply,
                     summary.hot_path[0] ? summary.hot_path : "-");
            memcpy(text, combined, strlen(combined) + 1);
        }
        if (width < sizeof(text) && strlen(text) > width) text[width] = '\0';
    }
    else format_progress(display, snapshot, width, text, sizeof(text));
    if (overwrite) fputc('\r', stderr);
    fputs(text, stderr);
    length = strlen(text);
    clear_to = display->previous_width < width ? display->previous_width : width;
    if (overwrite)
    {
        for (size_t i = length; i < clear_to; ++i) fputc(' ', stderr);
        display->previous_width = length;
    }
    else fputc('\n', stderr);
    fflush(stderr);
}

static uint64_t deadline_after(uint64_t now, uint64_t delay)
{
    return UINT64_MAX - now < delay ? UINT64_MAX : now + delay;
}

static void wait_display(CliSearchProgress *display, uint64_t milliseconds)
{
#ifdef _WIN32
    SleepConditionVariableCS(&display->changed, &display->mutex, (DWORD)milliseconds);
#else
    struct timespec deadline;
    if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) return;
    deadline.tv_sec += (time_t)(milliseconds / 1000);
    deadline.tv_nsec += (long)(milliseconds % 1000) * 1000000;
    if (deadline.tv_nsec >= 1000000000)
    {
        ++deadline.tv_sec;
        deadline.tv_nsec -= 1000000000;
    }
    pthread_cond_timedwait(&display->changed, &display->mutex, &deadline);
#endif
}

static void signal_display(CliSearchProgress *display)
{
#ifdef _WIN32
    WakeConditionVariable(&display->changed);
#else
    pthread_cond_signal(&display->changed);
#endif
}

/* Preserve the original progress-only loop: enabling ordinary --progress must
 * not run the sampler's request, drain or report machinery. */
static void display_progress_only(CliSearchProgress *display)
{
    lock_display(display);
    while (!display->stop)
    {
        XqSearchProgress snapshot = display->latest;
        unlock_display(display);
        draw(display, snapshot, true);
        lock_display(display);
        if (display->stop) break;
#ifdef _WIN32
        SleepConditionVariableCS(&display->changed, &display->mutex, 100);
#else
        struct timespec deadline;
        if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) break;
        deadline.tv_nsec += 100000000;
        if (deadline.tv_nsec >= 1000000000)
        {
            ++deadline.tv_sec;
            deadline.tv_nsec -= 1000000000;
        }
        while (!display->stop)
            if (pthread_cond_timedwait(&display->changed, &display->mutex, &deadline) != 0)
                break;
#endif
    }
    unlock_display(display);
}

/* The mutex protects only queue metadata, requests and UI snapshots. No report
 * processing or file I/O runs while it is held. Queue saturation applies
 * backpressure instead of discarding diagnostic events. */
static void enqueue_record(CliSearchProgress *display, const CliTimelineEvent *event)
{
    while (display->record_count == CLI_SAMPLE_QUEUE_CAPACITY)
    {
#ifdef _WIN32
        SleepConditionVariableCS(&display->record_space, &display->mutex, INFINITE);
#else
        pthread_cond_wait(&display->record_space, &display->mutex);
#endif
    }
    size_t index = (display->record_head + display->record_count++) % CLI_SAMPLE_QUEUE_CAPACITY;
    display->record_queue[index] = *event;
#ifdef _WIN32
    WakeConditionVariable(&display->record_ready);
#else
    pthread_cond_signal(&display->record_ready);
#endif
}

static void wake_writer(CliSearchProgress *display)
{
#ifdef _WIN32
    WakeAllConditionVariable(&display->record_ready);
    WakeAllConditionVariable(&display->record_space);
#else
    pthread_cond_broadcast(&display->record_ready);
    pthread_cond_broadcast(&display->record_space);
#endif
}

static void wait_record(CliSearchProgress *display)
{
#ifdef _WIN32
    SleepConditionVariableCS(&display->record_ready, &display->mutex, 100);
#else
    struct timespec deadline;
    if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) return;
    deadline.tv_nsec += 100000000;
    if (deadline.tv_nsec >= 1000000000)
    {
        ++deadline.tv_sec;
        deadline.tv_nsec -= 1000000000;
    }
    pthread_cond_timedwait(&display->record_ready, &display->mutex, &deadline);
#endif
}

#ifdef _WIN32
static DWORD WINAPI report_main(LPVOID argument)
#else
static void *report_main(void *argument)
#endif
{
    CliSearchProgress *display = argument;
    uint64_t next_summary = 0;
    unsigned finished_depth = 0;
    for (;;)
    {
        lock_display(display);
        if (!display->record_count && !display->writer_stop) wait_record(display);
        size_t count = display->record_count;
        if (count > CLI_SAMPLE_BATCH_CAPACITY) count = CLI_SAMPLE_BATCH_CAPACITY;
        for (size_t i = 0; i < count; ++i)
            display->record_batch[i] = display->record_queue[(display->record_head + i) % CLI_SAMPLE_QUEUE_CAPACITY];
        display->record_head = (display->record_head + count) % CLI_SAMPLE_QUEUE_CAPACITY;
        display->record_count -= count;
        bool stopping = display->writer_stop && !display->record_count;
        CliSampleCounters counters = display->counters;
        unsigned depth = display->latest.depth;
#ifdef _WIN32
        WakeAllConditionVariable(&display->record_space);
#else
        pthread_cond_broadcast(&display->record_space);
#endif
        unlock_display(display);

        uint64_t now = display->started_ms;
        monotonic_ms(&now);
        /* Publish this batch's counters before a boundary can write a final
         * report. The report owner also throttles expensive checkpoints. */
        cli_sample_report_tick(display->report, now, &counters);
        for (size_t i = 0; i < count; ++i)
        {
            const CliTimelineEvent *event = &display->record_batch[i];
            if (event->kind == CLI_TIMELINE_SAMPLE)
            {
                cli_sample_report_sample_timed(display->report, &event->sample, event->requested_at);
                free((void *)event->sample.path);
            }
            else if (event->kind == CLI_TIMELINE_STATE)
            {
                cli_sample_report_timeline_state(display->report, &event->sample);
                /* Advance completion from consumed FIFO events, never a newer
                 * producer snapshot that could skip still-buffered paths. */
                if (event->sample.thread_id == 0 && event->sample.state == XQ_SEARCH_THREAD_IDLE &&
                    event->sample.iteration_depth > finished_depth)
                    finished_depth = event->sample.iteration_depth;
            }
            else if (event->kind == CLI_SAMPLE_BOUNDARY)
            {
                cli_sample_report_boundary(display->report, event->sample.iteration_depth, event->finished);
                if (event->finished && event->sample.iteration_depth > finished_depth)
                    finished_depth = event->sample.iteration_depth;
            }
            else
                cli_sample_report_allocation_failed(display->report);
        }
        /* Also reclaim rounds when timeline setup failed after marking the
         * report as requested: the non-timeline boundary API then defers to us. */
        if (finished_depth)
            cli_sample_report_complete_through(display->report, finished_depth);
        monotonic_ms(&now);
        if (now >= next_summary || stopping)
        {
            CliSampleSummary summary = {0};
            cli_sample_report_summary(display->report, depth, now, &summary);
            lock_display(display);
            display->summary = summary;
            unlock_display(display);
            next_summary = deadline_after(now, 100);
        }
        if (stopping)
        {
            cli_sample_report_finish(display->report, &counters);
            display->report = NULL;
            break;
        }
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

#ifdef _WIN32
static DWORD WINAPI display_main(LPVOID argument)
#else
static void *display_main(void *argument)
#endif
{
    CliSearchProgress *display = argument;
    if (display->sampler == NULL)
        display_progress_only(display);
    else
    {
        uint64_t next_request = 0, next_draw = 0;
        for (;;)
        {
            lock_display(display);
            if (display->stop) { unlock_display(display); break; }
            XqSearchProgress snapshot = display->latest;
            uint64_t now = display->started_ms;
            bool now_valid = monotonic_ms(&now);
            if (now >= next_request)
            {
                ++display->requested;
                size_t index = (size_t)(display->requested % CLI_SAMPLE_REQUEST_HISTORY);
                display->request_times[index] = now_valid ? now : 0;
                display->request_ids[index] = display->requested;
                xq_search_sampler_request(display->sampler);
                next_request = deadline_after(now, display->interval_ms);
            }
            unsigned active = 0;
            for (unsigned i = 0; i < display->thread_capacity; ++i)
            {
                const CliSampleSlot *slot = &display->slots[i];
                if (slot->seen_state && slot->state_depth == snapshot.depth &&
                    (slot->state == XQ_SEARCH_THREAD_SEARCH || slot->state == XQ_SEARCH_THREAD_QUIESCENCE)) ++active;
            }
            display->active_threads = active;
            unlock_display(display);
            if (display->terminal && now >= next_draw)
            {
                draw(display, snapshot, true);
                next_draw = deadline_after(now, 100);
            }
            lock_display(display);
            if (!display->stop)
            {
                uint64_t after = now;
                monotonic_ms(&after);
                uint64_t delay = next_request > after ? next_request - after : 1;
                if (display->terminal && next_draw > after && next_draw - after < delay)
                    delay = next_draw - after;
                if (delay > 100) delay = 100;
                wait_display(display, delay);
            }
            unlock_display(display);
        }
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

static void free_sampling_buffers(CliSearchProgress *display)
{
    xq_search_sampler_destroy(display->sampler);
    display->sampler = NULL;
    free(display->slots); display->slots = NULL;
    free(display->request_times); display->request_times = NULL;
    free(display->request_ids); display->request_ids = NULL;
    free(display->record_queue); display->record_queue = NULL;
    free(display->record_batch); display->record_batch = NULL;
    display->thread_capacity = 0;
    display->timeline = false;
}

static void join_writer(CliSearchProgress *display, bool startup_failed)
{
    if (!display->writer_started) return;
    lock_display(display);
    if (startup_failed)
    {
        CliTimelineEvent event = {0};
        event.kind = CLI_SAMPLE_ALLOCATION_FAILURE;
        enqueue_record(display, &event);
    }
    display->writer_stop = true;
    wake_writer(display);
    unlock_display(display);
#ifdef _WIN32
    WaitForSingleObject(display->writer, INFINITE);
    CloseHandle(display->writer);
#else
    pthread_join(display->writer, NULL);
#endif
    display->writer_started = false;
}

static CliSearchProgress *start_display(unsigned max_depth, uint64_t time_limit_ms,
                                       bool show_progress, unsigned thread_capacity,
                                       uint64_t interval_ms, const char *output_dir,
                                       const XqPosition *position, const char *mode,
                                       bool timeline, uint64_t timeline_byte_limit,
                                       size_t statistics_byte_limit)
{
    CliSearchProgress *display = calloc(1, sizeof(*display));
    if (display == NULL)
    {
        fputs("warning: cannot allocate search progress display; progress/sampling disabled\n", stderr);
        return NULL;
    }
    display->max_depth = max_depth;
    display->budget_ms = time_limit_ms;
    display->show_progress = show_progress;
    display->terminal = stderr_is_terminal();
    display->clock_available = monotonic_ms(&display->started_ms);
    display->interval_ms = interval_ms ? interval_ms : 20;
    if (thread_capacity)
    {
        display->sampler = xq_search_sampler_create(thread_capacity);
        display->slots = calloc(thread_capacity, sizeof(*display->slots));
        display->request_times = calloc(CLI_SAMPLE_REQUEST_HISTORY, sizeof(*display->request_times));
        display->request_ids = calloc(CLI_SAMPLE_REQUEST_HISTORY, sizeof(*display->request_ids));
        display->record_queue = calloc(CLI_SAMPLE_QUEUE_CAPACITY, sizeof(*display->record_queue));
        display->record_batch = calloc(CLI_SAMPLE_BATCH_CAPACITY, sizeof(*display->record_batch));
        if (display->sampler && display->slots && display->request_times && display->request_ids &&
            display->record_queue && display->record_batch && display->clock_available)
            display->report = cli_sample_report_create(output_dir, position, mode,
                xq_search_sampler_id(display->sampler), thread_capacity,
                display->interval_ms, max_depth, time_limit_ms);
        if (!display->report)
        {
            fputs("warning: cannot allocate search sampler; sampling disabled\n", stderr);
            free_sampling_buffers(display);
        }
        else
        {
            display->thread_capacity = thread_capacity;
            cli_sample_report_set_statistics_limit(display->report, statistics_byte_limit);
            if (timeline)
            {
                display->timeline = cli_sample_report_enable_timeline(display->report,
                    display->started_ms, timeline_byte_limit);
                if (!display->timeline)
                    fputs("warning: cannot allocate search timeline; continuing with path sampling\n", stderr);
            }
        }
    }
    if (!display->sampler && (!show_progress || !display->terminal)) return display;
#ifdef _WIN32
    if (!InitializeCriticalSectionEx(&display->mutex, 0, 0)) goto fallback;
    InitializeConditionVariable(&display->changed);
    InitializeConditionVariable(&display->record_ready);
    InitializeConditionVariable(&display->record_space);
    if (display->sampler)
    {
        display->writer = CreateThread(NULL, 0, report_main, display, 0, NULL);
        if (display->writer == NULL) goto destroy_mutex;
        display->writer_started = true;
    }
    display->thread = CreateThread(NULL, 0, display_main, display, 0, NULL);
    if (display->thread == NULL)
    {
        join_writer(display, true);
        goto destroy_mutex;
    }
#else
    if (pthread_mutex_init(&display->mutex, NULL) != 0) goto fallback;
    if (pthread_cond_init(&display->changed, NULL) != 0) goto destroy_mutex;
    if (pthread_cond_init(&display->record_ready, NULL) != 0) goto destroy_changed;
    if (pthread_cond_init(&display->record_space, NULL) != 0) goto destroy_ready;
    if (display->sampler)
    {
        if (pthread_create(&display->writer, NULL, report_main, display) != 0) goto destroy_space;
        display->writer_started = true;
    }
    if (pthread_create(&display->thread, NULL, display_main, display) != 0)
    {
        join_writer(display, true);
        goto destroy_space;
    }
#endif
    display->threaded = true;
    return display;
#ifndef _WIN32
 destroy_space:
    pthread_cond_destroy(&display->record_space);
 destroy_ready:
    pthread_cond_destroy(&display->record_ready);
 destroy_changed:
    pthread_cond_destroy(&display->changed);
#endif
 destroy_mutex:
#ifdef _WIN32
    DeleteCriticalSection(&display->mutex);
#else
    pthread_mutex_destroy(&display->mutex);
#endif
 fallback:
    fputs("warning: cannot start search collector/display; sampling disabled, using iteration summaries\n", stderr);
    if (display->report)
    {
        cli_sample_report_allocation_failed(display->report);
        cli_sample_report_finish(display->report, &display->counters);
    }
    display->report = NULL;
    free_sampling_buffers(display);
    return display;
}

CliSearchProgress *cli_search_progress_start(unsigned max_depth, uint64_t time_limit_ms)
{
    return start_display(max_depth, time_limit_ms, true, 0, 20, NULL, NULL, NULL, false, 0, 0);
}

CliSearchProgress *cli_search_progress_start_sampling(unsigned max_depth, uint64_t time_limit_ms,
                                                     bool show_progress, unsigned thread_capacity,
                                                     uint64_t interval_ms, const char *output_dir,
                                                     const XqPosition *initial_position,
                                                     const char *search_mode)
{
    return start_display(max_depth, time_limit_ms, show_progress,
                         thread_capacity ? thread_capacity : 1, interval_ms,
                         output_dir, initial_position, search_mode, false, 0, 0);
}

CliSearchProgress *cli_search_progress_start_timeline(unsigned max_depth, uint64_t time_limit_ms,
                                                     bool show_progress, unsigned thread_capacity,
                                                     uint64_t interval_ms, const char *output_dir,
                                                     const XqPosition *initial_position,
                                                     const char *search_mode,
                                                     uint64_t timeline_byte_limit)
{
    return start_display(max_depth, time_limit_ms, show_progress,
                         thread_capacity ? thread_capacity : 1, interval_ms,
                         output_dir, initial_position, search_mode, true, timeline_byte_limit, 0);
}

CliSearchProgress *cli_search_progress_start_sampling_with_limits(
    unsigned max_depth, uint64_t time_limit_ms, bool show_progress,
    unsigned thread_capacity, uint64_t interval_ms, const char *output_dir,
    const XqPosition *initial_position, const char *search_mode,
    bool timeline, uint64_t timeline_byte_limit, size_t statistics_byte_limit)
{
    return start_display(max_depth, time_limit_ms, show_progress,
                         thread_capacity ? thread_capacity : 1, interval_ms,
                         output_dir, initial_position, search_mode, timeline,
                         timeline_byte_limit, statistics_byte_limit);
}

XqSearchSampler *cli_search_progress_sampler(CliSearchProgress *display)
{
    return display ? display->sampler : NULL;
}

void cli_search_progress_update(const XqSearchProgress *update, void *user)
{
    CliSearchProgress *display = user;
    if (display == NULL || update == NULL) return;
    if (display->sampler == NULL)
    {
        if (update->event == XQ_SEARCH_PATH_SAMPLE || update->event == XQ_SEARCH_THREAD_STATE)
            return;
        if (!display->threaded && update->event == XQ_SEARCH_ROOT_MOVE_COMPLETED) return;
        if (display->threaded) lock_display(display);
        if (update->event == XQ_SEARCH_ITERATION_STARTED)
            display->latest = *update;
        else if (update->depth == display->latest.depth)
        {
            if (update->completed_moves > display->latest.completed_moves)
                display->latest.completed_moves = update->completed_moves;
            display->latest.event = update->event;
        }
        if (display->threaded) unlock_display(display);
        else if (display->show_progress && update->event == XQ_SEARCH_ITERATION_FINISHED)
            draw(display, display->latest, false);
        return;
    }
    if (update->event == XQ_SEARCH_PATH_SAMPLE || update->event == XQ_SEARCH_THREAD_STATE)
    {
        if (!display->threaded || !update->sample) return;
        const XqSearchSample *sample = update->sample;
        lock_display(display);
        if (sample->thread_id >= display->thread_capacity ||
            sample->search_id != xq_search_sampler_id(display->sampler) ||
            sample->state < XQ_SEARCH_THREAD_IDLE || sample->state > XQ_SEARCH_THREAD_WAIT_HELPERS)
            ++display->counters.dropped;
        else
        {
            CliSampleSlot *slot = &display->slots[sample->thread_id];
            bool fresh_state = !slot->seen_state || sample->sequence > slot->state_sequence;
            if (!slot->seen_state || sample->sequence >= slot->state_sequence)
            {
                slot->state = sample->state;
                slot->state_depth = sample->iteration_depth;
                slot->state_sequence = sample->sequence;
                slot->seen_state = true;
            }
            if (display->timeline && update->event == XQ_SEARCH_THREAD_STATE && fresh_state)
            {
                CliTimelineEvent event = {0};
                event.kind = CLI_TIMELINE_STATE;
                event.sample = *sample;
                event.sample.path = NULL;
                event.sample.path_length = 0;
                enqueue_record(display, &event);
            }
            if (update->event == XQ_SEARCH_PATH_SAMPLE)
            {
                if ((sample->state != XQ_SEARCH_THREAD_SEARCH && sample->state != XQ_SEARCH_THREAD_QUIESCENCE) ||
                    sample->path_length > XQ_SEARCH_SAMPLE_MAX_MOVES ||
                    (sample->path_length && !sample->path)) ++display->counters.dropped;
                else if (slot->seen_request && sample->request_id <= slot->last_request)
                    ++display->counters.duplicates;
                else if (display->timeline && (!fresh_state || sample->sequence == 0))
                    ++display->counters.dropped;
                else
                {
                    CliTimelineEvent event = {0};
                    event.kind = CLI_TIMELINE_SAMPLE;
                    event.sample = *sample;
                    event.sample.path = NULL;
                    if (sample->path_length)
                    {
                        XqMove *path = calloc(sample->path_length, sizeof(*path));
                        if (path)
                        {
                            memcpy(path, sample->path, sample->path_length * sizeof(*path));
                            event.sample.path = path;
                        }
                        else
                        {
                            /* This marker needs no allocation and uses the same
                             * backpressure queue. The writer makes both reports
                             * explicitly incomplete rather than hiding the loss. */
                            event.kind = CLI_SAMPLE_ALLOCATION_FAILURE;
                            ++display->counters.dropped;
                        }
                    }
                    size_t index = (size_t)(sample->request_id % CLI_SAMPLE_REQUEST_HISTORY);
                    event.requested_at = sample->request_id && display->request_ids[index] == sample->request_id ?
                                         display->request_times[index] : 0;
                    slot->seen_request = true;
                    slot->last_request = sample->request_id;
                    if (sample->request_id < display->requested) ++display->counters.delayed;
                    else if (event.requested_at && sample->timestamp_ms > event.requested_at &&
                             sample->timestamp_ms - event.requested_at > display->interval_ms)
                        ++display->counters.delayed;
                    enqueue_record(display, &event);
                }
            }
        }
        unlock_display(display);
        return;
    }
    if (display->threaded) lock_display(display);
    if (update->event == XQ_SEARCH_ITERATION_STARTED)
    {
        display->latest = *update;
        display->latest.sample = NULL;
    }
    else if (update->depth == display->latest.depth)
    {
        if (update->completed_moves > display->latest.completed_moves)
            display->latest.completed_moves = update->completed_moves;
        display->latest.event = update->event;
    }
    if (display->sampler && !display->timeline && (update->event == XQ_SEARCH_ITERATION_STARTED ||
                            update->event == XQ_SEARCH_ITERATION_FINISHED))
    {
        CliTimelineEvent event = {0};
        event.kind = CLI_SAMPLE_BOUNDARY;
        event.sample.iteration_depth = update->depth;
        event.finished = update->event == XQ_SEARCH_ITERATION_FINISHED;
        enqueue_record(display, &event);
    }
    signal_display(display);
    if (display->threaded) unlock_display(display);
}

void cli_search_progress_finish(CliSearchProgress *display, const char *status,
                                const XqSearchStats *stats)
{
    if (display == NULL) return;
    if (display->threaded)
    {
        lock_display(display);
        display->stop = true;
        signal_display(display);
        unlock_display(display);
#ifdef _WIN32
        WaitForSingleObject(display->thread, INFINITE);
        CloseHandle(display->thread);
#else
        pthread_join(display->thread, NULL);
#endif
        /* No search callback remains (the public finish contract). Stop the
         * ticker first, then let the report owner drain every accepted event. */
        join_writer(display, false);
#ifdef _WIN32
        DeleteCriticalSection(&display->mutex);
#else
        pthread_cond_destroy(&display->record_space);
        pthread_cond_destroy(&display->record_ready);
        pthread_cond_destroy(&display->changed);
        pthread_mutex_destroy(&display->mutex);
#endif
        if (display->terminal)
        {
            unsigned columns = terminal_width();
            size_t clear_to = columns > 1 ? columns - 1 : 1;
            if (clear_to > display->previous_width) clear_to = display->previous_width;
            fputc('\r', stderr);
            for (size_t i = 0; i < clear_to; ++i) fputc(' ', stderr);
            fputc('\r', stderr);
        }
    }
    if (display->show_progress)
    {
        char text[256];
        format_progress(display, display->latest, sizeof(text) - 1, text, sizeof(text));
        fprintf(stderr, "%s; %s", text, status);
        if (stats != NULL && stats->available)
            fprintf(stderr, "; selected depth %u", stats->selected_move_depth);
        fputc('\n', stderr);
        fflush(stderr);
    }
    free_sampling_buffers(display);
    free(display);
}
