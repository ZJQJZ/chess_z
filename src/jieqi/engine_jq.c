#include "xiangqi/jieqi/engine_jq.h"

#include <stddef.h>
#include <stdint.h>

/**
 * @brief Base material values indexed by `XqPieceType`, in the engine's score units.
 *
 * Both colors share these values. Static evaluation adds the corresponding bonus from
 * `piece_square_values` to each piece's base value. These values also guide capture ordering,
 * favoring more valuable captured pieces and less valuable attacking pieces.
 *
 * These values depend only on piece type; they do not account for position or game phase.
 * Unspecified entries are `0`.
 */
static const int piece_values[XQ_PIECE_TYPE_NB] = {
    [XQ_KING] = 10000, [XQ_ADVISOR] = 120, [XQ_BISHOP] = 120, [XQ_KNIGHT] = 270,
    [XQ_ROOK] = 600,   [XQ_CANNON] = 285,  [XQ_PAWN] = 70,
};

/**
 * @brief Positional bonuses added to `piece_values`, in the same score units.
 *
 * Rows run from the piece owner's home rank (`0`) to the opponent's home rank (`9`), unlike FEN's
 * display order. Red uses the board rank directly; Black uses `9 - rank`. Every row is symmetric
 * across files, so both colors share the tables without a file reversal.
 *
 * These tables describe location only; they do not account for attacks, blocked horse legs, cannon
 * screens or game phase. Unspecified rows for palace-bound pieces and bishops are `0`.
 */
static const int piece_square_values[XQ_PIECE_TYPE_NB][XQ_RANKS][XQ_FILES] =
    {
        [XQ_KING] =
            {
                {0, 0, 0, 0, 0, 0, 0, 0, 0},
                {0, 0, 0, -8, -4, -8, 0, 0, 0},
                {0, 0, 0, -16, -12, -16, 0, 0, 0},
            },
        [XQ_ADVISOR] =
            {
                {0, 0, 0, 0, 0, 0, 0, 0, 0},
                {0, 0, 0, 0, 6, 0, 0, 0, 0},
                {0, 0, 0, 0, 0, 0, 0, 0, 0},
            },
        [XQ_BISHOP] =
            {
                {0, 0, 0, 0, 0, 0, 0, 0, 0},
                {0, 0, 0, 0, 0, 0, 0, 0, 0},
                {2, 0, 0, 0, 6, 0, 0, 0, 2},
                {0, 0, 0, 0, 0, 0, 0, 0, 0},
                {0, 0, 2, 0, 0, 0, 2, 0, 0},
            },
        [XQ_KNIGHT] =
            {
                {-20, -8, -12, -8, -8, -8, -12, -8, -20},
                {-16, -4, 0, 4, 4, 4, 0, -4, -16},
                {-12, 0, 8, 12, 12, 12, 8, 0, -12},
                {-8, 4, 12, 20, 20, 20, 12, 4, -8},
                {-4, 8, 16, 24, 24, 24, 16, 8, -4},
                {-4, 8, 20, 28, 28, 28, 20, 8, -4},
                {-8, 8, 20, 28, 28, 28, 20, 8, -8},
                {-12, 4, 16, 24, 24, 24, 16, 4, -12},
                {-16, 0, 8, 12, 12, 12, 8, 0, -16},
                {-20, -8, -4, 0, 0, 0, -4, -8, -20},
            },
        [XQ_ROOK] =
            {
                {0, 0, 2, 4, 2, 4, 2, 0, 0},
                {2, 4, 6, 8, 6, 8, 6, 4, 2},
                {4, 6, 8, 10, 8, 10, 8, 6, 4},
                {4, 8, 10, 12, 10, 12, 10, 8, 4},
                {6, 8, 12, 14, 12, 14, 12, 8, 6},
                {6, 8, 12, 14, 12, 14, 12, 8, 6},
                {6, 8, 12, 16, 14, 16, 12, 8, 6},
                {8, 12, 16, 20, 18, 20, 16, 12, 8},
                {8, 12, 16, 20, 18, 20, 16, 12, 8},
                {4, 8, 12, 16, 14, 16, 12, 8, 4},
            },
        [XQ_CANNON] =
            {
                {-4, -2, 0, 2, 4, 2, 0, -2, -4},
                {0, 2, 4, 6, 8, 6, 4, 2, 0},
                {2, 4, 8, 10, 14, 10, 8, 4, 2},
                {2, 4, 6, 10, 12, 10, 6, 4, 2},
                {0, 2, 4, 8, 10, 8, 4, 2, 0},
                {0, 2, 4, 6, 8, 6, 4, 2, 0},
                {-2, 0, 2, 4, 6, 4, 2, 0, -2},
                {-2, 0, 2, 4, 6, 4, 2, 0, -2},
                {-4, -2, 0, 2, 4, 2, 0, -2, -4},
                {-8, -4, 0, 2, 4, 2, 0, -4, -8},
            },
        [XQ_PAWN] =
            {
                {0, 0, 0, 0, 0, 0, 0, 0, 0},
                {0, 0, 0, 0, 0, 0, 0, 0, 0},
                {0, 0, 0, 0, 0, 0, 0, 0, 0},
                {0, 0, 0, 0, 0, 0, 0, 0, 0},
                {4, 6, 8, 10, 12, 10, 8, 6, 4},
                {18, 22, 26, 30, 32, 30, 26, 22, 18},
                {24, 28, 36, 40, 44, 40, 36, 28, 24},
                {28, 32, 40, 46, 50, 46, 40, 32, 28},
                {24, 28, 36, 42, 46, 42, 36, 28, 24},
                {8, 12, 18, 22, 26, 22, 18, 12, 8},
            },
};

enum
{
    CAPTURE_ORDER_BASE = 100000
};

/**
 * @brief A move with a context-dependent ordering score.
 *
 * Higher scores are searched first. Scores are ordering priorities, not static evaluations or
 * search results.
 */
typedef struct ScoredMove
{
    XqMove move;
    int    score;
} ScoredMove;

/**
 * @brief Sums the material values of all publicly unrevealed identities for one side.
 *
 * The pool also includes captured identities that were never revealed. Those identities participate
 * in the average but are not counted as live material by `evaluate_side()`.
 */
static int64_t pool_material_value(const XqJqPosition *pos,
                                   XqColor             color)
{
    int64_t value = 0;

    for (int type = 0; type < XQ_PIECE_TYPE_NB; ++type)
        value += (int64_t)pos->pool[color][type] * piece_values[type];

    return value;
}

/**
 * @brief Sums material and piece-square bonuses for one side without modifying the position.
 *
 * Black's ranks are reflected so both sides use the same tables relative to their own home rank.
 * Dark pieces have no positional bonus and share their side's public pool average. Their combined
 * contribution is divided once, avoiding repeated rounding of the same fractional average.
 *
 * @param pos   Valid public position; must not be `NULL` and is left unchanged.
 * @param color Side whose pieces are evaluated; must be `XQ_RED` or `XQ_BLACK`.
 * @return      Total material value plus revealed-piece square bonuses for the specified side,
 *              without subtracting the opponent's score.
 */
static int evaluate_side(const XqJqPosition *pos,
                         XqColor             color)
{
    int score      = 0;
    int dark_count = 0;

    for (int type = 0; type < XQ_PIECE_TYPE_NB; ++type)
    {
        XqBitboard remaining = pos->pieces[color][type];

        while (!xq_bb_is_empty(remaining))
        {
            XqSquare sq   = xq_bb_first_square(remaining);
            int      rank = xq_square_rank(sq);
            int      file = xq_square_file(sq);

            xq_bb_clear(&remaining, sq);
            if (xq_jq_position_is_dark(pos, sq))
            {
                ++dark_count;
                continue;
            }
            if (color == XQ_BLACK)
                rank = XQ_RANKS - 1 - rank;
            score += piece_values[type] + piece_square_values[type][rank][file];
        }
    }

    if (dark_count > 0 && pos->pool_total[color] > 0)
        score +=
            (int)((int64_t)dark_count * pool_material_value(pos, color) / pos->pool_total[color]);

    return score;
}

/**
 * @brief Evaluates a position from the requested side's perspective using the built-in evaluator.
 *
 * Revealed pieces use fixed material and piece-square values. Dark pieces use the public pool's
 * average material value without a positional bonus. Positive scores favor `perspective`.
 *
 * @param pos         Valid public position; must not be `NULL` and is left unchanged.
 * @param perspective Side for which a positive score is favorable.
 * @param user        Unused user-data pointer, accepted for compatibility with `XqJqEvaluateFn`.
 * @return            Static position score from `perspective`'s point of view.
 */
int xq_jq_engine_default_static_evaluate(const XqJqPosition *pos,
                                         XqColor             perspective,
                                         void               *user)
{
    XqColor opponent = xq_color_opponent(perspective);

    (void)user;
    int score = evaluate_side(pos, perspective) - evaluate_side(pos, opponent);

    return score;
}

/**
 * @brief Returns a piece's material value before a move, using the pool average for dark pieces.
 *
 * Valid positions with live dark pieces have a nonempty pool. The zero-denominator guard avoids
 * dividing by zero if an invalid setup is supplied before validation.
 */
static int move_piece_value(const XqJqPosition *pos,
                            XqSquare            sq,
                            int                 piece)
{
    if (xq_jq_position_is_dark(pos, sq))
    {
        XqColor color = xq_piece_color(piece);

        if (pos->pool_total[color] == 0)
            return 0;
        return (int)(pool_material_value(pos, color) / pos->pool_total[color]);
    }

    return piece_values[xq_piece_type(piece)];
}

/**
 * @brief Computes a move-ordering score using the built-in scoring function.
 *
 * Quiet moves receive a score of `0`. Captures use the following MVV-LVA-style formula:
 *
 * `CAPTURE_ORDER_BASE + captured_piece_value * 16 - moving_piece_value`
 *
 * A dark piece's value comes from its side's public pool average rather than its initial type. The
 * factor `16` is an empirical weight and does not guarantee strict lexicographic MVV-LVA order.
 * This score is an ordering priority; it does not evaluate the resulting position.
 *
 * @param pos  Valid public position before the move; must not be `NULL` and is left unchanged.
 * @param move Move for which to compute an ordering score, including current piece metadata.
 * @param user Unused user-data pointer, accepted for compatibility with `XqJqMoveScoreFn`.
 * @return     Ordering score; higher values indicate higher ordering priority.
 */
int xq_jq_engine_default_move_order_score(const XqJqPosition *pos,
                                          XqMove              move,
                                          void               *user)
{
    (void)user;

    if (move.captured == XQ_EMPTY_PIECE)
        return 0;

    return CAPTURE_ORDER_BASE + move_piece_value(pos, (XqSquare)move.to, move.captured) * 16 -
           move_piece_value(pos, (XqSquare)move.from, move.piece);
}

/**
 * @brief Sorts a move list by descending move-ordering score.
 *
 * Scores are computed once per move, and stable insertion sort preserves the relative order of
 * moves with equal scores.
 *
 * @param pos        Valid public position in which the moves are being ordered; must not be `NULL`.
 * @param list       Move list to reorder in place; must not be `NULL`.
 * @param score_move Optional custom scorer; `NULL` selects the built-in scoring function.
 * @param user       User data borrowed by the custom scorer for the duration of this call.
 */
void xq_jq_engine_order_moves(const XqJqPosition *pos,
                              XqJqMoveList       *list,
                              XqJqMoveScoreFn     score_move,
                              void               *user)
{
    ScoredMove ordered[XQ_JQ_MAX_MOVES];

    for (int i = 0; i < list->count; ++i)
    {
        ScoredMove current;
        int        j = i;

        current.move  = list->moves[i];
        current.score = score_move != NULL
                            ? score_move(pos, current.move, user)
                            : xq_jq_engine_default_move_order_score(pos, current.move, NULL);
        while (j > 0 && ordered[j - 1].score < current.score)
        {
            ordered[j] = ordered[j - 1];
            --j;
        }
        ordered[j] = current;
    }

    for (int i = 0; i < list->count; ++i)
        list->moves[i] = ordered[i].move;
}
