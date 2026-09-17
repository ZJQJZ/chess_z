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
 * @return The number of milliseconds elapsed since a platform-specific fixed reference point, or 0
 *         if the clock cannot be read. The value alone is meaningless; only the difference between
 *         two calls is meaningful.
 */
static uint64_t monotonic_time_ms(void)
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
        return 0;
    return (uint64_t)((long double)counter.QuadPart * 1000.0L / (long double)frequency.QuadPart);
#else
    struct timespec now;

    /*
     * Similar to the `_WIN32` branch, this retrieves the elapsed time since an unspecified fixed
     * reference point. The result consists of the seconds stored in `now.tv_sec` plus the
     * nanoseconds stored in `now.tv_nsec`. The final return value discards any fractional
     * millisecond. */
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;
    return (uint64_t)now.tv_sec * UINT64_C(1000) + (uint64_t)now.tv_nsec / UINT64_C(1000000);
#endif
}

/**
 * @brief Reads the CPU time consumed by the current process and converts it to milliseconds.
 *
 * CPU time counts only the time during which the process is actively executing on the processor.
 * Time spent waiting or sleeping is generally not included. Fractions of a millisecond are
 * discarded when converting to integer milliseconds.
 *
 * @return The CPU time, in milliseconds, consumed by the current process since it started; returns
 *         0 if the time cannot be obtained.
 */
static uint64_t cpu_time_ms(void)
{
    clock_t now = clock();

    if (now == (clock_t)-1)
        return 0;
    return (uint64_t)((double)now * 1000.0 / (double)CLOCKS_PER_SEC);
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
 * @return     The current time in milliseconds from the selected clock, or 0 if the underlying
 *             clock cannot be read.
 */
static uint64_t search_time_ms(XqSearchTimeMode mode)
{
    return mode == XQ_SEARCH_TIME_CPU ? cpu_time_ms() : monotonic_time_ms();
}

/**
 * @brief Initializes the context used by a built-in search according to the search limits.
 *
 * The function resets the node count and stop state, normalizes the timing mode, and calculates the
 * absolute deadline when the time limit is nonzero. A zero time limit disables timeout checks.
 *
 * @param context Search context to initialize; must not be null.
 * @param limits  Depth, time, bonus, and timing mode configuration; must not be null.
 */
static void search_context_init(SearchContext *context, const XqSearchLimits *limits)
{
    uint64_t now;

    memset(context, 0, sizeof(*context));
    context->time_mode =
        limits->time_mode == XQ_SEARCH_TIME_CPU ? XQ_SEARCH_TIME_CPU : XQ_SEARCH_TIME_MONOTONIC;
    if (limits->time_limit_ms == 0)
        return;

    now = search_time_ms(context->time_mode);
    context->time_limited = true;
    if (UINT64_MAX - now < limits->time_limit_ms)
        context->deadline_ms = UINT64_MAX;
    else
        context->deadline_ms = now + limits->time_limit_ms;
}

/**
 * @brief Checks whether the search has already stopped or has reached its time limit.
 *
 * A non-forced check reads the clock only when the node count is a multiple of 1024, reducing the
 * overhead of frequent system clock queries. A forced check reads the clock immediately. When a
 * timeout is detected, `stopped` is set to true, and subsequent calls continue to report that the
 * search has stopped.
 *
 * @param context The current search context. If null, the search is considered active and no time
 *                check is performed.
 * @param force   If true, checks the time immediately; if false, checks only when the node count is
 *                a multiple of 1024.
 * @return        Returns true if the search has already stopped or if this check detects a timeout;
 *                otherwise, returns false.
 */
static bool search_check_time(SearchContext *context, bool force)
{
    if (context == NULL || context->stopped || !context->time_limited)
        return context != NULL && context->stopped;
    if (!force && (context->nodes & UINT64_C(1023)) != 0)
        return false;
    if (search_time_ms(context->time_mode) >= context->deadline_ms)
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
 * The default configuration limits the search time to 3 seconds, uses a monotonic wall clock, and
 * awards each root move a 70-point depth-confidence bonus for every additional completed ply.
 *
 * `max_depth` is normalized to the minimum depth of 1 if its input value is 0.
 *
 * @param max_depth Maximum search depth for iterative deepening search; normalized to 1 if set to
 *                  0.
 * @return          The normalized default limit configuration.
 */
XqSearchLimits xq_search_limits_default(unsigned max_depth)
{
    XqSearchLimits limits;

    limits.max_depth = max_depth == 0 ? 1 : max_depth;
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
        if (entry->key == key)
        {
            retention = tt_retention_score(entry, table->generation);
            entry->generation = table->generation;
            if (incoming_score >= retention)
                target = entry;
            else
                return;
            break;
        }

        retention = tt_retention_score(entry, table->generation);
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
 * @brief Evaluates a position from the requested side's perspective using the built-in evaluator.
 *
 * The evaluator assigns fixed values to each piece type and subtracts the opponent's total material
 * from the perspective side's total material.
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
    int score = 0;
    XqColor opponent = xq_color_opponent(perspective);
    int type;

    (void)user;
    for (type = 0; type < XQ_PIECE_TYPE_NB; ++type)
    {
        int value = piece_values[type];
        score += value * xq_bb_count(pos->pieces[perspective][type]);
        score -= value * xq_bb_count(pos->pieces[opponent][type]);
    }
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
 * @brief Computes a move-ordering score, with higher-scoring moves searched first.
 *
 * If the engine does not provide a custom scoring function, the default implementation uses an
 * MVV-LVA-style formula to place captures before quiet moves:
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
 * @param engine Engine adapter that may provide a custom move-scoring callback; may be null.
 * @param pos    Position in which `move` is being ordered; must not be null when a custom callback
 *               needs it.
 * @param move   Move for which to compute an ordering score.
 * @return       Ordering score; higher values receive higher search priority.
 */
static int move_order_score(const XqEngineAdapter *engine, const XqPosition *pos, XqMove move)
{
    if (engine != NULL && engine->score_move != NULL)
        return engine->score_move(pos, move, engine->user);

    if (move.captured == XQ_EMPTY_PIECE)
        return 0;

    return CAPTURE_ORDER_BASE + piece_values[xq_piece_type(move.captured)] * 16 -
           piece_values[xq_piece_type(move.piece)];
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
 * At each step, the function obtains a complete `XqMove` from the current position's pseudo-legal
 * move list instead of trusting potentially stale `piece` and `captured` fields in the cached move.
 * It temporarily makes each move while reconstructing the line, then unmakes all moves in reverse
 * order before returning so that `pos` is restored.
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
    int i;

    pv->count = 0;
    while (remaining > 0 && pv->count < XQ_MAX_PV_MOVES)
    {
        XqTranspositionEntry *entry = tt_find_entry(table, xq_position_hash(pos));
        XqMoveList list;

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

    for (i = made_count - 1; i >= 0; --i)
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
 * When the side to move is not in check, the static evaluation is used as the stand-pat score and
 * only captures are searched. When the side to move is in check, stand pat is not allowed and every
 * pseudo-legal evasion is searched. The function uses fail-hard alpha-beta semantics and therefore
 * returns `beta` on a beta cutoff.
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
    int stand_pat;
    int i;

    if (search_enter_node(context))
        return 0;

    if (depth <= -XQ_MAX_QUIESCENCE_DEPTH)
        return static_evaluate(engine, pos, pos->side_to_move);

    in_check = xq_position_in_check(pos, pos->side_to_move);

    if (!in_check)
    {
        stand_pat = static_evaluate(engine, pos, pos->side_to_move);
        if (stand_pat >= beta)
            return beta;
        if (stand_pat > alpha)
            alpha = stand_pat;
    }

    xq_generate_pseudo_legal(pos, &list);
    if (in_check && list.count == 0)
        return -XQ_MATE_SCORE + ply;
    order_moves(engine, pos, &list);

    for (i = 0; i < list.count; ++i)
    {
        int score;

        if (!in_check && list.moves[i].captured == XQ_EMPTY_PIECE)
            continue;
        if (list.moves[i].captured != XQ_EMPTY_PIECE &&
            xq_piece_type(list.moves[i].captured) == XQ_KING)
            return XQ_MATE_SCORE - (ply + 1);

        xq_position_make_move(pos, list.moves[i]);
        score = -quiescence(engine, pos, depth - 1, ply + 1, -beta, -alpha, context);
        xq_position_unmake_move(pos, list.moves[i]);
        if (context != NULL && context->stopped)
            return 0;
        if (score >= beta)
            return beta;
        if (score > alpha)
            alpha = score;
    }

    return alpha;
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
        return -XQ_MATE_SCORE + ply;
    order_moves(engine, pos, &list);

    /* For example, suppose the requested depth is 6, the window is [50, 100], and the table
     * contains an older entry with depth=8, LOWER_BOUND=80, and best_move=A. Because 80 < beta,
     * the cached score cannot produce a cutoff, but the entry still supplies hash_move=A. If the
     * first move of the previous iteration's complete PV is B, then pv_hint[0]=B and the hash move
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
 * @brief Runs the built-in iterative-deepening search.
 *
 * The function searches from depth 1 through the requested maximum depth. Scores from all root
 * moves in the previous completed iteration determine their ordering in the next iteration, while
 * the previous principal variation supplies an additional ordering hint.
 *
 * If the time limit expires during an iteration, the function selects among the root moves whose
 * searches completed.
 *
 * @param engine    Engine adapter used for evaluation, move ordering, and optional transposition
 *                  table access; may be null.
 * @param pos       Position to search; must not be null and is restored before the function
 *                  returns.
 * @param limits    Depth, time, depth-bonus, and clock-mode configuration; must not be null.
 * @param best_move Output that receives the selected move; must not be null.
 * @return          True if at least one root move exists and a move is selected; false if the
 *                  position is invalid for searching or contains no pseudo-legal moves.
 */
static bool builtin_search(const XqEngineAdapter *engine, XqPosition *pos,
                           const XqSearchLimits *limits, XqMove *best_move)
{
    XqMoveList list;
    ScoredMove root_moves[XQ_MAX_MOVES];
    PrincipalVariation previous_pv = {0};
    XqTranspositionTable *table = engine != NULL ? engine->transposition_table : NULL;
    uint64_t root_key;
    XqMove hash_move;
    SearchContext context;
    unsigned depth = limits->max_depth == 0 ? 1 : limits->max_depth;
    unsigned current_depth;
    int i;

    if (pos == NULL || xq_position_king_square(pos, pos->side_to_move) == XQ_NO_SQUARE)
        return false;

    xq_generate_pseudo_legal(pos, &list);
    if (list.count == 0)
        return false;
    order_moves(engine, pos, &list);
    root_key = xq_position_hash(pos);
    tt_new_generation(table);
    if (tt_get_hash_move(table, root_key, &hash_move))
        (void)prioritize_move(&list, hash_move);

    search_context_init(&context, limits);

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
            if (score > alpha)
            {
                alpha = score;
                build_principal_variation(&current_pv, root_moves[i].move, &child_pv);
            }

            if (search_check_time(&context, true))
                break;
        }

        if (context.stopped)
        {
            *best_move = select_timed_root_move(root_moves, list.count, limits->depth_bonus);
            break;
        }

        order_root_moves(root_moves, list.count);
        *best_move = root_moves[0].move;
        previous_pv = current_pv;
        tt_store(table, root_key, current_depth, root_moves[0].score, 0, XQ_SEARCH_SCORE_EXACT,
                 root_moves[0].move);

        if (current_depth == depth)
            break;
    }

    return true;
}

/**
 * @brief Finds the best move using a custom search callback or the built-in search.
 *
 * The requested depth is converted to default search limits. If `engine` provides a custom search
 * callback, that callback is invoked directly.
 *
 * Otherwise, `builtin_search()` is used.
 *
 * @param engine    Engine adapter that may provide a custom search callback; may be null.
 * @param pos       Position to search. The built-in search requires a non-null position and
 *                  restores it before returning.
 * @param depth     Requested maximum search depth. The built-in search treats zero as one.
 * @param best_move Output that receives the selected move; must not be null.
 * @return          True if a best move was found and written; false on invalid input, when no move
 *                  is available, or when the custom search callback fails.
 */
bool xq_engine_find_best_move(const XqEngineAdapter *engine, XqPosition *pos, unsigned depth,
                              XqMove *best_move)
{
    XqSearchLimits limits = xq_search_limits_default(depth);

    if (best_move == NULL)
        return false;
    if (engine != NULL && engine->search != NULL)
        return engine->search(pos, depth, best_move, engine->user);
    return builtin_search(engine, pos, &limits, best_move);
}

/**
 * @brief Finds the best move under explicit depth and time limits.
 *
 * The built-in search performs iterative deepening and uses the depth bonus in `limits` to choose a
 * result after a timeout. A `max_depth` of zero is normalized to one.
 *
 * If `engine` provides a custom search callback, only the normalized maximum depth is passed to it;
 * the custom search remains responsible for its own time limit and depth bonus policy.
 *
 * @param engine    Engine adapter; may be null. The built-in search is used when no custom search
 *                  callback is provided.
 * @param pos       Position to search. The built-in search requires a non-null position and leaves
 *                  it unchanged after the search.
 * @param limits    Search depth, time limit, depth bonus, and clock-mode configuration; must not be
 *                  null.
 * @param best_move Output that receives the selected move; must not be null.
 * @return          True if a best move was found and written; false on invalid input, when no move
 *                  is available, or when the custom search callback fails.
 */
bool xq_engine_find_best_move_with_limits(const XqEngineAdapter *engine, XqPosition *pos,
                                          const XqSearchLimits *limits, XqMove *best_move)
{
    XqSearchLimits effective_limits;

    if (best_move == NULL || limits == NULL)
        return false;
    effective_limits = *limits;
    if (effective_limits.max_depth == 0)
        effective_limits.max_depth = 1;
    if (engine != NULL && engine->search != NULL)
        return engine->search(pos, effective_limits.max_depth, best_move, engine->user);
    return builtin_search(engine, pos, &effective_limits, best_move);
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
 * The result first exposes the stand-pat evaluation when it is legal, then records the captures or
 * check evasions that quiescence search actually examines.
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
    if (!result->in_check)
    {
        result->stand_pat_used = true;
        result->stand_pat = static_evaluate(engine, pos, pos->side_to_move);
        if (result->stand_pat >= beta)
        {
            result->stand_pat_cutoff = true;
            result->final_score = beta;
            return true;
        }
        if (result->stand_pat > alpha)
            alpha = result->stand_pat;
        result->alpha_after_stand_pat = alpha;
    }

    xq_generate_pseudo_legal(pos, &list);
    if (result->in_check && list.count == 0)
    {
        result->final_score = -30000;
        return true;
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

        if (score >= beta)
        {
            result->best_index = result->count;
            explained->score_kind = XQ_SEARCH_SCORE_LOWER_BOUND;
            explained->caused_cutoff = true;
            ++result->count;
            result->final_score = beta;
            result->moves[result->best_index].is_best = true;
            return true;
        }
        if (score > alpha)
        {
            alpha = score;
            result->best_index = result->count;
            explained->score_kind = XQ_SEARCH_SCORE_EXACT;
        }

        ++result->count;
    }

    result->final_score = alpha;
    if (result->best_index >= 0 && result->best_index < result->count)
        result->moves[result->best_index].is_best = true;
    return true;
}
