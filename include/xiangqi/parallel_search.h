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
 * NULL evaluator user data; NULL engine also disables real-game root history filtering. The
 * evaluator and its user data must support concurrent calls and must not modify positions. Opposite
 * perspectives must yield opposite evaluation scores. Cached evaluation must be deterministic for a position
 * and perspective. Changing the effective evaluator or user pointer
 * clears the borrowed table; changing root color also clears it for custom evaluators. Clear it
 * explicitly after mutating evaluation parameters behind the same user pointer. Keep the adapter,
 * history, evaluator data and table alive until return; callers must not mutate the adapter,
 * history, evaluator parameters or table while the search is running.
 * Before iterative deepening, real-game history filters legal root moves that close the third of
 * three consecutive identical even-length move sequences with four matching boundary position
 * hashes, using the same rule as built-in search. NULL, empty or mismatched history disables this
 * filtering. Normal-search paths are not checked for repetitions and search moves are never
 * appended to real history. Progress totals include only surviving root moves. When all candidates
 * are filtered, return the original legal fallback without a score, at selected_move_depth == 0 and
 * with no iteration events. A root result with any filtered candidates is never stored in the position-only
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
 * check when evading. Each depth-zero entry starts a private stack history at its entry position,
 * excluding real-game history and the normal-search prefix. Quiescence filters moves that lose
 * under the same three-cycle rule above, without sharing history between entries or branches.
 * An originally empty list or an in-check position with no surviving legal evasion is a loss;
 * outside check stand pat remains available, without an extra legal-move scan. King and loss
 * checks and cycle filtering precede the 500-ply extension cap; at the cap an in-check position
 * must have a surviving legal evasion before the original position is statically evaluated.
 * The evaluator receives the side to move, and its result is clamped to +/-28999 for negamax.
 * Quiescence flips child scores and windows, then ordinary search converts its result back to
 * the root perspective. Wins/losses remain +/-30000. Quiescence shares time and ancestor cancellation checks,
 * preserves cutoff bounds and never splits tasks, accesses the table or extends PV hints.
 * Node counts include normal and quiescence entries (the shared leaf counts once for each);
 * reported depths exclude extensions and progress still counts only root candidates.
 * The first root iteration and internal nodes use the built-in stable capture ordering. With a
 * table, non-root normal-search nodes that do not return early also borrow move-ordering hints from
 * similar positions, even when a matching cache or PV move exists. Initially these positions are
 * legal siblings: the parent's current best branch first, then its ordered move list, excluding
 * the current branch and duplicate positions. Other sources can share the same per-node budget.
 * The `PARALLEL_MAX_SIMILAR_POSITION_QUERIES` enum constant in `parallel_search.c` (set to `0`
 * to disable) limits actual lookup attempts, including misses, busy buckets and unusable hits;
 * rejected candidate positions do not consume the budget. Only `XQ_SEARCH_SCORE_EXACT` or
 * `XQ_SEARCH_SCORE_LOWER_BOUND` entries supply hints, regardless of depth. Hints must match a current
 * move's `from`, `to` and `piece` (including color/type); the current move's complete data is retained.
 * An entry with `score == WIN` (`30000`, from the candidate's side-to-move perspective) supplies a
 * winning hint, which may be a capture or quiet move. Winning hints do not stop similar-position
 * queries: enumeration continues until the budget or sources are exhausted, or the node stops.
 * Distinct winning hints are retained in discovery order, along with ordinary quiet hints.
 * Other accepted entries supply quiet hints only. Similar-position scores identify hint priority
 * only; their scores, bounds and PVs are never reused to score or cut off the current node.
 * Hint collection does not reorder moves. After collection, one ordering stage applies ordinary
 * quiet hints, the matching-position cache move, winning hints and the PV hint together, before
 * task splitting. Ordering precedence is previous-iteration PV, winning hints in discovery order,
 * matching-position cache move, captures, ordinary similar-position quiet hints in discovery order,
 * then other quiet moves; unaffected moves retain their relative order. Later root iterations retain
 * the previous root ordering. A valid PV first move retains its matching suffix even when a
 * different winning hint exists; an invalid or absent PV first move clears any retained suffix.
 * Up to 64 moves are retained even without a table, without limiting search depth. Exact cache
 * cutoffs may rebuild a PV prefix; terminal positions, cache bounds/misses/contention or invalid
 * moves can shorten it.
 * PV hints affect ordering only and are not exposed in the result. No custom move scorer is used.
 * Similar-position lookups skip busy buckets without waiting and do not refresh entry generations.
 * TT statistics accumulate on the table, excluding PV reconstruction and similar-position lookups;
 * stats contains per-call counters without resetting the table. Each normal-search node may split
 * once after all move ordering, provided its remaining normal-search depth is at least
 * PARALLEL_MIN_SPLIT_DEPTH (5, defined in parallel_search.c). This includes the next move and excludes quiescence extensions;
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

/* Same search with optional observation of root progress, cooperative logical-path samples
 * and thread-state events (see XqSearchProgressFn concurrency contract).
 * Pass NULL for sampler to receive only root progress. Terminal roots, fully filtered roots
 * and argument/resource failures emit no iteration events. The sampler is borrowed until
 * return; a controller with capacity smaller than options.thread_count disables sampling for
 * this invocation. The basic entrypoint never emits sampling events. */
XqParallelStatus xq_engine_parallel_search_with_observer(const XqEngineAdapter *engine,
    const XqPosition *pos, const XqParallelOptions *options, XqParallelResult *result,
    XqSearchProgressFn progress, void *progress_user, XqSearchSampler *sampler);

#endif
