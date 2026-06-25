#include "xiangqi/movegen.h"

#include <stdio.h>

static bool is_friend(const XqPosition *pos, XqSquare sq, XqColor color) {
    int piece = pos->board[sq];
    return piece != XQ_EMPTY_PIECE && xq_piece_color(piece) == color;
}

static bool is_enemy(const XqPosition *pos, XqSquare sq, XqColor color) {
    int piece = pos->board[sq];
    return piece != XQ_EMPTY_PIECE && xq_piece_color(piece) != color;
}

static bool in_palace(XqColor color, int file, int rank) {
    if (file < 3 || file > 5) {
        return false;
    }
    return color == XQ_RED ? (rank >= 0 && rank <= 2) : (rank >= 7 && rank <= 9);
}

static bool bishop_on_own_side(XqColor color, int rank) {
    return color == XQ_RED ? rank <= 4 : rank >= 5;
}

static void add_move(const XqPosition *pos, XqMoveList *list, XqSquare from, XqSquare to) {
    int piece;
    int captured;

    if (list->count >= XQ_MAX_MOVES || to < 0 || to >= XQ_SQUARES) {
        return;
    }

    piece = pos->board[from];
    captured = pos->board[to];
    if (captured != XQ_EMPTY_PIECE && xq_piece_color(captured) == xq_piece_color(piece)) {
        return;
    }

    list->moves[list->count].from = (uint8_t)from;
    list->moves[list->count].to = (uint8_t)to;
    list->moves[list->count].piece = (int8_t)piece;
    list->moves[list->count].captured = (int8_t)captured;
    ++list->count;
}

static void add_step_if_valid(const XqPosition *pos, XqMoveList *list, XqSquare from, int file, int rank) {
    XqColor color = xq_piece_color(pos->board[from]);
    if (xq_square_is_valid(file, rank) && !is_friend(pos, xq_square_make(file, rank), color)) {
        add_move(pos, list, from, xq_square_make(file, rank));
    }
}

static void gen_rook(const XqPosition *pos, XqMoveList *list, XqSquare from) {
    static const int dirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    XqColor color = xq_piece_color(pos->board[from]);
    int i;

    for (i = 0; i < 4; ++i) {
        int file = xq_square_file(from) + dirs[i][0];
        int rank = xq_square_rank(from) + dirs[i][1];
        while (xq_square_is_valid(file, rank)) {
            XqSquare to = xq_square_make(file, rank);
            if (pos->board[to] == XQ_EMPTY_PIECE) {
                add_move(pos, list, from, to);
            } else {
                if (is_enemy(pos, to, color)) {
                    add_move(pos, list, from, to);
                }
                break;
            }
            file += dirs[i][0];
            rank += dirs[i][1];
        }
    }
}

static void gen_cannon(const XqPosition *pos, XqMoveList *list, XqSquare from) {
    static const int dirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    XqColor color = xq_piece_color(pos->board[from]);
    int i;

    for (i = 0; i < 4; ++i) {
        bool screen_seen = false;
        int file = xq_square_file(from) + dirs[i][0];
        int rank = xq_square_rank(from) + dirs[i][1];

        while (xq_square_is_valid(file, rank)) {
            XqSquare to = xq_square_make(file, rank);
            if (!screen_seen) {
                if (pos->board[to] == XQ_EMPTY_PIECE) {
                    add_move(pos, list, from, to);
                } else {
                    screen_seen = true;
                }
            } else if (pos->board[to] != XQ_EMPTY_PIECE) {
                if (is_enemy(pos, to, color)) {
                    add_move(pos, list, from, to);
                }
                break;
            }
            file += dirs[i][0];
            rank += dirs[i][1];
        }
    }
}

static void gen_king(const XqPosition *pos, XqMoveList *list, XqSquare from) {
    static const int steps[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    XqColor color = xq_piece_color(pos->board[from]);
    int file = xq_square_file(from);
    int rank = xq_square_rank(from);
    int i;
    int scan_rank;

    for (i = 0; i < 4; ++i) {
        int to_file = file + steps[i][0];
        int to_rank = rank + steps[i][1];
        if (in_palace(color, to_file, to_rank)) {
            add_step_if_valid(pos, list, from, to_file, to_rank);
        }
    }

    scan_rank = rank + (color == XQ_RED ? 1 : -1);
    while (scan_rank >= 0 && scan_rank < XQ_RANKS) {
        XqSquare to = xq_square_make(file, scan_rank);
        int piece = pos->board[to];
        if (piece != XQ_EMPTY_PIECE) {
            if (xq_piece_type(piece) == XQ_KING && xq_piece_color(piece) != color) {
                add_move(pos, list, from, to);
            }
            break;
        }
        scan_rank += color == XQ_RED ? 1 : -1;
    }
}

static void gen_advisor(const XqPosition *pos, XqMoveList *list, XqSquare from) {
    static const int steps[4][2] = {{1, 1}, {1, -1}, {-1, 1}, {-1, -1}};
    XqColor color = xq_piece_color(pos->board[from]);
    int file = xq_square_file(from);
    int rank = xq_square_rank(from);
    int i;

    for (i = 0; i < 4; ++i) {
        int to_file = file + steps[i][0];
        int to_rank = rank + steps[i][1];
        if (in_palace(color, to_file, to_rank)) {
            add_step_if_valid(pos, list, from, to_file, to_rank);
        }
    }
}

static void gen_bishop(const XqPosition *pos, XqMoveList *list, XqSquare from) {
    static const int steps[4][2] = {{2, 2}, {2, -2}, {-2, 2}, {-2, -2}};
    XqColor color = xq_piece_color(pos->board[from]);
    int file = xq_square_file(from);
    int rank = xq_square_rank(from);
    int i;

    for (i = 0; i < 4; ++i) {
        int to_file = file + steps[i][0];
        int to_rank = rank + steps[i][1];
        int eye_file = file + steps[i][0] / 2;
        int eye_rank = rank + steps[i][1] / 2;

        if (!xq_square_is_valid(to_file, to_rank) || !bishop_on_own_side(color, to_rank)) {
            continue;
        }
        if (pos->board[xq_square_make(eye_file, eye_rank)] != XQ_EMPTY_PIECE) {
            continue;
        }
        add_step_if_valid(pos, list, from, to_file, to_rank);
    }
}

static void gen_knight(const XqPosition *pos, XqMoveList *list, XqSquare from) {
    static const int legs[8][4] = {
        {0, 1, 1, 2}, {0, 1, -1, 2},
        {0, -1, 1, -2}, {0, -1, -1, -2},
        {1, 0, 2, 1}, {1, 0, 2, -1},
        {-1, 0, -2, 1}, {-1, 0, -2, -1}
    };
    int file = xq_square_file(from);
    int rank = xq_square_rank(from);
    int i;

    for (i = 0; i < 8; ++i) {
        int leg_file = file + legs[i][0];
        int leg_rank = rank + legs[i][1];
        int to_file = file + legs[i][2];
        int to_rank = rank + legs[i][3];

        if (!xq_square_is_valid(to_file, to_rank)) {
            continue;
        }
        if (pos->board[xq_square_make(leg_file, leg_rank)] != XQ_EMPTY_PIECE) {
            continue;
        }
        add_step_if_valid(pos, list, from, to_file, to_rank);
    }
}

static void gen_pawn(const XqPosition *pos, XqMoveList *list, XqSquare from) {
    XqColor color = xq_piece_color(pos->board[from]);
    int file = xq_square_file(from);
    int rank = xq_square_rank(from);
    int forward = color == XQ_RED ? 1 : -1;
    bool crossed = color == XQ_RED ? rank >= 5 : rank <= 4;

    add_step_if_valid(pos, list, from, file, rank + forward);
    if (crossed) {
        add_step_if_valid(pos, list, from, file - 1, rank);
        add_step_if_valid(pos, list, from, file + 1, rank);
    }
}

void xq_movelist_clear(XqMoveList *list) {
    list->count = 0;
}

void xq_generate_pseudo_legal(const XqPosition *pos, XqMoveList *list) {
    int sq;

    xq_movelist_clear(list);
    for (sq = 0; sq < XQ_SQUARES; ++sq) {
        int piece = pos->board[sq];
        if (piece == XQ_EMPTY_PIECE || xq_piece_color(piece) != pos->side_to_move) {
            continue;
        }

        switch (xq_piece_type(piece)) {
        case XQ_KING:
            gen_king(pos, list, (XqSquare)sq);
            break;
        case XQ_ADVISOR:
            gen_advisor(pos, list, (XqSquare)sq);
            break;
        case XQ_BISHOP:
            gen_bishop(pos, list, (XqSquare)sq);
            break;
        case XQ_KNIGHT:
            gen_knight(pos, list, (XqSquare)sq);
            break;
        case XQ_ROOK:
            gen_rook(pos, list, (XqSquare)sq);
            break;
        case XQ_CANNON:
            gen_cannon(pos, list, (XqSquare)sq);
            break;
        case XQ_PAWN:
            gen_pawn(pos, list, (XqSquare)sq);
            break;
        default:
            break;
        }
    }
}

bool xq_square_attacked(const XqPosition *pos, XqSquare sq, XqColor by_color) {
    XqPosition tmp = *pos;
    XqMoveList list;
    int i;

    tmp.side_to_move = by_color;
    xq_generate_pseudo_legal(&tmp, &list);
    for (i = 0; i < list.count; ++i) {
        if ((XqSquare)list.moves[i].to == sq) {
            return true;
        }
    }
    return false;
}

bool xq_position_in_check(const XqPosition *pos, XqColor color) {
    XqSquare king = xq_position_king_square(pos, color);
    if (king == XQ_NO_SQUARE) {
        return true;
    }
    return xq_square_attacked(pos, king, xq_color_opponent(color));
}

void xq_generate_legal(const XqPosition *pos, XqMoveList *list) {
    XqMoveList pseudo;
    int i;

    xq_movelist_clear(list);
    xq_generate_pseudo_legal(pos, &pseudo);
    for (i = 0; i < pseudo.count; ++i) {
        XqPosition next = *pos;
        XqColor mover = pos->side_to_move;
        if (!xq_position_make_move(&next, pseudo.moves[i])) {
            continue;
        }
        if (!xq_position_in_check(&next, mover)) {
            if (list->count < XQ_MAX_MOVES) {
                list->moves[list->count++] = pseudo.moves[i];
            }
        }
    }
}

uint64_t xq_perft(const XqPosition *pos, int depth) {
    XqMoveList list;
    uint64_t nodes = 0;
    int i;

    if (depth == 0) {
        return 1u;
    }

    xq_generate_legal(pos, &list);
    if (depth == 1) {
        return (uint64_t)list.count;
    }

    for (i = 0; i < list.count; ++i) {
        XqPosition next = *pos;
        xq_position_make_move(&next, list.moves[i]);
        nodes += xq_perft(&next, depth - 1);
    }
    return nodes;
}

const char *xq_move_to_string(XqMove move, char *buffer, size_t buffer_size) {
    if (buffer == NULL || buffer_size < 6) {
        return "";
    }

    (void)snprintf(
        buffer,
        buffer_size,
        "%c%d%c%d",
        (char)('a' + xq_square_file((XqSquare)move.from)),
        xq_square_rank((XqSquare)move.from),
        (char)('a' + xq_square_file((XqSquare)move.to)),
        xq_square_rank((XqSquare)move.to));
    return buffer;
}
