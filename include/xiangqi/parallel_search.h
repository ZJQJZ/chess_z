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
    XQ_PARALLEL_SPLIT_INTERLEAVED    /* After ordering: round-robin among owner and available helpers. */
} XqParallelSplitStrategy;

typedef struct XqParallelOptions
{
    unsigned depth;          /* Maximum normal-search depth >= 1; default 3. Excludes quiescence. */
    unsigned thread_count;   /* >= 1, includes calling thread; default 2. */
    unsigned split_percent;  /* 0..100, default 50; contiguous only (0 disables); ignored by interleaved. */
    XqParallelTranspositionTable *transposition_table; /* Borrowed; NULL disables caching (default). */
    XqParallelSplitStrategy split_strategy; /* Default CONTIGUOUS; applied after all move ordering. */
    uint64_t time_limit_ms;  /* Monotonic wall time; 0 disables timing (default). */
} XqParallelOptions;

typedef struct XqParallelResult
{
    bool move_available;
    XqMove best_move;
    int score; /* Root-side value, valid only when score_available; see score_kind. */
    bool score_available;
    XqSearchScoreKind score_kind;
    XqSearchStats stats;
} XqParallelResult;

XqParallelOptions xq_parallel_options_default(void);
const char *xq_parallel_status_text(XqParallelStatus status);

/* pos is never modified. NULL options selects defaults. Only engine->static_evaluate, user and
 * history are used; score_move and the built-in transposition_table are ignored. The parallel
 * table remains in options. NULL engine or static_evaluate selects the built-in evaluator with
 * NULL evaluator user data; NULL engine also disables history filtering. The evaluator and its
 * user data must support concurrent calls and must not modify positions. Opposite perspectives
 * must yield opposite evaluation scores. Cached evaluation must be deterministic for a position
 * and perspective. Changing the effective evaluator or user pointer
 * clears the borrowed table; changing root color also clears it for custom evaluators. Clear it
 * explicitly after mutating evaluation parameters behind the same user pointer. Keep the adapter,
 * history, evaluator data and table alive until return; callers must not mutate the adapter,
 * history, evaluator parameters or table while the search is running.
 * Before iterative deepening, real-game history filters legal root moves that close the third of
 * three consecutive identical even-length move sequences with four matching boundary position
 * hashes, using the same rule as built-in search. NULL, empty or mismatched history disables this
 * filtering. Search-tree repetitions are not checked and search moves are never appended to real
 * history. Progress totals include only surviving root moves. When all candidates are filtered,
 * return the original legal fallback without a score, at selected_move_depth == 0 and with no
 * iteration events. A root result with any filtered candidates is never stored in the position-only
 * table; history-independent descendant results remain cacheable.
 * Internal TT scores use the side-to-move perspective and may come from greater search depths;
 * the root probes only for move ordering in the first iteration. Searches depths 1..options.depth,
 * reusing the helper pool and previous root scores for ordering, even without a table. Stops early
 * for a terminal root, proven winning move or time stop. Exact results describe the selected
 * iteration; a partial first iteration supplies a lower bound. An unsearched legal fallback has
 * score_available == false and selected_move_depth == 0. Table generations advance per iteration
 * and entries survive between them.
 * Depth-zero leaves run quiescence in the executing thread: stand pat plus captures when not in
 * check, all legal evasions without stand pat when in check. All move lists are pseudo-legal; the
 * move loop skips non-captures outside check and rejects moves that leave the original mover in
 * check when evading. An empty list or an in-check position with no legal evasion is a loss;
 * outside check there is no extra legal-move scan before stand pat. King and loss checks precede
 * the 500-ply extension cap; at the cap an in-check position must have a legal evasion before the
 * original position is statically evaluated. The evaluator receives the side to move, and its
 * result is clamped to +/-28999 for negamax.
 * Quiescence flips child scores and windows, then ordinary search converts its result back to
 * the root perspective. Wins/losses remain +/-30000. Quiescence shares time and ancestor cancellation checks,
 * preserves cutoff bounds and never splits tasks, accesses the table or extends PV hints.
 * Node counts include normal and quiescence entries (the shared leaf counts once for each);
 * reported depths exclude extensions and progress still counts only root candidates.
 * The first root iteration and internal nodes use the built-in stable capture ordering, then
 * promote cache and previous-iteration PV moves (PV takes precedence). Later root iterations
 * retain the previous root ordering. PV suffixes follow only matching moves; up to 64 moves are
 * retained even without a table, without limiting search depth. Exact cache cutoffs may rebuild
 * a PV prefix; terminal positions, cache bounds/misses/contention or invalid moves can shorten it.
 * PV hints affect ordering only and are not exposed in the result. No custom move scorer is used.
 * TT statistics accumulate on the table, excluding PV reconstruction lookups; stats contains
 * per-call counters without resetting the table. Each normal-search node may split once after all
 * move ordering, provided its remaining normal-search depth is at least PARALLEL_MIN_SPLIT_DEPTH
 * (5, defined in parallel_search.c). This includes the next move and excludes quiescence extensions;
 * it applies to root and descendant nodes under both strategies.
 * Shallower nodes search serially. Pruning, terminal positions and cancellation may still end a
 * task early. The default maximum depth of 3 therefore dispatches no helper tasks.
 * CONTIGUOUS assigns a tail batch to at most one helper according to split_percent.
 * INTERLEAVED uses at most PARALLEL_INTERLEAVED_SPLIT_THREADS participants (3, defined in
 * parallel_search.c), including the owner, and ignores split_percent even when zero. It prepares
 * idle helpers before fixing the actual participant count N, capped by the number of moves; the
 * owner takes indices 0,N,2N,... and helper k takes k,k+N,k+2N,... for k in 1..N-1. Insufficient
 * idle workers reduce N without waiting. A limit of 1 disables interleaved splitting; 2 gives
 * even/odd batches. Completion allocation failure stops further reservations but keeps already
 * prepared helpers. With no prepared helper or fewer than two moves, the owner searches all moves
 * serially. thread_count == 1 disables splitting for either strategy. The global thread pool size
 * and its default of 2 are unchanged; three-way splitting requires thread_count >= 3 and two idle
 * helpers. Every dispatched helper completes before the node returns, including on cancellation.
 * The budget covers preparation, pool creation and all iterations. At the first detected deadline,
 * an unfinished iteration with strictly more than 30% of its root candidates completed may finish;
 * otherwise running nodes are cancelled. No new iteration starts after the deadline. Clock failure
 * stops even an extended iteration. A cancelled iteration preserves the last complete result;
 * without one, select the best completed first-iteration candidate or a legal fallback. Timeout is
 * OK with stats.stopped set. Workers and callbacks finish before return, so the budget is not a hard
 * return deadline. Completed proofs remain valid if a final time check requests stopping.
 * Results are initialized even on failure.
 * Equal-score choices may depend on scheduling. POSIX pthread implementation requiring
 * always-lock-free atomic int; Windows returns UNSUPPORTED. */
XqParallelStatus xq_engine_parallel_search(const XqEngineAdapter *engine,
    const XqPosition *pos, const XqParallelOptions *options, XqParallelResult *result);

/* Same search with optional root progress (see XqSearchProgressFn concurrency contract).
 * Terminal roots, fully filtered roots and argument/resource failures emit no iteration events. */
XqParallelStatus xq_engine_parallel_search_with_progress(const XqEngineAdapter *engine,
    const XqPosition *pos, const XqParallelOptions *options, XqParallelResult *result,
    XqSearchProgressFn progress, void *progress_user);

#endif
