#include "movegen_internal_jq.h"

#include <stdio.h>
#include <stdlib.h>

#define CHECK(value) do { if (!(value)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #value); abort(); \
} } while (0)

static void check_move_list(const XqJqMoveList *cached, const XqJqMoveList *expected,
                            const XqJqPosition *pos)
{
    int i = 0;
    while (i < cached->count && i < expected->count)
    {
        XqMove a = cached->moves[i];
        XqMove b = expected->moves[i];
        if (a.from != b.from || a.to != b.to || a.piece != b.piece || a.captured != b.captured)
            break;
        ++i;
    }
    if (i != cached->count || i != expected->count)
    {
        char fen[256] = {0};
        (void)xq_jq_position_to_fen(pos, fen, sizeof(fen));
        fprintf(stderr, "move %d differs (cached %d, expected %d) in %s\n", i,
                cached->count, expected->count, fen);
        abort();
    }
}

static int check_moves(XqJqMoveCache *cache, const XqJqPosition *pos)
{
    XqJqMoveList full;
    XqJqMoveList captures = {.count = 0};
    XqJqMoveList cached;
    xq_jq_generate_pseudo_legal(pos, &full);
    int opposing_king = xq_make_piece(xq_color_opponent(pos->side_to_move), XQ_KING);
    bool king_capture = false;
    for (int i = 0; i < full.count; ++i)
    {
        XqMove move = full.moves[i];
        if (move.captured != XQ_EMPTY_PIECE)
        {
            captures.moves[captures.count++] = move;
            king_capture |= move.captured == opposing_king;
        }
    }

    XqJqMoveCacheSummary summary = xq_jq_move_cache_refresh(cache, pos);
    CHECK(summary.count == full.count);
    CHECK(summary.king_capture == king_capture);
    if (!king_capture)
        CHECK(summary.in_check == xq_jq_position_in_check(pos, pos->side_to_move));
    xq_jq_move_cache_collect(cache, pos, &cached, false);
    check_move_list(&cached, &full, pos);
    xq_jq_move_cache_collect(cache, pos, &cached, true);
    check_move_list(&cached, &captures, pos);
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

static bool row_depends_on(const XqJqCachedPieceMoves *row, int file, int rank)
{
    return xq_bb_test(row->dependencies, xq_square_make(file, rank));
}

static void rook_ray_dependencies(void)
{
    XqJqMoveCache cache = {0};
    XqJqPosition pos;
    CHECK(xq_jq_position_from_fen(&pos, "3k5/9/1p*7/1R*2P4/9/9/9/9/9/4K4 r - - 0 1"));
    check_both_sides(&cache, &pos);

    XqSquare from = xq_square_make(1, 6);
    XqSquare blocker = xq_square_make(1, 7);
    const XqJqCachedPieceMoves *row = &cache.rows[from];
    CHECK(row_depends_on(row, 1, 6));
    CHECK(row_depends_on(row, 1, 7));
    CHECK(row_depends_on(row, 4, 6));
    CHECK(row_depends_on(row, 1, 0));
    CHECK(!row_depends_on(row, 1, 8));
    CHECK(!row_depends_on(row, 7, 6));

    /* Changes beyond either blocker leave the rook's dependencies unchanged. */
    XqBitboard before = row->dependencies;
    edit_square(&cache, &pos, 1, 8, xq_make_piece(XQ_BLACK, XQ_KNIGHT));
    edit_square(&cache, &pos, 7, 6, xq_make_piece(XQ_BLACK, XQ_PAWN));
    CHECK(row->dependencies.lo == before.lo && row->dependencies.hi == before.hi);

    edit_square(&cache, &pos, 4, 6, XQ_EMPTY_PIECE);
    CHECK(row_depends_on(row, 7, 6));
    CHECK(!row_depends_on(row, 8, 6));
    edit_square(&cache, &pos, 7, 6, xq_make_piece(XQ_RED, XQ_PAWN));

    XqMove capture = {(uint8_t)from, (uint8_t)blocker, pos.board[from], pos.board[blocker]};
    XqJqUndo undo;
    CHECK(xq_jq_position_save_undo(&pos, capture, XQ_ROOK, &undo));
    CHECK(xq_jq_position_make_move(&pos, capture, XQ_ROOK));
    xq_jq_move_cache_invalidate(&cache, from, blocker);
    check_both_sides(&cache, &pos);
    CHECK(row_depends_on(&cache.rows[blocker], 1, 8));
    CHECK(xq_jq_position_unmake_move(&pos, &undo));
    xq_jq_move_cache_invalidate(&cache, from, blocker);
    check_both_sides(&cache, &pos);
    CHECK(!row_depends_on(row, 1, 8));

    edit_square(&cache, &pos, 1, 6, xq_make_piece(XQ_RED, XQ_CANNON));
    CHECK(row_depends_on(row, 1, 8));
}

static void cannon_ray_dependencies(void)
{
    XqJqMoveCache cache = {0};
    XqJqPosition pos;
    CHECK(xq_jq_position_from_fen(&pos, "3k5/9/2P2C*3/5P3/9/5p3/9/9/9/4K4 r - - 0 1"));
    check_both_sides(&cache, &pos);

    const XqJqCachedPieceMoves *row = &cache.rows[xq_square_make(5, 7)];
    CHECK(row_depends_on(row, 5, 7));
    CHECK(row_depends_on(row, 5, 6));
    CHECK(row_depends_on(row, 5, 5));
    CHECK(row_depends_on(row, 5, 4));
    CHECK(!row_depends_on(row, 5, 3));
    CHECK(row_depends_on(row, 0, 7));

    edit_square(&cache, &pos, 5, 2, xq_make_piece(XQ_BLACK, XQ_ROOK));
    CHECK(!row_depends_on(row, 5, 2));
    edit_square(&cache, &pos, 5, 4, xq_make_piece(XQ_RED, XQ_PAWN));
    edit_square(&cache, &pos, 5, 6, XQ_EMPTY_PIECE);
    CHECK(row_depends_on(row, 5, 2));
    CHECK(!row_depends_on(row, 5, 1));

    edit_square(&cache, &pos, 5, 3, xq_make_piece(XQ_RED, XQ_PAWN));
    CHECK(row_depends_on(row, 5, 3));
    CHECK(!row_depends_on(row, 5, 2));
}

static void king_check_dependencies(void)
{
    XqJqMoveCache cache = {0};
    XqJqPosition pos;
    CHECK(xq_jq_position_from_fen(&pos, "3k5/9/9/9/9/9/9/2b6/3P5/4K4 r - - 0 1"));
    check_both_sides(&cache, &pos);

    const XqJqCachedKingCheck *check = &cache.king_check[XQ_RED];
    CHECK(check->valid && !check->in_check);
    CHECK(xq_bb_test(check->dependencies, xq_square_make(2, 2)));
    CHECK(xq_bb_test(check->dependencies, xq_square_make(3, 1)));
    CHECK(!xq_bb_test(check->dependencies, xq_square_make(3, 8)));

    /* The bishop attacks across the river once its eye is empty. */
    edit_square(&cache, &pos, 3, 1, XQ_EMPTY_PIECE);
    CHECK(check->valid && check->in_check);

    /* A king capture skips the check, even if an earlier edit touched its dependencies. */
    edit_square(&cache, &pos, 3, 8, xq_make_piece(XQ_RED, XQ_ROOK));
    edit_square(&cache, &pos, 3, 1, xq_make_piece(XQ_RED, XQ_PAWN));
    CHECK(!check->valid);
    edit_square(&cache, &pos, 3, 8, XQ_EMPTY_PIECE);
    CHECK(check->valid && !check->in_check);

    xq_jq_move_cache_reset(&cache);
    CHECK(!check->valid && !cache.king_check[XQ_BLACK].valid);
    check_both_sides(&cache, &pos);
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
    rook_ray_dependencies();
    cannon_ray_dependencies();
    king_check_dependencies();
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
