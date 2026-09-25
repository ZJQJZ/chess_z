#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "xiangqi/engine.h"

#include "xiangqi/movegen.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#endif

static const int piece_values[XQ_PIECE_TYPE_NB] = {
    [XQ_KING] = 10000, [XQ_ADVISOR] = 120, [XQ_BISHOP] = 120, [XQ_KNIGHT] = 270,
    [XQ_ROOK] = 600,   [XQ_CANNON] = 285,  [XQ_PAWN] = 70,
};

/*
 * Positional bonuses added to `piece_values`, in the same score units. These are conservative
 * hand-selected starting values, not parameters tuned through self-play.
 *
 * Rows run from the piece owner's home rank (0) to the opponent's home rank (9), unlike FEN's
 * display order. Red uses the board rank directly; Black uses 9 - rank. Every row is symmetric
 * across files, so both colors share the tables without a file reversal.
 *
 * These tables describe location only; they do not account for attacks, blocked horse legs, cannon
 * screens or game phase. Unspecified rows for palace-bound pieces and bishops are zero.
 */
static const int piece_square_values[XQ_PIECE_TYPE_NB][XQ_RANKS][XQ_FILES] =
    {
        [XQ_KING] =
            {
                {0, 0, 0, 0, 0, 0, 0, 0, 0},
                {0, 0, 0, -8, -4, -8, 0, 0, 0},
                {0, 0, 0, -16, -12, -16, 0, 0, 0},
            },
        [XQ_ADVISOR] =
            {
                {0, 0, 0, 0, 0, 0, 0, 0, 0},
                {0, 0, 0, 0, 6, 0, 0, 0, 0},
                {0, 0, 0, 0, 0, 0, 0, 0, 0},
            },
        [XQ_BISHOP] =
            {
                {0, 0, 0, 0, 0, 0, 0, 0, 0},
                {0, 0, 0, 0, 0, 0, 0, 0, 0},
                {2, 0, 0, 0, 6, 0, 0, 0, 2},
                {0, 0, 0, 0, 0, 0, 0, 0, 0},
                {0, 0, 2, 0, 0, 0, 2, 0, 0},
            },
        [XQ_KNIGHT] =
            {
                {-20, -8, -12, -8, -8, -8, -12, -8, -20},
                {-16, -4, 0, 4, 4, 4, 0, -4, -16},
                {-12, 0, 8, 12, 12, 12, 8, 0, -12},
                {-8, 4, 12, 20, 20, 20, 12, 4, -8},
                {-4, 8, 16, 24, 24, 24, 16, 8, -4},
                {-4, 8, 20, 28, 28, 28, 20, 8, -4},
                {-8, 8, 20, 28, 28, 28, 20, 8, -8},
                {-12, 4, 16, 24, 24, 24, 16, 4, -12},
                {-16, 0, 8, 12, 12, 12, 8, 0, -16},
                {-20, -8, -4, 0, 0, 0, -4, -8, -20},
            },
        [XQ_ROOK] =
            {
                {0, 0, 2, 4, 2, 4, 2, 0, 0},
                {2, 4, 6, 8, 6, 8, 6, 4, 2},
                {4, 6, 8, 10, 8, 10, 8, 6, 4},
                {4, 8, 10, 12, 10, 12, 10, 8, 4},
                {6, 8, 12, 14, 12, 14, 12, 8, 6},
                {6, 8, 12, 14, 12, 14, 12, 8, 6},
                {6, 8, 12, 16, 14, 16, 12, 8, 6},
                {8, 12, 16, 20, 18, 20, 16, 12, 8},
                {8, 12, 16, 20, 18, 20, 16, 12, 8},
                {4, 8, 12, 16, 14, 16, 12, 8, 4},
            },
        [XQ_CANNON] =
            {
                {-4, -2, 0, 2, 4, 2, 0, -2, -4},
                {0, 2, 4, 6, 8, 6, 4, 2, 0},
                {2, 4, 8, 10, 14, 10, 8, 4, 2},
                {2, 4, 6, 10, 12, 10, 6, 4, 2},
                {0, 2, 4, 8, 10, 8, 4, 2, 0},
                {0, 2, 4, 6, 8, 6, 4, 2, 0},
                {-2, 0, 2, 4, 6, 4, 2, 0, -2},
                {-2, 0, 2, 4, 6, 4, 2, 0, -2},
                {-4, -2, 0, 2, 4, 2, 0, -2, -4},
                {-8, -4, 0, 2, 4, 2, 0, -4, -8},
            },
        [XQ_PAWN] =
            {
                {0, 0, 0, 0, 0, 0, 0, 0, 0},
                {0, 0, 0, 0, 0, 0, 0, 0, 0},
                {0, 0, 0, 0, 0, 0, 0, 0, 0},
                {0, 0, 0, 0, 0, 0, 0, 0, 0},
                {4, 6, 8, 10, 12, 10, 8, 6, 4},
                {18, 22, 26, 30, 32, 30, 26, 22, 18},
                {24, 28, 36, 40, 44, 40, 36, 28, 24},
                {28, 32, 40, 46, 50, 46, 40, 32, 28},
                {24, 28, 36, 42, 46, 42, 36, 28, 24},
                {8, 12, 18, 22, 26, 22, 18, 12, 8},
            },
};

static const int legal_move_values[XQ_PIECE_TYPE_NB] = {
    [XQ_KING] = 10, [XQ_ADVISOR] = 10, [XQ_BISHOP] = 10, [XQ_KNIGHT] = 20,
    [XQ_ROOK] = 20, [XQ_CANNON] = 15,  [XQ_PAWN] = 10,
};

enum
{
    CAPTURE_ORDER_BASE = 100000,
    XQ_MAX_QUIESCENCE_DEPTH = 500,
    XQ_MAX_PV_MOVES = 64,
    XQ_MATE_SCORE = 30000,
    XQ_MATE_THRESHOLD = 29000,
    XQ_TT_BUCKET_SIZE = 4,
    XQ_TT_BUCKET_COUNT = 1 << 18
};

typedef struct ScoredMove
{
    XqMove move;
    int score;
    unsigned completed_depth;
} ScoredMove;

typedef struct SearchContext
{
    uint64_t deadline_ms;
    uint64_t nodes;
    bool time_limited;
    bool stopped;
    XqSearchTimeMode time_mode;
} SearchContext;

typedef struct PrincipalVariation
{
    XqMove moves[XQ_MAX_PV_MOVES];
    int count;
} PrincipalVariation;

typedef struct XqTranspositionEntry
{
    uint64_t key;
    int score;
    XqMove best_move;
    uint16_t depth;
    uint8_t generation;
    uint8_t score_kind;
} XqTranspositionEntry;

typedef struct XqTranspositionBucket
{
    XqTranspositionEntry entries[XQ_TT_BUCKET_SIZE];
} XqTranspositionBucket;

struct XqTranspositionTable
{
    XqTranspositionBucket *buckets; // size: 1 << 18
    uint8_t generation;
    XqTranspositionStats stats;
};

/**
 * @brief Reads the current monotonic clock and converts it to milliseconds.
 *
 * The monotonic clock is unaffected by changes to the system date, time zone or manual clock
 * adjustments, making it suitable for measuring search duration and deadlines.
 *
 * Windows uses a high-resolution performance counter, while other platforms use
 * `clock_gettime(CLOCK_MONOTONIC)`. Any fractional millisecond is discarded during conversion.
 *
 * @return Milliseconds elapsed since a platform-specific fixed reference point, or -1 if the clock
 *         cannot be read. This is not calendar time; use it to measure elapsed time or compare
 *         deadlines based on the same clock.
 */
static int64_t monotonic_time_ms(void)
{
#ifdef _WIN32
    LARGE_INTEGER counter;
    LARGE_INTEGER frequency;

    /*
     * Both `counter` and `frequency` use counter ticks as the unit for QuadPart, but their QuadPart
     * values have different meanings:
     *     - `counter`'s represents the total number of ticks accumulated so far.
     *     - `frequency`'s represents the number of ticks added per second by the high-resolution
     *       performance counter.
     */
    if (!QueryPerformanceFrequency(&frequency) || !QueryPerformanceCounter(&counter) ||
        frequency.QuadPart <= 0)
        return -1;
    return (int64_t)((long double)counter.QuadPart * 1000.0L / (long double)frequency.QuadPart);
#else
    struct timespec now;

    /*
     * Similar to the `_WIN32` branch, this retrieves the elapsed time since an unspecified fixed
     * reference point. The result consists of the seconds stored in `now.tv_sec` plus the
     * nanoseconds stored in `now.tv_nsec`. The final return value discards any fractional
     * millisecond. */
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return -1;
    return (int64_t)now.tv_sec * INT64_C(1000) + (int64_t)now.tv_nsec / INT64_C(1000000);
#endif
}

/**
 * @brief Reads `clock()` and converts its result to integer milliseconds.
 *
 * On implementations conforming to ISO C, `clock()` reports processor time consumed by the process
 * since an implementation-defined reference point associated with process startup. Time spent
 * sleeping or waiting without using the CPU is excluded. The Microsoft C runtime instead reports
 * elapsed wall-clock time since process startup, including time spent sleeping or waiting.
 *
 * Fractional milliseconds are discarded during conversion.
 *
 * @return The `clock()` reading converted to milliseconds, or `-1` if `clock()` returns
 *         `(clock_t)-1`. A reading of `0` is valid.
 */
static int64_t cpu_time_ms(void)
{
    clock_t now = clock();

    if (now == (clock_t)-1)
        return -1;
    return (int64_t)((double)now * 1000.0 / (double)CLOCKS_PER_SEC);
}

/**
 * @brief Reads the current time according to the search timing mode.
 *
 * Two modes support different search budgets: monotonic wall-clock time limits how long the user
 * waits, while CPU time budgets processor work, generally excluding time spent waiting or
 * descheduled. Wall-clock mode suits interactive play; CPU mode helps compare computation costs but
 * does not guarantee a real-time response deadline.
 *
 * @param mode Timing mode. `XQ_SEARCH_TIME_CPU` uses process CPU time; all other values use
 *             monotonic wall-clock time.
 * @return     The current time in milliseconds from the selected clock, or -1 if the underlying
 *             clock cannot be read.
 */
static int64_t search_time_ms(XqSearchTimeMode mode)
{
    return mode == XQ_SEARCH_TIME_CPU ? cpu_time_ms() : monotonic_time_ms();
}

/**
 * @brief Initializes the context used by a built-in search according to the search limits.
 *
 * The function resets the node count and stop state, normalizes the timing mode, and calculates the
 * absolute deadline when the time limit is nonzero. A zero time limit disables timeout checks.
 *
 * If the clock cannot be read for a timed search, the search is marked as stopped.
 *
 * @param context Search context to initialize; must not be null.
 * @param limits  Depth, time, bonus, and timing mode configuration; must not be null.
 */
static void search_context_init(SearchContext *context, const XqSearchLimits *limits)
{
    int64_t now;

    memset(context, 0, sizeof(*context));
    context->time_mode =
        limits->time_mode == XQ_SEARCH_TIME_CPU ? XQ_SEARCH_TIME_CPU : XQ_SEARCH_TIME_MONOTONIC;
    if (limits->time_limit_ms == 0)
        return;

    now = search_time_ms(context->time_mode);
    context->time_limited = true;
    if (now < 0)
    {
        context->stopped = true;
        return;
    }
    if (UINT64_MAX - (uint64_t)now < limits->time_limit_ms)
        context->deadline_ms = UINT64_MAX;
    else
        context->deadline_ms = (uint64_t)now + limits->time_limit_ms;
}

/**
 * @brief Checks whether the search has already stopped or has reached its time limit.
 *
 * A non-forced check reads the clock only when the node count is a multiple of 1024, reducing the
 * overhead of frequent system clock queries. A forced check reads the clock immediately. When a
 * timeout or clock read failure is detected, `stopped` is set to true, and subsequent calls
 * continue to report that the search has stopped.
 *
 * @param context The current search context. If null, the search is considered active and no time
 *                check is performed.
 * @param force   If true, checks the time immediately; if false, checks only when the node count is
 *                a multiple of 1024.
 * @return        Returns true if the search has already stopped, the time limit is reached, or the
 *                clock cannot be read; otherwise, returns false.
 */
static bool search_check_time(SearchContext *context, bool force)
{
    int64_t now;

    if (context == NULL || context->stopped || !context->time_limited)
        return context != NULL && context->stopped;
    if (!force && (context->nodes & UINT64_C(1023)) != 0)
        return false;
    now = search_time_ms(context->time_mode);
    if (now < 0 || (uint64_t)now >= context->deadline_ms)
        context->stopped = true;
    return context->stopped;
}

/**
 * @brief Records entry into a new search node and checks the time limit at node intervals.
 *
 * If context is not null, increments the node count and then performs a non-forced time check.
 * Calls that do not use a search context, such as explanatory searches, may pass null; in that
 * case, no nodes are counted and no time check is performed.
 *
 * @param context The current search context; may be null.
 * @return        If the search has already stopped or this node check detects a timeout, returns
 *                true; otherwise, returns false.
 */
static bool search_enter_node(SearchContext *context)
{
    if (context == NULL)
        return false;
    ++context->nodes;
    return search_check_time(context, false);
}

/**
 * @brief Creates and initializes a fixed-capacity transposition table on the heap.
 *
 * Returns null if creation fails. Call `xq_transposition_table_destroy()` to release the table when
 * it is no longer needed.
 *
 * @return A pointer to the newly created transposition table on success; null if memory allocation
 *         fails.
 */
XqTranspositionTable *xq_transposition_table_create(void)
{
    XqTranspositionTable *table = (XqTranspositionTable *)malloc(sizeof(*table));

    if (table == NULL)
        return NULL;

    table->buckets = (XqTranspositionBucket *)calloc(XQ_TT_BUCKET_COUNT, sizeof(*table->buckets));
    if (table->buckets == NULL)
    {
        free(table);
        return NULL;
    }

    table->generation = 0;
    memset(&table->stats, 0, sizeof(table->stats));
    return table;
}

/**
 * @brief Clears all entries in the transposition table and resets its generation and statistics.
 *
 * Does nothing if table is null.
 *
 * @param table The transposition table to clear; may be null.
 */
void xq_transposition_table_clear(XqTranspositionTable *table)
{
    if (table == NULL)
        return;

    memset(table->buckets, 0, XQ_TT_BUCKET_COUNT * sizeof(*table->buckets));
    table->generation = 0;
    memset(&table->stats, 0, sizeof(table->stats));
}

/**
 * @brief Destroys the transposition table and frees the heap memory used by its bucket array and
 *        the table itself.
 *
 * Does nothing if `table` is null.
 *
 * @param table The table whose bucket array and the table itself are both to be freed.
 */
void xq_transposition_table_destroy(XqTranspositionTable *table)
{
    if (table == NULL)
        return;
    free(table->buckets);
    free(table);
}

/**
 * @brief Resets all transposition table statistics counters to 0.
 *
 * Does nothing if `table` is null.
 *
 * @param table The transposition table all whose statistics counters are to be reset to 0.
 */
void xq_transposition_table_reset_stats(XqTranspositionTable *table)
{
    if (table != NULL)
        memset(&table->stats, 0, sizeof(table->stats));
}

/**
 * @brief Retrieves current statistics from the transposition table `table` and stores them in
 *        `stats`.
 *
 * Does nothing if `stats` is null. Resets `stats` to zero if `table` is null.
 *
 * @param table The transposition table to retrieve from.
 * @param stats The output parameter used for receiving statistics from `table`.
 */
void xq_transposition_table_get_stats(const XqTranspositionTable *table,
                                      XqTranspositionStats *stats)
{
    if (stats == NULL)
        return;
    if (table == NULL)
        memset(stats, 0, sizeof(*stats));
    else
        *stats = table->stats;
}

/**
 * @brief Creates a default limit configuration for the built-in search.
 *
 * The default configuration limits the search to depth 10 and 3 seconds, uses a monotonic wall
 * clock, and awards each root move a 70-point depth-confidence bonus per additional completed ply.
 *
 * @return The default limit configuration.
 */
XqSearchLimits xq_search_limits_default(void)
{
    XqSearchLimits limits;

    limits.max_depth = 10;
    limits.time_limit_ms = 3000;
    limits.depth_bonus = 70;
    limits.time_mode = XQ_SEARCH_TIME_MONOTONIC;
    return limits;
}

/**
 * @brief Increments the transposition table generation at the start of a new iterative deepening
 *        iteration.
 *
 * The `generation` field of `table` is a `uint8_t`, so it wraps around according to unsigned
 * integer rules when it reaches its maximum value.
 *
 * Does nothing if `table` is null.
 *
 * @param table The transposition table whose generation is incremented; may be null.
 */
static void tt_new_generation(XqTranspositionTable *table)
{
    if (table != NULL)
        ++table->generation;
}

/**
 * @brief Uses the low bits of `key` to index into the bucket array, then searches the four entries
 *        in the selected bucket for a valid entry whose full 64-bit key matches `key`.
 *
 * Two keys with the same low-bit index but different full keys constitute a bucket-index collision
 * and can be distinguished by comparing their full keys. An entry whose depth is zero is treated as
 * an empty slot and does not match anything.
 *
 * Different positions may generate the same 64-bit Zobrist key. The transposition table does not
 * store complete position data for secondary verification to avoid increasing entry size and
 * memory-access overhead. It therefore accepts the extremely low probability of such a collision.
 *
 * @param table The transposition table to search; may be null.
 * @param key   The full 64-bit position hash to look up.
 * @return      A pointer to the matching entry, or null if no matching entry exists.
 */
static XqTranspositionEntry *tt_find_entry(XqTranspositionTable *table, uint64_t key)
{
    XqTranspositionBucket *bucket;
    int i;

    if (table == NULL)
        return NULL;

    bucket = &table->buckets[key & (XQ_TT_BUCKET_COUNT - 1u)];
    for (i = 0; i < XQ_TT_BUCKET_SIZE; ++i)
        if (bucket->entries[i].depth != 0 && bucket->entries[i].key == key)
            return &bucket->entries[i];
    return NULL;
}

/**
 * @brief Converts a score relative to the current search root into a form suitable for storage in
 *        the transposition table.
 *
 * A mate score is relative to the search root, so its value depends on the current ply. The same
 * position reached via different paths may have different scores because it occurs at different ply
 * depths.
 *
 * Before storing the score in the transposition table, the function adds ply to a positive mate
 * score or subtracts ply from a negative mate score, thereby eliminating the effect of the path
 * length used to reach the current position. When the score is retrieved from the transposition
 * table, `score_from_tt()` uses the current ply at the point of lookup to restore the mate score
 * relative to the search root.
 *
 * Non-mate scores do not depend on ply and therefore remain unchanged.
 *
 * @param score Score obtained at the current search node.
 * @param ply   Number of plies from the search root to the current node.
 * @return      Normalized score suitable for storage in the transposition table.
 */
static int score_to_tt(int score, int ply)
{
    if (score >= XQ_MATE_THRESHOLD)
        return score + ply;
    if (score <= -XQ_MATE_THRESHOLD)
        return score - ply;
    return score;
}

/**
 * @brief Restores a transposition-table score for the current search ply.
 *
 * This function is the inverse of `score_to_tt()`. For mate scores, it restores the mate distance
 * according to the current node's distance from the search root. Ordinary position scores are left
 * unchanged.
 *
 * @param score Normalized score read from the transposition table.
 * @param ply   Number of plies from the search root to the current node.
 * @return      The score restored for use at the current ply.
 */
static int score_from_tt(int score, int ply)
{
    if (score >= XQ_MATE_THRESHOLD)
        return score - ply;
    if (score <= -XQ_MATE_THRESHOLD)
        return score + ply;
    return score;
}

/**
 * @brief Probes the transposition table and refreshes the generation of a matching entry.
 *
 * The function also returns the hash move and score kind when requested. It restores and accepts a
 * cached score only when the stored depth is sufficient and the score is either exact or a bound
 * that directly cuts off the current search.
 *
 * A false return value does not necessarily mean that the probe missed; a returned hash move may
 * still be used for move ordering.
 *
 * @param table         Transposition table to probe; may be null, in which case the function
 *                      returns false immediately.
 * @param key           Full 64-bit hash of the current position, including the side to move.
 * @param depth         Remaining search depth required at the current node.
 * @param alpha         Lower bound of the current alpha-beta search window.
 * @param beta          Upper bound of the current alpha-beta search window.
 * @param ply           Number of plies from the search root, used to restore mate scores.
 * @param score         Optional output; receives the reusable score at the current ply when the
 *                      function returns true.
 * @param hash_move     Optional output; receives the cached best move when an entry matches.
 * @param has_hash_move Optional output; receives whether a matching entry supplied a hash move.
 * @param score_kind    Optional output; receives the cached score's bound kind when an entry
 *                      matches.
 * @return              True if the cached score can be reused or can cut off the search; false
 *                      otherwise.
 */
static bool tt_probe(XqTranspositionTable *table, uint64_t key, unsigned depth, int alpha, int beta,
                     int ply, int *score, XqMove *hash_move, bool *has_hash_move,
                     XqSearchScoreKind *score_kind)
{
    XqTranspositionEntry *entry;
    int cached_score;

    if (has_hash_move != NULL)
        *has_hash_move = false;
    if (table == NULL)
        return false;

    ++table->stats.probes;
    entry = tt_find_entry(table, key);
    if (entry == NULL)
        return false;

    ++table->stats.hits;
    entry->generation = table->generation;
    if (hash_move != NULL)
        *hash_move = entry->best_move;
    if (has_hash_move != NULL)
        *has_hash_move = true;
    if (score_kind != NULL)
        *score_kind = (XqSearchScoreKind)entry->score_kind;

    if ((unsigned)entry->depth < depth)
        return false;

    cached_score = score_from_tt(entry->score, ply);
    if (entry->score_kind == XQ_SEARCH_SCORE_EXACT ||
        (entry->score_kind == XQ_SEARCH_SCORE_LOWER_BOUND && cached_score >= beta) ||
        (entry->score_kind == XQ_SEARCH_SCORE_UPPER_BOUND && cached_score <= alpha))
    {
        if (score != NULL)
            *score = cached_score;
        ++table->stats.cutoffs;
        return true;
    }

    return false;
}

/**
 * @brief Retrieves the hash move for the current position from the transposition table.
 *
 * A matching entry is refreshed to the current generation. Even when the cached depth is
 * insufficient or its score bound cannot produce a cutoff, `best_move` remains a useful ordering
 * hint. Consequently, this function does not inspect the stored depth, score kind, or alpha-beta
 * window.
 *
 * The caller validates the move against the current move list, so the hint affects only search
 * order, not correctness.
 *
 * @param table     Transposition table to query; may be null, in which case the function returns
 *                  false.
 * @param key       Full 64-bit hash of the current position, including the side to move.
 * @param hash_move Output that receives the cached best move; must not be null.
 * @return          True if a matching entry was found and written to `hash_move`; false otherwise.
 */
static bool tt_get_hash_move(XqTranspositionTable *table, uint64_t key, XqMove *hash_move)
{
    XqTranspositionEntry *entry;

    if (table == NULL || hash_move == NULL)
        return false;

    ++table->stats.probes;
    entry = tt_find_entry(table, key);
    if (entry == NULL)
        return false;

    ++table->stats.hits;
    entry->generation = table->generation;
    *hash_move = entry->best_move;
    return true;
}

/**
 * @brief Computes the retention score of a transposition-table entry.
 *
 * Entries with lower scores are more likely to be evicted after a bucket collision. The formula is
 * `8 * depth - 4 * age + type_bonus`, where an exact entry receives a bonus of 3. For example, an
 * exact entry of depth 5 from generation 7 has age 3 in generation 10, giving a retention score of
 * `8 * 5 - 4 * 3 + 3 = 31`.
 *
 * @param entry              Transposition-table entry to evaluate; must not be null.
 * @param current_generation Current generation of the transposition table.
 * @return                   The entry's retention score; a higher value makes the entry more
 *                           desirable to retain.
 */
static int tt_retention_score(const XqTranspositionEntry *entry, uint8_t current_generation)
{
    unsigned age = (uint8_t)(current_generation - entry->generation);
    int type_bonus = entry->score_kind == XQ_SEARCH_SCORE_EXACT ? 3 : 0;

    return (int)entry->depth * 8 - (int)age * 4 + type_bonus;
}

/**
 * @brief Stores a search result in the transposition table.
 *
 * The function prefers an empty slot or an entry with the same key. When the bucket is full, it
 * chooses the entry with the lowest retention score based on depth, age, and score kind, and
 * replaces it only if the incoming entry is not weaker.
 *
 * Quiescence nodes at depth zero are not cached because zero is reserved as the empty-entry marker.
 *
 * @param table      Transposition table in which to store the result; may be null, in which case
 *                   the function does nothing.
 * @param key        Full 64-bit hash of the current position, including the side to move.
 * @param depth      Remaining search depth at the current node; a value of zero is not stored.
 * @param score      Search score at the current ply; mate scores are normalized before storage.
 * @param ply        Number of plies from the search root to the current node.
 * @param score_kind Score kind: exact, lower bound, or upper bound.
 * @param best_move  Best move found while searching the current position.
 */
static void tt_store(XqTranspositionTable *table, uint64_t key, unsigned depth, int score, int ply,
                     XqSearchScoreKind score_kind, XqMove best_move)
{
    XqTranspositionBucket *bucket;
    XqTranspositionEntry incoming;
    XqTranspositionEntry *target = NULL;
    int weakest_score = INT_MAX;
    int incoming_score;
    int i;

    if (table == NULL || depth == 0)
        return;

    incoming.key = key;
    incoming.score = score_to_tt(score, ply);
    incoming.best_move = best_move;
    incoming.depth = depth > UINT16_MAX ? UINT16_MAX : (uint16_t)depth;
    incoming.generation = table->generation;
    incoming.score_kind = (uint8_t)score_kind;
    incoming_score = tt_retention_score(&incoming, table->generation);

    bucket = &table->buckets[key & (XQ_TT_BUCKET_COUNT - 1u)];
    for (i = 0; i < XQ_TT_BUCKET_SIZE; ++i)
    {
        XqTranspositionEntry *entry = &bucket->entries[i];
        int retention;

        if (entry->depth == 0)
        {
            target = entry;
            break;
        }

        retention = tt_retention_score(entry, table->generation);
        if (entry->key == key)
        {
            entry->generation = table->generation;
            if (incoming_score < retention)
                return;
            target = entry;
            break;
        }

        if (retention < weakest_score)
        {
            weakest_score = retention;
            target = entry;
        }
    }

    if (target == NULL)
        return;
    if (target->depth != 0 && target->key != key)
    {
        if (incoming_score < weakest_score)
            return;
        ++table->stats.replacements;
    }

    *target = incoming;
    ++table->stats.stores;
}

/**
 * @brief Computes the mobility value of all legal moves available to one side.
 *
 * @param pos   Position whose legal moves are evaluated; must not be null.
 * @param color Side for which to generate and score legal moves.
 * @return      Sum of the piece-specific mobility values for all generated legal moves.
 */
static int evaluate_legal_moves(const XqPosition *pos, XqColor color)
{
    XqPosition evaluation_pos = *pos;
    XqMoveList legal;
    int score = 0;
    int i;

    evaluation_pos.side_to_move = color;
    xq_generate_legal(&evaluation_pos, &legal);
    for (i = 0; i < legal.count; ++i)
        score += legal_move_values[xq_piece_type(legal.moves[i].piece)];
    return score;
}

/**
 * @brief Sums material and piece-square bonuses for one side without modifying the position.
 *
 * Black's ranks are reflected so both sides use the same tables relative to their own home rank.
 *
 * @param pos   Position to evaluate; must not be null and is left unchanged.
 * @param color Side whose pieces are evaluated; must be XQ_RED or XQ_BLACK.
 * @return      Total material value plus piece-square bonuses for the specified side, without
 *              subtracting the opponent's score.
 */
static int evaluate_side(const XqPosition *pos, XqColor color)
{
    int score = 0;
    int type;

    for (type = 0; type < XQ_PIECE_TYPE_NB; ++type)
    {
        XqBitboard remaining = pos->pieces[color][type];

        while (!xq_bb_is_empty(remaining))
        {
            XqSquare sq = xq_bb_first_square(remaining);
            int rank = xq_square_rank(sq);
            int file = xq_square_file(sq);

            xq_bb_clear(&remaining, sq);
            if (color == XQ_BLACK)
                rank = XQ_RANKS - 1 - rank;
            score += piece_values[type] + piece_square_values[type][rank][file];
        }
    }
    return score;
}

/**
 * @brief Evaluates a position from the requested side's perspective using the built-in evaluator.
 *
 * The evaluator adds a piece-square bonus to each piece's fixed material value, then subtracts the
 * opponent's total from the perspective side's total. Positive scores favor perspective.
 *
 * Legal-move mobility support is present but currently disabled.
 *
 * @param pos         Position to evaluate; must not be null.
 * @param perspective Side for which a positive score is favorable.
 * @param user        Unused user-data pointer, accepted for compatibility with `XqEvaluateFn`.
 * @return            Static position score from `perspective`'s point of view.
 */
int xq_engine_default_static_evaluate(const XqPosition *pos, XqColor perspective, void *user)
{
    XqColor opponent = xq_color_opponent(perspective);
    int score;

    (void)user;
    score = evaluate_side(pos, perspective) - evaluate_side(pos, opponent);
    // score += evaluate_legal_moves(pos, perspective);
    // score -= evaluate_legal_moves(pos, opponent);
    return score;
}

/**
 * @brief Evaluates a position through the configured engine adapter.
 *
 * If `engine` provides a custom static evaluator, the function invokes it with the adapter's user
 * data. Otherwise, it falls back to `xq_engine_default_static_evaluate()`.
 *
 * @param engine      Engine adapter that may provide a custom evaluator; may be null.
 * @param pos         Position to evaluate; must not be null.
 * @param perspective Side for which a positive score is favorable.
 * @return            Static position score from `perspective`'s point of view.
 */
static int static_evaluate(const XqEngineAdapter *engine, const XqPosition *pos,
                           XqColor perspective)
{
    if (engine != NULL && engine->static_evaluate != NULL)
        return engine->static_evaluate(pos, perspective, engine->user);
    return xq_engine_default_static_evaluate(pos, perspective, NULL);
}

/**
 * @brief Computes a move-ordering score using the built-in scoring function.
 *
 * Quiet moves receive a score of `0`. Captures use an MVV-LVA-style formula to place them before
 * quiet moves:
 *
 * `CAPTURE_ORDER_BASE + captured_piece_value * 16 - moving_piece_value`
 *
 * `CAPTURE_ORDER_BASE` ensures that captures as a group precede quiet moves. Multiplying the
 * captured piece's value by 16 generally prioritizes more valuable victims, while subtracting the
 * moving piece's value favors less valuable attackers when the victim is the same. For example, a
 * pawn capturing a rook is ordered before a rook capturing a pawn, and a pawn capturing a cannon is
 * ordered before a rook capturing a cannon.
 *
 * The factor 16 is an empirical weight and does not guarantee strict lexicographic MVV-LVA order.
 * This formula is not an evaluation of the resulting position; it only predicts promising moves so
 * that cutoffs occur earlier. It affects search efficiency but not the result of a complete
 * alpha-beta search.
 *
 * @param pos  Unused position pointer, accepted for compatibility with `XqMoveScoreFn`.
 * @param move Move for which to compute an ordering score.
 * @param user Unused user-data pointer, accepted for compatibility with `XqMoveScoreFn`.
 * @return     Ordering score; higher values receive higher search priority.
 */
int xq_engine_default_move_order_score(const XqPosition *pos, XqMove move, void *user)
{
    (void)pos;
    (void)user;

    if (move.captured == XQ_EMPTY_PIECE)
        return 0;

    return CAPTURE_ORDER_BASE + piece_values[xq_piece_type(move.captured)] * 16 -
           piece_values[xq_piece_type(move.piece)];
}

/**
 * @brief Computes a move-ordering score through the configured engine adapter.
 *
 * If `engine` provides a custom move-scoring callback, the function invokes it with the adapter's
 * user data. Otherwise, it falls back to `xq_engine_default_move_order_score()`.
 *
 * @param engine Engine adapter that may provide a custom move-scoring callback; may be `NULL`.
 * @param pos    Position in which `move` is being ordered; must not be `NULL` when a custom
 *               callback needs it.
 * @param move   Move for which to compute an ordering score.
 * @return       Ordering score; higher values receive higher search priority.
 */
static int move_order_score(const XqEngineAdapter *engine, const XqPosition *pos, XqMove move)
{
    if (engine != NULL && engine->score_move != NULL)
        return engine->score_move(pos, move, engine->user);
    return xq_engine_default_move_order_score(pos, move, NULL);
}

/**
 * @brief Sorts a move list by descending move-ordering score.
 *
 * Scores are computed once per move, and stable insertion sort preserves the relative order of
 * moves with equal scores.
 *
 * @param engine Engine adapter used to score moves; may be null.
 * @param pos    Position in which the moves are being ordered; must not be null.
 * @param list   Move list to reorder in place; must not be null.
 */
static void order_moves(const XqEngineAdapter *engine, const XqPosition *pos, XqMoveList *list)
{
    ScoredMove ordered[XQ_MAX_MOVES];
    int i;

    for (i = 0; i < list->count; ++i)
    {
        ScoredMove current;
        int j = i;

        current.move = list->moves[i];
        current.score = move_order_score(engine, pos, current.move);
        while (j > 0 && ordered[j - 1].score < current.score)
        {
            ordered[j] = ordered[j - 1];
            --j;
        }
        ordered[j] = current;
    }

    for (i = 0; i < list->count; ++i)
        list->moves[i] = ordered[i].move;
}

/**
 * @brief Sorts root moves stably by descending search score.
 *
 * Moves with equal scores retain their relative order from the previous iteration.
 *
 * @param moves Root-move array to reorder in place; must not be null.
 * @param count Number of initialized entries in `moves`.
 */
static void order_root_moves(ScoredMove *moves, int count)
{
    int i;

    for (i = 1; i < count; ++i)
    {
        ScoredMove current = moves[i];
        int j = i;

        while (j > 0 && moves[j - 1].score < current.score)
        {
            moves[j] = moves[j - 1];
            --j;
        }
        moves[j] = current;
    }
}

/**
 * @brief Tests whether two moves have the same source and destination squares.
 *
 * The cached piece and captured-piece fields are intentionally ignored.
 *
 * @param a First move to compare.
 * @param b Second move to compare.
 * @return  True if both moves have identical source and destination squares; false otherwise.
 */
static bool moves_equal(XqMove a, XqMove b)
{
    return a.from == b.from && a.to == b.to;
}

/**
 * @brief Moves a matching move to the front of a move list while preserving all other order.
 *
 * @param list Move list to modify in place; must not be null.
 * @param move Move to prioritize, matched by source and destination squares.
 * @return     True if the move was found and moved to the front; false otherwise.
 */
static bool prioritize_move(XqMoveList *list, XqMove move)
{
    int i;

    for (i = 0; i < list->count; ++i)
        if (moves_equal(list->moves[i], move))
        {
            XqMove prioritized = list->moves[i];

            while (i > 0)
            {
                list->moves[i] = list->moves[i - 1];
                --i;
            }
            list->moves[0] = prioritized;
            return true;
        }

    return false;
}

/**
 * @brief Builds the current node's principal variation from a move and its child's variation.
 *
 * The child variation is truncated when necessary so the resulting line fits within
 * `XQ_MAX_PV_MOVES`.
 *
 * @param pv       Output principal variation; must not be null.
 * @param move     Best move at the current node, placed at the start of the variation.
 * @param child_pv Child node's principal variation; may be null.
 */
static void build_principal_variation(PrincipalVariation *pv, XqMove move,
                                      const PrincipalVariation *child_pv)
{
    int child_count = child_pv != NULL ? child_pv->count : 0;
    int i;

    if (child_count > XQ_MAX_PV_MOVES - 1)
        child_count = XQ_MAX_PV_MOVES - 1;

    pv->moves[0] = move;
    for (i = 0; i < child_count; ++i)
        pv->moves[i + 1] = child_pv->moves[i];
    pv->count = child_count + 1;
}

/**
 * @brief Reconstructs a principal variation from sufficiently deep exact table entries.
 *
 * At each step, the function matches the cached best move by source and destination against the
 * current position's pseudo-legal move list, then uses the complete `XqMove` from that list. For an
 * identical position and a correctly stored entry, the move should be present and its `piece` and
 * `captured` fields should agree; cache age alone does not invalidate those fields.
 *
 * A missing move is unexpected and may indicate a full 64-bit hash collision, incorrect hash
 * updates during move making or unmaking, inconsistent move generation, or an incorrectly stored or
 * corrupted entry. A bucket-index collision alone cannot explain it because lookup also checks the
 * full key. As a defensive measure, reconstruction stops at the first unmatched move and keeps the
 * prefix already collected. Matching a pseudo-legal move does not verify that the moving side's
 * king remains safe, nor does it rule out a full hash collision.
 *
 * The function temporarily makes each move while reconstructing the line, then unmakes all moves
 * in reverse order before returning so that `pos` is restored.
 *
 * @param table Transposition table used to find subsequent exact entries; may be null, in which
 *              case an empty variation is produced.
 * @param pos   Position at the start of the principal variation; must not be null and is restored
 *              before the function returns.
 * @param depth Maximum remaining search depth to reconstruct.
 * @param pv    Output principal variation; must not be null and is cleared before reconstruction.
 */
static void build_pv_from_tt(XqTranspositionTable *table, XqPosition *pos, unsigned depth,
                             PrincipalVariation *pv)
{
    unsigned remaining = depth;
    int made_count = 0;

    pv->count = 0;
    while (remaining > 0 && pv->count < XQ_MAX_PV_MOVES)
    {
        XqTranspositionEntry *entry = tt_find_entry(table, xq_position_hash(pos));
        XqMoveList list;
        int i;

        if (entry == NULL || entry->score_kind != XQ_SEARCH_SCORE_EXACT ||
            (unsigned)entry->depth < remaining)
            break;

        xq_generate_pseudo_legal(pos, &list);
        for (i = 0; i < list.count; ++i)
            if (moves_equal(list.moves[i], entry->best_move))
                break;
        if (i == list.count)
            break;

        entry->generation = table->generation;
        pv->moves[pv->count++] = list.moves[i];
        xq_position_make_move(pos, list.moves[i]);
        ++made_count;
        --remaining;
    }

    for (int i = made_count - 1; i >= 0; --i)
        xq_position_unmake_move(pos, pv->moves[i]);
}

/**
 * @brief Orders moves for an explanation result and retains their ordering scores.
 *
 * This follows the same stable descending ordering as `order_moves()`, while also writing the score
 * associated with each reordered move to the parallel `scores` array.
 *
 * @param engine Engine adapter used to score moves; may be null.
 * @param pos    Position in which the moves are being ordered; must not be null.
 * @param list   Move list to reorder in place; must not be null.
 * @param scores Output array that receives one ordering score per reordered move; must have room
 *               for at least `list->count` entries.
 */
static void order_moves_for_explain(const XqEngineAdapter *engine, const XqPosition *pos,
                                    XqMoveList *list, int *scores)
{
    ScoredMove ordered[XQ_MAX_MOVES];
    int i;

    for (i = 0; i < list->count; ++i)
    {
        ScoredMove current;
        int j = i;

        current.move = list->moves[i];
        current.score = move_order_score(engine, pos, current.move);
        while (j > 0 && ordered[j - 1].score < current.score)
        {
            ordered[j] = ordered[j - 1];
            --j;
        }
        ordered[j] = current;
    }

    for (i = 0; i < list->count; ++i)
    {
        list->moves[i] = ordered[i].move;
        scores[i] = ordered[i].score;
    }
}

/**
 * @brief Performs quiescence search at a negamax leaf.
 *
 * Quiescence search reduces the horizon effect, which occurs when the normal search stops in a
 * tactically unstable part of the game tree. Captures and check evasions are currently treated as
 * the moves most likely to cause large score swings, so those are the continuations searched here.
 *
 * `depth` is the internal quiescence depth. It decreases by one at each recursive call, and the
 * function falls back to static evaluation after `XQ_MAX_QUIESCENCE_DEPTH` plies. `ply` is the
 * current node's distance from the root of the full search. It increases at each recursive call and
 * is used to encode mate distance.
 *
 * Before generating moves, an attacked opponent king is scored as a win by king capture on the
 * next ply. This rejects a preceding pseudo-legal move that left its own king attacked, even when
 * both kings are attacked, before legal check-evasion generation or stand pat can hide that fact.
 *
 * When the side to move is not in check, the static evaluation is used as the stand-pat score and
 * only captures are searched. When the side to move is in check, stand pat is not allowed and every
 * legal evasion is searched; no legal evasion means checkmate. The function uses fail-soft
 * alpha-beta semantics: it returns the best searched score, which may lie outside the original
 * window and represent an upper or lower bound rather than an exact value.
 *
 * @param engine  Engine adapter used for static evaluation and move ordering; may be null.
 * @param pos     Current position; must not be null and is restored before the function returns.
 * @param depth   Internal quiescence depth, decreasing from zero into negative values.
 * @param ply     Number of plies from the root of the full search to the current node.
 * @param alpha   Lower bound of the current alpha-beta search window.
 * @param beta    Upper bound of the current alpha-beta search window.
 * @param context Search context used for node counting and time control; may be null.
 * @return        Quiescence score from the side-to-move perspective, or zero if the search is
 *                stopped by the time limit.
 */
static int quiescence(const XqEngineAdapter *engine, XqPosition *pos, int depth, int ply, int alpha,
                      int beta, SearchContext *context)
{
    XqMoveList list;
    bool in_check;
    int best = INT_MIN / 2;
    int stand_pat;
    int i;

    if (search_enter_node(context))
        return 0;

    if (xq_position_in_check(pos, xq_color_opponent(pos->side_to_move)))
        return XQ_MATE_SCORE - (ply + 1);

    in_check = xq_position_in_check(pos, pos->side_to_move);
    if (in_check)
        xq_generate_legal(pos, &list);
    else
        xq_generate_pseudo_legal(pos, &list);
    if (list.count == 0)
        return -XQ_MATE_SCORE + ply + 2;

    if (depth <= -XQ_MAX_QUIESCENCE_DEPTH)
        return static_evaluate(engine, pos, pos->side_to_move);

    if (!in_check)
    {
        stand_pat = static_evaluate(engine, pos, pos->side_to_move);
        best = stand_pat;
        if (stand_pat >= beta)
            return stand_pat;
        if (stand_pat > alpha)
            alpha = stand_pat;
    }

    order_moves(engine, pos, &list);

    for (i = 0; i < list.count; ++i)
    {
        int score;

        if (!in_check && list.moves[i].captured == XQ_EMPTY_PIECE)
            continue;

        xq_position_make_move(pos, list.moves[i]);
        score = -quiescence(engine, pos, depth - 1, ply + 1, -beta, -alpha, context);
        xq_position_unmake_move(pos, list.moves[i]);
        if (context != NULL && context->stopped)
            return 0;
        if (score > best)
            best = score;
        if (score >= beta)
            return best;
        if (score > alpha)
            alpha = score;
    }

    return best;
}

/**
 * @brief Searches a position with negamax and alpha-beta pruning.
 *
 * Minimax is the standard search for the optimal score of one side in a zero-sum game. It selects
 * the best move on that side's turn and the worst move on the opponent's turn, where "worst" means
 * optimal for an opponent who is assumed to play correctly.
 *
 * Consider the following tree before examining alpha-beta pruning in detail:
 *
 * MAX :       (1:>=x)
 *             /     \
 * MIN :    (2:x)  (3:!(<=x),<=y)
 *                   /           \
 * MAX :          (4:y)      (5:!(<=x),!(>=y))
 *
 * Suppose node 2 has score `x` and node 4 has score `y`. Node 2 establishes that node 1 has a score
 * of at least `x`; consequently, node 3 cannot change the result if its score is at most `x`. In
 * the diagram, `!(expression)` means that satisfying `expression` cannot affect the result already
 * found, so node 3 is marked `!(<=x)`. Node 4 also establishes that node 3 has a score no greater
 * than `y`. Combining this with `!(<=x)` shows that node 5 cannot affect the result if its score is
 * either at most `x` or at least `y`. Thus the alpha and beta values used to search node 5 are `x`
 * and `y`, respectively.
 *
 * More specifically, while evaluating a MAX node, each completed child can only raise the known
 * lower bound of its parent. For example, after node 2 produces `x`, node 1 is known to be at least
 * `x`. The remaining children of a MAX node can therefore be skipped only after one child raises
 * the parent score to at least `beta`. This is the beta cutoff. If a child's score instead falls
 * inside
 * `(alpha, beta)`, the intersection between the parent's newly established range and the search
 * window remains inside `(alpha, beta)`, so `alpha` can be raised to that child's score. The logic
 * for a MIN node is symmetric with signs reversed.
 *
 * Negamax simplifies the minimax implementation by negating the opponent's position score, thereby
 * expressing both sides as maximizers with the same code path.
 *
 * Compared with unpruned minimax, alpha-beta uses the `alpha` and `beta` bounds to skip irrelevant
 * branches. The tradeoff is that when the true score lies outside the search window, the returned
 * value may be only an upper or lower bound sufficient for pruning and decision-making rather than
 * an exact score.
 *
 * The function probes the transposition table before generating moves, orders a valid hash move and
 * the previous iteration's principal-variation move first, and stores the resulting exact score or
 * bound after the node has been searched.
 *
 * Time control is shared across recursive calls, including quiescence search, through `context`.
 * On entry, `search_enter_node()` counts the node and checks for a stop request. With a time limit
 * enabled, it reads the configured clock every 1024 nodes and sets `context->stopped` when the
 * deadline is reached. These periodic checks do not guarantee an exact cutoff at the deadline.
 * A null context disables node counting and time checks; a context with no time limit still counts
 * nodes and honors an existing stop request.
 *
 * After each recursive child search, the move is undone before checking `context->stopped`. If
 * stopped, the function returns zero immediately, without using the child's score or storing the
 * interrupted node in the transposition table. The zero is an interruption placeholder, not a
 * draw evaluation: callers must check `context->stopped` and discard the score and any partial
 * `pv_out`. The root search then selects a move from previously completed search results.
 *
 * @param engine        Engine adapter used for static evaluation and move ordering; may be null.
 * @param table         Transposition table used for probing, move ordering, and storage; may be
 *                      null.
 * @param pos           Current position; must not be null and is restored before the function
 *                      returns.
 * @param depth         Remaining normal-search depth. At zero, quiescence search is entered.
 * @param ply           Number of plies from the search root to the current node.
 * @param alpha         Lower bound of the current alpha-beta search window.
 * @param beta          Upper bound of the current alpha-beta search window.
 * @param pv_hint       Optional principal-variation moves from the previous iteration.
 * @param pv_hint_count Number of valid moves available in `pv_hint`.
 * @param pv_out        Optional output for the best principal variation found at this node.
 * @param context       Search context used for node counting and time control; may be null.
 * @return              Search score from the side-to-move perspective, or zero if the search is
 *                      stopped by the time limit.
 */
static int negamax(const XqEngineAdapter *engine, XqTranspositionTable *table, XqPosition *pos,
                   unsigned depth, int ply, int alpha, int beta, const XqMove *pv_hint,
                   int pv_hint_count, PrincipalVariation *pv_out, SearchContext *context)
{
    XqMoveList list;
    int best = INT_MIN / 2;
    int alpha_original = alpha;
    uint64_t key;
    XqMove hash_move = {0};
    XqMove node_best_move = {0};
    bool has_hash_move = false;
    bool has_best_move = false;
    bool pv_move_found = false;
    XqSearchScoreKind cached_kind = XQ_SEARCH_SCORE_UPPER_BOUND;
    int cached_score;
    int i;

    if (pv_out != NULL)
        pv_out->count = 0;

    if (search_enter_node(context))
        return 0;

    if (xq_position_king_square(pos, pos->side_to_move) == XQ_NO_SQUARE)
        return -XQ_MATE_SCORE + ply;

    if (depth == 0)
        return quiescence(engine, pos, 0, ply, alpha, beta, context);

    key = xq_position_hash(pos);
    if (tt_probe(table, key, depth, alpha, beta, ply, &cached_score, &hash_move, &has_hash_move,
                 &cached_kind))
    {
        if (pv_out != NULL && cached_kind == XQ_SEARCH_SCORE_EXACT)
            build_pv_from_tt(table, pos, depth, pv_out);
        return cached_score;
    }

    xq_generate_pseudo_legal(pos, &list);
    if (list.count == 0)
        return -XQ_MATE_SCORE + ply + 2;
    order_moves(engine, pos, &list);

    /* For example, suppose the requested depth is 6, the window is [50, 100], and the table
     * contains an older entry with depth=8, LOWER_BOUND=80, and best_move=A. Because 80 < beta, the
     * cached score cannot produce a cutoff, but the entry still supplies hash_move=A. If the first
     * move of the previous iteration's complete PV is B, then pv_hint[0]=B and the hash move
     * differs from the PV move. */
    if (has_hash_move)
        (void)prioritize_move(&list, hash_move);
    if (pv_hint != NULL && pv_hint_count > 0)
        pv_move_found = prioritize_move(&list, pv_hint[0]);

    for (i = 0; i < list.count; ++i)
    {
        PrincipalVariation child_pv;
        const XqMove *child_hint = NULL;
        int child_hint_count = 0;
        int score;

        if (pv_move_found && moves_equal(list.moves[i], pv_hint[0]))
        {
            child_hint = pv_hint + 1;
            child_hint_count = pv_hint_count - 1;
        }

        xq_position_make_move(pos, list.moves[i]);
        score = -negamax(engine, table, pos, depth - 1, ply + 1, -beta, -alpha, child_hint,
                         child_hint_count, pv_out != NULL ? &child_pv : NULL, context);
        xq_position_unmake_move(pos, list.moves[i]);
        if (context != NULL && context->stopped)
            return 0;
        if (score > best)
        {
            best = score;
            node_best_move = list.moves[i];
            has_best_move = true;
            if (pv_out != NULL)
                build_principal_variation(pv_out, list.moves[i], &child_pv);
        }
        if (score > alpha)
            alpha = score;
        if (alpha >= beta)
            break;
    }

    if (has_best_move)
    {
        XqSearchScoreKind score_kind;

        if (best <= alpha_original)
            score_kind = XQ_SEARCH_SCORE_UPPER_BOUND;
        else if (best >= beta)
            score_kind = XQ_SEARCH_SCORE_LOWER_BOUND;
        else
            score_kind = XQ_SEARCH_SCORE_EXACT;
        tt_store(table, key, depth, best, ply, score_kind, node_best_move);
    }

    return best;
}

/**
 * @brief Selects a root move after the search has timed out.
 *
 * Each candidate receives an adjusted score of `score + completed_depth * depth_bonus`. Ties in
 * adjusted score favor the move searched to a greater completed depth. If both score and depth are
 * tied, the earlier move in the list is kept.
 *
 * Moves with a `completed_depth` of zero do not participate. If no move completed a search, the
 * first move in the list is used as a fallback.
 *
 * @param moves       Root moves and their most recently completed scores and depths; must not be
 *                    null.
 * @param count       Number of root moves; must be greater than zero.
 * @param depth_bonus Confidence bonus added for each completed ply of search.
 * @return            Root move selected by adjusted score and the stable tie-breaking rules.
 */
static XqMove select_timed_root_move(const ScoredMove *moves, int count, int depth_bonus)
{
    int best_index = -1;
    int64_t best_adjusted = INT64_MIN;
    int i;

    for (i = 0; i < count; ++i)
    {
        int64_t adjusted;

        if (moves[i].completed_depth == 0)
            continue;
        adjusted = (int64_t)moves[i].score + (int64_t)moves[i].completed_depth * depth_bonus;
        if (best_index < 0 || adjusted > best_adjusted ||
            (adjusted == best_adjusted &&
             moves[i].completed_depth > moves[best_index].completed_depth))
        {
            best_index = i;
            best_adjusted = adjusted;
        }
    }

    return moves[best_index >= 0 ? best_index : 0].move;
}

/**
 * @brief Computes the rolling hash of the move sequence between two recorded positions.
 *
 * Position indices are used as boundaries: the path includes moves in entries `begin + 1` through
 * `end`. For example, begin=2 and end=5 selects the three moves in entries 3, 4, and 5.
 *
 * Let H[i] be the prefix hash through entry i and B the hash base. Subtracting `H[begin] * B^(end -
 * begin)` from `H[end]` removes the earlier moves and leaves the requested path hash. Each entry's
 * `power` stores B raised to its index, so the calculation takes constant time without traversing
 * moves. Unsigned arithmetic intentionally wraps modulo 2^64.
 *
 * Equal-length identical paths have equal hashes regardless of their offsets in history. Hash
 * equality alone does not prove path equality; callers must compare the moves to exclude hash
 * collisions and check position hashes separately when detecting a closed cycle.
 *
 * @param history Recorded game history; must not be null and is left unchanged.
 * @param begin   Starting position index; must satisfy begin <= end.
 * @param end     Ending position index; must be less than history->count.
 * @return        Hash of the selected move sequence, or zero for an empty path (begin == end).
 */
static uint64_t history_path_hash(const XqHistory *history, size_t begin, size_t end)
{
    return history->entries[end].prefix -
           history->entries[begin].prefix * history->entries[end - begin].power;
}

/**
 * @brief Determines whether completing a detected threefold cycle makes the moving side lose.
 *
 * The caller has already verified that appending candidate would complete three consecutive,
 * identical move sequences whose four boundary positions have matching hashes. This function
 * applies the loss rule only; it does not detect cycles or play the candidate move.
 *
 * The current rule always returns true, making the side to move in root lose for closing the third
 * cycle. Keeping this decision separate allows future rules to inspect the cycle, for example to
 * require that every move by the closing side gives check before declaring a loss.
 *
 * History contains only real moves, and root is the position before candidate. The third cycle's
 * final move is supplied separately as candidate and is not yet present in history. A future rule
 * can use a copy of root and the recorded moves to reconstruct and inspect the cycle positions.
 *
 * @param history   Real-game history ending at root; must not be null and is left unchanged.
 * @param start     Position index at the beginning of the first of the three cycles.
 * @param length    Number of individual moves in one cycle; a positive even number satisfying start
 *                  + 3 * length == history->count.
 * @param root      Position before candidate is played; must not be null and is left unchanged.
 * @param candidate Legal root move that would close the third identical cycle.
 * @return          True if the side playing candidate loses under the cycle rule. The current
 *                  implementation always returns true; future rules may return false to allow it.
 */
static bool root_cycle_is_loss(const XqHistory *history, size_t start, size_t length,
                               const XqPosition *root, XqMove candidate)
{
    (void)history;
    (void)start;
    (void)length;
    (void)root;
    (void)candidate;
    return true;
}

/**
 * @brief Removes legal root moves that would complete a cycle penalized as a loss.
 *
 * Called once before iterative deepening, this function examines only real-game history plus a
 * candidate root move. It does not detect repetitions inside negamax or quiescence search. Null,
 * empty, or mismatched history leaves the move list unchanged; matching requires the last recorded
 * position hash to equal the current position hash.
 *
 * The virtual position index after the candidate is `end = history->count`. For each possible even
 * cycle length, the first cycle starts at `end - 3 * length`. Only even lengths can return to the
 * same side to move. The first three boundary positions must have equal hashes, and prefix hashes
 * must match for the first two complete paths and for the third path without its final move. That
 * missing move must match the last move of the first path in the supplied legal move list.
 *
 * Hash matches are checked move by move using source and destination squares. The candidate is then
 * temporarily played to verify the fourth boundary position hash and immediately unmade.
 * `root_cycle_is_loss()` decides whether the verified cycle penalizes the moving side, keeping rule
 * changes separate from cycle detection. Candidates are never appended to real history.
 *
 * Prefix-hash screening takes O(history->count) time; matching paths additionally require move
 * lookup and exact comparison. Rejected moves are removed in place while preserving the relative
 * order of surviving moves. The list may become empty; choosing a fallback is the caller's job.
 *
 * @param history Optional real-game history ending at pos; may be null and is left unchanged.
 * @param pos     Root position; must not be null and is restored after each temporary move.
 * @param list    Legal root moves to filter in place; must not be null. Both moves and count are
 *                updated when candidates are rejected.
 * @return        True if at least one move was removed; false if no move was removed, including
 *                when history is absent, empty, or does not match the root position.
 */
static bool filter_root_cycles(const XqHistory *history, XqPosition *pos, XqMoveList *list)
{
    bool rejected[XQ_MAX_MOVES] = {false};
    size_t end;
    size_t length;
    int count = 0;
    int original_count = list->count;

    if (history == NULL || history->count == 0 ||
        history->entries[history->count - 1].key != xq_position_hash(pos))
        return false;

    end = history->count;
    for (length = 2; length <= end / 3; length += 2)
    {
        size_t start = end - 3 * length;
        size_t second = start + length;
        size_t third = second + length;
        uint64_t key = history->entries[start].key;
        size_t j;
        int i;
        bool closed;

        if (history->entries[second].key != key || history->entries[third].key != key ||
            history_path_hash(history, start, second) !=
                history_path_hash(history, second, third) ||
            history_path_hash(history, start, second - 1) !=
                history_path_hash(history, third, end - 1))
            continue;

        /* The missing move must equal the last move of the first period. */
        for (i = 0; i < list->count; ++i)
            if (!rejected[i] && moves_equal(list->moves[i], history->entries[second].move))
                break;
        if (i == list->count)
            continue;

        for (j = 1; j <= length; ++j)
            if (!moves_equal(history->entries[start + j].move, history->entries[second + j].move) ||
                (j < length &&
                 !moves_equal(history->entries[start + j].move, history->entries[third + j].move)))
                break;
        if (j <= length)
            continue;

        xq_position_make_move(pos, list->moves[i]);
        closed = xq_position_hash(pos) == key;
        xq_position_unmake_move(pos, list->moves[i]);
        if (closed && root_cycle_is_loss(history, start, length, pos, list->moves[i]))
            rejected[i] = true;
    }

    for (int i = 0; i < list->count; ++i)
        if (!rejected[i])
            list->moves[count++] = list->moves[i];
    list->count = count;
    return count != original_count;
}

/**
 * @brief Runs the built-in iterative-deepening search.
 *
 * The function searches only legal root moves from depth 1 through the requested maximum depth.
 * Scores from all root moves in the previous completed iteration determine their ordering in the
 * next iteration, while the previous principal variation supplies an additional ordering hint.
 *
 * If the time limit expires during an iteration, the function selects among the root moves whose
 * searches completed.
 *
 * Before iterative deepening, real-game history excludes moves that close three identical cycles.
 * If every legal move loses this way, the original first move is returned without searching.
 *
 * @param engine    Engine adapter used for evaluation, move ordering, and optional transposition
 *                  table access; may be null.
 * @param pos       Position to search; must not be null and is restored before the function
 *                  returns.
 * @param limits    Depth, time, depth-bonus, and clock-mode configuration; must not be null.
 * @param best_move Output that receives the selected move; must not be null.
 * @param stats     Optional, zero-initialized output for built-in search counters.
 * @return          True if at least one root move exists and a move is selected; false if the
 *                  position is invalid for searching or contains no legal moves.
 */
static bool builtin_search(const XqEngineAdapter *engine, XqPosition *pos,
                           const XqSearchLimits *limits, XqMove *best_move, XqSearchStats *stats)
{
    XqMoveList list;
    ScoredMove root_moves[XQ_MAX_MOVES];
    PrincipalVariation previous_pv = {0};
    XqTranspositionTable *table = engine != NULL ? engine->transposition_table : NULL;
    uint64_t root_key;
    XqMove hash_move;
    SearchContext context;
    bool filtered_cycles;
    unsigned depth = limits->max_depth == 0 ? 1 : limits->max_depth;
    unsigned current_depth;
    int i;

    if (pos == NULL || xq_position_king_square(pos, pos->side_to_move) == XQ_NO_SQUARE)
        return false;

    xq_generate_legal(pos, &list);
    if (list.count == 0)
        return false;
    order_moves(engine, pos, &list);
    root_key = xq_position_hash(pos);
    tt_new_generation(table);
    if (tt_get_hash_move(table, root_key, &hash_move))
        (void)prioritize_move(&list, hash_move);

    search_context_init(&context, limits);
    if (stats != NULL)
        stats->stopped = context.stopped;

    *best_move = list.moves[0];
    filtered_cycles = filter_root_cycles(engine != NULL ? engine->history : NULL, pos, &list);
    if (list.count == 0)
        return true; /* All legal moves lose: retain the original first move. */

    for (i = 0; i < list.count; ++i)
    {
        root_moves[i].move = list.moves[i];
        root_moves[i].score = INT_MIN / 2;
        root_moves[i].completed_depth = 0;
    }
    *best_move = root_moves[0].move;

    for (current_depth = 1;; ++current_depth)
    {
        PrincipalVariation current_pv = {0};
        int alpha = INT_MIN / 2;
        int beta = INT_MAX / 2;
        int completed_roots = 0;

        if (current_depth > 1)
            tt_new_generation(table);

        for (i = 0; i < list.count; ++i)
        {
            PrincipalVariation child_pv;
            const XqMove *child_hint = NULL;
            int child_hint_count = 0;
            int score;

            if (search_check_time(&context, true))
                break;

            if (stats != NULL)
                stats->max_started_depth = current_depth;
            if (previous_pv.count > 0 && moves_equal(root_moves[i].move, previous_pv.moves[0]))
            {
                child_hint = previous_pv.moves + 1;
                child_hint_count = previous_pv.count - 1;
            }

            xq_position_make_move(pos, root_moves[i].move);
            score = -negamax(engine, table, pos, current_depth - 1, 1, -beta, -alpha, child_hint,
                             child_hint_count, &child_pv, &context);
            xq_position_unmake_move(pos, root_moves[i].move);

            if (context.stopped)
                break;

            root_moves[i].score = score;
            root_moves[i].completed_depth = current_depth;
            ++completed_roots;
            if (score > alpha)
            {
                alpha = score;
                build_principal_variation(&current_pv, root_moves[i].move, &child_pv);
            }

            if (search_check_time(&context, true))
                break;
        }

        /* The clock may expire just after the last root move completed. */
        if (stats != NULL && completed_roots == list.count)
            stats->completed_depth = current_depth;

        if (context.stopped)
        {
            *best_move = select_timed_root_move(root_moves, list.count, limits->depth_bonus);
            break;
        }

        order_root_moves(root_moves, list.count);
        *best_move = root_moves[0].move;
        previous_pv = current_pv;
        /* A history-dependent root result must not enter the position-only cache. */
        if (!filtered_cycles)
            tt_store(table, root_key, current_depth, root_moves[0].score, 0, XQ_SEARCH_SCORE_EXACT,
                     root_moves[0].move);

        if (current_depth == depth)
            break;
    }

    if (stats != NULL)
    {
        stats->nodes = context.nodes;
        stats->stopped = context.stopped;
        for (i = 0; i < list.count; ++i)
            if (moves_equal(root_moves[i].move, *best_move))
            {
                stats->selected_move_depth = root_moves[i].completed_depth;
                break;
            }
    }
    return true;
}

/**
 * @brief Finds the best move using a custom search callback or the built-in search.
 *
 * Default search limits are created only when `limits` is null. Otherwise, the supplied limits
 * are used. A maximum depth of zero is normalized to one. If `engine` provides a custom search
 * callback, it receives the normalized maximum depth and manages its own time limit.
 *
 * Otherwise, `builtin_search()` is used.
 *
 * @param engine    Engine adapter that may provide a custom search callback; may be null.
 * @param pos       Position to search. The built-in search requires a non-null position and
 *                  restores it before returning.
 * @param limits    Optional search limits; null selects the default configuration.
 * @param best_move Output that receives the selected move; must not be null.
 * @return          True if a best move was found and written; false on invalid input, when no move
 *                  is available, or when the custom search callback fails.
 */
bool xq_engine_find_best_move(const XqEngineAdapter *engine, XqPosition *pos,
                              const XqSearchLimits *limits, XqMove *best_move)
{
    return xq_engine_find_best_move_with_stats(engine, pos, limits, best_move, NULL);
}

/**
 * @brief Finds the best move and optionally collects per-call search statistics.
 *
 * Default search limits are used when `limits` is null, and a maximum depth of zero is
 * normalized to one. If `engine` provides a custom search callback, it receives the normalized
 * maximum depth and manages its own time limit. Otherwise, `builtin_search()` is used; a null
 * engine selects the default evaluation and move ordering without a transposition table or
 * game history.
 *
 * When supplied, `stats` is initialized even if the search fails. Internal search counters are
 * available only for the built-in search. Cache statistics are per-call counter differences
 * when the built-in search uses a transposition table; collecting them does not reset the cache
 * or its cumulative counters. Elapsed time is measured with the monotonic clock for either
 * search path when clock readings succeed, independently of the configured search time mode.
 *
 * @param engine    Engine adapter that may provide custom callbacks, a cache, and history;
 *                  may be null to use the built-in defaults.
 * @param pos       Position to search. The built-in search requires a non-null position and
 *                  restores it before returning.
 * @param limits    Optional search limits; null selects the default configuration.
 * @param best_move Output that receives the selected move; must not be null.
 * @param stats     Optional output statistics; null disables diagnostics collection. Check the
 *                  availability flags before using internal counters, elapsed time, or cache data.
 * @return          True if a best move was found and written; false on invalid input, when no move
 *                  is available, or when the custom search callback fails.
 */
bool xq_engine_find_best_move_with_stats(const XqEngineAdapter *engine, XqPosition *pos,
                                         const XqSearchLimits *limits, XqMove *best_move,
                                         XqSearchStats *stats)
{
    XqSearchLimits effective_limits;
    XqTranspositionStats before;
    bool custom = engine != NULL && engine->search != NULL;
    XqTranspositionTable *table =
        stats != NULL && !custom && engine != NULL ? engine->transposition_table : NULL;
    bool found;
    int64_t start = -1;

    if (stats != NULL)
    {
        memset(stats, 0, sizeof(*stats));
        stats->available = !custom;
        stats->cache_available = !custom && table != NULL;
    }

    if (best_move == NULL)
        return false;
    effective_limits = limits != NULL ? *limits : xq_search_limits_default();
    if (effective_limits.max_depth == 0)
        effective_limits.max_depth = 1;
    if (stats != NULL)
    {
        start = monotonic_time_ms();
        xq_transposition_table_get_stats(table, &before);
    }
    if (custom)
        found = engine->search(pos, effective_limits.max_depth, best_move, engine->user);
    else
        found = builtin_search(engine, pos, &effective_limits, best_move, stats);
    if (stats != NULL)
    {
        int64_t end = monotonic_time_ms();
        if (start >= 0 && end >= start)
        {
            stats->elapsed_available = true;
            stats->elapsed_ms = (uint64_t)(end - start);
        }
        if (stats->cache_available)
        {
            XqTranspositionStats after;
            xq_transposition_table_get_stats(table, &after);
            stats->cache.probes = after.probes - before.probes;
            stats->cache.hits = after.hits - before.hits;
            stats->cache.cutoffs = after.cutoffs - before.cutoffs;
            stats->cache.stores = after.stores - before.stores;
            stats->cache.replacements = after.replacements - before.replacements;
        }
    }
    return found;
}

/**
 * @brief Explains one ply of the normal search at the current position.
 *
 * Moves are visited in search order. For each visited move, the result records the alpha value
 * before the move, beta, recursive score, ordering score, bound kind, and whether the move became
 * best or caused a cutoff. The recursive searches do not use a transposition table or time limit.
 *
 * The adapter's custom search callback is not invoked; only its evaluation and move-ordering
 * callbacks are relevant.
 *
 * @param engine Engine adapter used for evaluation and move ordering; may be null.
 * @param pos    Position to explain; must not be null and is restored before the function returns.
 * @param depth  Remaining normal-search depth; zero is normalized to one.
 * @param result Output explanation structure; must not be null and is initialized by the function.
 * @return       True if at least one move was explained; false on invalid input or when no move is
 *               available.
 */
bool xq_engine_explain_search_one_ply(const XqEngineAdapter *engine, XqPosition *pos,
                                      unsigned depth, XqExplainResult *result)
{
    XqMoveList list;
    int order_scores[XQ_MAX_MOVES];
    int alpha = INT_MIN / 2;
    int beta = INT_MAX / 2;
    int i;

    if (result == NULL)
        return false;

    result->count = 0;
    result->best_index = -1;
    result->final_score = INT_MIN / 2;

    if (pos == NULL)
        return false;
    if (depth == 0)
        depth = 1;
    if (xq_position_king_square(pos, pos->side_to_move) == XQ_NO_SQUARE)
        return false;

    xq_generate_pseudo_legal(pos, &list);
    if (list.count == 0)
        return false;
    order_moves_for_explain(engine, pos, &list, order_scores);

    for (i = 0; i < list.count; ++i)
    {
        XqExplainedMove *explained = &result->moves[result->count];
        int alpha_before = alpha;
        int score;

        xq_position_make_move(pos, list.moves[i]);
        score = -negamax(engine, NULL, pos, depth - 1, 1, -beta, -alpha, NULL, 0, NULL, NULL);
        xq_position_unmake_move(pos, list.moves[i]);

        explained->move = list.moves[i];
        explained->depth = depth;
        explained->alpha_before = alpha_before;
        explained->beta = beta;
        explained->score = score;
        explained->order_score = order_scores[i];
        explained->score_kind = XQ_SEARCH_SCORE_UPPER_BOUND;
        explained->is_best = false;
        explained->caused_cutoff = false;

        if (score > alpha)
        {
            alpha = score;
            result->best_index = result->count;
            explained->score_kind = XQ_SEARCH_SCORE_EXACT;
        }
        if (alpha >= beta)
        {
            explained->score_kind = XQ_SEARCH_SCORE_LOWER_BOUND;
            explained->caused_cutoff = true;
            ++result->count;
            break;
        }

        ++result->count;
    }

    result->final_score = alpha;
    if (result->best_index >= 0 && result->best_index < result->count)
        result->moves[result->best_index].is_best = true;
    return result->count > 0;
}

/**
 * @brief Explains one ply of quiescence search at the current position.
 *
 * An attacked opponent king immediately produces a winning score, matching quiescence search.
 * Otherwise, the result exposes the stand-pat evaluation when it is legal, then records the
 * captures or legal check evasions that quiescence search actually examines.
 *
 * Each move's score is still computed by a recursive call to `quiescence()` so that the explanation
 * remains consistent with the real search.
 *
 * @param engine Engine adapter used for static evaluation and move ordering; may be null.
 * @param pos    Position to explain; must not be null and is restored before the function returns.
 * @param result Output explanation structure; must not be null and is initialized by the function.
 * @return       True when an explanation is produced; false if `pos` or `result` is null or the
 *               side to move has no king.
 */
bool xq_engine_explain_quiescence_one_ply(const XqEngineAdapter *engine, XqPosition *pos,
                                          XqQuiescenceExplainResult *result)
{
    XqMoveList list;
    int order_scores[XQ_MAX_MOVES];
    int alpha = INT_MIN / 2;
    int beta = INT_MAX / 2;
    int best = INT_MIN / 2;
    int i;

    if (result == NULL)
        return false;

    result->count = 0;
    result->best_index = -1;
    result->alpha_before = alpha;
    result->alpha_after_stand_pat = alpha;
    result->beta = beta;
    result->stand_pat = 0;
    result->final_score = alpha;
    result->in_check = false;
    result->stand_pat_used = false;
    result->stand_pat_cutoff = false;

    if (pos == NULL)
        return false;
    if (xq_position_king_square(pos, pos->side_to_move) == XQ_NO_SQUARE)
        return false;

    result->in_check = xq_position_in_check(pos, pos->side_to_move);
    if (xq_position_in_check(pos, xq_color_opponent(pos->side_to_move)))
    {
        result->final_score = XQ_MATE_SCORE - 1;
        return true;
    }
    if (result->in_check)
        xq_generate_legal(pos, &list);
    else
        xq_generate_pseudo_legal(pos, &list);
    if (list.count == 0)
    {
        result->final_score = -XQ_MATE_SCORE + 2;
        return true;
    }

    if (!result->in_check)
    {
        result->stand_pat_used = true;
        result->stand_pat = static_evaluate(engine, pos, pos->side_to_move);
        best = result->stand_pat;
        if (result->stand_pat >= beta)
        {
            result->stand_pat_cutoff = true;
            result->final_score = best;
            return true;
        }
        if (result->stand_pat > alpha)
            alpha = result->stand_pat;
        result->alpha_after_stand_pat = alpha;
    }

    order_moves_for_explain(engine, pos, &list, order_scores);

    for (i = 0; i < list.count; ++i)
    {
        XqExplainedMove *explained;
        int alpha_before;
        int score;

        if (!result->in_check && list.moves[i].captured == XQ_EMPTY_PIECE)
            continue;

        explained = &result->moves[result->count];
        alpha_before = alpha;

        if (list.moves[i].captured != XQ_EMPTY_PIECE &&
            xq_piece_type(list.moves[i].captured) == XQ_KING)
            score = XQ_MATE_SCORE - 1;
        else
        {
            xq_position_make_move(pos, list.moves[i]);
            score = -quiescence(engine, pos, -1, 1, -beta, -alpha, NULL);
            xq_position_unmake_move(pos, list.moves[i]);
        }

        explained->move = list.moves[i];
        explained->depth = 0;
        explained->alpha_before = alpha_before;
        explained->beta = beta;
        explained->score = score;
        explained->order_score = order_scores[i];
        explained->score_kind = XQ_SEARCH_SCORE_UPPER_BOUND;
        explained->is_best = false;
        explained->caused_cutoff = false;

        if (score > best)
        {
            best = score;
            result->best_index = result->count;
        }
        if (score >= beta)
        {
            result->best_index = result->count;
            explained->score_kind = XQ_SEARCH_SCORE_LOWER_BOUND;
            explained->caused_cutoff = true;
            ++result->count;
            result->final_score = best;
            result->moves[result->best_index].is_best = true;
            return true;
        }
        if (score > alpha)
        {
            alpha = score;
            explained->score_kind = XQ_SEARCH_SCORE_EXACT;
        }

        ++result->count;
    }

    result->final_score = best;
    if (result->best_index >= 0 && result->best_index < result->count)
        result->moves[result->best_index].is_best = true;
    return true;
}
