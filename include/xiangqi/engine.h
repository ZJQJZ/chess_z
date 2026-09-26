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
    bool available; /* Internal counters are available only for built-in search. */
    unsigned max_started_depth; /* Zero if no root move was searched. */
    unsigned completed_depth; /* All root moves completed at this depth. */
    unsigned selected_move_depth; /* Zero for an unsearched fallback move. */
    uint64_t nodes; /* Entries into negamax and quiescence, including their shared leaf. */
    bool stopped; /* Time limit reached or search clock failed. */
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
    uint64_t time_limit_ms;
    int depth_bonus;
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
XqTranspositionIoStatus xq_transposition_table_load(XqTranspositionTable *table,
                                                   const char *path);
XqSearchLimits xq_search_limits_default(void);
/* limits 为 NULL 时使用默认限制；搜索深度由 limits->max_depth 指定。 */
/* 时间限制和深度奖励仅适用于内置搜索；自定义 search 回调仍自行管理时间。 */
bool xq_engine_find_best_move(const XqEngineAdapter *engine, XqPosition *pos,
                              const XqSearchLimits *limits, XqMove *best_move);
/* stats may be NULL; otherwise initialized on every return, including failure.
 * Custom callbacks provide wall time only; internal/cache statistics are unavailable. */
bool xq_engine_find_best_move_with_stats(const XqEngineAdapter *engine, XqPosition *pos,
                                       const XqSearchLimits *limits, XqMove *best_move,
                                       XqSearchStats *stats);
bool xq_engine_explain_search_one_ply(const XqEngineAdapter *engine, XqPosition *pos, unsigned depth, XqExplainResult *result);
bool xq_engine_explain_quiescence_one_ply(const XqEngineAdapter *engine, XqPosition *pos, XqQuiescenceExplainResult *result);

#endif
