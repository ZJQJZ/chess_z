#include "history_internal.h"

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
 * @brief Builds the next entry for real history or a private search path.
 *
 * The source and destination squares form a nonzero token for the rolling path hash. The entry
 * stores `previous_prefix * HISTORY_HASH_BASE + token` and the next power of the base. Unsigned
 * overflow intentionally computes these values modulo 2^64. The complete move is retained for
 * exact path comparison and replay; its piece and captured-piece fields are not hashed.
 *
 * The caller has already played the move. No legality, continuity or capacity checks are made;
 * neither the preceding entry nor the resulting position is modified, and no storage is allocated.
 *
 * @param previous Entry preceding the move; must not be null.
 * @param move     Move just played, including its piece and captured-piece information.
 * @param pos      Position after the move; must not be null and is left unchanged.
 * @return         Entry recording the resulting position and extended move-path hash.
 */
XqHistoryEntry xq_history_next_entry(const XqHistoryEntry *previous,
                                   XqMove                move,
                                   const XqPosition     *pos)
{
    uint64_t token = (uint64_t)move.from * XQ_SQUARES + move.to + 1;

    return (XqHistoryEntry){
        .key = xq_position_hash(pos),
        .prefix = previous->prefix * HISTORY_HASH_BASE + token,
        .power = previous->power * HISTORY_HASH_BASE,
        .move = move,
    };
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

    if (history->count == 0 || !history_reserve(history))
        return false;
    previous = &history->entries[history->count - 1];
    history->entries[history->count] = xq_history_next_entry(previous, move, pos);
    ++history->count;
    return true;
}

bool xq_history_truncate(XqHistory *history, size_t count)
{
    if (count == 0 || count > history->count)
        return false;
    history->count = count;
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

/**
 * @brief Tests whether two recorded moves have the same source and destination squares.
 *
 * Piece and captured-piece fields do not participate in cycle-path comparison.
 *
 * @param a First move to compare.
 * @param b Second move to compare.
 * @return  `true` if both moves have identical source and destination squares; `false` otherwise.
 */
static bool moves_equal(XqMove a,
                        XqMove b)
{
    return a.from == b.from && a.to == b.to;
}

/**
 * @brief Computes the rolling hash of the move sequence between two recorded positions.
 *
 * Entry `0` is the initial position; entry `i >= 1` records the move reaching position `i`. Each
 * move is represented by a numeric token:
 *
 *     t[i] = entries[i].move.from * XQ_SQUARES + entries[i].move.to + 1
 *
 * Square indices are in `[0, XQ_SQUARES)`, so this uniquely encodes the source/destination pair.
 * Adding `1` makes every token nonzero. Piece and captured-piece fields are not encoded.
 *
 * `xq_history_push()` precomputes the prefix hash `H[i]` in `entries[i].prefix`, using the fixed
 * base `B = HISTORY_HASH_BASE`:
 *
 *     H[0] = 0
 *     H[i] = H[i - 1] * B + t[i]
 *
 * Multiplying by `B` increases every earlier token's exponent by one before appending `t[i]`.
 *
 * Position indices are interval boundaries: this function selects moves `begin + 1` through `end`.
 * Let `L = end - begin`. For a nonempty interval, the prefix through `end` can be split as:
 *
 *     H[end] = H[begin] * B^L
 *            + t[begin + 1] * B^(L - 1) + ... + t[end]
 *
 * The first term is the contribution of all moves before the selected interval, with their
 * exponents increased by `L` to match their weights in `H[end]`. Subtracting that term cancels
 * those earlier moves and leaves only the selected tokens, weighted from `B^(L - 1)` down to `1`.
 * Thus the returned hash is:
 *
 *     H[end] - H[begin] * B^L = t[begin + 1] * B^(L - 1) + ... + t[end]
 *
 * For example, `begin=2`, `end=5` yields `t[3] * B^2 + t[4] * B + t[5]`.
 *
 * `entries[L].power` supplies the precomputed `B^L`, so extraction takes constant time without
 * traversing moves. All hash and power arithmetic intentionally wraps modulo `2^64`. When `L=0`,
 * `entries[0].power` is `1`, so the subtraction returns `0` for the empty path.
 *
 * Equal-length paths with identical source/destination sequences have equal hashes regardless of
 * their offsets in history. Hash equality alone does not prove path equality; callers must compare
 * the actual moves to exclude hash collisions and check position hashes separately for a cycle.
 *
 * @param history Recorded game history; must not be `NULL` and is left unchanged.
 * @param begin   Starting position index; must satisfy `begin <= end`.
 * @param end     Ending position index; must be less than `history->count`.
 * @return        Hash of the selected move sequence, or `0` for an empty path (`begin == end`).
 */
static uint64_t history_path_hash(const XqHistory *history,
                                  size_t           begin,
                                  size_t           end)
{
    return history->entries[end].prefix -
           history->entries[begin].prefix * history->entries[end - begin].power;
}

/**
 * @brief Determines whether completing a detected threefold cycle makes the moving side lose.
 *
 * The caller has already verified that appending `candidate` would complete three consecutive,
 * identical move sequences whose four boundary positions have matching hashes. This function
 * applies the loss rule only; it does not detect cycles or play the `candidate` move.
 *
 * The current rule always returns `true`, making the side to move in `pos` lose for closing the
 * third cycle. Keeping this decision separate allows future rules to inspect the cycle, for example
 * to require that every move by the closing side gives check before declaring a loss.
 *
 * `history` contains the preceding real or private search path, and `pos` is the position before
 * `candidate`. The third cycle's final move is supplied separately as `candidate` and is not yet
 * present in `history`. A future rule can use a copy of `pos` and the recorded moves to reconstruct
 * and inspect the cycle positions.
 *
 * @param history   History ending at `pos`; must not be `NULL` and is left unchanged.
 * @param start     Position index at the beginning of the first of the three cycles.
 * @param length    Number of individual moves in one cycle; a positive even number satisfying
 *                  `start + 3 * length == history->count`.
 * @param pos       Position before `candidate` is played; must not be `NULL` and is left unchanged.
 * @param candidate Candidate move that would close the third identical cycle.
 * @return          `true` if the side playing `candidate` loses under the cycle rule. The current
 *                  implementation always returns `true`; future rules may return `false` to allow
 *                  it.
 */
static bool cycle_is_loss(const XqHistory  *history,
                          size_t            start,
                          size_t            length,
                          const XqPosition *pos,
                          XqMove            candidate)
{
    (void)history;
    (void)start;
    (void)length;
    (void)pos;
    (void)candidate;
    return true;
}

/**
 * @brief Removes candidates that would complete a cycle penalized as a loss.
 *
 * Examines the supplied real-game history or private transient search path plus a candidate move.
 * Candidate moves may be pseudo-legal; the caller remains responsible for checking legality.
 * `NULL`, empty, or mismatched history leaves the move list unchanged; matching requires the last
 * recorded position hash to equal the current position hash.
 *
 * The virtual position index after the candidate is `end = history->count`. For each possible even
 * cycle length, the first cycle starts at `end - 3 * length`. Only even lengths can return to the
 * same side to move. The first three boundary positions must have equal hashes, and prefix hashes
 * must match for the first two complete paths and for the third path without its final move. That
 * missing move must match the last move of the first path in the supplied candidate list.
 *
 * Hash matches are checked move by move using source and destination squares. The candidate is then
 * played on a position copy to verify the fourth boundary position hash.
 * `cycle_is_loss()` decides whether the verified cycle penalizes the moving side, keeping rule
 * changes separate from cycle detection. Candidates are never appended to the supplied history.
 *
 * Prefix-hash screening takes `O(history->count)` time; matching paths additionally require move
 * lookup and exact comparison. Rejected moves are removed in place while preserving the relative
 * order of surviving moves. The list may become empty; choosing a fallback is the caller's job.
 *
 * @param history Optional real or private history ending at `pos`; may be `NULL` and is unchanged.
 * @param pos     Position before a candidate move; must not be `NULL` and is left unchanged.
 * @param list    Candidate moves to filter in place; must not be `NULL`. Both `moves` and `count`
 *                are updated when candidates are rejected.
 * @return        `true` if at least one move was removed; `false` if no move was removed, including
 *                when history is absent, empty, or does not match the current position.
 */
bool xq_history_filter_cycles(const XqHistory  *history,
                             const XqPosition *pos,
                             XqMoveList       *list)
{
    int original_count = list->count;

    if (history == NULL || history->count == 0 ||
        history->entries[history->count - 1].key != xq_position_hash(pos))
        return false;

    size_t end                    = history->count;
    bool   rejected[XQ_MAX_MOVES] = {false};
    for (size_t length = 2; length <= end / 3; length += 2)
    {
        size_t   start  = end - 3 * length;
        size_t   second = start + length;
        size_t   third  = second + length;
        uint64_t key    = history->entries[start].key;

        if (history->entries[second].key != key || history->entries[third].key != key ||
            history_path_hash(history, start, second) !=
                history_path_hash(history, second, third) ||
            history_path_hash(history, start, second - 1) !=
                history_path_hash(history, third, end - 1))
            continue;

        /* The missing move must equal the last move of the first period. */
        int i = 0;
        for (; i < list->count; ++i)
            if (!rejected[i] && moves_equal(list->moves[i], history->entries[second].move))
                break;
        if (i == list->count)
            continue;

        size_t j = 1;
        for (; j <= length; ++j)
            if (!moves_equal(history->entries[start + j].move, history->entries[second + j].move) ||
                (j < length &&
                 !moves_equal(history->entries[start + j].move, history->entries[third + j].move)))
                break;
        if (j <= length)
            continue;

        XqPosition next = *pos;
        xq_position_make_move(&next, list->moves[i]);
        bool closed = xq_position_hash(&next) == key;
        if (closed && cycle_is_loss(history, start, length, pos, list->moves[i]))
            rejected[i] = true;
    }

    int count = 0;
    for (int i = 0; i < list->count; ++i)
        if (!rejected[i])
            list->moves[count++] = list->moves[i];
    list->count = count;
    return count != original_count;
}
