#include "xiangqi/history.h"

#include <stdlib.h>

/* Polynomial hashing modulo 2^64; unsigned overflow is intentional. */
#define HISTORY_HASH_BASE UINT64_C(1000000007)

/**
 * @brief Ensures that the history has space for one more entry.
 *
 * Existing spare capacity is reused. Otherwise, the entry array is allocated with an initial
 * capacity of 64 or grown to twice its current capacity. The size check prevents overflow when
 * calculating the allocation size. Growing the array may invalidate pointers to its entries.
 *
 * This function does not append an entry or change the count. On failure, the history is unchanged.
 *
 * @param history Initialized or zeroed history whose storage is reserved; must not be null.
 * @return        True if space is available; false if the allocation size would overflow or memory
 *                allocation fails.
 */
static bool history_reserve(XqHistory *history)
{
    size_t capacity;
    XqHistoryEntry *entries;

    if (history->count < history->capacity)
        return true;
    if (history->capacity > SIZE_MAX / sizeof(*entries) / 2)
        return false;
    capacity = history->capacity == 0 ? 64 : history->capacity * 2;
    entries = realloc(history->entries, capacity * sizeof(*entries));
    if (entries == NULL)
        return false;
    history->entries = entries;
    history->capacity = capacity;
    return true;
}

/**
 * @brief Initializes a history with the starting position of a game.
 *
 * The object is zeroed before its initial entry is created by `xq_history_reset()`. On success, the
 * count is one because the initial position itself occupies entry zero. On allocation failure, the
 * object remains zeroed and can safely be passed to `xq_history_destroy()`.
 *
 * Do not call this on an object that still owns storage; use `xq_history_reset()` to start a new
 * game while retaining that storage, or destroy the object before initializing it again.
 *
 * @param history Unused history object to initialize; must not be null.
 * @param initial Starting position to record; must not be null and is left unchanged.
 * @return        True if the initial entry was created; false if memory allocation fails.
 */
bool xq_history_init(XqHistory *history, const XqPosition *initial)
{
    *history = (XqHistory){0};
    return xq_history_reset(history, initial);
}

/**
 * @brief Starts a new game history while retaining any previously allocated storage.
 *
 * Entry zero stores the initial position hash, an empty path hash of zero, and a hash-base power of
 * one. Its move is zero-initialized and does not represent an actual move. Setting the count to one
 * discards the previous game logically; entries beyond that count are no longer valid.
 *
 * Storage is allocated only when none exists. On allocation failure, the history is unchanged.
 *
 * @param history Initialized or zeroed history to reset; must not be null.
 * @param initial Starting position of the new game; must not be null and is left unchanged.
 * @return        True if the history was reset; false if memory allocation fails.
 */
bool xq_history_reset(XqHistory *history, const XqPosition *initial)
{
    if (history->capacity == 0 && !history_reserve(history))
        return false;
    history->entries[0] = (XqHistoryEntry){.key = xq_position_hash(initial), .power = 1};
    history->count = 1;
    return true;
}

/**
 * @brief Appends a real game move and its resulting position to the history.
 *
 * The caller must already have played the move from the history's last position. This function does
 * not make the move, validate its legality, or verify that it continues the recorded game.
 *
 * The source and destination squares form a nonzero token for the rolling path hash. Each entry
 * stores `previous_prefix * HISTORY_HASH_BASE + token` and the next power of the base, allowing
 * equal-length path hashes to be compared in constant time. Unsigned overflow intentionally
 * computes these values modulo 2^64. The complete move is also retained for exact path comparison
 * and replay; its piece and captured-piece fields are not part of the rolling hash.
 *
 * Appending takes amortized constant time through geometric storage growth. On failure, the history
 * is unchanged, and the caller must handle the unrecorded move before continuing play.
 *
 * @param history History containing an initial position; must not be null.
 * @param move    Real move just played, including its piece and captured-piece information.
 * @param pos     Position after the move; must not be null and is left unchanged.
 * @return        True if the entry was appended; false if the history is empty, the allocation size
 *                would overflow, or memory allocation fails.
 */
bool xq_history_push(XqHistory *history, XqMove move, const XqPosition *pos)
{
    const XqHistoryEntry *previous;
    uint64_t token = (uint64_t)move.from * XQ_SQUARES + move.to + 1;

    if (history->count == 0 || !history_reserve(history))
        return false;
    previous = &history->entries[history->count - 1];
    history->entries[history->count++] = (XqHistoryEntry){
        .key = xq_position_hash(pos),
        .prefix = previous->prefix * HISTORY_HASH_BASE + token,
        .power = previous->power * HISTORY_HASH_BASE,
        .move = move,
    };
    return true;
}

/**
 * @brief Releases the history's entry array and resets the object to zero.
 *
 * The history object itself is not freed. Calling this on a zeroed or already destroyed object is
 * safe. All pointers to former entries become invalid; the object can subsequently be initialized
 * or reset to begin another game.
 *
 * @param history Initialized or zeroed history whose storage is released; must not be null.
 */
void xq_history_destroy(XqHistory *history)
{
    free(history->entries);
    *history = (XqHistory){0};
}
