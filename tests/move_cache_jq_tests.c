#include "movegen_internal_jq.h"

#include <stdio.h>
#include <stdlib.h>

#define CHECK(value) do { if (!(value)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #value); abort(); \
} } while (0)

static int check_moves(XqJqMoveCache *cache, const XqJqPosition *pos)
{
    XqJqMoveList cached;
    XqJqMoveList full;
    xq_jq_move_cache_generate(cache, pos, &cached);
    xq_jq_generate_pseudo_legal(pos, &full);
    int i = 0;
    while (i < cached.count && i < full.count)
    {
        XqMove a = cached.moves[i];
        XqMove b = full.moves[i];
        if (a.from != b.from || a.to != b.to || a.piece != b.piece || a.captured != b.captured)
            break;
        ++i;
    }
    if (i != cached.count || i != full.count)
    {
        char fen[256] = {0};
        (void)xq_jq_position_to_fen(pos, fen, sizeof(fen));
        fprintf(stderr, "move %d differs (cached %d, full %d) in %s\n", i,
                cached.count, full.count, fen);
        abort();
    }
    return full.count;
}

static void check_both_sides(XqJqMoveCache *cache, const XqJqPosition *pos)
{
    XqJqPosition opposite = *pos;
    opposite.side_to_move = xq_color_opponent(pos->side_to_move);
    (void)check_moves(cache, pos);
    (void)check_moves(cache, &opposite);
}

static void edit_square(XqJqMoveCache *cache, XqJqPosition *pos, int file, int rank, int piece)
{
    XqSquare square = xq_square_make(file, rank);
    if (piece == XQ_EMPTY_PIECE)
        CHECK(xq_jq_position_remove_piece(pos, square));
    else
        CHECK(xq_jq_position_set_piece(pos, square, xq_piece_color(piece), xq_piece_type(piece)));
    xq_jq_move_cache_invalidate(cache, square, square);
    check_both_sides(cache, pos);
}

static void piece_dependencies(void)
{
    XqJqMoveCache cache = {0};
    XqJqPosition pos;
    CHECK(xq_jq_position_from_fen(&pos, "4k4/9/n8/3R5/r8/9/9/P1BN5/9/C3K4 r - - 0 1"));
    check_both_sides(&cache, &pos);

    /* The cannon depends on both the screen and the type occupying its target. */
    edit_square(&cache, &pos, 0, 5, xq_make_piece(XQ_BLACK, XQ_BISHOP));
    edit_square(&cache, &pos, 0, 2, XQ_EMPTY_PIECE);
    edit_square(&cache, &pos, 0, 2, xq_make_piece(XQ_BLACK, XQ_PAWN));

    /* The same blocker covers a knight leg and a bishop eye. */
    edit_square(&cache, &pos, 3, 3, xq_make_piece(XQ_RED, XQ_PAWN));
    edit_square(&cache, &pos, 3, 3, XQ_EMPTY_PIECE);

    edit_square(&cache, &pos, 4, 6, xq_make_piece(XQ_BLACK, XQ_PAWN));
    edit_square(&cache, &pos, 4, 6, XQ_EMPTY_PIECE);

    /* Reveal rules allow an advisor outside the palace and a bishop across the river. */
    CHECK(xq_jq_position_from_fen(&pos, "4k4/9/9/9/9/2A6/1B7/9/9/4K4 r - - 0 1"));
    xq_jq_move_cache_reset(&cache);
    check_both_sides(&cache, &pos);
    edit_square(&cache, &pos, 3, 5, xq_make_piece(XQ_BLACK, XQ_PAWN));
    edit_square(&cache, &pos, 2, 4, XQ_EMPTY_PIECE);
    edit_square(&cache, &pos, 3, 5, xq_make_piece(XQ_RED, XQ_PAWN));
}

static void reveal_capture_and_undo(void)
{
    XqJqMoveCache cache = {0};
    XqJqPosition pos;
    CHECK(xq_jq_position_from_fen(&pos, "4k4/9/9/9/9/p*8/9/1N*7/9/R*3K4 r - - 0 1"));
    check_both_sides(&cache, &pos);
    XqSquare from = xq_square_make(1, 2);
    XqSquare to = xq_square_make(0, 4);
    XqMove move = {(uint8_t)from, (uint8_t)to, pos.board[from], pos.board[to]};
    const XqPieceType reveals[] = {XQ_ROOK, XQ_KNIGHT};
    for (unsigned i = 0; i < sizeof(reveals) / sizeof(reveals[0]); ++i)
    {
        XqJqUndo undo;
        CHECK(xq_jq_position_save_undo(&pos, move, reveals[i], &undo));
        CHECK(xq_jq_position_make_move(&pos, move, reveals[i]));
        CHECK(pos.board[to] == xq_make_piece(XQ_RED, reveals[i]));
        CHECK(!xq_jq_position_is_dark(&pos, to));
        xq_jq_move_cache_invalidate(&cache, from, to);
        check_both_sides(&cache, &pos);
        CHECK(xq_jq_position_unmake_move(&pos, &undo));
        CHECK(xq_jq_position_is_dark(&pos, from) && xq_jq_position_is_dark(&pos, to));
        xq_jq_move_cache_invalidate(&cache, from, to);
        check_both_sides(&cache, &pos);
    }
}

static void large_move_list(void)
{
    XqJqMoveCache cache = {0};
    XqJqPosition pos;
    CHECK(xq_jq_position_from_fen(&pos, "kR3C1Rn/R1N1N1N1R/1N1N1N1N1/2N1N1N2/1N1N1N1N1/"
                                     "2N1N1N2/1N1N1N1N1/2N1N1N2/1C1N1N1N1/C2K4C r - - 0 1"));
    check_both_sides(&cache, &pos);
    /* Removing the blocker opens enough moves to exceed the Xiangqi list capacity. */
    edit_square(&cache, &pos, 3, 0, XQ_EMPTY_PIECE);
    CHECK(check_moves(&cache, &pos) > XQ_MAX_MOVES);
    edit_square(&cache, &pos, 0, 0, XQ_EMPTY_PIECE);
}

static void test_branches(XqJqMoveCache *cache, XqJqPosition *pos, unsigned depth, uint64_t *random)
{
    (void)check_moves(cache, pos);
    if (depth == 0)
        return;

    XqJqMoveList legal;
    xq_jq_generate_legal(pos, &legal);
    unsigned branches = legal.count < 2 ? (unsigned)legal.count : 2u;
    for (unsigned branch = 0; branch < branches; ++branch)
    {
        XqMove move = legal.moves[xq_jq_random_next(random) % (unsigned)legal.count];
        XqSquare from = (XqSquare)move.from;
        XqSquare to = (XqSquare)move.to;
        int reveal = XQ_JQ_NO_REVEAL;
        if (xq_jq_position_is_dark(pos, from))
        {
            XqPieceType type;
            CHECK(xq_jq_position_sample_type(pos, pos->side_to_move, random, &type));
            reveal = (int)type;
        }
        XqJqUndo undo;
        CHECK(xq_jq_position_save_undo(pos, move, reveal, &undo));
        CHECK(xq_jq_position_make_move(pos, move, reveal));
        xq_jq_move_cache_invalidate(cache, from, to);
        test_branches(cache, pos, depth - 1, random);
        CHECK(xq_jq_position_unmake_move(pos, &undo));
        xq_jq_move_cache_invalidate(cache, from, to);
    }
    check_both_sides(cache, pos);
}

int main(void)
{
    piece_dependencies();
    reveal_capture_and_undo();
    large_move_list();
    XqJqMoveCache cache = {0};
    XqJqPosition pos;
    xq_jq_position_startpos(&pos);
    uint64_t random = UINT64_C(20261120);
    test_branches(&cache, &pos, 10, &random);
    puts("Jieqi move cache checks passed");
    return 0;
}
