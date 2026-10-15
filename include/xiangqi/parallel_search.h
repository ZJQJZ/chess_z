#ifndef XIANGQI_PARALLEL_SEARCH_H
#define XIANGQI_PARALLEL_SEARCH_H

#include "xiangqi/engine.h"

typedef enum XqParallelStatus
{
    XQ_PARALLEL_OK, /* Includes terminal positions: inspect move_available. */
    XQ_PARALLEL_INVALID_ARGUMENT,
    XQ_PARALLEL_RESOURCE_ERROR,
    XQ_PARALLEL_UNSUPPORTED
} XqParallelStatus;

typedef struct XqParallelTranspositionTable XqParallelTranspositionTable;

/* Independent, fixed-capacity, in-memory cache for parallel search. One top-level search at a
 * time may borrow a table; its workers share it internally. Do not query statistics, clear,
 * reset, destroy, or reuse the table externally until that search returns. Different tables
 * may be used concurrently. This cache is not compatible with the built-in engine's table/files.
 * Creation returns NULL on allocation failure or Windows. Clear/destroy/reset accept NULL;
 * get_stats ignores a NULL output and zeroes it for a NULL table (always zero on Windows). */
XqParallelTranspositionTable *xq_parallel_transposition_table_create(void);
void xq_parallel_transposition_table_clear(XqParallelTranspositionTable *table);
void xq_parallel_transposition_table_destroy(XqParallelTranspositionTable *table);
void xq_parallel_transposition_table_reset_stats(XqParallelTranspositionTable *table);
void xq_parallel_transposition_table_get_stats(const XqParallelTranspositionTable *table,
                                             XqTranspositionStats *stats);

typedef enum XqParallelSplitStrategy
{
    XQ_PARALLEL_SPLIT_CONTIGUOUS = 0, /* Helper takes a tail batch sized by split_percent. */
    XQ_PARALLEL_SPLIT_INTERLEAVED    /* After ordering: owner even indices, helper odd (zero-based). */
} XqParallelSplitStrategy;

typedef struct XqParallelOptions
{
    unsigned depth;          /* Maximum iterative-deepening depth >= 1; default 3. No deadline or quiescence. */
    unsigned thread_count;   /* >= 1, includes calling thread; default 2. */
    unsigned split_percent;  /* 0..100, default 50; contiguous only (0 disables); ignored by interleaved. */
    XqParallelTranspositionTable *transposition_table; /* Borrowed; NULL disables caching (default). */
    XqParallelSplitStrategy split_strategy; /* Default CONTIGUOUS; applied after all move ordering. */
} XqParallelOptions;

typedef struct XqParallelResult
{
    bool move_available;
    XqMove best_move;
    int score; /* Last iteration's root-side value on OK; deeper cached results may be reused. */
} XqParallelResult;

XqParallelOptions xq_parallel_options_default(void);
const char *xq_parallel_status_text(XqParallelStatus status);

/* pos is never modified. NULL options selects defaults; NULL evaluate uses the
 * built-in evaluator. The evaluator and its user data must support concurrent
 * calls and must not modify positions. Cached evaluation must be deterministic for a position
 * and perspective. Changing the effective evaluator or user pointer clears the borrowed table;
 * changing root color also clears it for custom evaluators. Clear it explicitly after mutating
 * evaluation parameters behind the same user pointer. No adapter cache/history/search is used.
 * Internal TT scores use the side-to-move perspective and may come from greater search depths;
 * the root probes only for move ordering in the first iteration. Searches depths 1..options.depth,
 * reusing the helper pool and previous root scores for ordering, even without a table. Stops early
 * for a terminal root or proven winning move. Without a table, scores are exact for the last
 * iteration's depth. Table generations advance per iteration and entries survive between them.
 * The first root iteration and internal nodes use the built-in stable capture ordering, then
 * promote cache and previous-iteration PV moves (PV takes precedence). Later root iterations
 * retain the previous root ordering. PV suffixes follow only matching moves; up to 64 moves are
 * retained even without a table, without limiting search depth. Exact cache cutoffs may rebuild
 * a PV prefix; terminal positions, cache bounds/misses/contention or invalid moves can shorten it.
 * PV hints affect ordering only and are not exposed in the result. No custom move scorer is used.
 * TT statistics accumulate on the table, excluding PV reconstruction lookups; no trace or other
 * search statistics are collected. Each node may split once with one idle helper, after all move
 * ordering. CONTIGUOUS assigns a tail batch according to split_percent; INTERLEAVED assigns odd
 * zero-based indices to the helper and even indices to the owner, ignoring split_percent even
 * when zero. With fewer than two moves, no available helper or failed task-completion allocation,
 * the owner searches all moves serially. thread_count == 1 disables splitting for either strategy.
 * Results are initialized even on failure.
 * Equal-score choices may depend on scheduling. POSIX pthread implementation requiring
 * always-lock-free atomic int; Windows returns UNSUPPORTED. */
XqParallelStatus xq_engine_parallel_search(const XqPosition *pos,
    const XqParallelOptions *options, XqEvaluateFn evaluate, void *evaluate_user,
    XqParallelResult *result);

/* Same search with optional root progress (see XqSearchProgressFn concurrency contract).
 * Terminal roots and argument/resource failures emit no iteration events. */
XqParallelStatus xq_engine_parallel_search_with_progress(const XqPosition *pos,
    const XqParallelOptions *options, XqEvaluateFn evaluate, void *evaluate_user,
    XqParallelResult *result, XqSearchProgressFn progress, void *progress_user);

#endif
