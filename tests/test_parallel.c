#define _POSIX_C_SOURCE 200809L
#include "xiangqi/parallel_search.h"
#include "xiangqi/movegen.h"

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* 测试专用钩子在 search mutex 外暂停线程。事件回调只通知，不等待。 */
static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static int synchronization_mode;
static bool paused, released, observed;
static uint64_t target_node, paused_node;
static unsigned target_depth;
static int create_count, fail_after = -1;

int xq_parallel_test_create(pthread_t *thread, void *(*entry)(void *), void *user)
{
    if (fail_after >= 0 && create_count++ == fail_after)
        return EAGAIN;
    return pthread_create(thread, NULL, entry, user);
}

static void await_flag(const bool *flag)
{
    struct timespec deadline;
    assert(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
    deadline.tv_sec += 15; /* Failure guard, not a scheduling mechanism. */
    while (!*flag)
        assert(pthread_cond_timedwait(&changed, &gate, &deadline) == 0);
}

void xq_parallel_test_checkpoint(const XqParallelEvent *event, const XqPosition *pos)
{
    (void)pos;
    pthread_mutex_lock(&gate);
    if (synchronization_mode != 0 && target_node != 0)
    {
        if (event->worker_id == 1 && event->depth == 0 && !paused)
        {
            paused_node = event->node_id;
            paused = true;
            pthread_cond_broadcast(&changed);
            await_flag(&released);
        }
        else if (event->worker_id == 0 && event->parent_id == target_node)
            await_flag(&paused);
    }
    pthread_mutex_unlock(&gate);
}

static void observe(const XqParallelEvent *event, void *user)
{
    uint64_t *last = user;
    assert(event->sequence == ++*last);
    assert(event->lower <= event->upper);
    pthread_mutex_lock(&gate);
    if (synchronization_mode != 0)
    {
        if (strcmp(event->kind, "dispatch") == 0 && target_node == 0 &&
            event->maximizing == (synchronization_mode != 2))
        {
            target_node = event->node_id;
            target_depth = event->depth;
        }
        if (event->node_id == target_node && strcmp(event->kind, "submit") == 0 &&
            (synchronization_mode == 2 ? event->upper == 0 :
             event->lower == (synchronization_mode == 3 ? 30000 : 0)))
        {
            assert(paused);
            released = true;
            pthread_cond_broadcast(&changed);
        }
        if (synchronization_mode != 3 && event->node_id == paused_node &&
            strcmp(event->kind, "refresh") == 0 &&
            (synchronization_mode == 1 ?
             event->alpha == 0 && event->alpha_source == target_node :
             event->beta == 0 && event->beta_source == target_node))
        {
            assert(released && target_depth >= 2); /* Across multiple ancestor levels. */
            observed = true;
        }
        if (synchronization_mode == 3 && event->node_id == paused_node &&
            strcmp(event->kind, "cancel") == 0)
        {
            assert(released);
            observed = true;
        }
    }
    pthread_mutex_unlock(&gate);
}

static int zero_evaluate(const XqPosition *pos, XqColor color, void *user)
{
    (void)pos; (void)color; (void)user;
    return 0;
}

/* 独立参考：没有 alpha/beta、线程、缓存、排序或任何剪枝。 */
static int reference(const XqPosition *pos, unsigned depth, XqColor root,
                     XqEvaluateFn evaluate, void *user)
{
    XqMoveList moves;
    int best = pos->side_to_move == root ? -30000 : 30000;
    int i;
    if (xq_position_king_square(pos, root) == XQ_NO_SQUARE) return -30000;
    if (xq_position_king_square(pos, xq_color_opponent(root)) == XQ_NO_SQUARE) return 30000;
    xq_generate_legal(pos, &moves);
    if (moves.count == 0) return pos->side_to_move == root ? -30000 : 30000;
    if (depth == 0)
    {
        int score = evaluate(pos, root, user);
        return score > 28999 ? 28999 : score < -28999 ? -28999 : score;
    }
    for (i = 0; i < moves.count; ++i)
    {
        XqPosition child = *pos;
        int score;
        assert(xq_position_make_move(&child, moves.moves[i]));
        score = reference(&child, depth - 1, root, evaluate, user);
        if ((pos->side_to_move == root && score > best) ||
            (pos->side_to_move != root && score < best)) best = score;
    }
    return best;
}

static XqParallelResult compare(const char *fen, unsigned depth, unsigned threads,
                                unsigned percent, XqEvaluateFn evaluate, void *user)
{
    XqPosition pos, before;
    XqMoveList legal;
    XqParallelOptions options = xq_parallel_options_default();
    XqParallelResult result;
    uint64_t sequence = 0;
    int expected, i;
    assert(xq_position_from_fen(&pos, fen));
    before = pos;
    expected = reference(&pos, depth, pos.side_to_move, evaluate, user);
    options.depth = depth;
    options.thread_count = threads;
    options.split_percent = percent;
    options.trace = observe;
    options.trace_user = &sequence;
    assert(xq_engine_parallel_search(&pos, &options, evaluate, user, &result) == XQ_PARALLEL_OK);
    assert(memcmp(&pos, &before, sizeof(pos)) == 0);
    if (result.score != expected)
        fprintf(stderr, "mismatch %s depth %u threads %u ratio %u: %d != %d\n",
                fen, depth, threads, percent, result.score, expected);
    assert(result.score == expected);
    if (threads == 1 || percent == 0) assert(result.splits == 0);
    xq_generate_legal(&pos, &legal);
    if (result.move_available)
    {
        for (i = 0; i < legal.count; ++i)
            if (legal.moves[i].from == result.best_move.from && legal.moves[i].to == result.best_move.to)
                break;
        assert(i < legal.count);
        assert(xq_position_make_move(&pos, result.best_move));
        assert(reference(&pos, depth - 1, before.side_to_move, evaluate, user) == expected);
    }
    else
        assert(legal.count == 0 || xq_position_king_square(&pos, XQ_RED) == XQ_NO_SQUARE ||
               xq_position_king_square(&pos, XQ_BLACK) == XQ_NO_SQUARE);
    return result;
}

static void test_positions(void)
{
    static const char *fens[] = {
        "3k5/9/9/9/9/9/9/9/9/R3K4 r - -",
        "3k5/9/9/9/9/9/9/9/9/R3K4 b - -",
        "3k5/9/9/9/9/9/9/4r4/9/4K4 r - -", /* One legal move. */
        "3k5/9/9/9/9/9/9/9/3rrr3/4K4 r - -", /* Mate. */
        "3k5/9/9/9/9/9/9/9/3r1r3/4K4 r - -", /* Stalemate. */
        "4k4/9/9/9/9/9/9/9/4R4/3K5 r - -", /* King capture. */
        "3k5/9/9/9/4p4/9/9/r8/c8/R3K4 r - -",
        "4k4/9/9/9/9/9/9/9/9/3K5 r - -", /* Equal scores. */
    };
    unsigned threads[] = {1, 2, 4}, ratios[] = {0, 1, 50, 100};
    size_t f, t, r;
    unsigned depth;
    for (f = 0; f < sizeof(fens) / sizeof(fens[0]); ++f)
        for (depth = 1; depth <= 3; ++depth)
            for (t = 0; t < sizeof(threads) / sizeof(threads[0]); ++t)
                for (r = 0; r < sizeof(ratios) / sizeof(ratios[0]); ++r)
                    compare(fens[f], depth, threads[t], ratios[r],
                            f == 7 ? zero_evaluate : xq_engine_default_static_evaluate, NULL);
}

static void test_live_updates(void)
{
    int mode, repeat;
    for (mode = 1; mode <= 3; ++mode)
        for (repeat = 0; repeat < 5; ++repeat)
        {
            const char *fen = mode == 1 ?
                "3k5/9/9/9/9/9/9/9/9/4K4 r - -" : mode == 2 ?
                "3k5/9/9/9/9/9/9/4r4/9/4K4 r - -" :
                "3k5/9/9/9/9/9/9/9/9/R3K4 r - -";
            XqParallelResult result;
            synchronization_mode = mode;
            paused = released = observed = false;
            target_node = paused_node = 0;
            result = compare(fen, 3, 2, 50, zero_evaluate, NULL);
            assert(paused && released && observed);
            assert(result.bound_updates > 0 && result.splits > 0);
            if (mode == 3) assert(result.cancelled_tasks > 0);
            synchronization_mode = 0;
        }
}

static int extreme_evaluate(const XqPosition *pos, XqColor root, void *user)
{
    (void)pos; (void)root;
    return *(const int *)user;
}

/* Irregular leaf values exercise fail-low/fail-high intervals and changing best
 * moves much more aggressively than equal/material-only leaf evaluations. */
static int hash_evaluate(const XqPosition *pos, XqColor root, void *user)
{
    uint64_t hash = xq_position_hash(pos);
    (void)root; (void)user;
    hash ^= hash >> 29;
    hash *= UINT64_C(0xbf58476d1ce4e5b9);
    return (int)(hash % 1001) - 500;
}

static void test_varied_trees(void)
{
    XqPosition pos;
    unsigned sample, threads;
    xq_position_startpos(&pos);
    for (sample = 0; sample < 18; ++sample)
    {
        XqMoveList moves;
        char fen[128];
        xq_generate_legal(&pos, &moves);
        if (moves.count == 0)
        {
            xq_position_startpos(&pos);
            xq_generate_legal(&pos, &moves);
        }
        assert(xq_position_to_fen(&pos, fen, sizeof(fen)));
        for (threads = 1; threads <= 4; threads *= 2)
            compare(fen, sample % 6 == 0 ? 3 : 2, threads, 50, hash_evaluate, NULL);
        assert(xq_position_make_move(&pos, moves.moves[xq_position_hash(&pos) % (unsigned)moves.count]));
    }
}

static void test_errors_and_reuse(void)
{
    XqPosition pos;
    XqParallelOptions options = xq_parallel_options_default();
    XqParallelResult result;
    int extreme = INT_MAX, i;
    const char *fen = "3k5/9/9/9/9/9/9/9/9/R3K4 r - -";
    assert(xq_position_from_fen(&pos, fen));
    compare(fen, 2, 4, 50, extreme_evaluate, &extreme);
    extreme = INT_MIN;
    compare(fen, 2, 4, 50, extreme_evaluate, &extreme);
    assert(xq_engine_parallel_search(NULL, NULL, NULL, NULL, &result) == XQ_PARALLEL_INVALID_ARGUMENT);
    assert(!result.move_available && result.nodes == 0);
    assert(xq_engine_parallel_search(&pos, NULL, NULL, NULL, NULL) == XQ_PARALLEL_INVALID_ARGUMENT);
    options.depth = 0;
    assert(xq_engine_parallel_search(&pos, &options, NULL, NULL, &result) == XQ_PARALLEL_INVALID_ARGUMENT);
    options.depth = 1; options.thread_count = 0;
    assert(xq_engine_parallel_search(&pos, &options, NULL, NULL, &result) == XQ_PARALLEL_INVALID_ARGUMENT);
    options.thread_count = 2; options.split_percent = 101;
    assert(xq_engine_parallel_search(&pos, &options, NULL, NULL, &result) == XQ_PARALLEL_INVALID_ARGUMENT);
    options = xq_parallel_options_default();
    options.thread_count = 4;
    for (i = 0; i < 3; ++i)
    {
        create_count = 0; fail_after = i;
        assert(xq_engine_parallel_search(&pos, &options, NULL, NULL, &result) == XQ_PARALLEL_RESOURCE_ERROR);
        assert(!result.move_available && result.nodes == 0);
        fail_after = -1;
        assert(xq_engine_parallel_search(&pos, &options, NULL, NULL, &result) == XQ_PARALLEL_OK);
    }
}

static void benchmark(void)
{
    XqPosition pos;
    XqParallelOptions options = xq_parallel_options_default();
    XqParallelResult result;
    unsigned threads;
    int score = 0;
    xq_position_startpos(&pos);
    for (threads = 1; threads <= 4; threads *= 2)
    {
        options.depth = 3;
        options.thread_count = threads;
        assert(xq_engine_parallel_search(&pos, &options, NULL, NULL, &result) == XQ_PARALLEL_OK);
        if (threads == 1) score = result.score;
        assert(result.score == score);
        printf("benchmark startpos threads=%u depth=3 score=%d nodes=%llu splits=%llu elapsed_ms=%llu\n",
               threads, score, (unsigned long long)result.nodes,
               (unsigned long long)result.splits, (unsigned long long)result.elapsed_ms);
    }
}

int main(void)
{
    test_positions();
    test_live_updates();
    test_errors_and_reuse();
    test_varied_trees();
    benchmark();
    puts("parallel search tests passed");
    return 0;
}
