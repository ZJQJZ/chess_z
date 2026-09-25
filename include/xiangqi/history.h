#ifndef XIANGQI_HISTORY_H
#define XIANGQI_HISTORY_H

#include "xiangqi/position.h"

typedef struct XqHistoryEntry
{
    uint64_t key;
    uint64_t prefix; /* Rolling hash of moves 1 through this entry. */
    uint64_t power;  /* Hash base raised to this entry's index. */
    XqMove move;
} XqHistoryEntry;

/* Entry 0 is the initial position; entry i records the move reaching position i.
 * Fields are read-only to callers. Only real game moves belong in this history. */
typedef struct XqHistory
{
    XqHistoryEntry *entries;
    size_t count; /* Number of positions, including the initial position. */
    size_t capacity;
} XqHistory;

/* Initialize an unused object. On allocation failure it remains safe to destroy. */
bool xq_history_init(XqHistory *history, const XqPosition *initial);
/* Start a new game on an initialized or zeroed object, retaining allocated storage.
 * Failure leaves history unchanged. */
bool xq_history_reset(XqHistory *history, const XqPosition *initial);
/* Append after a successful real move; pos is the resulting position.
 * Requires initialized history. Allocation failure leaves history unchanged. */
bool xq_history_push(XqHistory *history, XqMove move, const XqPosition *pos);
/* Retain the first count positions, including the initial position (count >= 1).
 * Rejects counts beyond the current history; retains storage for subsequent moves.
 * The caller must restore the board to the retained final position separately. */
bool xq_history_truncate(XqHistory *history, size_t count);
void xq_history_destroy(XqHistory *history);

#endif
