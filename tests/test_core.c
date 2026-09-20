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

static void test_direct_attack_detection(void) {
    XqPosition pos;

    assert(xq_position_from_fen(&pos, "4k4/9/9/9/4r4/9/9/9/9/4K4 r - -"));
    assert(xq_square_attacked(&pos, xq_square_make(4, 0), XQ_BLACK));

    assert(xq_position_from_fen(&pos, "4k4/9/9/9/4c4/9/4P4/9/9/4K4 r - -"));
    assert(xq_square_attacked(&pos, xq_square_make(4, 0), XQ_BLACK));
    assert(xq_position_set_piece(&pos, xq_square_make(4, 2), XQ_RED, XQ_PAWN));
    assert(!xq_square_attacked(&pos, xq_square_make(4, 0), XQ_BLACK));

    assert(xq_position_from_fen(&pos, "4k4/9/9/9/4P4/9/9/3n5/9/4K4 r - -"));
    assert(xq_square_attacked(&pos, xq_square_make(4, 0), XQ_BLACK));
    assert(xq_position_set_piece(&pos, xq_square_make(3, 1), XQ_RED, XQ_PAWN));
    assert(!xq_square_attacked(&pos, xq_square_make(4, 0), XQ_BLACK));

    assert(xq_position_from_fen(&pos, "4k4/9/9/9/9/9/9/9/4p4/4K4 r - -"));
    assert(xq_square_attacked(&pos, xq_square_make(4, 0), XQ_BLACK));
}

static bool pseudo_list_attacks(const XqPosition *pos, XqSquare sq, XqColor color) {
    XqPosition tmp = *pos;
    XqMoveList pseudo;
    int i;

    tmp.side_to_move = color;
    xq_generate_pseudo_legal(&tmp, &pseudo);
    for (i = 0; i < pseudo.count; ++i)
        if ((XqSquare)pseudo.moves[i].to == sq)
            return true;
    return false;
}

static void test_direct_attack_matches_pseudo_generation(void) {
    static const char *fens[] = {
        "rnbakabnr/9/1c5c1/p1p1p1p1p/9/9/P1P1P1P1P/1C5C1/9/RNBAKABNR r - -",
        "4k4/9/9/9/4c4/9/4P4/9/9/4K4 r - -",
        "4k4/9/9/9/4P4/9/9/3n5/9/4K4 r - -",
        "3aka3/9/2b3b2/9/4p4/4P4/9/2B3B2/9/3AKA3 r - -",
    };
    XqPosition pos;
    size_t fen_index;
    int color;
    int sq;

    for (fen_index = 0; fen_index < sizeof(fens) / sizeof(fens[0]); ++fen_index) {
        assert(xq_position_from_fen(&pos, fens[fen_index]));
        for (color = 0; color < XQ_COLOR_NB; ++color)
            for (sq = 0; sq < XQ_SQUARES; ++sq)
            {
                int target = pos.board[sq];
                bool direct = xq_square_attacked(&pos, (XqSquare)sq, (XqColor)color);
                bool generated = target != XQ_EMPTY_PIECE &&
                                 xq_piece_color(target) != (XqColor)color &&
                                 pseudo_list_attacks(&pos, (XqSquare)sq, (XqColor)color);
                if (direct != generated)
                    fprintf(stderr, "attack mismatch: fen=%zu color=%d sq=%d direct=%d generated=%d\n",
                            fen_index, color, sq, direct, generated);
                assert(direct == generated);
            }
    }
}

static void test_unmake_move_restores_position(void) {
    XqPosition pos;
    XqPosition before;
    XqMoveList legal;
    int i;

    xq_position_startpos(&pos);
    before = pos;
    xq_generate_legal(&pos, &legal);
    assert(legal.count > 0);
    assert(xq_position_make_move(&pos, legal.moves[0]));
    assert(xq_position_unmake_move(&pos, legal.moves[0]));
    assert(memcmp(&pos, &before, sizeof(pos)) == 0);

    assert(xq_position_from_fen(&pos, "4k4/9/9/9/9/9/9/9/r8/R3K4 r - -"));
    before = pos;
    xq_generate_legal(&pos, &legal);
    for (i = 0; i < legal.count; ++i)
        if (legal.moves[i].captured != XQ_EMPTY_PIECE)
            break;
    assert(i < legal.count);
    assert(xq_position_make_move(&pos, legal.moves[i]));
    assert(xq_position_unmake_move(&pos, legal.moves[i]));
    assert(memcmp(&pos, &before, sizeof(pos)) == 0);
}

static void test_engine_adapter(void) {
    XqPosition pos;
    XqMove best;

    xq_position_startpos(&pos);
    assert(xq_engine_find_best_move(NULL, &pos, NULL, &best));
    assert(best.from < XQ_SQUARES);
    assert(best.to < XQ_SQUARES);
}

static void test_default_static_evaluate(void) {
    XqPosition pos;
    XqPosition before;

    xq_position_startpos(&pos);
    assert(xq_engine_default_static_evaluate(&pos, XQ_RED, NULL) == 0);
    assert(xq_engine_default_static_evaluate(&pos, XQ_BLACK, NULL) == 0);

    assert(xq_position_from_fen(&pos, "3k5/9/9/9/9/9/9/9/9/R3K4 b - -"));
    before = pos;
    assert(xq_engine_default_static_evaluate(&pos, XQ_RED, NULL) == 600);
    assert(xq_engine_default_static_evaluate(&pos, XQ_BLACK, NULL) == -600);
    assert(memcmp(&pos, &before, sizeof(pos)) == 0);
}

static int constant_evaluate(const XqPosition *pos, XqColor perspective, void *user) {
    (void)pos;
    (void)perspective;
    (void)user;
    return 0;
}

static int prefer_a0a1(const XqPosition *pos, XqMove move, void *user) {
    int *calls = user;

    (void)pos;
    ++*calls;
    return move.from == (uint8_t)xq_square_make(0, 0) &&
           move.to == (uint8_t)xq_square_make(0, 1)
               ? 1
               : 0;
}

static void test_default_move_ordering(void) {
    XqPosition pos;
    XqMove best;
    XqEngineAdapter engine = {
        .static_evaluate = constant_evaluate,
        .search = NULL,
        .user = NULL,
        .score_move = NULL,
    };

    xq_position_startpos(&pos);
    assert(xq_engine_find_best_move(&engine, &pos, NULL, &best));
    assert(best.captured != XQ_EMPTY_PIECE);
}

static void test_quiescence_avoids_bad_capture(void) {
    XqPosition pos;
    XqMove best;

    assert(xq_position_from_fen(&pos, "4k4/9/9/9/4p4/9/9/r8/c8/R3K4 r - -"));
    assert(xq_engine_find_best_move(NULL, &pos, NULL, &best));
    assert(best.captured == XQ_EMPTY_PIECE);
}

static void test_custom_move_ordering(void) {
    XqPosition pos;
    XqMoveList legal;
    XqMove best;
    int score_calls = 0;
    XqEngineAdapter engine = {
        .static_evaluate = constant_evaluate,
        .search = NULL,
        .user = &score_calls,
        .score_move = prefer_a0a1,
    };

    xq_position_startpos(&pos);
    xq_generate_legal(&pos, &legal);
    assert(xq_engine_find_best_move(&engine, &pos, NULL, &best));
    assert(best.from == (uint8_t)xq_square_make(0, 0));
    assert(best.to == (uint8_t)xq_square_make(0, 1));
    assert(score_calls >= legal.count);
}

static void test_explain_search_one_ply(void) {
    XqPosition pos;
    XqMove best;
    XqExplainResult result;

    /* An immediate king capture is best at both one ply and the default search depth. */
    assert(xq_position_from_fen(&pos, "4k4/9/9/9/9/9/9/9/9/4K4 r - -"));
    assert(xq_engine_explain_search_one_ply(NULL, &pos, 1, &result));
    assert(result.count > 0);
    assert(result.best_index >= 0);
    assert(result.best_index < result.count);
    assert(result.moves[result.best_index].is_best);

    assert(xq_engine_find_best_move(NULL, &pos, NULL, &best));
    assert(best.captured != XQ_EMPTY_PIECE);
    assert(xq_piece_type(best.captured) == XQ_KING);
    assert(result.moves[result.best_index].move.from == best.from);
    assert(result.moves[result.best_index].move.to == best.to);

    assert(!xq_engine_explain_search_one_ply(NULL, &pos, 1, NULL));
}

static void test_explain_custom_move_ordering(void) {
    XqPosition pos;
    XqMoveList legal;
    XqExplainResult result;
    int score_calls = 0;
    XqEngineAdapter engine = {
        .static_evaluate = constant_evaluate,
        .search = NULL,
        .user = &score_calls,
        .score_move = prefer_a0a1,
    };

    xq_position_startpos(&pos);
    xq_generate_legal(&pos, &legal);
    assert(xq_engine_explain_search_one_ply(&engine, &pos, 1, &result));
    assert(result.best_index >= 0);
    assert(result.moves[result.best_index].move.from == (uint8_t)xq_square_make(0, 0));
    assert(result.moves[result.best_index].move.to == (uint8_t)xq_square_make(0, 1));
    assert(score_calls >= legal.count);
}

static void test_explain_quiescence(void) {
    XqPosition pos;
    XqQuiescenceExplainResult quiet;
    XqQuiescenceExplainResult tactical;

    xq_position_startpos(&pos);
    assert(xq_engine_explain_quiescence_one_ply(NULL, &pos, &quiet));
    assert(quiet.stand_pat_used);
    assert(quiet.stand_pat == 0);
    assert(quiet.final_score == 0);
    assert(quiet.count > 0);

    assert(xq_position_from_fen(&pos, "4k4/9/9/9/4p4/9/9/9/r8/R3K4 r - -"));
    assert(xq_engine_explain_quiescence_one_ply(NULL, &pos, &tactical));
    assert(!tactical.in_check);
    assert(tactical.count > 0);
    assert(tactical.best_index >= 0);
    assert(tactical.best_index < tactical.count);
    assert(tactical.moves[tactical.best_index].is_best);

    assert(!xq_engine_explain_quiescence_one_ply(NULL, &pos, NULL));
}

/* Check every ordered evasion, including those in recursive quiescence nodes. */
static int check_evasion_order_score(const XqPosition *pos, XqMove move, void *user) {
    int *checked = user;

    if (xq_position_in_check(pos, pos->side_to_move)) {
        XqPosition next = *pos;
        assert(xq_position_make_move(&next, move));
        assert(!xq_position_in_check(&next, pos->side_to_move));
        ++*checked;
    }
    return xq_engine_default_move_order_score(pos, move, NULL);
}

static void test_quiescence_legal_evasions(void) {
    static const char *fens[] = {
        /* a1e1 blocks the check; a2e2 captures the checking rook. */
        "3k5/9/9/9/9/9/9/4r4/R8/4K4 r - -",
        "3k5/9/9/9/9/9/9/R3r4/9/4K4 r - -",
    };
    size_t index;
    int checked = 0;
    XqEngineAdapter engine = {
        .score_move = check_evasion_order_score,
        .user = &checked,
    };

    for (index = 0; index < sizeof(fens) / sizeof(fens[0]); ++index) {
        XqPosition pos;
        XqPosition before;
        XqMoveList legal;
        XqQuiescenceExplainResult result;
        bool found_rook_evasion = false;
        int i;

        assert(xq_position_from_fen(&pos, fens[index]));
        before = pos;
        xq_generate_legal(&pos, &legal);
        assert(legal.count > 0);
        assert(xq_engine_explain_quiescence_one_ply(&engine, &pos, &result));
        assert(result.in_check);
        assert(!result.stand_pat_used);
        assert(result.count == legal.count);
        for (i = 0; i < result.count; ++i) {
            XqPosition next = pos;
            XqMove move = result.moves[i].move;
            assert(xq_position_make_move(&next, move));
            assert(!xq_position_in_check(&next, pos.side_to_move));
            if (move.from == xq_square_make(0, (int)index + 1) &&
                move.to == xq_square_make(4, (int)index + 1))
                found_rook_evasion = true;
        }
        assert(found_rook_evasion);
        assert(memcmp(&pos, &before, sizeof(pos)) == 0);
    }
    assert(checked > 0);
}

static void test_quiescence_checkmate(void) {
    XqPosition pos;
    XqPosition before;
    XqMoveList pseudo;
    XqMoveList legal;
    XqQuiescenceExplainResult result;

    assert(xq_position_from_fen(&pos, "3k5/9/9/9/9/9/9/9/3rrr3/4K4 r - -"));
    before = pos;
    xq_generate_pseudo_legal(&pos, &pseudo);
    xq_generate_legal(&pos, &legal);
    assert(pseudo.count > 0);
    assert(legal.count == 0);
    assert(xq_engine_explain_quiescence_one_ply(NULL, &pos, &result));
    assert(result.in_check);
    assert(!result.stand_pat_used);
    assert(result.count == 0);
    assert(result.best_index == -1);
    assert(result.final_score == -30000);
    assert(memcmp(&pos, &before, sizeof(pos)) == 0);

    /* e2e1 captures a pawn and mates in the recursive quiescence search. */
    assert(xq_position_from_fen(&pos, "4k4/9/9/9/9/9/9/4r4/3rPr3/4K4 b - -"));
    before = pos;
    assert(xq_engine_explain_quiescence_one_ply(NULL, &pos, &result));
    assert(result.best_index >= 0);
    assert(result.moves[result.best_index].move.from == xq_square_make(4, 2));
    assert(result.moves[result.best_index].move.to == xq_square_make(4, 1));
    assert(result.final_score == 29999);
    assert(memcmp(&pos, &before, sizeof(pos)) == 0);
}

int main(void) {
    test_startpos();
    test_validate_rejects_extra_piece_bits();
    test_flying_king_check();
    test_direct_attack_detection();
    test_direct_attack_matches_pseudo_generation();
    test_unmake_move_restores_position();
    test_engine_adapter();
    test_default_static_evaluate();
    test_default_move_ordering();
    test_quiescence_avoids_bad_capture();
    test_custom_move_ordering();
    test_explain_search_one_ply();
    test_explain_custom_move_ordering();
    test_explain_quiescence();
    test_quiescence_legal_evasions();
    test_quiescence_checkmate();
    printf("xiangqi core tests passed\n");
    return 0;
}
