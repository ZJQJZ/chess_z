#ifndef XIANGQI_TYPES_H
#define XIANGQI_TYPES_H

#include <stdbool.h>
#include <stdint.h>

#define XQ_FILES 9
#define XQ_RANKS 10
#define XQ_SQUARES 90
#define XQ_MAX_MOVES 256

typedef int8_t XqSquare;

enum {
    XQ_NO_SQUARE = -1,
    XQ_EMPTY_PIECE = -1
};

typedef enum XqColor {
    XQ_RED = 0,
    XQ_BLACK = 1,
    XQ_COLOR_NB = 2
} XqColor;

typedef enum XqPieceType {
    XQ_KING = 0,
    XQ_ADVISOR,
    XQ_BISHOP,
    XQ_KNIGHT,
    XQ_ROOK,
    XQ_CANNON,
    XQ_PAWN,
    XQ_PIECE_TYPE_NB
} XqPieceType;

typedef struct XqMove {
    uint8_t from;
    uint8_t to;
    int8_t piece;
    int8_t captured;
} XqMove;

typedef struct XqMoveList {
    XqMove moves[XQ_MAX_MOVES];
    int count;
} XqMoveList;

static inline XqColor xq_color_opponent(XqColor color) {
    return color == XQ_RED ? XQ_BLACK : XQ_RED;
}

static inline int xq_make_piece(XqColor color, XqPieceType type) {
    return (int)color * XQ_PIECE_TYPE_NB + (int)type;
}

static inline XqColor xq_piece_color(int piece) {
    return (XqColor)(piece / XQ_PIECE_TYPE_NB);
}

static inline XqPieceType xq_piece_type(int piece) {
    return (XqPieceType)(piece % XQ_PIECE_TYPE_NB);
}

static inline XqSquare xq_square_make(int file, int rank) {
    return (XqSquare)(rank * XQ_FILES + file);
}

static inline int xq_square_file(XqSquare sq) {
    return sq % XQ_FILES;
}

static inline int xq_square_rank(XqSquare sq) {
    return sq / XQ_FILES;
}

static inline bool xq_square_is_valid(int file, int rank) {
    return file >= 0 && file < XQ_FILES && rank >= 0 && rank < XQ_RANKS;
}

const char *xq_piece_type_name(XqPieceType type);
char xq_piece_to_char(int piece);
int xq_piece_from_char(char ch);

#endif
