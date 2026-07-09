#ifndef XIANGQI_ENGINE_H
#define XIANGQI_ENGINE_H

#include <stdbool.h>

#include "xiangqi/position.h"
#include "xiangqi/types.h"

typedef int (*XqEvaluateFn)(const XqPosition *pos, XqColor perspective, void *user);
typedef bool (*XqSearchFn)(const XqPosition *pos, unsigned depth, XqMove *best_move, void *user);
typedef int (*XqMoveScoreFn)(const XqPosition *pos, XqMove move, void *user);

typedef struct XqEngineAdapter
{
    XqEvaluateFn static_evaluate;
    XqSearchFn search;
    void *user;
    XqMoveScoreFn score_move;
} XqEngineAdapter;

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
bool xq_engine_find_best_move(const XqEngineAdapter *engine, XqPosition *pos, unsigned depth, XqMove *best_move);
bool xq_engine_explain_search_one_ply(const XqEngineAdapter *engine, XqPosition *pos, unsigned depth, XqExplainResult *result);
bool xq_engine_explain_quiescence_one_ply(const XqEngineAdapter *engine, XqPosition *pos, XqQuiescenceExplainResult *result);

#endif
