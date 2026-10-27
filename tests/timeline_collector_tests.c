#define _POSIX_C_SOURCE 200809L
#include "search_progress.h"
#include "xiangqi/movegen.h"
#include "xiangqi/parallel_search.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CHECK(value) do { if (!(value)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #value); abort(); \
} } while (0)

typedef struct TestCase
{
    const char *name;
    uint64_t search_id;
    unsigned threads;
} TestCase;

static const char *position_fen = "4k4/9/9/9/4p4/4P4/9/9/9/4K4 r - -";
static TestCase cases[12];
static unsigned case_count;

static uint64_t now_ms(void)
{
    struct timespec now;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}

static void pause_ms(unsigned milliseconds)
{
    struct timespec delay = {(time_t)(milliseconds / 1000), (long)(milliseconds % 1000) * 1000000};
    nanosleep(&delay, NULL);
}

static CliSearchProgress *start(const char *directory, const char *name, unsigned threads,
                                XqPosition *position)
{
    CHECK(xq_position_from_fen(position, position_fen));
    CliSearchProgress *display = cli_search_progress_start_timeline(6, 0, false, threads,
        1, directory, position, name, UINT64_C(512) * 1024 * 1024);
    CHECK(display != NULL && cli_search_progress_sampler(display) != NULL);
    cases[case_count++] = (TestCase){name,
        xq_search_sampler_id(cli_search_progress_sampler(display)), threads};
    return display;
}

static int paced_evaluate(const XqPosition *position, XqColor perspective, void *user)
{
    (void)user;
    /* Short deterministic delays spread cooperative replies across real wall
     * time without changing the evaluator or search tree. */
    static _Thread_local unsigned evaluations;
    if (++evaluations % 4 == 0)
    {
        struct timespec delay = {0, 100000};
        nanosleep(&delay, NULL);
    }
    return xq_engine_default_static_evaluate(position, perspective, NULL);
}

static void real_searches(const char *directory)
{
    XqPosition position;
    CliSearchProgress *display = start(directory, "real-builtin", 1, &position);
    XqEngineAdapter engine = {0};
    engine.static_evaluate = paced_evaluate;
    XqSearchLimits limits = xq_search_limits_default();
    limits.max_depth = 6;
    limits.time_limit_ms = 0;
    XqSearchStats stats;
    XqMove move;
    CHECK(xq_engine_find_best_move_with_sampling(&engine, &position, &limits, &move,
          &stats, cli_search_progress_update, display, cli_search_progress_sampler(display)));
    CHECK(stats.completed_depth == 6);
    cli_search_progress_finish(display, "done", &stats);
    const unsigned threads[] = {1, 2, 16};
    static const char *names[2][3] = {
        {"real-contiguous-1", "real-contiguous-2", "real-contiguous-16"},
        {"real-interleaved-1", "real-interleaved-2", "real-interleaved-16"}
    };
    for (unsigned strategy = 0; strategy < 2; ++strategy)
        for (unsigned i = 0; i < 3; ++i)
        {
            display = start(directory, names[strategy][i], threads[i], &position);
            XqParallelOptions options = xq_parallel_options_default();
            options.depth = 6;
            options.thread_count = threads[i];
            options.split_strategy = (XqParallelSplitStrategy)strategy;
            XqParallelResult result;
            CHECK(xq_engine_parallel_search_with_observer(&engine, &position, &options,
                  &result, cli_search_progress_update, display,
                  cli_search_progress_sampler(display)) == XQ_PARALLEL_OK);
            CHECK(result.stats.completed_depth == 6);
            cli_search_progress_finish(display, "done", &result.stats);
        }
}

static void emit(CliSearchProgress *display, XqSearchSample *sample, bool path)
{
    XqSearchProgress event = {path ? XQ_SEARCH_PATH_SAMPLE : XQ_SEARCH_THREAD_STATE,
                              sample->iteration_depth, 0, 0, sample};
    cli_search_progress_update(&event, display);
}

static void root_progress(CliSearchProgress *display, XqSearchProgressEvent kind)
{
    XqSearchProgress event = {kind, 1, 0, 1, NULL};
    cli_search_progress_update(&event, display);
}

static void synthetic_search(const char *directory)
{
    XqPosition position;
    CliSearchProgress *display = start(directory, "synthetic", 3, &position);
    uint64_t search_id = xq_search_sampler_id(cli_search_progress_sampler(display));
    uint64_t stamp = now_ms();
    XqSearchSample caller = {.search_id = search_id, .thread_id = 0, .sequence = 1,
        .timestamp_ms = stamp, .state = XQ_SEARCH_THREAD_IDLE};
    XqSearchSample helper = caller;
    helper.thread_id = 1;
    emit(display, &caller, false);
    emit(display, &helper, false);
    /* Slot 2 is never registered: it must remain unknown, not fabricated IDLE. */
    pause_ms(5);
    caller.iteration_depth = helper.iteration_depth = 1;
    caller.state = XQ_SEARCH_THREAD_SEARCH;
    caller.timestamp_ms = now_ms();
    ++caller.sequence;
    emit(display, &caller, false); /* A sample may precede root STARTED. */
    pause_ms(3);
    caller.request_id = UINT64_C(100000000);
    caller.timestamp_ms = now_ms();
    ++caller.sequence;
    emit(display, &caller, true); /* True root zero-step path. */
    root_progress(display, XQ_SEARCH_ITERATION_STARTED);
    pause_ms(3);
    ++caller.sequence;
    emit(display, &caller, true); /* Duplicate request must not count twice. */
    helper.state = XQ_SEARCH_THREAD_SEARCH;
    helper.timestamp_ms = now_ms();
    ++helper.sequence;
    emit(display, &helper, false);
    helper.state = XQ_SEARCH_THREAD_WAIT_HELPERS;
    ++helper.sequence;
    emit(display, &helper, false);
    emit(display, &helper, false); /* Duplicate sequence must not create a state. */
    pause_ms(5);
    helper.state = XQ_SEARCH_THREAD_SEARCH;
    helper.timestamp_ms = now_ms();
    ++helper.sequence;
    emit(display, &helper, false);
    helper.state = XQ_SEARCH_THREAD_IDLE;
    ++helper.sequence;
    emit(display, &helper, false);
    XqMoveList moves;
    xq_generate_legal(&position, &moves);
    CHECK(moves.count != 0);
    caller.state = XQ_SEARCH_THREAD_QUIESCENCE;
    caller.path = &moves.moves[0];
    caller.path_length = 1;
    caller.quiescence_ply = 1;
    caller.timestamp_ms = now_ms();
    ++caller.sequence;
    ++caller.request_id;
    emit(display, &caller, true);
    pause_ms(3);
    caller.path = NULL;
    caller.path_length = caller.quiescence_ply = 0;
    caller.state = XQ_SEARCH_THREAD_SEARCH;
    caller.timestamp_ms = 0;
    ++caller.sequence;
    ++caller.request_id;
    emit(display, &caller, true); /* Clock unavailable: preserved as unknown time. */
    pause_ms(3);
    caller.timestamp_ms = now_ms();
    ++caller.sequence;
    emit(display, &caller, false);
    XqSearchSample stale = caller;
    --stale.sequence;
    ++stale.request_id;
    emit(display, &stale, true); /* New request cannot rescue a stale sequence. */
    caller.state = XQ_SEARCH_THREAD_WAIT_HELPERS;
    caller.timestamp_ms -= 1000;
    ++caller.sequence;
    emit(display, &caller, false); /* Backwards/before-origin time is unknown. */
    pause_ms(3);
    caller.state = XQ_SEARCH_THREAD_SEARCH;
    caller.timestamp_ms = now_ms();
    ++caller.sequence;
    emit(display, &caller, false);
    root_progress(display, XQ_SEARCH_ITERATION_FINISHED);
    pause_ms(3); /* FINISHED cannot reclaim the pending caller IDLE. */
    caller.state = XQ_SEARCH_THREAD_IDLE;
    caller.timestamp_ms = now_ms();
    ++caller.sequence;
    emit(display, &caller, false);
    XqSearchStats stats = {.available = true, .completed_depth = 1};
    cli_search_progress_finish(display, "done", &stats);
}

static void overflow_search(const char *directory)
{
    /* Retain the historical case name, but exercise a tiny lossless queue with
     * a short mixed burst instead of relying on a state-event overflow. */
    XqPosition position;
    CliSearchProgress *display = start(directory, "overflow", 2, &position);
    XqMoveList legal;
    XqMove root_moves[2];
    const char *move_names[] = {"e0d0", "e0f0"};
    xq_generate_legal(&position, &legal);
    for (unsigned choice = 0; choice < 2; ++choice)
    {
        bool found = false;
        for (int i = 0; i < legal.count; ++i)
        {
            char text[8];
            if (strcmp(xq_move_to_string(legal.moves[i], text, sizeof(text)), move_names[choice]) == 0)
            {
                root_moves[choice] = legal.moves[i];
                found = true;
                break;
            }
        }
        CHECK(found);
    }
    XqSearchSample caller = {.search_id = xq_search_sampler_id(cli_search_progress_sampler(display)),
        .thread_id = 0, .sequence = 1, .timestamp_ms = now_ms(), .state = XQ_SEARCH_THREAD_IDLE};
    XqSearchSample helper = caller;
    helper.thread_id = 1;
    emit(display, &caller, false);
    emit(display, &helper, false);
    caller.iteration_depth = helper.iteration_depth = 1;
    root_progress(display, XQ_SEARCH_ITERATION_STARTED);
    caller.state = XQ_SEARCH_THREAD_SEARCH;
    caller.timestamp_ms = now_ms();
    ++caller.sequence;
    emit(display, &caller, false);
    XqMove reused_path;
    unsigned sample_count = 0;
    for (unsigned i = 0; i < 32; ++i)
    {
        helper.state = XQ_SEARCH_THREAD_WAIT_HELPERS;
        helper.timestamp_ms = now_ms();
        ++helper.sequence;
        emit(display, &helper, false);
        helper.state = XQ_SEARCH_THREAD_SEARCH;
        helper.timestamp_ms = now_ms();
        ++helper.sequence;
        emit(display, &helper, false);
        if (i == 0 || i == 15 || i == 31)
        {
            reused_path = root_moves[sample_count % 2];
            caller.path = &reused_path;
            caller.path_length = caller.normal_ply = 1;
            caller.request_id = UINT64_C(100000001) + sample_count;
            caller.timestamp_ms = now_ms();
            ++caller.sequence;
            emit(display, &caller, true);
            /* The callback must own a copy before returning. Delayed report
             * consumption cannot borrow this overwritten producer buffer. */
            memset(&reused_path, 0, sizeof(reused_path));
            ++sample_count;
        }
    }
    CHECK(sample_count == 3);
    helper.state = XQ_SEARCH_THREAD_IDLE;
    helper.timestamp_ms = now_ms();
    ++helper.sequence;
    emit(display, &helper, false);
    root_progress(display, XQ_SEARCH_ITERATION_FINISHED);
    caller.path = NULL;
    caller.path_length = caller.normal_ply = 0;
    caller.state = XQ_SEARCH_THREAD_IDLE;
    caller.timestamp_ms = now_ms();
    ++caller.sequence;
    emit(display, &caller, false);
    /* Do not pause: finish must drain the tail of the bounded FIFO as well. */
    XqSearchStats stats = {.available = true, .completed_depth = 1};
    cli_search_progress_finish(display, "done", &stats);
}

int main(int argc, char **argv)
{
    const char *directory = argc >= 2 ? argv[1] : "build/timeline_validation/integration";
    bool overflow_only = argc >= 3 && strcmp(argv[2], "overflow") == 0;
    alarm(40);
    if (overflow_only)
        overflow_search(directory);
    else
    {
        real_searches(directory);
        synthetic_search(directory);
    }
    char path[4096];
    CHECK(snprintf(path, sizeof(path), "%s/collector-cases.json", directory) < (int)sizeof(path));
    FILE *file = fopen(path, "w");
    CHECK(file != NULL);
    fprintf(file, "{\"pid\":%ld,\"overflow_only\":%s,\"cases\":[", (long)getpid(), overflow_only ? "true" : "false");
    for (unsigned i = 0; i < case_count; ++i)
        fprintf(file, "%s{\"name\":\"%s\",\"search_id\":%" PRIu64 ",\"threads\":%u}",
                i ? "," : "", cases[i].name, cases[i].search_id, cases[i].threads);
    CHECK(fputs("]}\n", file) >= 0 && fclose(file) == 0);
    puts("timeline collector tests passed");
    return 0;
}
