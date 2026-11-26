#ifndef XIANGQI_JIEQI_PARALLEL_SEARCH_JQ_H
#define XIANGQI_JIEQI_PARALLEL_SEARCH_JQ_H

#include "xiangqi/jieqi/engine_jq.h"

typedef enum XqJqSearchProgressEvent
{
    XQ_JQ_SEARCH_ITERATION_STARTED,
    XQ_JQ_SEARCH_ROOT_MOVE_COMPLETED,
    XQ_JQ_SEARCH_ITERATION_FINISHED
} XqJqSearchProgressEvent;

typedef struct XqJqSearchProgress
{
    XqJqSearchProgressEvent event;
    unsigned depth;
    unsigned completed_moves;
    unsigned total_moves;
} XqJqSearchProgress;

/* Snapshots are borrowed for the duration of the callback. Parallel root-move callbacks
 * may overlap and arrive out of count order; consumers must synchronize and merge counts
 * monotonically within each iteration. STARTED precedes all root callbacks; FINISHED follows
 * them, including for partial iterations. No callbacks outlive the search. Keep callbacks
 * short and do not mutate/reenter the search, its position, table or evaluator state.
 * A root move completes only after all of its required reveal samples have completed.
 * Events describe ordinary iteration progress; no path or thread-state sampling is provided. */
typedef void (*XqJqSearchProgressFn)(const XqJqSearchProgress *progress, void *user);

typedef struct XqJqTranspositionStats
{
    uint64_t probes;
    uint64_t hits;
    uint64_t cutoffs;
    uint64_t stores;
    uint64_t replacements;
} XqJqTranspositionStats;

/* Per-call statistics; depth is a normal-search budget, excluding quiescence extensions.
 * Revealing a dark piece consumes its configured total cost rather than one ordinary ply. */
typedef struct XqJqSearchStats
{
    bool available;               /* Whether internal search counters are available. */
    unsigned max_started_depth;   /* Deepest iteration in which a root move was searched. */
    unsigned iteration_completed_moves; /* Completed root candidates at max_started_depth. */
    unsigned iteration_total_moves;     /* Root candidates, after any history filtering, even if unstarted. */
    unsigned completed_depth;     /* All root moves completed at this depth. */
    unsigned selected_move_depth; /* Selected result depth; zero for an unsearched fallback. */
    uint64_t nodes; /* Normal/quiescence entries, including one of each at their shared leaf. */
    bool stopped;   /* Time limit reached (including an extended iteration) or search clock failed. */
    bool winning_move_found; /* Selected a root move with a non-estimated proven winning score. */
    bool elapsed_available;
    uint64_t elapsed_ms; /* Monotonic wall time, independent of the search time mode. */
    bool cache_available;
    XqJqTranspositionStats cache; /* Per-call counters; cumulative table stats are not reset by collection. */
} XqJqSearchStats;

typedef enum XqJqSearchScoreKind
{
    XQ_JQ_SEARCH_SCORE_EXACT,
    XQ_JQ_SEARCH_SCORE_UPPER_BOUND,
    XQ_JQ_SEARCH_SCORE_LOWER_BOUND
} XqJqSearchScoreKind;

typedef enum XqJqParallelStatus
{
    XQ_JQ_PARALLEL_OK, /* Includes terminal positions: inspect move_available. */
    XQ_JQ_PARALLEL_INVALID_ARGUMENT,
    XQ_JQ_PARALLEL_RESOURCE_ERROR,
    XQ_JQ_PARALLEL_UNSUPPORTED
} XqJqParallelStatus;

typedef struct XqJqParallelTranspositionTable XqJqParallelTranspositionTable;

/* Independent, fixed-capacity, in-memory cache for parallel search. One top-level search at a
 * time may borrow a table; its workers share it internally. Do not query statistics, clear,
 * reset, destroy, or reuse the table externally until that search returns. Different tables
 * may be used concurrently. This cache is not compatible with the ordinary engine's tables.
 * Creation returns NULL on allocation failure or Windows. Clear/destroy/reset accept NULL;
 * get_stats ignores a NULL output and zeroes it for a NULL table (always zero on Windows). */
XqJqParallelTranspositionTable *xq_jq_parallel_transposition_table_create(void);
void xq_jq_parallel_transposition_table_clear(XqJqParallelTranspositionTable *table);
void xq_jq_parallel_transposition_table_destroy(XqJqParallelTranspositionTable *table);
void xq_jq_parallel_transposition_table_reset_stats(XqJqParallelTranspositionTable *table);
void xq_jq_parallel_transposition_table_get_stats(const XqJqParallelTranspositionTable *table,
                                                XqJqTranspositionStats *stats);

typedef enum XqJqParallelSplitStrategy
{
    XQ_JQ_PARALLEL_SPLIT_CONTIGUOUS = 0, /* Helper takes a tail batch sized by split_percent. */
    XQ_JQ_PARALLEL_SPLIT_INTERLEAVED    /* After ordering: round-robin among owner and available helpers. */
} XqJqParallelSplitStrategy;

/* Called for a dark-moving candidate after all ordering hints have been applied. move_order
 * is its 1-based ordinal among all candidates, not only the dark moves. remaining_depth is
 * the current ordinary node's remaining budget; quiescence always supplies zero. The callback
 * and user data must support concurrent calls and remain unchanged until the search returns.
 * A returned zero is treated as one sample. */
typedef unsigned (*XqJqSampleCountFn)(unsigned remaining_depth, unsigned move_order, void *user);

/* Returns 1 + min(remaining_depth, 7) / rank, where rank is max(move_order, 1).
 * Thus smaller remaining depths and later moves receive no more samples; quiescence gets one.
 * user is unused. This default function is the single place to adjust the default policy. */
unsigned xq_jq_parallel_sample_count_default(unsigned remaining_depth, unsigned move_order,
                                             void *user);

typedef struct XqJqParallelOptions
{
    unsigned depth;          /* Maximum normal-search budget >= 1; default UINT_MAX (effectively unlimited). Excludes quiescence. */
    unsigned thread_count;   /* >= 1, includes calling thread; default 2. */
    unsigned split_percent;  /* 0..100, default 50; contiguous only (0 disables); ignored by interleaved. */
    XqJqParallelTranspositionTable *transposition_table; /* Borrowed; NULL disables caching (default). */
    XqJqParallelSplitStrategy split_strategy; /* Default INTERLEAVED; applied after all move ordering. */
    uint64_t time_limit_ms;  /* Monotonic wall time; default 5000 ms; 0 disables timing. */
    XqJqSampleCountFn sample_count; /* NULL selects xq_jq_parallel_sample_count_default. */
    void *sample_user;       /* Borrowed by sample_count; unused by the default policy. */
    uint64_t random_seed;    /* Search-only randomness; independent of hidden real-game identities. */
} XqJqParallelOptions;

typedef struct XqJqParallelResult
{
    bool move_available;
    XqMove best_move;
    int score; /* Root-side value, valid only when score_available; see score_kind. */
    bool score_available;
    XqJqSearchScoreKind score_kind;
    XqJqSearchStats stats;
    bool score_estimated; /* Sampling-dependent value, including a sampled mean of +/-30000. */
} XqJqParallelResult;

XqJqParallelOptions xq_jq_parallel_options_default(void);
const char *xq_jq_parallel_status_text(XqJqParallelStatus status);

/* pos is never modified and contains only public information. NULL options selects defaults.
 * NULL engine or static_evaluate selects the built-in evaluator with NULL evaluator user data;
 * NULL engine also disables real-game root history filtering. The evaluator and its user data
 * must support concurrent calls and must not modify positions. Opposite perspectives must yield
 * opposite evaluation scores. Keep the adapter, history, callbacks, user data and table alive and
 * unchanged until return.
 *
 * Dark-move samples draw from the current side's public identity pool. Every sample starts from
 * the same parent position, and the resulting scores are averaged without clipping a sampled
 * +/-30000 into the non-winning score range. score_estimated distinguishes that finite-sample
 * estimate from a proof. EXACT denotes a resolved value for the current sampled search tree;
 * it does not claim an exact underlying expectation when score_estimated is true.
 * Ordinary dark moves enter max(remaining_depth - C, 0), where the source enum constant C defaults
 * to 2 and is the total depth cost. Quiescence dark moves use sample_count with
 * remaining_depth == 0.
 *
 * The public search entry scores a root missing a king as terminal. After history filtering,
 * cached outcome and time-stop checks, the entry scans the remaining legal root moves before
 * creating the helper pool. It scores the first king capture directly, skipping the move and
 * reveal sampling. The score is an exact +30000 with score_estimated == false,
 * selected_move_depth == 1 and winning_move_found == true. Node counts, started and completed
 * depths, and completed root move counts are zero. When a table is available and no root
 * candidates were filtered, the entry attempts to store the exact win at depth 1.
 *
 * Ordinary non-root nodes scan pseudo-legal candidates and score a win for the side to move if
 * any candidate captures the opposing king, even when both kings are attacked. This check precedes
 * path repetition, quiescence and table lookup. Scoring a king capture skips the move and reveal
 * sampling, so both kings remain on the board throughout recursion.
 *
 * Quiescence starts with the ordinary leaf's refreshed move cache and check state. Deeper nodes
 * refresh their own candidates and score a king capture as +30000 before repetition, the extension
 * cap, stand pat or evasion filtering. Continuing nodes collect only captures outside check and
 * all candidates in check.
 *
 * Ordinary search and quiescence classify cycles at the first repetition of a public position.
 * The cycle uses the closest ancestor with the same side to move, board, dark markers and public
 * identity counts. It starts after that ancestor and includes the closing move. If both sides
 * always gave check, the score is a draw. If only the side to move always gave check, that side
 * loses. If only its opponent always gave check, search continues. If neither side always gave
 * check, ordinary search enters quiescence and a quiescence branch returns a static evaluation.
 * Each quiescence invocation starts a private path at its entry position. At 500 plies or a
 * repetition requiring static evaluation, quiescence evaluates the current position after the
 * king-capture scan and clamps the score to +/-28999, even while in check. Repetition draws and
 * losses take precedence over the cap. The root still filters real-game cycles using the supplied
 * history.
 *
 * Cache keys include public dark markers and identity counts. Scores and bounds, including
 * estimates, may be reused when the cached remaining depth is at least the requested depth.
 * Estimated +/-30000 never proves a game outcome. A change of effective
 * evaluator/user, sample-count function/user, search seed, reveal-depth cost, thread count, split
 * strategy, or split percent clears the table context. Clear it explicitly after changing
 * parameters behind an unchanged user pointer.
 *
 * Searches depths 1..options.depth with the existing helper pool, ordering, split strategies,
 * root history filtering, repetition rules, monotonic timing and fallback selection. Sampling may
 * resolve the current search tree at an estimated maximum score before all root moves finish;
 * that result can be selected and followed by the next iteration, but it does not set
 * winning_move_found or stop later deepening as a proven win. selected_move_depth can therefore
 * exceed completed_depth, which only counts depths with all root candidates completed. Progress
 * continues to report actual completed candidates. A cancelled, unfinished sample set supplies no
 * candidate score. A fallback has score_available == false and selected_move_depth == 0.
 *
 * PV ordering hints stop at the first dark-moving move, including that move but no hypothetical
 * revealed continuation. Cache-reconstructed PVs follow the same rule; neighboring hints do not
 * randomly reveal dark pieces. Hints do not prove values or change hidden real-game assignments.
 * The neighboring lookup limit in `parallel_search_jq.c` counts each attempted lookup, including
 * repeated keys, misses, busy buckets and unusable entries. The current position's key is skipped.
 * Hint moves are matched by `from`, `to` and `piece` and retain the current position's capture data.
 *
 * Results are initialized even on failure. Every helper and callback completes before return.
 * Equal-score choices may depend on scheduling. POSIX pthread implementation requiring
 * always-lock-free atomic int; Windows returns UNSUPPORTED. */
XqJqParallelStatus xq_jq_engine_parallel_search(const XqJqEngineAdapter *engine,
    const XqJqPosition *pos, const XqJqParallelOptions *options, XqJqParallelResult *result);

/* Same search with optional observation of ordinary root progress. Terminal roots, cached proven
 * root outcomes, immediate root king captures, fully filtered roots and argument/resource failures
 * emit no iteration events.
 * There is no path-sampling controller or per-thread state reporting. */
XqJqParallelStatus xq_jq_engine_parallel_search_with_observer(const XqJqEngineAdapter *engine,
    const XqJqPosition *pos, const XqJqParallelOptions *options, XqJqParallelResult *result,
    XqJqSearchProgressFn progress, void *progress_user);

#endif
