#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "xiangqi/parallel_search.h"
#include "xiangqi/movegen.h"
#include "../src/search_sampling_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef _WIN32
#include <pthread.h>
#endif

/* Keep checks enabled in Release builds too. */
#define CHECK(value) do { if (!(value)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #value); abort(); \
} } while (0)

typedef struct PathCheck
{
    XqSearchSampler *sampler;
    XqPosition root;
    uint64_t request[16], sequence[16];
    uint64_t samples, normal, quiet, quiet_extensions, helpers, waits, idle;
    bool request_each_event;
#ifndef _WIN32
    pthread_mutex_t lock;
#endif
} PathCheck;

static void check_init(PathCheck *check, const char *fen, unsigned capacity)
{
    memset(check, 0, sizeof(*check));
    CHECK(xq_position_from_fen(&check->root, fen));
    check->sampler = xq_search_sampler_create(capacity);
    CHECK(check->sampler != NULL);
    check->request_each_event = true;
#ifndef _WIN32
    CHECK(pthread_mutex_init(&check->lock, NULL) == 0);
#endif
    xq_search_sampler_request(check->sampler);
}

static void check_destroy(PathCheck *check)
{
    xq_search_sampler_destroy(check->sampler);
#ifndef _WIN32
    CHECK(pthread_mutex_destroy(&check->lock) == 0);
#endif
}

static bool same_move(XqMove a, XqMove b)
{
    return a.from == b.from && a.to == b.to && a.piece == b.piece &&
           a.captured == b.captured;
}

static void validate_path(const PathCheck *check, const XqSearchSample *sample)
{
    CHECK(!sample->truncated);
    CHECK(sample->path_length == sample->normal_ply + sample->quiescence_ply);
    CHECK(sample->normal_ply <= sample->iteration_depth);
    XqPosition position = check->root;
    for (size_t i = 0; i < sample->path_length; ++i)
    {
        XqMove move = sample->path[i];
        CHECK(move.from < XQ_SQUARES && move.to < XQ_SQUARES);
        CHECK(position.board[move.from] == move.piece);
        CHECK(position.board[move.to] == move.captured);
        XqMoveList moves;
        xq_generate_pseudo_legal(&position, &moves);
        bool found = false;
        for (int j = 0; j < moves.count; ++j)
            if (same_move(move, moves.moves[j]))
                found = true;
        CHECK(found);
        CHECK(xq_position_make_move(&position, move));
    }
}

static void path_progress(const XqSearchProgress *event, void *user)
{
    PathCheck *check = user;
#ifndef _WIN32
    CHECK(pthread_mutex_lock(&check->lock) == 0);
#endif
    if (event->event == XQ_SEARCH_PATH_SAMPLE || event->event == XQ_SEARCH_THREAD_STATE)
    {
        const XqSearchSample *sample = event->sample;
        CHECK(sample != NULL && sample->thread_id < 16);
        CHECK(sample->search_id == xq_search_sampler_id(check->sampler));
        CHECK(sample->iteration_depth >= 1);
        CHECK(sample->sequence > check->sequence[sample->thread_id]);
        check->sequence[sample->thread_id] = sample->sequence;
        if (event->event == XQ_SEARCH_PATH_SAMPLE)
        {
            CHECK(sample->state == XQ_SEARCH_THREAD_SEARCH ||
                  sample->state == XQ_SEARCH_THREAD_QUIESCENCE);
            CHECK(sample->request_id > check->request[sample->thread_id]);
            check->request[sample->thread_id] = sample->request_id;
            validate_path(check, sample);
            ++check->samples;
            check->normal += sample->state == XQ_SEARCH_THREAD_SEARCH;
            check->quiet += sample->state == XQ_SEARCH_THREAD_QUIESCENCE;
            check->quiet_extensions += sample->quiescence_ply != 0;
            check->helpers += sample->thread_id != 0;
        }
        else
        {
            CHECK(sample->path_length == 0 && sample->path == NULL);
            check->waits += sample->state == XQ_SEARCH_THREAD_WAIT_HELPERS;
            check->idle += sample->state == XQ_SEARCH_THREAD_IDLE;
        }
    }
    else
        CHECK(event->sample == NULL);
    /* Request is the sole controller operation allowed concurrently with search.
     * Advancing here makes short-lived recursive entries deterministic in this unit test. */
    if (check->request_each_event)
        xq_search_sampler_request(check->sampler);
#ifndef _WIN32
    CHECK(pthread_mutex_unlock(&check->lock) == 0);
#endif
}

static const char *quiet_fen = "4k4/9/9/9/4p4/4P4/9/9/9/4K4 r - -";
static const char *capture_fen = "4k4/9/9/9/9/4p4/4P4/4R4/9/4K4 r - -";

static void builtin_paths(void)
{
    const char *fens[] = {quiet_fen, capture_fen};
    for (unsigned i = 0; i < 2; ++i)
    {
        PathCheck check;
        check_init(&check, fens[i], 1);
        XqSearchLimits limits = xq_search_limits_default();
        limits.max_depth = i == 0 ? 6 : 2;
        limits.time_limit_ms = 0;
        XqMove move;
        XqSearchStats stats;
        XqPosition position = check.root;
        XqMove plain_move;
        XqSearchStats plain_stats;
        CHECK(xq_engine_find_best_move_with_progress(NULL, &position, &limits, &plain_move,
              &plain_stats, NULL, NULL));
        uint64_t before = xq_position_hash(&check.root);
        CHECK(xq_engine_find_best_move_with_sampling(NULL, &position, &limits, &move,
              &stats, path_progress, &check, check.sampler));
        CHECK(xq_position_hash(&position) == before);
        CHECK(same_move(move, plain_move));
        CHECK(stats.nodes == plain_stats.nodes);
        CHECK(check.samples && check.normal && check.quiet && check.idle);
        if (i != 0)
            CHECK(check.quiet_extensions);
        check_destroy(&check);
    }
}

#ifndef _WIN32
static void parallel_paths(void)
{
    const unsigned counts[] = {1, 2, 16};
    for (unsigned strategy = 0; strategy < 2; ++strategy)
        for (unsigned c = 0; c < 3; ++c)
        {
            PathCheck check;
            check_init(&check, quiet_fen, counts[c]);
            XqParallelOptions options = xq_parallel_options_default();
            options.depth = 7; /* Root and descendant nodes both meet the split threshold. */
            options.thread_count = counts[c];
            options.split_strategy = (XqParallelSplitStrategy)strategy;
            XqParallelResult result;
            XqParallelResult plain_result;
            if (counts[c] == 1)
                CHECK(xq_engine_parallel_search(NULL, &check.root, &options,
                      &plain_result) == XQ_PARALLEL_OK);
            CHECK(xq_engine_parallel_search_with_observer(NULL, &check.root, &options,
                  &result, path_progress, &check, check.sampler) == XQ_PARALLEL_OK);
            CHECK(check.samples && check.normal && check.quiet && check.idle);
            if (counts[c] == 1)
            {
                CHECK(same_move(result.best_move, plain_result.best_move));
                CHECK(result.score == plain_result.score);
                CHECK(result.stats.nodes == plain_result.stats.nodes);
            }
            if (counts[c] > 1)
                CHECK(check.helpers);
            check_destroy(&check);
        }

    PathCheck check;
    check_init(&check, capture_fen, 2);
    XqParallelOptions options = xq_parallel_options_default();
    options.depth = 2;
    XqParallelResult result;
    CHECK(xq_engine_parallel_search_with_observer(NULL, &check.root, &options, &result,
          path_progress, &check, check.sampler) == XQ_PARALLEL_OK);
    CHECK(check.quiet_extensions);
    check_destroy(&check);
}

static int slow_evaluate(const XqPosition *position, XqColor perspective, void *user)
{
    (void)position;
    (void)perspective;
    (void)user;
    struct timespec delay = {0, 2000000};
    nanosleep(&delay, NULL);
    return 0;
}

static void cancellation(void)
{
    const char *start = "rnbakabnr/9/1c5c1/p1p1p1p1p/9/9/P1P1P1P1P/1C5C1/9/RNBAKABNR r - -";
    PathCheck check;
    check_init(&check, start, 16);
    XqEngineAdapter adapter = {0};
    adapter.static_evaluate = slow_evaluate;
    XqParallelOptions options = xq_parallel_options_default();
    options.depth = 10;
    options.thread_count = 16;
    options.time_limit_ms = 1;
    XqParallelResult result;
    CHECK(xq_engine_parallel_search_with_observer(&adapter, &check.root, &options, &result,
          path_progress, &check, check.sampler) == XQ_PARALLEL_OK);
    CHECK(result.stats.stopped);
    check_destroy(&check);

    check_init(&check, start, 1);
    XqSearchLimits limits = xq_search_limits_default();
    limits.max_depth = 10;
    limits.time_limit_ms = 1;
    XqMove move;
    XqSearchStats stats;
    XqPosition position = check.root;
    CHECK(xq_engine_find_best_move_with_sampling(&adapter, &position, &limits, &move,
          &stats, path_progress, &check, check.sampler));
    CHECK(stats.stopped);
    check_destroy(&check);
}
#endif

typedef struct CoreCheck
{
    uint64_t count;
    XqSearchSample last;
    XqMove moves[XQ_SEARCH_SAMPLE_MAX_MOVES];
} CoreCheck;

static void core_progress(const XqSearchProgress *event, void *user)
{
    if (event->event != XQ_SEARCH_PATH_SAMPLE)
        return;
    CoreCheck *check = user;
    ++check->count;
    check->last = *event->sample;
    memcpy(check->moves, event->sample->path, event->sample->path_length * sizeof(XqMove));
    check->last.path = check->moves;
}

static void core_boundaries(void)
{
    XqSearchSampler *sampler = xq_search_sampler_create(2);
    CHECK(sampler);
    CoreCheck check = {0};
    XqSamplingThread *owner = xq_sampling_thread(sampler, 0);
    XqSamplingThread *helper = xq_sampling_thread(sampler, 1);
    xq_sampling_begin(owner, 3, core_progress, &check);
    XqMove first = {1, 2, 3, 0}, second = {2, 3, 4, 0};
    for (unsigned i = 0; i < 1050; ++i)
        xq_sampling_push(owner, i == 0 ? first : second, i >= 3);
    xq_search_sampler_request(sampler);
    xq_sampling_checkpoint(owner, true);
    CHECK(check.count == 1 && check.last.truncated);
    CHECK(check.last.path_length == 1024);
    CHECK(check.last.normal_ply == 3 && check.last.quiescence_ply == 1047);
    CHECK(same_move(check.moves[0], first));
    xq_sampling_checkpoint(owner, true);
    CHECK(check.count == 1); /* Same physical thread and epoch must never be duplicated. */
    xq_sampling_inherit(helper, owner);
    xq_sampling_state(helper, XQ_SEARCH_THREAD_SEARCH);
    for (unsigned i = 0; i < 50; ++i)
        xq_sampling_pop(helper, true);
    xq_search_sampler_request(sampler);
    xq_sampling_checkpoint(helper, true);
    CHECK(check.count == 2 && !check.last.truncated);
    CHECK(check.last.thread_id == 1 && check.last.path_length == 1000);
    CHECK(same_move(check.moves[0], first));
    for (unsigned i = 0; i < 1047; ++i)
        xq_sampling_pop(owner, true);
    xq_sampling_pop(owner, false);
    xq_sampling_pop(owner, false);
    xq_sampling_push(owner, first, true);
    xq_search_sampler_request(sampler);
    xq_sampling_checkpoint(owner, true);
    CHECK(check.last.path_length == 2 && check.last.normal_ply == 1);
    CHECK(same_move(check.moves[0], first) && same_move(check.moves[1], first));
    uint64_t count = check.count;
    xq_sampling_state(owner, XQ_SEARCH_THREAD_WAIT_HELPERS);
    xq_search_sampler_request(sampler);
    xq_sampling_checkpoint(owner, false);
    CHECK(check.count == count);
    xq_sampling_state(owner, XQ_SEARCH_THREAD_IDLE);
    xq_sampling_checkpoint(owner, true);
    CHECK(check.count == count);
    xq_sampling_state(owner, XQ_SEARCH_THREAD_SEARCH);
    xq_sampling_checkpoint(owner, false);
    CHECK(check.count == count + 1);
    xq_search_sampler_destroy(sampler);
}

static void disabled_and_terminal(void)
{
    PathCheck check;
    check_init(&check, quiet_fen, 1);
    XqSearchLimits limits = xq_search_limits_default();
    limits.max_depth = 2;
    limits.time_limit_ms = 0;
    XqSearchStats stats;
    XqMove move;
    CHECK(xq_engine_find_best_move_with_progress(NULL, &check.root, &limits, &move,
          &stats, path_progress, &check));
    CHECK(check.samples == 0);
#ifndef _WIN32
    XqParallelOptions options = xq_parallel_options_default();
    options.depth = 2;
    options.thread_count = 2; /* A capacity-one sampler disables the entire invocation. */
    XqParallelResult result;
    CHECK(xq_engine_parallel_search_with_observer(NULL, &check.root, &options, &result,
          path_progress, &check, check.sampler) == XQ_PARALLEL_OK);
    CHECK(check.samples == 0);
    CHECK(xq_engine_parallel_search_with_observer(NULL, &check.root, &options, &result,
          path_progress, &check, NULL) == XQ_PARALLEL_OK);
    CHECK(check.samples == 0);
#endif
    CHECK(xq_position_remove_piece(&check.root, xq_position_king_square(&check.root, XQ_RED)));
    CHECK(!xq_engine_find_best_move_with_sampling(NULL, &check.root, &limits, &move,
          &stats, path_progress, &check, check.sampler));
    CHECK(check.samples == 0);
#ifndef _WIN32
    options.thread_count = 1;
    CHECK(xq_engine_parallel_search_with_observer(NULL, &check.root, &options, &result,
          path_progress, &check, check.sampler) == XQ_PARALLEL_OK);
    CHECK(!result.move_available && check.samples == 0);
#endif
    check_destroy(&check);
}

static void winning_and_no_moves(void)
{
    PathCheck check;
    check_init(&check, "4k4/9/9/9/4R4/9/9/9/9/3K5 r - -", 1);
    XqSearchLimits limits = xq_search_limits_default();
    limits.max_depth = 5;
    limits.time_limit_ms = 0;
    XqSearchStats stats;
    XqMove move;
    XqPosition position = check.root;
    CHECK(xq_engine_find_best_move_with_sampling(NULL, &position, &limits, &move,
          &stats, path_progress, &check, check.sampler));
    CHECK(stats.winning_move_found && stats.max_started_depth == 1);
    check_destroy(&check);
#ifndef _WIN32
    check_init(&check, "4k4/9/9/9/4R4/9/9/9/9/3K5 r - -", 1);
    XqParallelOptions options = xq_parallel_options_default();
    options.depth = 5;
    options.thread_count = 1;
    XqParallelResult result;
    CHECK(xq_engine_parallel_search_with_observer(NULL, &check.root, &options, &result,
          path_progress, &check, check.sampler) == XQ_PARALLEL_OK);
    CHECK(result.stats.winning_move_found && result.stats.max_started_depth == 1);
    check_destroy(&check);
#endif
    check_init(&check, "4k4/9/9/9/9/9/9/9/3rr4/3K5 r - -", 1);
    position = check.root;
    CHECK(!xq_engine_find_best_move_with_sampling(NULL, &position, &limits, &move,
          &stats, path_progress, &check, check.sampler));
    CHECK(check.samples == 0);
#ifndef _WIN32
    CHECK(xq_engine_parallel_search_with_observer(NULL, &check.root, &options, &result,
          path_progress, &check, check.sampler) == XQ_PARALLEL_OK);
    CHECK(!result.move_available && check.samples == 0);
#endif
    check_destroy(&check);
}

int main(void)
{
    core_boundaries();
    builtin_paths();
#ifndef _WIN32
    parallel_paths();
    cancellation();
#endif
    disabled_and_terminal();
    winning_and_no_moves();
    puts("search sampling tests passed");
    return 0;
}
