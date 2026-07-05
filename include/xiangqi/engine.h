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

int xq_engine_default_static_evaluate(const XqPosition *pos, XqColor perspective, void *user);
bool xq_engine_find_best_move(const XqEngineAdapter *engine, const XqPosition *pos, unsigned depth, XqMove *best_move);

#endif
