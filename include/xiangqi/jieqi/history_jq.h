#ifndef XIANGQI_JIEQI_HISTORY_JQ_H
#define XIANGQI_JIEQI_HISTORY_JQ_H

#include "xiangqi/jieqi/position_jq.h"

typedef struct XqJqHistoryEntry
{
    uint64_t key;
    uint64_t prefix; /* Rolling hash of moves 1 through this entry. */
    uint64_t power;  /* Hash base raised to this entry's index. */
    XqMove move;
    bool in_check; /* Side to move is in check in this entry's resulting position. */
    int8_t revealed_type; /* Actual reveal for replay; XQ_JQ_NO_REVEAL for bright moves. */
} XqJqHistoryEntry;

/* Entry 0 is the initial position; entry i records the move reaching position i.
 * Fields are read-only to callers. Only real game moves belong in this history. */
typedef struct XqJqHistory
{
    XqJqHistoryEntry *entries;
    size_t count; /* Number of positions, including the initial position. */
    size_t capacity;
} XqJqHistory;

/* Initialize an unused object. On allocation failure it remains safe to destroy. */
bool xq_jq_history_init(XqJqHistory *history, const XqJqPosition *initial);
/* Start a new game on an initialized or zeroed object, retaining allocated storage.
 * Failure leaves history unchanged. */
bool xq_jq_history_reset(XqJqHistory *history, const XqJqPosition *initial);
/* Append after a successful real move; pos is the resulting position and
 * revealed_type is the value passed to position_make_move, retained for replay.
 * Requires initialized history. Allocation failure leaves history unchanged. */
bool xq_jq_history_push(XqJqHistory *history, XqMove move, int revealed_type,
                        const XqJqPosition *pos);
/* Retain the first count positions, including the initial position (count >= 1).
 * Rejects counts beyond the current history; retains storage for subsequent moves.
 * The caller must restore the board to the retained final position separately. */
bool xq_jq_history_truncate(XqJqHistory *history, size_t count);
void xq_jq_history_destroy(XqJqHistory *history);

#endif
