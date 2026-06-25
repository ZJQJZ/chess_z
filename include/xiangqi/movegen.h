#ifndef XIANGQI_MOVEGEN_H
#define XIANGQI_MOVEGEN_H

#include <stdbool.h>
#include <stdint.h>

#include "xiangqi/position.h"
#include "xiangqi/types.h"

void xq_movelist_clear(XqMoveList *list);
void xq_generate_pseudo_legal(const XqPosition *pos, XqMoveList *list);
void xq_generate_legal(const XqPosition *pos, XqMoveList *list);

bool xq_square_attacked(const XqPosition *pos, XqSquare sq, XqColor by_color);
bool xq_position_in_check(const XqPosition *pos, XqColor color);
uint64_t xq_perft(const XqPosition *pos, int depth);

const char *xq_move_to_string(XqMove move, char *buffer, size_t buffer_size);

#endif
