#define _POSIX_C_SOURCE 200809L
#include "xiangqi/parallel_search.h"
#include "xiangqi/movegen.h"

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>

/* Private position-inspection and thread-creation hooks; no production trace dependency. */
static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static int create_count, fail_after = -1;
static bool inspect_pseudo_moves;
static unsigned illegal_leaves, illegal_internal_nodes, both_kings_attacked;

int xq_parallel_test_create(pthread_t *thread, void *(*entry)(void *), void *user)
{
    if (fail_after >= 0 && create_count++ == fail_after)
        return EAGAIN;
    return pthread_create(thread, NULL, entry, user);
}

void xq_parallel_test_checkpoint(const XqPosition *pos, unsigned depth)
{
    pthread_mutex_lock(&gate);
    if (inspect_pseudo_moves &&
        xq_position_in_check(pos, xq_color_opponent(pos->side_to_move)))
    {
        if (depth == 0)
            ++illegal_leaves;
        else
            ++illegal_internal_nodes;
        if (xq_position_in_check(pos, pos->side_to_move))
            ++both_kings_attacked;
    }
    pthread_mutex_unlock(&gate);
}

static int zero_evaluate(const XqPosition *pos, XqColor color, void *user)
{
    (void)pos; (void)color; (void)user;
    return 0;
}

static int safe_evaluate(const XqPosition *pos, XqColor color, void *user)
{
    XqMoveList legal;
    /* Illegal pseudo-legal branches and terminal leaves must never reach evaluation. */
    assert(!xq_position_in_check(pos, xq_color_opponent(pos->side_to_move)));
    xq_generate_legal(pos, &legal);
    assert(legal.count > 0);
    return xq_engine_default_static_evaluate(pos, color, user);
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
    int expected, i;
    assert(xq_position_from_fen(&pos, fen));
    before = pos;
    expected = reference(&pos, depth, pos.side_to_move, evaluate, user);
    options.depth = depth;
    options.thread_count = threads;
    options.split_percent = percent;
    assert(xq_engine_parallel_search(&pos, &options, evaluate, user, &result) == XQ_PARALLEL_OK);
    assert(memcmp(&pos, &before, sizeof(pos)) == 0);
    if (result.score != expected)
        fprintf(stderr, "mismatch %s depth %u threads %u ratio %u: %d != %d\n",
                fen, depth, threads, percent, result.score, expected);
    assert(result.score == expected);
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
        "4k4/9/9/9/9/9/9/4r4/3rPr3/4K4 b - -", /* Mate at the horizon. */
        "3k5/9/9/9/9/9/9/9/2r2r3/4K4 b - -", /* Stalemate at the horizon. */
        "4k4/9/9/9/R8/9/9/1r7/9/3K5 r - -", /* Ignoring check can attack both kings. */
        "4k4/9/9/9/9/9/9/9/4R4/4K4 r - -", /* Moving the blocker exposes facing kings. */
    };
    unsigned threads[] = {1, 2, 4}, ratios[] = {0, 1, 50, 100};
    size_t f, t, r;
    unsigned depth;
    inspect_pseudo_moves = true;
    for (f = 0; f < sizeof(fens) / sizeof(fens[0]); ++f)
        for (depth = 1; depth <= 3; ++depth)
            for (t = 0; t < sizeof(threads) / sizeof(threads[0]); ++t)
                for (r = 0; r < sizeof(ratios) / sizeof(ratios[0]); ++r)
                    compare(fens[f], depth, threads[t], ratios[r],
                            f == 7 ? zero_evaluate : safe_evaluate, NULL);
    inspect_pseudo_moves = false;
    assert(illegal_leaves > 0 && illegal_internal_nodes > 0 && both_kings_attacked > 0);
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

static int yielding_evaluate(const XqPosition *pos, XqColor root, void *user)
{
    /* Let sibling work publish bounds/completion while this branch is outside the mutex. */
    sched_yield();
    return hash_evaluate(pos, root, user);
}

static void test_atomic_completion(void)
{
    const char *fen = "4k4/9/9/9/R8/9/9/1r7/9/3K5 r - -";
    const unsigned ratios[] = {1, 50, 100};
    unsigned repeat;
    size_t ratio;
    /* Preserve the exact score and a legal optimal move with competing MAX/MIN updates,
     * skewed batches and more helpers than shallow splits. */
    for (repeat = 0; repeat < 3; ++repeat)
        for (ratio = 0; ratio < sizeof(ratios) / sizeof(ratios[0]); ++ratio)
            compare(fen, 3, 8, ratios[ratio], yielding_evaluate, NULL);
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
    assert(!result.move_available && result.score == 0);
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
        assert(!result.move_available && result.score == 0);
        fail_after = -1;
        assert(xq_engine_parallel_search(&pos, &options, NULL, NULL, &result) == XQ_PARALLEL_OK);
    }
}

static void test_startpos_consistency(void)
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
    }
}

int main(void)
{
    test_positions();
    test_errors_and_reuse();
    test_varied_trees();
    test_atomic_completion();
    test_startpos_consistency();
    puts("parallel search tests passed");
    return 0;
}
