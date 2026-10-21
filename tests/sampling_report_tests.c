#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "search_progress.h"
#include "search_sampling_internal.h"
#include "xiangqi/movegen.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "check failed: %s (%s:%d)\n", #condition, __FILE__, __LINE__); \
    exit(EXIT_FAILURE); } } while (0)

static void drain_collector(void)
{
    struct timespec delay = {0, 5000000};
    nanosleep(&delay, NULL);
}

typedef struct Capture
{
    CliSearchProgress *display;
    XqSearchSample last;
    XqMove path[XQ_SEARCH_SAMPLE_MAX_MOVES];
} Capture;

static void forward(const XqSearchProgress *progress, void *user)
{
    Capture *capture = user;
    if (progress->event == XQ_SEARCH_PATH_SAMPLE)
    {
        capture->last = *progress->sample;
        for (size_t i = 0; i < capture->last.path_length; ++i)
            capture->path[i] = capture->last.path[i];
        capture->last.path = capture->path;
    }
    cli_search_progress_update(progress, capture->display);
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    XqPosition position;
    XqMoveList legal;
    xq_position_startpos(&position);
    Capture capture = {0};
    capture.display = cli_search_progress_start_sampling(2, 0, false, 2, 1,
                                                        argv[1], &position, "test");
    CHECK(capture.display != NULL);
    XqSearchSampler *sampler = cli_search_progress_sampler(capture.display);
    CHECK(sampler != NULL);
    XqSamplingThread *thread = xq_sampling_thread(sampler, 0);
    XqSamplingThread *helper = xq_sampling_thread(sampler, 1);
    XqSearchProgress event = {XQ_SEARCH_ITERATION_STARTED, 1, 0, 2, NULL};
    forward(&event, &capture);
    xq_sampling_begin(thread, 1, forward, &capture);
    xq_generate_legal(&position, &legal);
    XqMove first = legal.moves[0];
    xq_sampling_push(thread, first, false);
    xq_search_sampler_request(sampler);
    xq_sampling_checkpoint(thread, false);
    drain_collector();
    CHECK(capture.last.path_length == 1);
    XqSearchProgress duplicate = {XQ_SEARCH_PATH_SAMPLE, 1, 0, 0, &capture.last};
    cli_search_progress_update(&duplicate, capture.display);
    drain_collector();

    CHECK(xq_position_make_move(&position, first));
    xq_generate_legal(&position, &legal);
    xq_sampling_push(thread, legal.moves[0], true);
    xq_search_sampler_request(sampler);
    xq_sampling_checkpoint(thread, true);
    drain_collector();
    CHECK(capture.last.normal_ply == 1 && capture.last.quiescence_ply == 1);
    xq_sampling_inherit(helper, thread);
    xq_sampling_state(helper, XQ_SEARCH_THREAD_SEARCH);
    xq_search_sampler_request(sampler);
    xq_sampling_checkpoint(helper, true);
    drain_collector();
    xq_sampling_state(helper, XQ_SEARCH_THREAD_IDLE);
    xq_sampling_pop(thread, true);
    xq_sampling_pop(thread, false);
    xq_sampling_state(thread, XQ_SEARCH_THREAD_IDLE);
    event = (XqSearchProgress){XQ_SEARCH_ITERATION_FINISHED, 1, 2, 2, NULL};
    forward(&event, &capture);
    event = (XqSearchProgress){XQ_SEARCH_ITERATION_STARTED, 2, 0, 2, NULL};
    forward(&event, &capture);
    xq_sampling_begin(thread, 2, forward, &capture);
    for (size_t i = 0; i < XQ_SEARCH_SAMPLE_MAX_MOVES + 3; ++i)
        xq_sampling_push(thread, first, i > 0);
    xq_search_sampler_request(sampler);
    xq_sampling_checkpoint(thread, true);
    drain_collector();
    CHECK(capture.last.truncated);
    CHECK(capture.last.path_length == XQ_SEARCH_SAMPLE_MAX_MOVES);
    for (size_t i = 0; i < XQ_SEARCH_SAMPLE_MAX_MOVES + 3; ++i)
        xq_sampling_pop(thread, i < XQ_SEARCH_SAMPLE_MAX_MOVES + 2);
    xq_sampling_state(thread, XQ_SEARCH_THREAD_IDLE);
    event = (XqSearchProgress){XQ_SEARCH_ITERATION_FINISHED, 2, 2, 2, NULL};
    forward(&event, &capture);
    XqSearchStats stats = {0};
    cli_search_progress_finish(capture.display, "test complete", &stats);
    puts("sampling report tests passed");
    return EXIT_SUCCESS;
}
