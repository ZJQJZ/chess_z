#ifndef XIANGQI_POSITION_H
#define XIANGQI_POSITION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "xiangqi/bitboard.h"
#include "xiangqi/types.h"

typedef struct XqPosition
{
    XqBitboard pieces[XQ_COLOR_NB][XQ_PIECE_TYPE_NB];
    XqBitboard occupied[XQ_COLOR_NB];
    XqBitboard all_occupied;
    int8_t board[XQ_SQUARES];
    XqColor side_to_move;
    uint16_t halfmove_clock;
    uint16_t fullmove_number;
} XqPosition;

void xq_position_clear(XqPosition *pos);
void xq_position_startpos(XqPosition *pos);
bool xq_position_from_fen(XqPosition *pos, const char *fen);
bool xq_position_to_fen(const XqPosition *pos, char *buffer, size_t buffer_size);

bool xq_position_set_piece(XqPosition *pos, XqSquare sq, XqColor color, XqPieceType type);
bool xq_position_remove_piece(XqPosition *pos, XqSquare sq);
bool xq_position_make_move(XqPosition *pos, XqMove move);

XqSquare xq_position_king_square(const XqPosition *pos, XqColor color);
int xq_position_piece_count(const XqPosition *pos, XqColor color, XqPieceType type);
bool xq_position_validate(const XqPosition *pos);
void xq_position_print(const XqPosition *pos);

#endif
