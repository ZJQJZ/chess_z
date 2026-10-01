#ifndef XIANGQI_ENGINE_H
#define XIANGQI_ENGINE_H

#include <stdbool.h>

#include "xiangqi/history.h"
#include "xiangqi/position.h"
#include "xiangqi/types.h"

typedef int (*XqEvaluateFn)(const XqPosition *pos, XqColor perspective, void *user);
typedef bool (*XqSearchFn)(const XqPosition *pos, unsigned depth, XqMove *best_move, void *user);
typedef int (*XqMoveScoreFn)(const XqPosition *pos, XqMove move, void *user);

typedef struct XqTranspositionTable XqTranspositionTable;

typedef struct XqTranspositionStats
{
    uint64_t probes;
    uint64_t hits;
    uint64_t cutoffs;
    uint64_t stores;
    uint64_t replacements;
} XqTranspositionStats;

/* Per-call statistics; depth is in plies and excludes quiescence extensions. */
typedef struct XqSearchStats
{
    bool available;               /* Internal counters are available only for built-in search. */
    unsigned max_started_depth;   /* Deepest iteration in which a root move was searched. */
    unsigned iteration_completed_moves; /* Completed root candidates at max_started_depth. */
    unsigned iteration_total_moves;     /* Root candidates after history filtering, even if unstarted. */
    unsigned completed_depth;     /* All root moves completed at this depth. */
    unsigned selected_move_depth; /* Selected result depth; zero for an unsearched fallback. */
    uint64_t nodes; /* Entries into negamax and quiescence, including their shared leaf. */
    bool stopped;   /* Time limit reached (including an extended iteration) or search clock failed. */
    bool elapsed_available;
    uint64_t elapsed_ms; /* Monotonic wall time, independent of the search time mode. */
    bool cache_available;
    XqTranspositionStats cache; /* Counter deltas; cumulative table stats are untouched. */
} XqSearchStats;

typedef struct XqEngineAdapter
{
    XqEvaluateFn static_evaluate;
    XqSearchFn search;
    void *user;
    XqMoveScoreFn score_move;
    XqTranspositionTable *transposition_table;
    /* Optional real-game history, borrowed for built-in root search only.
     * NULL, empty or mismatched history disables cycle detection. */
    const XqHistory *history;
} XqEngineAdapter;

typedef enum XqSearchTimeMode
{
    /* 用户实际等待的单调墙钟时间。 */
    XQ_SEARCH_TIME_MONOTONIC,
    /* 当前进程消耗的 CPU 时间。 */
    XQ_SEARCH_TIME_CPU
} XqSearchTimeMode;

typedef struct XqSearchLimits
{
    unsigned max_depth;
    uint64_t time_limit_ms; /* Zero disables timing; a nearly complete iteration may exceed the budget. */
    XqSearchTimeMode time_mode;
} XqSearchLimits;

typedef enum XqSearchScoreKind
{
    XQ_SEARCH_SCORE_EXACT,
    XQ_SEARCH_SCORE_UPPER_BOUND,
    XQ_SEARCH_SCORE_LOWER_BOUND
} XqSearchScoreKind;

typedef struct XqExplainedMove
{
    XqMove move;
    unsigned depth;
    int alpha_before;
    int beta;
    int score;
    int order_score;
    XqSearchScoreKind score_kind;
    bool is_best;
    bool caused_cutoff;
} XqExplainedMove;

typedef struct XqExplainResult
{
    XqExplainedMove moves[XQ_MAX_MOVES];
    int count;
    int best_index;
    int final_score;
} XqExplainResult;

typedef struct XqQuiescenceExplainResult
{
    XqExplainedMove moves[XQ_MAX_MOVES];
    int count;
    int best_index;
    int alpha_before;
    int alpha_after_stand_pat;
    int beta;
    int stand_pat;
    int final_score;
    bool in_check;
    bool stand_pat_used;
    bool stand_pat_cutoff;
} XqQuiescenceExplainResult;

typedef enum XqExplainTtStatus
{
    XQ_EXPLAIN_TT_DISABLED,
    XQ_EXPLAIN_TT_NOT_PROBED, /* E.g. a depth-zero leaf or a terminal return before lookup. */
    XQ_EXPLAIN_TT_MISS,
    XQ_EXPLAIN_TT_HIT,       /* Entry found without returning; may be an ordering-only lookup. */
    XQ_EXPLAIN_TT_CUTOFF     /* The node returned directly from this entry. */
} XqExplainTtStatus;

/* Snapshot of the actual entry probe, before descendant searches can replace it.
 * depth, score, score_kind and best_move are valid for HIT/CUTOFF only. score is
 * restored for the queried ply, from that node's side-to-move perspective.
 * hash_move_used means the move was matched and promoted for ordering; a PV
 * hint can subsequently take priority. It is false on a direct cache return. */
typedef struct XqExplainTtInfo
{
    XqExplainTtStatus status;
    unsigned depth;
    int score;
    XqSearchScoreKind score_kind;
    XqMove best_move;
    bool hash_move_used;
    bool ordering_only; /* Root lookup supplies ordering only, irrespective of depth/bound. */
} XqExplainTtInfo;

/* Latest observed cache cutoff at a strict ancestor on the requested path.
 * Independent of the retained target visit; root_depth identifies this event's
 * iteration, which need not be the target record's or the last started iteration. */
typedef struct XqExplainTtBlock
{
    bool available;
    unsigned root_depth;
    size_t ply;
    unsigned remaining_depth;
    int alpha;
    int beta;
    XqExplainTtInfo tt;
} XqExplainTtBlock;

/* Root candidate from the last complete iteration, or the partial first iteration
 * when none completed. score may be a bound and is valid only when completed_depth > 0.
 * All valid scores in the final selection snapshot belong to the same iteration. */
typedef struct XqRootMoveExplain
{
    XqMove move;
    int score;
    unsigned completed_depth;
} XqRootMoveExplain;

/* Final root decision, independent of the retained target-node visit. The root cache
 * is queried for ordering once, before iteration 1; ordering_tt preserves that query.
 * selected_index is -1 for no selection or a fallback outside the filtered move list. */
typedef struct XqRootSearchExplain
{
    bool move_available;
    XqMove selected_move;
    int selected_index;
    int count;
    XqRootMoveExplain moves[XQ_MAX_MOVES];
    XqExplainTtInfo ordering_tt;
} XqRootSearchExplain;

/* A normal-search node (remaining_depth >= 1) observed along an exact root path.
 * score_kind is meaningful only when complete; a partial final_score is the best
 * completed candidate score, not an exact node evaluation. visited is false if
 * the path was never reached with positive remaining normal-search depth. */
typedef struct XqPathExplainResult
{
    bool visited;
    bool complete;
    bool score_available;
    unsigned root_depth;
    unsigned remaining_depth;
    size_t ply;
    XqSearchScoreKind score_kind;
    int alpha_before;
    int beta;
    XqExplainResult node;
    XqExplainTtInfo tt; /* Target node's entry probe, not its descendants' probes. */
    XqExplainTtInfo move_tt[XQ_MAX_MOVES]; /* Parallel to node.moves; child perspective. */
    XqExplainTtBlock blocked_tt;
    XqRootSearchExplain root; /* CLI-compatible final choice; node.best_index is visit-local. */
    XqSearchStats stats;
} XqPathExplainResult;

int xq_engine_default_static_evaluate(const XqPosition *pos, XqColor perspective, void *user);
int xq_engine_default_move_order_score(const XqPosition *pos, XqMove move, void *user);
XqTranspositionTable *xq_transposition_table_create(void);
void xq_transposition_table_clear(XqTranspositionTable *table);
void xq_transposition_table_destroy(XqTranspositionTable *table);
void xq_transposition_table_reset_stats(XqTranspositionTable *table);
void xq_transposition_table_get_stats(const XqTranspositionTable *table,
                                      XqTranspositionStats *stats);
typedef enum XqTranspositionIoStatus
{
    XQ_TT_IO_OK,
    XQ_TT_IO_FILE_ERROR,
    XQ_TT_IO_INVALID_FORMAT,
    XQ_TT_IO_INCOMPATIBLE,
    XQ_TT_IO_NO_MEMORY,
    XQ_TT_IO_INVALID_ARGUMENT
} XqTranspositionIoStatus;

/* Built-in search caches only; callers must not search or mutate the table concurrently.
 * Save preserves the table and replaces path only after a complete temporary file is closed.
 * Load validates into temporary storage, preserving the table on any failure. On success it
 * restores slots and generations, resets statistics, and preserves the table object's address.
 * Neither operation creates parent directories or saves/restores a game position or history.
 * Files require matching format and engine-cache versions; custom evaluation/search semantics
 * must not be mixed with this built-in cache format. Null/empty arguments are rejected. */
XqTranspositionIoStatus xq_transposition_table_save(const XqTranspositionTable *table,
                                                    const char *path);
XqTranspositionIoStatus xq_transposition_table_load(XqTranspositionTable *table, const char *path);
XqSearchLimits xq_search_limits_default(void);
/* limits 为 NULL 时使用默认限制；搜索深度由 limits->max_depth 指定。 */
/* 时间限制仅适用于内置搜索；自定义 search 回调仍自行管理时间。 */
bool xq_engine_find_best_move(const XqEngineAdapter *engine, XqPosition *pos,
                              const XqSearchLimits *limits, XqMove *best_move);
/* stats may be NULL; otherwise initialized on every return, including failure.
 * Custom callbacks provide wall time only; internal/cache statistics are unavailable. */
bool xq_engine_find_best_move_with_stats(const XqEngineAdapter *engine, XqPosition *pos,
                                         const XqSearchLimits *limits, XqMove *best_move,
                                         XqSearchStats *stats);
bool xq_engine_explain_search_one_ply(const XqEngineAdapter *engine, XqPosition *pos,
                                      unsigned depth, XqExplainResult *result);
bool xq_engine_explain_quiescence_one_ply(const XqEngineAdapter *engine, XqPosition *pos,
                                          XqQuiescenceExplainResult *result);

/* Observe the exact legal from/to path (NULL for an empty path), searching from pos.
 * Uses an independent root driver aligned with CLI search, including history filtering,
 * PV/root ordering, cache generations, time checks and fallback to the last complete iteration.
 * If none completed, select the best completed first-iteration candidate or an unsearched fallback.
 * A root iteration strictly above the engine's progress threshold at timeout may finish before exit.
 * Production builtin_search, negamax and quiescence carry no explanation hooks.
 * Iterations always start at 1; limits is required with max_depth >= 1, and time_limit_ms=0
 * disables the deadline. Evaluation/ordering callbacks, table and history are used;
 * custom search callbacks are ignored. pos is restored on return.
 * The root cache is queried once for ordering, never for a direct score return. Internal
 * nodes can return cached scores. Memory updates and per-call cache deltas follow ordinary
 * search; the table is neither saved nor cleared and must match evaluation semantics.
 * Only positive normal-depth target visits are recorded. Retain the latest complete visit,
 * otherwise the latest partial one; blocked_tt independently records the latest ancestor
 * cache cutoff. root holds the actual final move and candidate returns used for selection,
 * separate from the retained target visit and its best_index. Tracing takes wall time, so
 * separate timed runs can still stop at different nodes. Terminal roots use root_depth=0.
 * False means invalid arguments; timeout, terminal and unvisited paths are valid results.
 * result is initialized even on failure (unless itself NULL). */
bool xq_engine_explain_path(const XqEngineAdapter *engine, XqPosition *pos,
                            const XqSearchLimits *limits, const XqMove *path, size_t path_count,
                            XqPathExplainResult *result);

#endif
