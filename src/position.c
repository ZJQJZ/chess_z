#include "xiangqi/position.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static bool valid_square(XqSquare sq) {
    return sq >= 0 && sq < XQ_SQUARES;
}

const char *xq_piece_type_name(XqPieceType type) {
    static const char *names[] = {
        "king", "advisor", "bishop", "knight", "rook", "cannon", "pawn"
    };
    if (type < 0 || type >= XQ_PIECE_TYPE_NB) {
        return "unknown";
    }
    return names[type];
}

char xq_piece_to_char(int piece) {
    static const char red_chars[] = {'K', 'A', 'B', 'N', 'R', 'C', 'P'};
    static const char black_chars[] = {'k', 'a', 'b', 'n', 'r', 'c', 'p'};
    XqPieceType type;

    if (piece == XQ_EMPTY_PIECE) {
        return '.';
    }

    type = xq_piece_type(piece);
    if (type < 0 || type >= XQ_PIECE_TYPE_NB) {
        return '?';
    }
    return xq_piece_color(piece) == XQ_RED ? red_chars[type] : black_chars[type];
}

int xq_piece_from_char(char ch) {
    XqColor color = isupper((unsigned char)ch) ? XQ_RED : XQ_BLACK;
    switch ((char)tolower((unsigned char)ch)) {
    case 'k': return xq_make_piece(color, XQ_KING);
    case 'a': return xq_make_piece(color, XQ_ADVISOR);
    case 'b': return xq_make_piece(color, XQ_BISHOP);
    case 'n':
    case 'h': return xq_make_piece(color, XQ_KNIGHT);
    case 'r': return xq_make_piece(color, XQ_ROOK);
    case 'c': return xq_make_piece(color, XQ_CANNON);
    case 'p': return xq_make_piece(color, XQ_PAWN);
    default: return XQ_EMPTY_PIECE;
    }
}

void xq_position_clear(XqPosition *pos) {
    int i;

    memset(pos, 0, sizeof(*pos));
    for (i = 0; i < XQ_SQUARES; ++i) {
        pos->board[i] = XQ_EMPTY_PIECE;
    }
    pos->side_to_move = XQ_RED;
    pos->fullmove_number = 1;
}

bool xq_position_set_piece(XqPosition *pos, XqSquare sq, XqColor color, XqPieceType type) {
    int piece;

    if (!valid_square(sq) || color < 0 || color >= XQ_COLOR_NB || type < 0 || type >= XQ_PIECE_TYPE_NB) {
        return false;
    }
    if (pos->board[sq] != XQ_EMPTY_PIECE) {
        xq_position_remove_piece(pos, sq);
    }

    piece = xq_make_piece(color, type);
    pos->board[sq] = (int8_t)piece;
    xq_bb_set(&pos->pieces[color][type], sq);
    xq_bb_set(&pos->occupied[color], sq);
    xq_bb_set(&pos->all_occupied, sq);
    return true;
}

bool xq_position_remove_piece(XqPosition *pos, XqSquare sq) {
    int piece;
    XqColor color;
    XqPieceType type;

    if (!valid_square(sq)) {
        return false;
    }

    piece = pos->board[sq];
    if (piece == XQ_EMPTY_PIECE) {
        return false;
    }

    color = xq_piece_color(piece);
    type = xq_piece_type(piece);
    pos->board[sq] = XQ_EMPTY_PIECE;
    xq_bb_clear(&pos->pieces[color][type], sq);
    xq_bb_clear(&pos->occupied[color], sq);
    xq_bb_clear(&pos->all_occupied, sq);
    return true;
}

bool xq_position_make_move(XqPosition *pos, XqMove move) {
    XqSquare from = (XqSquare)move.from;
    XqSquare to = (XqSquare)move.to;
    int piece;

    if (!valid_square(from) || !valid_square(to)) {
        return false;
    }

    piece = pos->board[from];
    if (piece == XQ_EMPTY_PIECE) {
        return false;
    }

    if (pos->board[to] != XQ_EMPTY_PIECE) {
        xq_position_remove_piece(pos, to);
    }
    xq_position_remove_piece(pos, from);
    xq_position_set_piece(pos, to, xq_piece_color(piece), xq_piece_type(piece));

    pos->side_to_move = xq_color_opponent(pos->side_to_move);
    if (pos->side_to_move == XQ_RED) {
        ++pos->fullmove_number;
    }
    return true;
}

void xq_position_startpos(XqPosition *pos) {
    static const char *start_fen =
        "rnbakabnr/9/1c5c1/p1p1p1p1p/9/9/P1P1P1P1P/1C5C1/9/RNBAKABNR r - - 0 1";
    (void)xq_position_from_fen(pos, start_fen);
}

bool xq_position_from_fen(XqPosition *pos, const char *fen) {
    int fen_rank = 0;
    int file = 0;
    const char *p = fen;

    if (fen == NULL) {
        return false;
    }

    xq_position_clear(pos);

    while (*p != '\0' && *p != ' ') {
        int rank = XQ_RANKS - 1 - fen_rank;

        if (*p == '/') {
            if (file != XQ_FILES) {
                return false;
            }
            ++fen_rank;
            file = 0;
            ++p;
            continue;
        }

        if (isdigit((unsigned char)*p)) {
            file += *p - '0';
            if (file > XQ_FILES) {
                return false;
            }
            ++p;
            continue;
        }

        if (file >= XQ_FILES || fen_rank >= XQ_RANKS) {
            return false;
        }

        {
            int piece = xq_piece_from_char(*p);
            if (piece == XQ_EMPTY_PIECE) {
                return false;
            }
            xq_position_set_piece(pos, xq_square_make(file, rank), xq_piece_color(piece), xq_piece_type(piece));
        }
        ++file;
        ++p;
    }

    if (fen_rank != XQ_RANKS - 1 || file != XQ_FILES) {
        return false;
    }

    while (*p == ' ') {
        ++p;
    }
    if (*p == 'b') {
        pos->side_to_move = XQ_BLACK;
    } else if (*p == 'r' || *p == 'w') {
        pos->side_to_move = XQ_RED;
    } else {
        return false;
    }

    return xq_position_validate(pos);
}

bool xq_position_to_fen(const XqPosition *pos, char *buffer, size_t buffer_size) {
    size_t used = 0;
    int fen_rank;

    if (buffer == NULL || buffer_size == 0) {
        return false;
    }

    for (fen_rank = 0; fen_rank < XQ_RANKS; ++fen_rank) {
        int rank = XQ_RANKS - 1 - fen_rank;
        int empty = 0;
        int file;

        for (file = 0; file < XQ_FILES; ++file) {
            XqSquare sq = xq_square_make(file, rank);
            int piece = pos->board[sq];
            char ch;

            if (piece == XQ_EMPTY_PIECE) {
                ++empty;
                continue;
            }

            if (empty > 0) {
                if (used + 1 >= buffer_size) {
                    return false;
                }
                buffer[used++] = (char)('0' + empty);
                empty = 0;
            }

            ch = xq_piece_to_char(piece);
            if (used + 1 >= buffer_size) {
                return false;
            }
            buffer[used++] = ch;
        }

        if (empty > 0) {
            if (used + 1 >= buffer_size) {
                return false;
            }
            buffer[used++] = (char)('0' + empty);
        }

        if (fen_rank != XQ_RANKS - 1) {
            if (used + 1 >= buffer_size) {
                return false;
            }
            buffer[used++] = '/';
        }
    }

    if (used + 7 >= buffer_size) {
        return false;
    }
    buffer[used++] = ' ';
    buffer[used++] = pos->side_to_move == XQ_RED ? 'r' : 'b';
    buffer[used++] = ' ';
    buffer[used++] = '-';
    buffer[used++] = ' ';
    buffer[used++] = '-';
    buffer[used] = '\0';
    return true;
}

XqSquare xq_position_king_square(const XqPosition *pos, XqColor color) {
    int sq;

    for (sq = 0; sq < XQ_SQUARES; ++sq) {
        if (xq_bb_test(pos->pieces[color][XQ_KING], (XqSquare)sq)) {
            return (XqSquare)sq;
        }
    }
    return XQ_NO_SQUARE;
}

int xq_position_piece_count(const XqPosition *pos, XqColor color, XqPieceType type) {
    return xq_bb_count(pos->pieces[color][type]);
}

bool xq_position_validate(const XqPosition *pos) {
    XqBitboard occupied[XQ_COLOR_NB] = {xq_bb_empty(), xq_bb_empty()};
    XqBitboard all = xq_bb_empty();
    int sq;

    for (sq = 0; sq < XQ_SQUARES; ++sq) {
        int piece = pos->board[sq];
        if (piece == XQ_EMPTY_PIECE) {
            continue;
        }

        if (piece < 0 || piece >= XQ_COLOR_NB * XQ_PIECE_TYPE_NB) {
            return false;
        }

        xq_bb_set(&occupied[xq_piece_color(piece)], (XqSquare)sq);
        xq_bb_set(&all, (XqSquare)sq);
        if (!xq_bb_test(pos->pieces[xq_piece_color(piece)][xq_piece_type(piece)], (XqSquare)sq)) {
            return false;
        }
    }

    if (occupied[XQ_RED].lo != pos->occupied[XQ_RED].lo || occupied[XQ_RED].hi != pos->occupied[XQ_RED].hi) {
        return false;
    }
    if (occupied[XQ_BLACK].lo != pos->occupied[XQ_BLACK].lo || occupied[XQ_BLACK].hi != pos->occupied[XQ_BLACK].hi) {
        return false;
    }
    if (all.lo != pos->all_occupied.lo || all.hi != pos->all_occupied.hi) {
        return false;
    }

    return xq_position_piece_count(pos, XQ_RED, XQ_KING) == 1 &&
        xq_position_piece_count(pos, XQ_BLACK, XQ_KING) == 1;
}

void xq_position_print(const XqPosition *pos) {
    int rank;

    for (rank = XQ_RANKS - 1; rank >= 0; --rank) {
        int file;
        printf("%d ", rank);
        for (file = 0; file < XQ_FILES; ++file) {
            XqSquare sq = xq_square_make(file, rank);
            printf("%c ", xq_piece_to_char(pos->board[sq]));
        }
        printf("\n");
    }
    printf("  0 1 2 3 4 5 6 7 8\n");
    printf("side: %s\n", pos->side_to_move == XQ_RED ? "red" : "black");
}
