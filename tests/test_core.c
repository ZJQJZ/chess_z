#include "xiangqi/engine.h"
#include "xiangqi/movegen.h"
#include "xiangqi/position.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_startpos(void) {
    XqPosition pos;
    XqMoveList legal;
    char fen[128];

    xq_position_startpos(&pos);
    assert(xq_position_validate(&pos));
    assert(xq_position_piece_count(&pos, XQ_RED, XQ_KING) == 1);
    assert(xq_position_piece_count(&pos, XQ_BLACK, XQ_KING) == 1);
    assert(xq_position_piece_count(&pos, XQ_RED, XQ_ROOK) == 2);
    assert(xq_position_piece_count(&pos, XQ_BLACK, XQ_CANNON) == 2);

    xq_generate_legal(&pos, &legal);
    assert(legal.count == 44);
    assert(xq_perft(&pos, 1) == 44u);

    assert(xq_position_to_fen(&pos, fen, sizeof(fen)));
    assert(strcmp(fen, "rnbakabnr/9/1c5c1/p1p1p1p1p/9/9/P1P1P1P1P/1C5C1/9/RNBAKABNR r - -") == 0);
}

static void test_validate_rejects_extra_piece_bits(void) {
    XqPosition pos;

    xq_position_startpos(&pos);
    xq_bb_set(&pos.pieces[XQ_RED][XQ_ROOK], xq_square_make(4, 4));
    assert(!xq_position_validate(&pos));
}

static void test_flying_king_check(void) {
    XqPosition pos;
    XqMoveList legal;

    assert(xq_position_from_fen(&pos, "4k4/9/9/9/9/9/9/9/9/4K4 r - - 0 1"));
    assert(xq_position_in_check(&pos, XQ_RED));
    assert(xq_position_in_check(&pos, XQ_BLACK));

    xq_generate_legal(&pos, &legal);
    assert(legal.count > 0);
}

static void test_engine_adapter(void) {
    XqPosition pos;
    XqMove best;

    xq_position_startpos(&pos);
    assert(xq_engine_find_best_move(NULL, &pos, 1, &best));
    assert(best.from < XQ_SQUARES);
    assert(best.to < XQ_SQUARES);
}

int main(void) {
    test_startpos();
    test_validate_rejects_extra_piece_bits();
    test_flying_king_check();
    test_engine_adapter();
    printf("xiangqi core tests passed\n");
    return 0;
}
