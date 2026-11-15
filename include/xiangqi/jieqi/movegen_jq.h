#ifndef XIANGQI_JIEQI_MOVEGEN_JQ_H
#define XIANGQI_JIEQI_MOVEGEN_JQ_H

#include <stdbool.h>
#include <stdint.h>

#include "xiangqi/jieqi/position_jq.h"
#include "xiangqi/types.h"

enum
{
    /* 任一棋子最多有 17 个目标格，覆盖自定义 FEN 的所有棋盘单位。 */
    XQ_JQ_MAX_MOVES = XQ_SQUARES * (XQ_FILES + XQ_RANKS - 2)
};

typedef struct XqJqMoveList
{
    XqMove moves[XQ_JQ_MAX_MOVES];
    int count;
} XqJqMoveList;

void xq_jq_movelist_clear(XqJqMoveList *list);
/* 暗子按初始类型生成走法；列表不包含揭示结果，也不进行随机采样。 */
void xq_jq_generate_pseudo_legal(const XqJqPosition *pos, XqJqMoveList *list);
void xq_jq_generate_legal(const XqJqPosition *pos, XqJqMoveList *list);

/* sq 必须有 by_color 的敌方棋子；判断该棋子是否受到 by_color 攻击。 */
bool xq_jq_square_attacked(const XqJqPosition *pos, XqSquare sq, XqColor by_color);
bool xq_jq_position_in_check(const XqJqPosition *pos, XqColor color);

const char *xq_jq_move_to_string(XqMove move, char *buffer, size_t buffer_size);

#endif
