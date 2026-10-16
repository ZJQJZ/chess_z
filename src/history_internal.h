#ifndef XIANGQI_HISTORY_INTERNAL_H
#define XIANGQI_HISTORY_INTERNAL_H

#include "xiangqi/history.h"

/* Filters legal root candidates against real-game history, preserving survivor order.
 * NULL, empty or mismatched history leaves the list unchanged. The position and history
 * are never modified. Returns whether any move was removed; the list may become empty. */
bool xq_history_filter_root_cycles(const XqHistory *history, const XqPosition *pos,
                                   XqMoveList *list);

#endif
