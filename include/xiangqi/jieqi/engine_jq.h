#ifndef XIANGQI_JIEQI_ENGINE_JQ_H
#define XIANGQI_JIEQI_ENGINE_JQ_H

#include "xiangqi/jieqi/history_jq.h"
#include "xiangqi/jieqi/movegen_jq.h"

typedef int (*XqJqEvaluateFn)(const XqJqPosition *pos, XqColor perspective, void *user);
typedef int (*XqJqMoveScoreFn)(const XqJqPosition *pos, XqMove move, void *user);

typedef struct XqJqEngineAdapter
{
    XqJqEvaluateFn static_evaluate;
    void *user;
    /* Optional real-game history, borrowed by parallel root searches only.
     * NULL, empty or mismatched history disables cycle detection. */
    const XqJqHistory *history;
} XqJqEngineAdapter;

/* Revealed pieces use the ordinary material and piece-square values. Dark pieces
 * use their side's public pool average without a positional bonus. The public
 * pool can include captured, unrevealed identities; only live pieces contribute.
 * The total dark material is rounded once per side. pos must be valid and is
 * left unchanged; user is unused. Opposite perspectives yield opposite scores. */
int xq_jq_engine_default_static_evaluate(const XqJqPosition *pos,
                                         XqColor perspective, void *user);

/* Quiet moves score zero. Captures use the ordinary MVV-LVA formula, substituting
 * the public pool average for a dark attacker or victim. pos and move must
 * describe the position before the move; user is unused. */
int xq_jq_engine_default_move_order_score(const XqJqPosition *pos,
                                          XqMove move, void *user);

/* Stable descending sort; each move is scored once. A NULL score_move selects
 * the default scorer. Otherwise user is passed to score_move. pos must be valid,
 * list must contain at most XQ_JQ_MAX_MOVES entries, and neither may be NULL. */
void xq_jq_engine_order_moves(const XqJqPosition *pos, XqJqMoveList *list,
                              XqJqMoveScoreFn score_move, void *user);

#endif
