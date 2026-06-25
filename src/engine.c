#include "xiangqi/engine.h"

#include "xiangqi/movegen.h"

#include <limits.h>

static const int piece_values[XQ_PIECE_TYPE_NB] = {
    10000, 120, 120, 270, 600, 285, 70};

int xq_engine_material_evaluate(const XqPosition *pos, XqColor perspective, void *user)
{
    int score = 0;
    int sq;

    (void)user;
    for (sq = 0; sq < XQ_SQUARES; ++sq)
    {
        int piece = pos->board[sq];
        int value;
        if (piece == XQ_EMPTY_PIECE)
        {
            continue;
        }
        value = piece_values[xq_piece_type(piece)];
        score += xq_piece_color(piece) == perspective ? value : -value;
    }
    return score;
}

static int default_eval(const XqEngineAdapter *engine, const XqPosition *pos, XqColor perspective)
{
    if (engine != NULL && engine->evaluate != NULL)
    {
        return engine->evaluate(pos, perspective, engine->user);
    }
    return xq_engine_material_evaluate(pos, perspective, NULL);
}

static int negamax(const XqEngineAdapter *engine, const XqPosition *pos, unsigned depth, int alpha, int beta)
{
    XqMoveList list;
    int best = INT_MIN / 2;
    int i;

    if (depth == 0)
    {
        return default_eval(engine, pos, pos->side_to_move);
    }

    xq_generate_legal(pos, &list);
    if (list.count == 0)
    {
        return xq_position_in_check(pos, pos->side_to_move) ? -30000 - (int)depth : 0;
    }

    for (i = 0; i < list.count; ++i)
    {
        XqPosition next = *pos;
        int score;

        xq_position_make_move(&next, list.moves[i]);
        score = -negamax(engine, &next, depth - 1, -beta, -alpha);
        if (score > best)
        {
            best = score;
        }
        if (score > alpha)
        {
            alpha = score;
        }
        if (alpha >= beta)
        {
            break;
        }
    }

    return best;
}

static bool builtin_search(const XqEngineAdapter *engine, const XqPosition *pos, unsigned depth, XqMove *best_move)
{
    XqMoveList list;
    int best_score = INT_MIN / 2;
    int i;

    if (depth == 0)
    {
        depth = 1;
    }

    xq_generate_legal(pos, &list);
    if (list.count == 0)
    {
        return false;
    }

    for (i = 0; i < list.count; ++i)
    {
        XqPosition next = *pos;
        int score;

        xq_position_make_move(&next, list.moves[i]);
        score = -negamax(engine, &next, depth - 1, INT_MIN / 2, INT_MAX / 2);
        if (score > best_score)
        {
            best_score = score;
            *best_move = list.moves[i];
        }
    }

    return true;
}

bool xq_engine_find_best_move(const XqEngineAdapter *engine, const XqPosition *pos, unsigned depth, XqMove *best_move)
{
    if (best_move == NULL)
    {
        return false;
    }
    if (engine != NULL && engine->search != NULL)
    {
        return engine->search(pos, depth, best_move, engine->user);
    }
    return builtin_search(engine, pos, depth, best_move);
}
