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

typedef struct XqParallelOptions
{
    unsigned depth;          /* >= 1; default 3. No deadline or quiescence. */
    unsigned thread_count;   /* >= 1, includes calling thread; default 2. */
    unsigned split_percent;  /* 0..100; zero disables splitting; default 50. */
    XqParallelTranspositionTable *transposition_table; /* Borrowed; NULL disables caching (default). */
} XqParallelOptions;

typedef struct XqParallelResult
{
    bool move_available;
    XqMove best_move;
    int score; /* Root-side value on OK; with caching, deeper stored results may be reused. */
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
 * the root probes only for move ordering. Without a table, scores retain fixed-depth semantics.
 * TT statistics accumulate on the table; no trace or other search statistics are collected.
 * Results are initialized even on failure.
 * Equal-score choices may depend on scheduling. POSIX pthread implementation requiring
 * always-lock-free atomic int; Windows returns UNSUPPORTED. */
XqParallelStatus xq_engine_parallel_search(const XqPosition *pos,
    const XqParallelOptions *options, XqEvaluateFn evaluate, void *evaluate_user,
    XqParallelResult *result);

#endif
