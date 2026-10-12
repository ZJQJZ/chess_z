#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#else
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#endif
#include "search_progress.h"

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

struct CliSearchProgress
{
    XqSearchProgress latest;
    unsigned max_depth;
    uint64_t budget_ms;
    uint64_t started_ms;
    bool clock_available;
    bool threaded;
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
    char text[256];
    unsigned columns = overwrite ? terminal_width() : (unsigned)sizeof(text);
    unsigned width = columns > 1 ? columns - 1 : 1; /* Avoid terminal auto-wrap. */
    size_t length, clear_to;
    format_progress(display, snapshot, width, text, sizeof(text));
    if (overwrite) fputc('\r', stderr);
    fputs(text, stderr);
    length = strlen(text);
    clear_to = display->previous_width < width ? display->previous_width : width;
    if (overwrite)
    {
        for (size_t i = length; i < clear_to; ++i) fputc(' ', stderr);
        display->previous_width = length;
    }
    else
        fputc('\n', stderr);
    fflush(stderr);
}

#ifdef _WIN32
static DWORD WINAPI display_main(LPVOID argument)
#else
static void *display_main(void *argument)
#endif
{
    CliSearchProgress *display = argument;
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
        /* Only stop signals this condition. Spurious wakes reuse the original deadline. */
        while (!display->stop)
        {
            int error = pthread_cond_timedwait(&display->changed, &display->mutex, &deadline);
            if (error != 0) break;
        }
#endif
    }
    unlock_display(display);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

CliSearchProgress *cli_search_progress_start(unsigned max_depth, uint64_t time_limit_ms)
{
    CliSearchProgress *display = calloc(1, sizeof(*display));
    if (display == NULL)
    {
        fprintf(stderr, "warning: cannot allocate search progress display; progress disabled\n");
        return NULL;
    }
    display->max_depth = max_depth;
    display->budget_ms = time_limit_ms;
    display->clock_available = monotonic_ms(&display->started_ms);
    if (!stderr_is_terminal()) return display;
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
    fprintf(stderr, "warning: cannot start search progress display; using iteration summaries\n");
    return display;
}

void cli_search_progress_update(const XqSearchProgress *update, void *user)
{
    CliSearchProgress *display = user;
    if (display == NULL) return;
    /* Without a display thread, only caller-thread iteration boundaries need recording.
     * In particular, resource-failure fallback needs no mutex for worker callbacks. */
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
    if (display->threaded)
        unlock_display(display);
    else if (update->event == XQ_SEARCH_ITERATION_FINISHED)
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
#ifdef _WIN32
        WakeConditionVariable(&display->changed);
#else
        pthread_cond_signal(&display->changed);
#endif
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
        /* Replace the live line with the final snapshot plus outcome in one retained line. */
        unsigned columns = terminal_width();
        size_t clear_to = columns > 1 ? columns - 1 : 1;
        if (clear_to > display->previous_width) clear_to = display->previous_width;
        fputc('\r', stderr);
        for (size_t i = 0; i < clear_to; ++i) fputc(' ', stderr);
        fputc('\r', stderr);
    }
    {
        char text[256];
        format_progress(display, display->latest, sizeof(text) - 1, text, sizeof(text));
        fprintf(stderr, "%s; %s", text, status);
        if (stats != NULL && stats->available)
            fprintf(stderr, "; selected depth %u", stats->selected_move_depth);
        fputc('\n', stderr);
        fflush(stderr);
    }
    free(display);
}
