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

#define CLI_SAMPLE_BOUNDARIES 1024u
#define CLI_SAMPLE_REQUEST_HISTORY 1024u

typedef struct CliSampleSlot
{
    XqSearchSample sample;
    XqMove path[XQ_SEARCH_SAMPLE_MAX_MOVES];
    uint64_t last_request, state_sequence;
    XqSearchThreadState state;
    unsigned state_depth;
    bool ready, seen_request, seen_state;
} CliSampleSlot;

typedef struct CliSampleBoundary
{
    unsigned depth;
    bool finished;
} CliSampleBoundary;

struct CliSearchProgress
{
    XqSearchProgress latest;
    unsigned max_depth;
    uint64_t budget_ms;
    uint64_t started_ms;
    bool clock_available;
    bool threaded;
    bool show_progress, terminal;
    XqSearchSampler *sampler;
    CliSampleReport *report;
    CliSampleSlot *slots, *drain;
    unsigned thread_capacity;
    uint64_t interval_ms, requested;
    uint64_t *request_times;
    CliSampleBoundary *boundaries, *drain_boundaries;
    size_t boundary_head, boundary_count;
    CliSampleCounters counters;
    CliSampleSummary summary;
    unsigned active_threads;
    unsigned finished_depth;
    bool stop;
    size_t previous_width; /* Display-thread owned until joined. */
#ifdef _WIN32
    HANDLE thread;
    CRITICAL_SECTION mutex;
    CONDITION_VARIABLE changed;
#else
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t changed;
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
        char base[256] = "";
        if (display->show_progress)
            format_progress(display, snapshot, sizeof(base) - 1, base, sizeof(base));
        else
            snprintf(base, sizeof(base), "search depth %u", snapshot.depth);
        /* Keep samples visible even on narrow terminals. The full prefix remains in reports. */
        snprintf(text, sizeof(text), "d%u samples/5s %" PRIu64 " active %u/%u maxQ %" PRIu64
                 "%s hot %s", snapshot.depth, display->summary.recent_samples,
                 display->active_threads, display->thread_capacity,
                 display->summary.max_quiescence_ply, display->summary.incomplete ? " incomplete" : "",
                 display->summary.hot_path[0] ? display->summary.hot_path : "-");
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
                     display->summary.recent_samples, display->active_threads,
                     display->thread_capacity, display->summary.max_quiescence_ply,
                     display->summary.hot_path[0] ? display->summary.hot_path : "-");
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

#ifdef _WIN32
static DWORD WINAPI display_main(LPVOID argument)
#else
static void *display_main(void *argument)
#endif
{
    CliSearchProgress *display = argument;
    if (display->sampler == NULL)
    {
        display_progress_only(display);
#ifdef _WIN32
        return 0;
#else
        return NULL;
#endif
    }
    uint64_t next_request = 0, next_draw = 0;
    for (;;)
    {
        CliSampleBoundary *boundaries = display->drain_boundaries;
        size_t boundary_count;
        CliSampleCounters counters;
        unsigned active = 0;
        lock_display(display);
        bool stopping = display->stop;
        XqSearchProgress snapshot = display->latest;
        unsigned finished_depth = display->finished_depth;
        uint64_t now = display->started_ms;
        monotonic_ms(&now);
        if (display->sampler && !stopping && now >= next_request)
        {
            ++display->requested;
            display->request_times[display->requested % CLI_SAMPLE_REQUEST_HISTORY] = now;
            xq_search_sampler_request(display->sampler);
            next_request = deadline_after(now, display->interval_ms);
        }
        for (unsigned i = 0; i < display->thread_capacity; ++i)
        {
            CliSampleSlot *slot = &display->slots[i];
            CliSampleSlot *drain = &display->drain[i];
            drain->ready = slot->ready;
            if (slot->ready)
            {
                drain->sample = slot->sample;
                memcpy(drain->path, slot->path, slot->sample.path_length * sizeof(*slot->path));
                drain->sample.path = drain->path;
                slot->ready = false;
            }
            if (slot->seen_state && slot->state_depth == snapshot.depth &&
                (slot->state == XQ_SEARCH_THREAD_SEARCH || slot->state == XQ_SEARCH_THREAD_QUIESCENCE)) ++active;
        }
        boundary_count = display->boundary_count;
        for (size_t i = 0; i < boundary_count; ++i)
            boundaries[i] = display->boundaries[(display->boundary_head + i) % CLI_SAMPLE_BOUNDARIES];
        display->boundary_head = (display->boundary_head + boundary_count) % CLI_SAMPLE_BOUNDARIES;
        display->boundary_count = 0;
        counters = display->counters;
        unlock_display(display);

        /* Every borrowed sample is copied before the callback returns. The following
         * trie work and all filesystem/terminal I/O run without the callback mutex. */
        for (unsigned i = 0; i < display->thread_capacity; ++i)
            if (display->drain[i].ready)
                cli_sample_report_sample(display->report, &display->drain[i].sample);
        cli_sample_report_tick(display->report, now, &counters);
        /* Drain samples before FINISHED: a sample in the final per-thread slot must
         * be included in its iteration before that iteration is written and freed. */
        for (size_t i = 0; i < boundary_count; ++i)
            cli_sample_report_boundary(display->report, boundaries[i].depth, boundaries[i].finished);
        /* A full boundary ring may lose lifecycle events, never the completion
         * watermark. Reclaim every known completed iteration even in that case. */
        cli_sample_report_complete_through(display->report, finished_depth);
        display->active_threads = active;
        cli_sample_report_summary(display->report, snapshot.depth, now, &display->summary);
        if (!stopping && display->terminal && (display->show_progress || display->sampler) && now >= next_draw)
        {
            draw(display, snapshot, true);
            next_draw = deadline_after(now, 100);
        }
        if (stopping)
        {
            cli_sample_report_finish(display->report, &counters);
            display->report = NULL;
            break;
        }
        lock_display(display);
        if (!display->stop)
        {
            uint64_t after = now;
            monotonic_ms(&after);
            uint64_t deadline = next_draw;
            if (display->sampler && (!display->terminal || next_request < deadline)) deadline = next_request;
            uint64_t delay = deadline > after ? deadline - after : 1;
            /* Even long custom sample intervals keep shutdown/progress responsive. */
            if (delay > 100) delay = 100;
            wait_display(display, delay);
        }
        unlock_display(display);
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

static CliSearchProgress *start_display(unsigned max_depth, uint64_t time_limit_ms,
                                       bool show_progress, unsigned thread_capacity,
                                       uint64_t interval_ms, const char *output_dir,
                                       const XqPosition *position, const char *mode)
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
        display->drain = calloc(thread_capacity, sizeof(*display->drain));
        display->boundaries = calloc(CLI_SAMPLE_BOUNDARIES, sizeof(*display->boundaries));
        display->drain_boundaries = calloc(CLI_SAMPLE_BOUNDARIES, sizeof(*display->drain_boundaries));
        display->request_times = calloc(CLI_SAMPLE_REQUEST_HISTORY, sizeof(*display->request_times));
        if (display->sampler && display->slots && display->drain && display->boundaries &&
            display->drain_boundaries &&
            display->request_times && display->clock_available)
            display->report = cli_sample_report_create(output_dir, position, mode,
                xq_search_sampler_id(display->sampler), thread_capacity,
                display->interval_ms, max_depth, time_limit_ms);
        if (!display->report)
        {
            fputs("warning: cannot allocate search sampler; sampling disabled\n", stderr);
            xq_search_sampler_destroy(display->sampler);
            display->sampler = NULL;
            free(display->slots); display->slots = NULL;
            free(display->drain); display->drain = NULL;
            free(display->boundaries); display->boundaries = NULL;
            free(display->drain_boundaries); display->drain_boundaries = NULL;
            free(display->request_times); display->request_times = NULL;
        }
        else display->thread_capacity = thread_capacity;
    }
    if (!display->sampler && (!show_progress || !display->terminal)) return display;
#ifdef _WIN32
    if (!InitializeCriticalSectionEx(&display->mutex, 0, 0)) goto fallback;
    InitializeConditionVariable(&display->changed);
    display->thread = CreateThread(NULL, 0, display_main, display, 0, NULL);
    if (display->thread == NULL)
    {
        DeleteCriticalSection(&display->mutex);
        goto fallback;
    }
#else
    if (pthread_mutex_init(&display->mutex, NULL) != 0) goto fallback;
    if (pthread_cond_init(&display->changed, NULL) != 0)
    {
        pthread_mutex_destroy(&display->mutex);
        goto fallback;
    }
    if (pthread_create(&display->thread, NULL, display_main, display) != 0)
    {
        pthread_cond_destroy(&display->changed);
        pthread_mutex_destroy(&display->mutex);
        goto fallback;
    }
#endif
    display->threaded = true;
    return display;
fallback:
    fputs("warning: cannot start search collector/display; sampling disabled, using iteration summaries\n", stderr);
    cli_sample_report_finish(display->report, &display->counters);
    display->report = NULL;
    xq_search_sampler_destroy(display->sampler);
    display->sampler = NULL;
    free(display->slots); display->slots = NULL;
    free(display->drain); display->drain = NULL;
    free(display->boundaries); display->boundaries = NULL;
    free(display->drain_boundaries); display->drain_boundaries = NULL;
    free(display->request_times); display->request_times = NULL;
    display->thread_capacity = 0;
    return display;
}

CliSearchProgress *cli_search_progress_start(unsigned max_depth, uint64_t time_limit_ms)
{
    return start_display(max_depth, time_limit_ms, true, 0, 20, NULL, NULL, NULL);
}

CliSearchProgress *cli_search_progress_start_sampling(unsigned max_depth, uint64_t time_limit_ms,
                                                     bool show_progress, unsigned thread_capacity,
                                                     uint64_t interval_ms, const char *output_dir,
                                                     const XqPosition *initial_position,
                                                     const char *search_mode)
{
    return start_display(max_depth, time_limit_ms, show_progress,
                         thread_capacity ? thread_capacity : 1, interval_ms,
                         output_dir, initial_position, search_mode);
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
        if (!display->threaded || !display->sampler || !update->sample) return;
        const XqSearchSample *sample = update->sample;
        lock_display(display);
        if (sample->thread_id >= display->thread_capacity ||
            sample->search_id != xq_search_sampler_id(display->sampler)) ++display->counters.dropped;
        else
        {
            CliSampleSlot *slot = &display->slots[sample->thread_id];
            if (!slot->seen_state || sample->sequence >= slot->state_sequence)
            {
                slot->state = sample->state;
                slot->state_depth = sample->iteration_depth;
                slot->state_sequence = sample->sequence;
                slot->seen_state = true;
            }
            if (update->event == XQ_SEARCH_PATH_SAMPLE)
            {
                if ((sample->state != XQ_SEARCH_THREAD_SEARCH && sample->state != XQ_SEARCH_THREAD_QUIESCENCE) ||
                    sample->path_length > XQ_SEARCH_SAMPLE_MAX_MOVES ||
                    (sample->path_length && !sample->path)) ++display->counters.dropped;
                else if (slot->seen_request && sample->request_id <= slot->last_request)
                    ++display->counters.duplicates;
                else
                {
                    if (slot->ready) ++display->counters.overwritten;
                    slot->sample = *sample;
                    if (sample->path_length)
                        memcpy(slot->path, sample->path, sample->path_length * sizeof(*sample->path));
                    slot->sample.path = slot->path;
                    slot->ready = slot->seen_request = true;
                    slot->last_request = sample->request_id;
                    if (sample->request_id < display->requested) ++display->counters.delayed;
                    else if (sample->request_id <= display->requested &&
                             display->requested - sample->request_id < CLI_SAMPLE_REQUEST_HISTORY)
                    {
                        uint64_t requested_at = display->request_times[sample->request_id % CLI_SAMPLE_REQUEST_HISTORY];
                        if (sample->timestamp_ms > requested_at &&
                            sample->timestamp_ms - requested_at > display->interval_ms)
                            ++display->counters.delayed;
                    }
                }
            }
        }
        unlock_display(display);
        return;
    }
    if (!display->threaded && update->event == XQ_SEARCH_ROOT_MOVE_COMPLETED) return;
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
    if (display->sampler && (update->event == XQ_SEARCH_ITERATION_STARTED ||
                            update->event == XQ_SEARCH_ITERATION_FINISHED))
    {
        if (update->event == XQ_SEARCH_ITERATION_FINISHED && update->depth > display->finished_depth)
            display->finished_depth = update->depth;
        if (display->boundary_count == CLI_SAMPLE_BOUNDARIES) ++display->counters.boundaries_dropped;
        else
        {
            size_t index = (display->boundary_head + display->boundary_count++) % CLI_SAMPLE_BOUNDARIES;
            display->boundaries[index] = (CliSampleBoundary){update->depth,
                update->event == XQ_SEARCH_ITERATION_FINISHED};
        }
        signal_display(display);
    }
    if (display->threaded) unlock_display(display);
    else if (display->show_progress && update->event == XQ_SEARCH_ITERATION_FINISHED)
        draw(display, display->latest, false);
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
        DeleteCriticalSection(&display->mutex);
#else
        pthread_join(display->thread, NULL);
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
    xq_search_sampler_destroy(display->sampler);
    free(display->slots);
    free(display->drain);
    free(display->boundaries);
    free(display->drain_boundaries);
    free(display->request_times);
    free(display);
}
