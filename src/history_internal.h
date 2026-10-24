#ifndef XIANGQI_HISTORY_INTERNAL_H
#define XIANGQI_HISTORY_INTERNAL_H

#include "xiangqi/history.h"

/* Builds one entry after an existing entry without changing either position or history.
 * The caller has already made move, and pos is its resulting position. Storage and
 * capacity management belong to the caller; this also supports private search paths. */
XqHistoryEntry xq_history_next_entry(const XqHistoryEntry *previous, XqMove move,
                                   const XqPosition *pos);

/* Filters candidates against a real or private transient history, preserving survivor
 * order. Candidates may be pseudo-legal; this function does not validate legality.
 * NULL, empty or mismatched history leaves the list unchanged. Position and history
 * are never modified. Returns whether any move was removed; the list may become empty. */
bool xq_history_filter_cycles(const XqHistory *history, const XqPosition *pos,
                             XqMoveList *list);

#endif
