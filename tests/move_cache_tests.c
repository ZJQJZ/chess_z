#include "movegen_internal.h"
#include "xiangqi/movegen.h"

#include <stdio.h>
#include <stdlib.h>

/* Keep the comparison enabled in Release builds. */
#define CHECK(value) do { if (!(value)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #value); abort(); \
} } while (0)

static void check_move_list(const XqMoveList *cached, const XqMoveList *expected,
                            const XqPosition *pos)
{
    int i = 0;
    while (i < cached->count && i < expected->count)
    {
        const XqMove a = cached->moves[i];
        const XqMove b = expected->moves[i];
        if (a.from != b.from || a.to != b.to || a.piece != b.piece || a.captured != b.captured)
            break;
        ++i;
    }
    if (i != cached->count || i != expected->count)
    {
        char fen[256] = {0};
        (void)xq_position_to_fen(pos, fen, sizeof(fen));
        fprintf(stderr, "move %d differs (cached count %d, expected count %d) in %s\n", i,
                cached->count, expected->count, fen);
        abort();
    }
}

static void check_moves(XqMoveCache *cache, const XqPosition *pos)
{
    XqMoveList full;
    XqMoveList captures;
    XqMoveList cached;
    xq_generate_pseudo_legal(pos, &full);

    int enemy_king = xq_make_piece(xq_color_opponent(pos->side_to_move), XQ_KING);
    bool king_capture = false;
    captures.count = 0;
    for (int i = 0; i < full.count; ++i)
    {
        XqMove move = full.moves[i];
        if (move.captured == XQ_EMPTY_PIECE)
            continue;
        captures.moves[captures.count++] = move;
        king_capture |= move.captured == enemy_king;
    }

    XqMoveCacheSummary summary = xq_move_cache_refresh(cache, pos);
    CHECK(summary.count == full.count);
    CHECK(summary.king_capture == king_capture);

    xq_move_cache_collect(cache, pos, &cached, false);
    check_move_list(&cached, &full, pos);
    xq_move_cache_collect(cache, pos, &cached, true);
    check_move_list(&cached, &captures, pos);
}

static void check_both_sides(XqMoveCache *cache, const XqPosition *pos)
{
    XqPosition opposite = *pos;
    opposite.side_to_move = xq_color_opponent(pos->side_to_move);
    check_moves(cache, pos);
    check_moves(cache, &opposite);
}

static void edit_square(XqMoveCache *cache, XqPosition *pos, int file, int rank, int piece)
{
    XqSquare square = xq_square_make(file, rank);
    if (piece == XQ_EMPTY_PIECE)
        CHECK(xq_position_remove_piece(pos, square));
    else
        CHECK(xq_position_set_piece(pos, square, xq_piece_color(piece), xq_piece_type(piece)));
    xq_move_cache_invalidate(cache, square, square);
    check_both_sides(cache, pos);
}

static void test_piece_dependencies(void)
{
    XqMoveCache cache = {0};
    XqPosition pos;
    CHECK(xq_position_from_fen(&pos, "4k4/9/n8/3R5/r8/9/9/P1BN5/9/C3K4 w - - 0 1"));
    check_both_sides(&cache, &pos);

    /* Replace the cannon's target without changing occupancy, then change the screen. */
    edit_square(&cache, &pos, 0, 5, xq_make_piece(XQ_BLACK, XQ_BISHOP));
    edit_square(&cache, &pos, 0, 2, XQ_EMPTY_PIECE);
    edit_square(&cache, &pos, 0, 2, xq_make_piece(XQ_BLACK, XQ_PAWN));

    /* Block and unblock a knight leg and a bishop eye with the same square. */
    edit_square(&cache, &pos, 3, 3, xq_make_piece(XQ_RED, XQ_PAWN));
    edit_square(&cache, &pos, 3, 3, XQ_EMPTY_PIECE);

    /* A blocker on the kings' file also changes the neighboring rook's ray. */
    edit_square(&cache, &pos, 4, 6, xq_make_piece(XQ_BLACK, XQ_PAWN));
    edit_square(&cache, &pos, 4, 6, XQ_EMPTY_PIECE);

    /* A worker can reuse its cache storage for a different subtree. */
    xq_move_cache_reset(&cache);
    xq_position_startpos(&pos);
    check_both_sides(&cache, &pos);
}

static void test_output_capacity(void)
{
    XqMoveCache cache = {0};
    XqPosition pos;
    CHECK(xq_position_from_fen(&pos, "kR3C1Rn/R1N1N1N1R/1N1N1N1N1/2N1N1N2/1N1N1N1N1/"
                                     "2N1N1N2/1N1N1N1N1/2N1N1N2/1C1N1N1N1/C2K4C r - -"));
    check_both_sides(&cache, &pos);
    edit_square(&cache, &pos, 3, 0, XQ_EMPTY_PIECE);
    XqMoveList full;
    xq_generate_pseudo_legal(&pos, &full);
    CHECK(full.count == XQ_MAX_MOVES);
    /* Dropping an early row makes a previously truncated move visible. */
    edit_square(&cache, &pos, 0, 0, XQ_EMPTY_PIECE);
    xq_generate_pseudo_legal(&pos, &full);
    CHECK(full.count < XQ_MAX_MOVES);
}

static void test_branches(XqMoveCache *cache, XqPosition *pos, unsigned depth, uint32_t *random)
{
    check_moves(cache, pos);
    if (depth == 0)
        return;
    XqMoveList legal;
    xq_generate_legal(pos, &legal);
    unsigned branches = legal.count < 2 ? (unsigned)legal.count : 2u;
    for (unsigned branch = 0; branch < branches; ++branch)
    {
        *random = *random * UINT32_C(1664525) + UINT32_C(1013904223);
        XqMove move = legal.moves[(*random >> 16) % (unsigned)legal.count];
        CHECK(xq_position_make_move(pos, move));
        xq_move_cache_invalidate(cache, (XqSquare)move.from, (XqSquare)move.to);
        test_branches(cache, pos, depth - 1, random);
        CHECK(xq_position_unmake_move(pos, move));
        xq_move_cache_invalidate(cache, (XqSquare)move.from, (XqSquare)move.to);
    }
    check_both_sides(cache, pos);
}

int main(void)
{
    test_piece_dependencies();
    test_output_capacity();
    XqMoveCache cache = {0};
    XqPosition pos;
    xq_position_startpos(&pos);
    uint32_t random = UINT32_C(20261120);
    test_branches(&cache, &pos, 10, &random);
    return 0;
}
