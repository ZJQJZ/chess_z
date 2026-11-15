#ifndef XIANGQI_JIEQI_HISTORY_INTERNAL_JQ_H
#define XIANGQI_JIEQI_HISTORY_INTERNAL_JQ_H

#include "xiangqi/jieqi/history_jq.h"
#include "xiangqi/jieqi/movegen_jq.h"

/* Builds one entry after an existing entry without changing either position or history.
 * The caller has already made move, and pos is its resulting position. Storage and
 * capacity management belong to the caller; this also supports private search paths. */
XqJqHistoryEntry xq_jq_history_next_entry(const XqJqHistoryEntry *previous, XqMove move,
                                        int revealed_type, const XqJqPosition *pos);

/* Filters candidates against a real or private transient history, preserving survivor
 * order. Candidates may be pseudo-legal; this function does not validate legality.
 * NULL, empty or mismatched history leaves the list unchanged. Position and history
 * are never modified. Dark moves cannot close a cycle because revealing reduces
 * the public pool; they are retained without choosing or sampling an identity.
 * Returns whether any move was removed; the list may become empty. */
bool xq_jq_history_filter_cycles(const XqJqHistory *history, const XqJqPosition *pos,
                                 XqJqMoveList *list);

#endif
