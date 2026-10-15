#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "xiangqi/parallel_search.h"
#include "xiangqi/movegen.h"

#include <assert.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief Creates a default configuration for the parallel minimax search.
 *
 * The default configuration iteratively deepens to at most 3 plies, uses 2 threads including the
 * calling thread, and uses contiguous splitting to assign a tail batch of 50 percent of a node's
 * children to an available helper. The search has no time limit and no transposition table by
 * default.
 *
 * @return The default parallel-search configuration.
 */
XqParallelOptions xq_parallel_options_default(void)
{
    XqParallelOptions options = {3, 2, 50, NULL, XQ_PARALLEL_SPLIT_CONTIGUOUS};
    return options;
}

/**
 * @brief Converts a parallel-search status code to a human-readable English message.
 *
 * Known status codes describe success, invalid arguments, resource creation failure, or an
 * unsupported platform. Unrecognized values produce "unknown parallel search status".
 *
 * @param status Status code to describe.
 * @return       A non-null pointer to a string literal with static lifetime. The caller must not
 *               modify or free the returned string.
 */
const char *xq_parallel_status_text(XqParallelStatus status)
{
    switch (status)
    {
    case XQ_PARALLEL_OK:
        return "ok";
    case XQ_PARALLEL_INVALID_ARGUMENT:
        return "invalid parallel search arguments";
    case XQ_PARALLEL_RESOURCE_ERROR:
        return "cannot create parallel search resources";
    case XQ_PARALLEL_UNSUPPORTED:
        return "parallel search requires macOS/Linux (pthread)";
    }
    return "unknown parallel search status";
}

#ifndef _WIN32
#include <pthread.h>
#include <stdatomic.h>
#ifdef __APPLE__
#include <dispatch/dispatch.h>
#else
#include <errno.h>
#include <semaphore.h>
#endif

/*
 * Notifications persist until consumed, including when posted before a wait. These wrappers avoid
 * explicit scheduling mutexes; the platform semaphore implementation need not be lock-free.
 */
typedef struct SearchSemaphore
{
#ifdef __APPLE__
    dispatch_semaphore_t handle;
#else
    sem_t handle;
#endif
} SearchSemaphore;

/**
 * @brief Initializes a counting semaphore with no pending notifications.
 *
 * Uses a Dispatch semaphore on macOS and an unnamed, process-local POSIX semaphore elsewhere. The
 * initial count is zero, so a wait blocks until a notification is posted. A notification posted
 * before a wait remains available for that wait to consume, preventing lost wakeups.
 *
 * Initialize before sharing with other threads; do not reinitialize a live semaphore. On success,
 * the caller must eventually call search_semaphore_destroy() after all users have finished. On
 * failure, the semaphore must not be waited on, posted to, or destroyed.
 *
 * @param semaphore Non-null storage that remains at a stable address throughout its use.
 * @return          True on successful initialization; false if the platform call fails.
 */
static bool search_semaphore_init(SearchSemaphore *semaphore)
{
#ifdef __APPLE__
    semaphore->handle = dispatch_semaphore_create(0);
    return semaphore->handle != NULL;
#else
    return sem_init(&semaphore->handle, 0, 0) == 0;
#endif
}

/**
 * @brief Consumes one notification, blocking the calling thread until one is available.
 *
 * Returns immediately if a previous post left a notification available; otherwise waits without a
 * deadline. Uses DISPATCH_TIME_FOREVER on macOS and retries POSIX sem_wait() when interrupted by a
 * signal (EINTR). An unexpected wait failure aborts the process because continuing without
 * consuming a notification would invalidate the task or completion handshake.
 *
 * Returning does not identify the event: the caller must acquire the corresponding task, worker or
 * shutdown state before deciding how to proceed. This function does not change those states.
 *
 * @param semaphore Non-null, successfully initialized semaphore; keep it alive throughout the wait.
 */
static void search_semaphore_wait(SearchSemaphore *semaphore)
{
#ifdef __APPLE__
    if (dispatch_semaphore_wait(semaphore->handle, DISPATCH_TIME_FOREVER) != 0)
        abort();
#else
    while (sem_wait(&semaphore->handle) != 0)
        if (errno != EINTR)
            abort();
#endif
}

/**
 * @brief Posts one notification, waking a waiter or retaining it for a later wait.
 *
 * Does not wait for the receiver to consume the notification or finish its work. The caller must
 * publish the associated task, completion or shutdown state before posting. Unlike a condition
 * variable notification, a post is not lost when the receiving thread has not started waiting.
 *
 * The Dispatch return value only indicates whether a waiter was woken and is ignored. A POSIX
 * sem_post() failure aborts the process because the receiver could otherwise remain blocked.
 *
 * @param semaphore Non-null, successfully initialized semaphore; keep it alive throughout the post.
 */
static void search_semaphore_post(SearchSemaphore *semaphore)
{
#ifdef __APPLE__
    /* The return value reports whether a waiter was woken, not an error. */
    (void)dispatch_semaphore_signal(semaphore->handle);
#else
    if (sem_post(&semaphore->handle) != 0)
        abort();
#endif
}

/**
 * @brief Releases the resources of a successfully initialized, unused semaphore.
 *
 * Releases the Dispatch object on macOS or destroys the unnamed POSIX semaphore elsewhere. A POSIX
 * destruction failure aborts the process. The containing SearchSemaphore storage is not freed, and
 * its handle must not be used again unless successfully reinitialized.
 *
 * The caller must ensure all users have finished and no further waits or posts can occur. Pool
 * teardown joins workers first; task-local completion cleanup waits for the final reference. Call
 * exactly once per successful initialization, including cleanup after partial pool creation.
 *
 * @param semaphore Non-null, initialized semaphore with no active or future users.
 */
static void search_semaphore_destroy(SearchSemaphore *semaphore)
{
#ifdef __APPLE__
    dispatch_release(semaphore->handle);
#else
    if (sem_destroy(&semaphore->handle) != 0)
        abort();
#endif
}

/*
 * Each split has its own completion, independent of the worker that executes it. The owner and
 * worker each hold one reference until their wait/post has returned. This keeps the semaphore alive
 * even if the owner wakes and returns from its stack frame before the worker's post returns.
 */
typedef struct TaskCompletion
{
    SearchSemaphore notification;
    atomic_int      finished;
    atomic_int      references;
} TaskCompletion;

/**
 * @brief Creates a task-local completion record with one reference for each participant.
 *
 * Allocates the record and initializes its notification semaphore with count zero, finished flag
 * with zero, and reference count with two. The node owner holds one reference; the other is
 * reserved for the worker and transferred when the task is published through WORKER_READY. Each
 * participant must call task_completion_release() once, after its last access to the record.
 *
 * Called after reserving a worker but before publishing its task. Allocation or semaphore creation
 * failure leaves no resources behind. The caller must then release the worker reservation and
 * search the whole batch serially. This function itself neither dispatches nor waits for work.
 *
 * @return Initialized completion record, or NULL if memory allocation or semaphore creation fails.
 */
static TaskCompletion *task_completion_create(void)
{
    TaskCompletion *completion = malloc(sizeof(*completion));
    if (completion == NULL)
        return NULL;
    if (!search_semaphore_init(&completion->notification))
    {
        free(completion);
        return NULL;
    }
    atomic_init(&completion->finished, 0);
    atomic_init(&completion->references, 2);
    return completion;
}

/**
 * @brief Releases one participant's reference and destroys the record after the final release.
 *
 * The worker releases after posting the completion notification; the owner releases after consuming
 * that notification and acquiring the finished flag. Both must finish all accesses, including the
 * entire semaphore post or wait call, before releasing. Either participant may release first.
 *
 * The atomic decrement returns the previous count: a value of one means this release changes it to
 * zero and must destroy the semaphore and free the record. Release ordering publishes each
 * participant's preceding accesses; acquire ordering lets the final releaser observe the other
 * participant's release before destruction. Thus an owner waking before the worker's post returns
 * cannot destroy a semaphore still in use by that worker.
 *
 * This function does not wait for task completion. Call exactly once per held reference and never
 * access the record through that reference afterward, even if the other participant still holds it.
 *
 * @param completion Non-null, initialized record for which the caller holds one live reference.
 */
static void task_completion_release(TaskCompletion *completion)
{
    if (atomic_fetch_sub_explicit(&completion->references, 1, memory_order_acq_rel) == 1)
    {
        search_semaphore_destroy(&completion->notification);
        free(completion);
    }
}

enum
{
    WIN                      = 30000,
    STATIC_LIMIT             = 28999,
    INFINITY_SCORE           = 30001,
    PARALLEL_MAX_PV_MOVES    = 64,
    PARALLEL_TT_BUCKET_SIZE  = 4,
    PARALLEL_TT_BUCKET_COUNT = 1 << 18
};

typedef struct ParallelTtEntry
{
    uint64_t key;
    int      score; /* Side-to-move perspective, as in engine.c. */
    XqMove   best_move;
    uint16_t depth; /* Zero marks an empty slot. */
    uint8_t  generation;
    uint8_t  score_kind;
} ParallelTtEntry;

typedef struct ParallelTtBucket
{
    atomic_int      occupied;
    ParallelTtEntry entries[PARALLEL_TT_BUCKET_SIZE];
} ParallelTtBucket;

struct XqParallelTranspositionTable
{
    ParallelTtBucket    *buckets;
    uint8_t              generation;
    XqTranspositionStats stats; /* Aggregated only after all search threads finish. */
    bool                 has_context;
    XqEvaluateFn         evaluate;
    void                *evaluate_user;
    XqColor              root_color;
};

/**
 * @brief Creates an independent, fixed-capacity transposition table for parallel search.
 *
 * Allocates the management structure and PARALLEL_TT_BUCKET_COUNT four-way buckets separately. Zero
 * initialization resets the generation and statistics, leaves the evaluator context unbound
 * (has_context == false), and marks every entry empty through depth == 0. Each bucket's atomic
 * occupied flag is explicitly initialized to zero before the table can be shared with workers;
 * later cache accesses use this flag to attempt exclusive access to the bucket.
 *
 * If bucket allocation fails, releases the already allocated management structure. On success, the
 * caller owns the table and may lend it to successive searches through XqParallelOptions. Only one
 * top-level search may use it at a time. After all users have finished, release it with
 * xq_parallel_transposition_table_destroy(). No search threads are created by this function.
 *
 * @return A newly initialized table, or NULL if either allocation fails.
 */
XqParallelTranspositionTable *xq_parallel_transposition_table_create(void)
{
    XqParallelTranspositionTable *table = calloc(1, sizeof(*table));
    unsigned                      i;
    if (table == NULL)
        return NULL;
    table->buckets = calloc(PARALLEL_TT_BUCKET_COUNT, sizeof(*table->buckets));
    if (table->buckets == NULL)
    {
        free(table);
        return NULL;
    }
    for (i = 0; i < PARALLEL_TT_BUCKET_COUNT; ++i)
        atomic_init(&table->buckets[i].occupied, 0);
    return table;
}

/**
 * @brief Clears cached entries, evaluator binding, generation and cumulative statistics.
 *
 * Zeroes only each bucket's entry array, making every slot empty through depth == 0. Retains the
 * allocated table and buckets for reuse, and leaves their initialized atomic occupied flags
 * untouched. Clearing has_context invalidates the previous evaluator binding; the retained
 * root_color is ignored until the next search establishes a new context.
 *
 * Requires exclusive access to a quiescent table: no worker may be accessing a bucket and no other
 * caller may be using the table. This function does not acquire the bucket guards or wait for an
 * active search to finish. External callers must wait until the borrowing search returns; search
 * setup may also clear the table before publishing any tasks when its evaluator context changes.
 * Use reset_stats instead when cached entries and their context should be retained.
 *
 * @param table Table to reset without freeing its storage; NULL is accepted and does nothing.
 */
void xq_parallel_transposition_table_clear(XqParallelTranspositionTable *table)
{
    unsigned i;
    if (table == NULL)
        return;
    for (i = 0; i < PARALLEL_TT_BUCKET_COUNT; ++i)
        memset(table->buckets[i].entries, 0, sizeof(table->buckets[i].entries));
    table->generation    = 0;
    table->has_context   = false;
    table->evaluate      = NULL;
    table->evaluate_user = NULL;
    memset(&table->stats, 0, sizeof(table->stats));
}

/**
 * @brief Releases the bucket array and management structure of a parallel transposition table.
 *
 * Requires exclusive access after all searches borrowing the table have returned and all other
 * users, including statistics readers, have finished. This function does not signal workers, join
 * threads, or acquire bucket guards. Clearing entries before destruction is unnecessary; the
 * evaluator's user data is borrowed and is not freed here.
 *
 * Invalidates the table and every alias to it, including pointers stored in XqParallelOptions. The
 * caller must clear or replace those pointers before using the options again; passing the same
 * non-null pointer to this function twice is invalid.
 *
 * @param table Table created by xq_parallel_transposition_table_create(); NULL does nothing.
 */
void xq_parallel_transposition_table_destroy(XqParallelTranspositionTable *table)
{
    if (table != NULL)
    {
        free(table->buckets);
        free(table);
    }
}

/**
 * @brief Resets cumulative cache statistics without discarding cached search results.
 *
 * Zeroes probes, hits, cutoffs, stores and replacements. Preserves all entries, their generation
 * metadata, the table's current generation and its evaluator binding. This allows subsequent
 * searches to measure cache activity while continuing to reuse previously stored results.
 *
 * Call only between searches with exclusive access to the table, including exclusion of statistics
 * readers. Counters are ordinary fields: this function provides no synchronization with readers or
 * with the statistics merged at search completion. Use the table's clear function instead when
 * cached entries and their evaluator binding must also be invalidated.
 *
 * @param table Table whose statistics are reset; NULL is accepted and does nothing.
 */
void xq_parallel_transposition_table_reset_stats(XqParallelTranspositionTable *table)
{
    if (table != NULL)
        memset(&table->stats, 0, sizeof(table->stats));
}

/**
 * @brief Copies cumulative cache statistics into caller-owned storage without resetting them.
 *
 * Returns probes, hits, cutoffs, stores and replacements accumulated since table creation or the
 * latest clear/reset. Search threads keep private counters that are merged after worker joins, so
 * these totals describe completed searches, not live progress. Copying them leaves cached entries,
 * generation metadata and the evaluator binding unchanged.
 *
 * Call only after the borrowing search has returned. The copy uses ordinary field accesses and
 * provides no synchronization; the caller must prevent concurrent searches, clear/reset calls and
 * destruction of the table. The output must also be safe for this caller to write.
 *
 * @param table Table to inspect; NULL produces zero counters in a non-null output.
 * @param stats Destination for the copied counters; NULL makes this function do nothing.
 */
void xq_parallel_transposition_table_get_stats(const XqParallelTranspositionTable *table,
                                               XqTranspositionStats               *stats)
{
    if (stats == NULL)
        return;
    if (table == NULL)
        memset(stats, 0, sizeof(*stats));
    else
        *stats = table->stats;
}

/**
 * @brief Prepares the table's evaluator binding and generation for a new search iteration.
 *
 * has_context indicates whether the stored evaluator configuration is valid, not whether the table
 * contains entries or a search is running. If a previous binding exists, changing the evaluator or
 * its user pointer clears the entries and statistics. A root-color change also clears the table for
 * custom evaluators: they need not return opposite scores for opposite perspectives, so converting
 * cached scores to side-to-move perspective alone is insufficient. The built-in evaluator is
 * antisymmetric and allows reuse across root-color changes.
 *
 * Records the new binding, marks it valid and advances the uint8_t generation once per iteration,
 * wrapping modulo 256. Compatible bindings retain their entries and cumulative statistics; after a
 * clear, the generation advances from zero to one. Mutations behind an unchanged user pointer
 * cannot be detected, so callers must explicitly clear the table when those mutations change
 * evaluation semantics.
 *
 * Call with exclusive access before publishing any tasks for this iteration, after the preceding
 * root search has waited for all its helpers. The context and generation remain unchanged while
 * workers access the table; this function provides no synchronization with active searches or
 * external table users.
 *
 * @param table         Borrowed table to prepare; NULL disables caching and does nothing here.
 * @param evaluate      Effective non-null evaluator, with the default already resolved.
 * @param evaluate_user Borrowed user-data pointer passed to the evaluator; may be NULL.
 * @param root_color    Root side whose perspective is used for this search's evaluations.
 */
static void parallel_tt_new_iteration(XqParallelTranspositionTable *table,
                                      XqEvaluateFn                  evaluate,
                                      void                         *evaluate_user,
                                      XqColor                       root_color)
{
    if (table == NULL)
        return;
    if (table->has_context &&
        (table->evaluate != evaluate || table->evaluate_user != evaluate_user ||
         (evaluate != xq_engine_default_static_evaluate && table->root_color != root_color)))
        xq_parallel_transposition_table_clear(table);
    table->evaluate      = evaluate;
    table->evaluate_user = evaluate_user;
    table->root_color    = root_color;
    table->has_context   = true;
    ++table->generation;
}

/**
 * @brief Attempts once to acquire exclusive access to a transposition-table bucket.
 *
 * A strong CAS changes occupied from 0 (available) to 1 (owned). Strong CAS avoids spurious failure
 * in this single-attempt path. If another caller owns the bucket, returns immediately; cache
 * callers skip that probe or store rather than retrying, spinning or blocking.
 *
 * Successful acquire ordering pairs with the previous owner's release store of zero, making its
 * entry writes visible to the new owner. Failure uses relaxed ordering because it grants no access
 * to the entries. During parallel search, both reads and writes of the ordinary entry fields
 * require this guard; it is a bucket-level try lock, not a lock-free snapshot.
 *
 * After success, the caller must release occupied with memory_order_release on every exit path.
 * Copy any needed entry data before releasing; do not access an entry through a retained pointer
 * afterward without acquiring the bucket again. Quiescent table management is handled separately.
 *
 * @param bucket Initialized, live bucket to acquire; must not be NULL.
 * @return       True if ownership was acquired; false if the bucket was occupied at the CAS
 *               attempt.
 */
static bool parallel_tt_try_acquire(ParallelTtBucket *bucket)
{
    int expected = 0;
    return atomic_compare_exchange_strong_explicit(&bucket->occupied, &expected, 1,
                                                   memory_order_acquire, memory_order_relaxed);
}

/**
 * @brief Copies a matching cache entry under a single-attempt bucket guard.
 *
 * Uses the low bits of key to select a bucket, then attempts exclusive access once. If the bucket
 * is occupied, returns immediately without retrying, spinning or blocking. While holding the guard,
 * scans the four slots for a non-empty entry (depth != 0) with the same full position key. Sharing
 * a bucket alone is not a hit; the full key, including the side to move, must match.
 *
 * On a hit, refreshes the shared entry's generation to the current iteration and copies the whole
 * entry into caller-owned storage. Thus the returned snapshot includes the refreshed generation.
 * Releases the bucket on both hit and miss paths before returning. The release publishes the
 * generation update to subsequent acquiring callers; the copied entry can be inspected without
 * holding the guard, even if another thread later replaces the shared entry. No shared pointer
 * escapes, and snapshots from successive calls need not describe one consistent table state.
 *
 * A true return means only that the key matched: this helper does not check search depth, score
 * kind, cutoff eligibility or move legality. parallel_tt_probe() handles score reuse, while PV
 * reconstruction checks exactness, depth and the hinted move separately. No cache counters are
 * changed here; ordinary probes count their own activity and PV lookups remain uncounted.
 *
 * The table must stay alive and its generation must remain stable during concurrent lookups.
 * Clearing, destroying or advancing the table belongs between searches/iterations, after helper
 * work has completed. Output storage must be private to the caller and must not alias table data.
 *
 * @param table    Initialized table borrowed by the search; NULL disables lookup.
 * @param key      Full position hash used for bucket selection and entry matching.
 * @param snapshot Non-null caller-owned output; written only on a hit, otherwise left unchanged.
 * @return         True on a matching entry; false for a disabled table, bucket contention or a
 *                 missing key. Inspect snapshot only when true is returned.
 */
static bool parallel_tt_snapshot(XqParallelTranspositionTable *table,
                                 uint64_t                      key,
                                 ParallelTtEntry              *snapshot)
{
    ParallelTtBucket *bucket;
    bool              hit = false;
    int               i;
    if (table == NULL)
        return false;
    bucket = &table->buckets[key & (PARALLEL_TT_BUCKET_COUNT - 1u)];
    if (!parallel_tt_try_acquire(bucket))
        return false;
    /* Ordinary entry fields may be accessed only while this bucket is exclusively owned. */
    for (i = 0; i < PARALLEL_TT_BUCKET_SIZE; ++i)
    {
        ParallelTtEntry *entry = &bucket->entries[i];
        if (entry->depth != 0 && entry->key == key)
        {
            /* Refresh before copying so the snapshot reflects this iteration's generation. */
            entry->generation = table->generation;
            *snapshot         = *entry;
            hit               = true;
            break;
        }
    }
    /* A miss must release the bucket too; callers receive a copy, never a borrowed entry. */
    atomic_store_explicit(&bucket->occupied, 0, memory_order_release);
    return hit;
}

/**
 * @brief Looks up a cached position and reports whether its score can resolve this search node.
 *
 * Selects a bucket using the low key bits and attempts to acquire it once. A busy bucket skips the
 * lookup without waiting. While holding the guard, matches a non-empty entry by its full key,
 * refreshes its generation and copies it into caller-owned storage. Releases the bucket before
 * interpreting the copy; no pointer into shared entry storage escapes the guard.
 *
 * A matching key sets *hit even when its depth or bound cannot resolve the node, allowing the
 * caller to use the cached move for ordering. Score reuse requires cached depth >= requested depth
 * and either an exact value, a lower bound >= beta, or an upper bound <= alpha. Scores and the
 * supplied window must use the side-to-move perspective. The caller must preserve the cached bound
 * kind when converting the result into an interval; a cutoff is not always exact. ordering_only
 * disables all score reuse and is used at the root to retain move selection.
 *
 * For a non-null table, increments probes even if bucket contention prevents inspection. Increments
 * hits on a key match and cutoffs only when returning true. Counters belong to the executing thread
 * and require no shared counter updates. A null table changes only *hit.
 *
 * @param table         Table borrowed by the current search; NULL disables the lookup.
 * @param key           Full position hash, including the side to move.
 * @param depth         Requested remaining search depth.
 * @param alpha         Lower window threshold in side-to-move perspective.
 * @param beta          Upper window threshold in side-to-move perspective.
 * @param ordering_only If true, return matching data for ordering but never reuse its score.
 * @param snapshot      Non-null output; written only on a key match, otherwise left unchanged.
 * @param hit           Non-null output; true on a key match, independently of the return value.
 * @param stats         Executing thread's private counters; required when table is non-null.
 * @return              True if the copied score can resolve the node; false for ordering-only hits,
 *                      unusable scores, misses, contention or a disabled table. Inspect *hit before
 *                      using snapshot.
 */
static bool parallel_tt_probe(XqParallelTranspositionTable *table,
                              uint64_t                      key,
                              unsigned                      depth,
                              int                           alpha,
                              int                           beta,
                              bool                          ordering_only,
                              ParallelTtEntry              *snapshot,
                              bool                         *hit,
                              XqTranspositionStats         *stats)
{
    *hit = false;
    if (table == NULL)
        return false;
    ++stats->probes;
    *hit = parallel_tt_snapshot(table, key, snapshot);
    if (!*hit)
        return false;
    ++stats->hits;
    if (!ordering_only && snapshot->depth >= depth &&
        (snapshot->score_kind == XQ_SEARCH_SCORE_EXACT ||
         (snapshot->score_kind == XQ_SEARCH_SCORE_LOWER_BOUND && snapshot->score >= beta) ||
         (snapshot->score_kind == XQ_SEARCH_SCORE_UPPER_BOUND && snapshot->score <= alpha)))
    {
        ++stats->cutoffs;
        return true;
    }
    return false;
}

/**
 * @brief Computes an entry's retention priority for the cache replacement policy.
 *
 * Uses the same formula as engine.c: 8 * depth - 4 * age + exact-value bonus. Deeper entries
 * receive higher priority, older entries receive lower priority, and exact values receive a bonus
 * of 3 over bounds. This is a replacement priority, not a chess position evaluation; a higher
 * result favors keeping the entry, and negative results are valid.
 *
 * Age is the generation difference converted to uint8_t, so it wraps modulo 256 along with the
 * table's generation. For example, generation 1 and entry generation 255 give age 2. Ages spanning
 * a full 256-generation cycle cannot be distinguished from newer ones. This function neither
 * refreshes generation nor modifies the entry.
 *
 * @param entry      Non-null entry to score, either a local candidate/copy or a shared entry
 *                   protected by its bucket guard while concurrent access is possible.
 * @param generation Current table generation used to calculate the entry's age.
 * @return           Retention priority used to compare incoming entries with existing entries.
 */
static int parallel_tt_retention_score(const ParallelTtEntry *entry,
                                       uint8_t                generation)
{
    unsigned age   = (uint8_t)(generation - entry->generation);
    int      bonus = entry->score_kind == XQ_SEARCH_SCORE_EXACT ? 3 : 0;
    return (int)entry->depth * 8 - (int)age * 4 + bonus;
}

/**
 * @brief Attempts to cache a search result using engine.c's slot selection and replacement rules.
 *
 * Ignores a disabled table or depth zero, which is reserved for empty entries. Builds a local
 * candidate with the current generation and depth clamped to UINT16_MAX, then attempts to acquire
 * the key's bucket once. Contention skips this optional store without waiting.
 *
 * Scans slots in order, stopping at the first empty slot or matching key. A matching entry's
 * generation is refreshed even if the incoming retention priority is too low to overwrite it; that
 * comparison uses the existing entry's priority calculated before the refresh. If all slots are
 * occupied by different keys, selects the first entry with the lowest retention priority and
 * replaces it only when the incoming priority is at least as high.
 *
 * Counts stores only when the candidate is written, and replacements only when that write evicts a
 * non-empty entry with a different key. Rejected writes and generation-only refreshes do not
 * increment either counter. Every path after acquisition releases the bucket with release ordering,
 * publishing entry changes to a subsequent acquiring reader or writer.
 *
 * The caller must supply a valid, non-cancelled result with its bound kind already classified and
 * its score converted to side-to-move perspective. This function does not inspect node completion,
 * validate the move, or convert score bounds.
 *
 * @param table     Table borrowed by the current search; NULL disables the store.
 * @param key       Full position hash, including the side to move.
 * @param depth     Searched remaining depth; zero is ignored, larger values are clamped for
 *                  storage.
 * @param score     Proven score or bound in side-to-move perspective.
 * @param kind      Whether score is exact, a lower bound, or an upper bound.
 * @param best_move Selected move to retain as a future move-ordering hint.
 * @param stats     Executing thread's private counters; required for a non-null table and depth >
 *                  0.
 */
static void parallel_tt_store(XqParallelTranspositionTable *table,
                              uint64_t                      key,
                              unsigned                      depth,
                              int                           score,
                              XqSearchScoreKind             kind,
                              XqMove                        best_move,
                              XqTranspositionStats         *stats)
{
    ParallelTtBucket *bucket;
    ParallelTtEntry   incoming;
    ParallelTtEntry  *target        = NULL;
    int               weakest_score = INT_MAX, incoming_score, i;
    if (table == NULL || depth == 0)
        return;
    incoming.key        = key;
    incoming.score      = score;
    incoming.best_move  = best_move;
    incoming.depth      = depth > UINT16_MAX ? UINT16_MAX : (uint16_t)depth;
    incoming.generation = table->generation;
    incoming.score_kind = (uint8_t)kind;
    incoming_score      = parallel_tt_retention_score(&incoming, table->generation);
    bucket              = &table->buckets[key & (PARALLEL_TT_BUCKET_COUNT - 1u)];
    if (!parallel_tt_try_acquire(bucket))
        return;
    for (i = 0; i < PARALLEL_TT_BUCKET_SIZE; ++i)
    {
        ParallelTtEntry *entry = &bucket->entries[i];
        int              retention;
        if (entry->depth == 0)
        {
            target = entry;
            break;
        }
        retention = parallel_tt_retention_score(entry, table->generation);
        if (entry->key == key)
        {
            entry->generation = table->generation;
            if (incoming_score < retention)
                goto release;
            target = entry;
            break;
        }
        if (retention < weakest_score)
        {
            weakest_score = retention;
            target        = entry;
        }
    }
    if (target == NULL)
        goto release;
    if (target->depth != 0 && target->key != key)
    {
        if (incoming_score < weakest_score)
            goto release;
        ++stats->replacements;
    }
    *target = incoming;
    ++stats->stores;
release:
    atomic_store_explicit(&bucket->occupied, 0, memory_order_release);
}

/**
 * @brief Moves a matching cache or PV hint to the front of the generated move list.
 *
 * Follows engine.c's prioritize_move behavior: matches only the from/to squares and promotes the
 * first match, shifting preceding moves one position to the right. Retains the complete move from
 * the current list rather than copying cached move metadata. All other moves keep their relative
 * order, and the move count is unchanged. An absent hint, an empty list, or a match already at the
 * front leaves the list unchanged.
 *
 * The supplied move is only an ordering hint; this function does not add moves or validate their
 * legality. The caller supplies the generated candidate list and must have exclusive access while
 * it is reordered, before sharing the list or assigning child ranges to workers.
 *
 * @param list Non-null generated move list to reorder in place.
 * @param move Hint whose from/to squares identify the candidate to prioritize.
 * @return     True if the move was found, including when it was already first.
 */
static bool parallel_prioritize_move(XqMoveList *list,
                                     XqMove      move)
{
    int i;
    for (i = 0; i < list->count; ++i)
        if (list->moves[i].from == move.from && list->moves[i].to == move.to)
        {
            XqMove prioritized = list->moves[i];
            for (; i > 0; --i)
                list->moves[i] = list->moves[i - 1];
            list->moves[0] = prioritized;
            return true;
        }
    return false;
}

/**
 * @brief Stably sorts candidates by the built-in capture-ordering score, highest first.
 *
 * Calls xq_engine_default_move_order_score() once per move, reusing engine.c's piece values and
 * MVV-LVA-style formula: quiet moves score zero; captures score CAPTURE_ORDER_BASE plus sixteen
 * times the victim's value minus the attacker's value. Captures precede quiet moves, with valuable
 * victims and cheaper attackers generally preferred. Both MAX and MIN nodes sort descending: these
 * scores predict promising moves for the moving side, not the root's position evaluation.
 *
 * Uses insertion sort directly on list->moves, keeping a parallel local array of ordering scores.
 * At iteration i, the prefix [0, i) is already sorted and its scores are initialized. Saves the
 * current move before shifting that prefix, then inserts the move and score at the same index. Only
 * strictly smaller scores shift right, so equal-scoring moves retain their input order. The move
 * count and all move metadata are preserved; the position is never modified.
 *
 * Call for the first root iteration and continuing internal nodes, before promoting cache/PV hints
 * and before assigning batches to helpers. Later root iterations retain their previous
 * search-result ordering instead. Terminal, depth-zero and cache-cutoff nodes skip this work. This
 * function provides no synchronization: the caller must exclusively own the move list. It neither
 * checks move legality nor searches children, and does not invoke a custom scorer.
 *
 * Empty and single-move lists require no shifts. Worst-case time is O(count squared), with
 * O(XQ_MAX_MOVES) stack storage and no heap allocation.
 *
 * @param position Position in which the candidates were generated; must not be null. Passed to the
 *                 default scorer, which currently does not inspect it; left unchanged.
 * @param list     Exclusively owned generated move list, sorted in place; must not be null and have
 *                 count in 0..XQ_MAX_MOVES with valid piece/captured metadata.
 */
static void parallel_order_moves(const XqPosition *position,
                                 XqMoveList       *list)
{
    int scores[XQ_MAX_MOVES];
    int i;
    for (i = 0; i < list->count; ++i)
    {
        /* The unprocessed suffix is untouched; score this original candidate exactly once. */
        XqMove move  = list->moves[i];
        int    score = xq_engine_default_move_order_score(position, move, NULL);
        int    j     = i;
        /* Move each score together with its candidate; strict comparison preserves ties. */
        while (j > 0 && scores[j - 1] < score)
        {
            list->moves[j] = list->moves[j - 1];
            scores[j]      = scores[j - 1];
            --j;
        }
        /* Close the gap with the saved candidate, extending the sorted prefix by one. */
        list->moves[j] = move;
        scores[j]      = score;
    }
}

typedef struct ParallelPv
{
    XqMove moves[PARALLEL_MAX_PV_MOVES];
    int    count;
} ParallelPv;

/*
 * Each node has at most two batches. Retain only each batch's selected child PV, rather than
 * allocating a full variation for every move. The batch's physical thread is its sole writer.
 */
typedef struct BatchPv
{
    int        best_index; /* -1 until this batch submits a valid child. */
    ParallelPv child_pv;
} BatchPv;

/**
 * @brief Prepends a selected move to its child's PV, truncating to the 64-move hint capacity.
 *
 * Builds the current node's line as [move] followed by the child's continuation. The child PV
 * starts after move has been played and must not already include it. For example, move A and child
 * line [B, C] produce [A, B, C]; an empty child line produces the one-move line [A].
 *
 * Reserves the first slot for move and copies at most PARALLEL_MAX_PV_MOVES - 1 child moves. A full
 * 64-move child line therefore loses its last move when prepended. Only the resulting prefix and
 * count are overwritten; any old array elements beyond count remain irrelevant. The child is
 * unchanged, and complete XqMove records are copied without modifying metadata.
 *
 * Called after the node selects its final best child and waits for any helper to finish. The
 * destination must be exclusively owned, and the child must be stable and visible to this thread.
 * The two objects must not overlap: writing the first move and using memcpy are not safe for
 * in-place prepending. This function supplies no synchronization or allocation of its own.
 *
 * Concatenates hints only; it does not validate move legality, search positions or change scores.
 * Truncation limits the next iteration's ordering hints, never the requested search depth. The
 * caller must discard cancelled child results before selecting a continuation to concatenate.
 *
 * @param pv    Non-null caller-owned output; receives a line of length 1..PARALLEL_MAX_PV_MOVES.
 * @param move  Selected move at the current node, placed at the start of the output line.
 * @param child Non-null child continuation, distinct from pv, with count in
 *              0..PARALLEL_MAX_PV_MOVES and that many initialized moves; left unchanged.
 */
static void parallel_build_pv(ParallelPv       *pv,
                              XqMove            move,
                              const ParallelPv *child)
{
    int count = child->count;
    /* Leave one slot for the current move, preserving the beginning of the child's line. */
    if (count > PARALLEL_MAX_PV_MOVES - 1)
        count = PARALLEL_MAX_PV_MOVES - 1;
    pv->moves[0] = move;
    memcpy(pv->moves + 1, child->moves, (size_t)count * sizeof(*pv->moves));
    /* Only this prefix is valid; trailing slots need not be cleared. */
    pv->count = count + 1;
}

/**
 * @brief Reconstructs ordering hints from sufficiently deep exact cache snapshots.
 *
 * Clears pv->count and starts with the caller's already-probed entry, avoiding a second lookup of
 * the initial position. The caller must have matched first->key to position; this function does not
 * repeat that check. Copies both inputs locally and follows cached best moves without modifying the
 * caller's position or snapshot. No unmake operations are needed.
 *
 * At each position, requires both kings to exist and an exact cache entry searched to at least the
 * remaining depth. Matches its best move by from/to against freshly generated pseudo-legal moves,
 * then uses the generated move's complete metadata rather than trusting cached piece and captured
 * fields. Makes that move on another copy and appends it only if making succeeds and the mover's
 * king is not in check afterward. A position with no legal continuation therefore cannot contribute
 * a move, even if its pseudo-legal list is non-empty.
 *
 * After appending, advances the local position, decreases the remaining depth, and reads the next
 * entry through parallel_tt_snapshot(). Each lookup tries its bucket once and copies the entry
 * before releasing the guard. A missing or occupied bucket, non-exact entry, insufficient depth,
 * missing king, absent candidate or illegal move stops reconstruction without discarding the prefix
 * already collected. Reaching depth zero or the 64-move capacity also stops it; these limits bound
 * cycles without a separate repetition detector. An initial depth of zero yields an empty line, and
 * array elements beyond the returned count are unspecified.
 *
 * Successive snapshots need not represent one consistent table state because other workers can
 * replace entries between lookups. The line is only an ordering hint: it neither proves a score nor
 * changes the score that caused the original cache cutoff. Reconstruction lookups refresh hit
 * generations but do not change cache counters. The table must remain alive with a stable
 * generation; its per-bucket guards protect concurrent entry access, not external clear/destroy.
 *
 * Does not inspect node cancellation. The caller must check its node afterward and clear the PV if
 * the result was cancelled. Output storage must be exclusively owned and must not overlap the
 * inputs or shared table storage.
 *
 * @param table    Search's borrowed cache for subsequent positions; NULL stops further lookups but
 *                 does not prevent using the supplied first entry.
 * @param position Non-null starting position corresponding to first; left unchanged.
 * @param depth    Maximum number of moves to reconstruct and initial required cache depth.
 * @param first    Non-null, stable entry snapshot already matched to position; left unchanged.
 * @param pv       Non-null caller-owned output; receives a possibly empty or truncated prefix.
 */
static void parallel_pv_from_tt(XqParallelTranspositionTable *table,
                                const XqPosition             *position,
                                unsigned                      depth,
                                const ParallelTtEntry        *first,
                                ParallelPv                   *pv)
{
    XqPosition      current = *position;
    ParallelTtEntry cached  = *first;
    /* Discard any old line, including when the requested depth is zero. */
    pv->count = 0;
    while (depth > 0 && pv->count < PARALLEL_MAX_PV_MOVES)
    {
        XqMoveList moves;
        XqPosition next;
        int        i;
        if (xq_position_king_square(&current, XQ_RED) == XQ_NO_SQUARE ||
            xq_position_king_square(&current, XQ_BLACK) == XQ_NO_SQUARE ||
            cached.score_kind != XQ_SEARCH_SCORE_EXACT || cached.depth < depth)
            break;
        /* Use the current position's move metadata; the cached move supplies only from/to. */
        xq_generate_pseudo_legal(&current, &moves);
        for (i = 0; i < moves.count; ++i)
            if (moves.moves[i].from == cached.best_move.from &&
                moves.moves[i].to == cached.best_move.to)
                break;
        if (i == moves.count)
            break;
        /* Validate before extending the prefix so an invalid hint is never appended. */
        next = current;
        if (!xq_position_make_move(&next, moves.moves[i]) ||
            xq_position_in_check(&next, current.side_to_move))
            break;
        pv->moves[pv->count++] = moves.moves[i];
        current                = next;
        --depth;
        /* Short-circuit at either limit without probing an unnecessary continuation. */
        if (depth == 0 || pv->count == PARALLEL_MAX_PV_MOVES ||
            !parallel_tt_snapshot(table, xq_position_hash(&current), &cached))
            break;
    }
}

/**
 * @brief Adds one set of cache counters to an existing aggregate without resetting the source.
 *
 * Accumulates probes, hits, cutoffs, stores and replacements field by field. Used after worker
 * joins to merge each worker's private counters into the caller's search totals, then to add the
 * completed search totals to the table's cumulative statistics. Each contribution must be merged
 * only once to avoid counting the same activity again.
 *
 * These are ordinary, non-atomic additions. The caller must have exclusive access to the
 * destination and ensure that the source is no longer being modified; thread joins provide that
 * synchronization for worker counters. This function performs no synchronization itself.
 *
 * @param total Non-null destination aggregate to update.
 * @param part  Non-null, distinct source counters to add; left unchanged.
 */
static void parallel_tt_add_stats(XqTranspositionStats       *total,
                                  const XqTranspositionStats *part)
{
    total->probes += part->probes;
    total->hits += part->hits;
    total->cutoffs += part->cutoffs;
    total->stores += part->stores;
    total->replacements += part->replacements;
}

typedef struct Interval
{
    int  lower, upper;
    bool cancelled; /* A cancelled result is not a score and must not be included in the parent's
                       interval aggregation. */
} Interval;

/* Returned and submitted intervals are ordinary values. Only shared bounds are atomic. */
typedef struct AtomicInterval
{
    atomic_int lower, upper;
} AtomicInterval;

typedef enum NodeState
{
    NODE_RUNNING,
    NODE_FINISHED,
    NODE_CANCELLED
} NodeState;

/*
 * Keep node updates and worker-state transitions genuinely lock-free on supported targets rather
 * than allowing a library-backed atomic int to silently acquire an internal lock.
 */
_Static_assert(ATOMIC_INT_LOCK_FREE == 2,
               "parallel search requires lock-free atomic int");

/*
 * The owner keeps this stack-allocated node and its parent chain alive until its helper completes.
 * Position, moves, depth, parent, maximizing and PV hints are immutable once shared. The previous
 * iteration's hint storage stays alive until the root and all helpers return. Each batch PV has one
 * writer; the owner reads the helper's PV only after completion synchronization. Bounds, window and
 * state use atomic access, as do submission counters and best_index. Each child slot has one writer
 * and is immutable after publication through best_index. Helper completion is acquired through its
 * task completion record after consuming the notification, before the owner returns. State combines
 * finished/cancelled in one atomic transition so readers never see a partial stop.
 */
typedef struct Node
{
    struct Node  *parent;
    XqPosition    position;
    XqMoveList    moves;
    unsigned      depth;
    bool          maximizing;
    const XqMove *pv_hint;
    int pv_hint_count; /* Nonzero only when the hint's first move matched and was moved to index 0.
                        */
    BatchPv        owner_pv, helper_pv;
    AtomicInterval value;
    Interval       children[XQ_MAX_MOVES];
    atomic_int     submitted_count;
    atomic_int submitted_bound; /* MAX: maximum submitted upper bound; MIN: minimum lower bound. */
    atomic_int best_index;
    atomic_int alpha, beta;
    atomic_int state;
} Node;

typedef enum WorkerState
{
    WORKER_IDLE,
    WORKER_RESERVED,
    WORKER_READY
} WorkerState;

/* Indices refer to the final ordered move list; end is exclusive and step is positive. */
typedef struct SearchRange
{
    int begin, end, step;
} SearchRange;

typedef struct Worker
{
    struct Search  *search;
    pthread_t       thread;
    atomic_int      state;
    SearchSemaphore task_available;
    /*
     * Written by the reserving owner, published through READY, cleared by the worker before IDLE.
     * Completion belongs to the task and may outlive the worker's next assignment.
     */
    TaskCompletion      *completion;
    Node                *node;
    SearchRange          range;
    XqTranspositionStats cache_stats; /* Written only by this physical worker thread. */
} Worker;

typedef struct Search
{
    Worker              *workers;
    unsigned             worker_count;
    unsigned             initialized_worker_count;
    atomic_int           shutdown;
    XqParallelOptions    options;
    XqColor              root_color;
    XqSearchProgressFn   progress;
    void                *progress_user;
    XqEvaluateFn         evaluate;
    void                *evaluate_user;
    XqMoveList           root_moves;
    XqTranspositionStats cache_stats;
} Search;

/**
 * @brief Copies a completed node's result after all of its helper work has exited.
 *
 * Completion alone does not freeze the bounds: an in-flight helper may still submit a valid child
 * result. Unless the node has never been shared, the owner must consume its helper's completion
 * notification and acquire its finished flag before calling this function. That synchronization (or
 * exclusive ownership) makes the result visible; these loads need only relaxed ordering.
 *
 * @param node Completed, quiescent node; must not be null and must remain alive during the call.
 * @return     Plain interval whose cancelled flag is derived from the single completion state.
 */
static Interval node_value(const Node *node)
{
    int      state = atomic_load_explicit(&node->state, memory_order_relaxed);
    Interval value = {atomic_load_explicit(&node->value.lower, memory_order_relaxed),
                      atomic_load_explicit(&node->value.upper, memory_order_relaxed),
                      state == NODE_CANCELLED};
    assert(state != NODE_RUNNING);
    return value;
}

/**
 * @brief Converts a cached score and bound kind into a root-perspective search interval.
 *
 * First reconstructs the interval in the cached side-to-move perspective: an exact score s becomes
 * [s, s], a lower bound becomes [s, WIN], and an upper bound becomes [-WIN, s]. WIN and -WIN
 * delimit the search's score range. A cached cutoff therefore retains its proven bound rather than
 * being incorrectly treated as an exact evaluation.
 *
 * At a MAX node the side to move is the root side, so the interval needs no conversion. At a MIN
 * node it is the opponent: negation reverses the endpoints, mapping [L, U] to [-U, -L]. For
 * example, an opponent-side lower bound of 100 becomes a root-side upper bound of -100, represented
 * by [-WIN, -100].
 *
 * This function only converts the supplied data; it does not check the key, depth, cutoff
 * eligibility or ancestor cancellation. The caller checks reuse eligibility before calling and
 * checks node cancellation separately afterward. No shared state is modified.
 *
 * @param entry      Non-null, stable entry snapshot with a valid bound kind and score in [-WIN,
 *                   WIN], normally copied by parallel_tt_probe().
 * @param maximizing True if the current side to move is the root side; false for its opponent.
 * @return           Root-perspective interval with cancelled set to false.
 */
static Interval parallel_tt_interval(const ParallelTtEntry *entry,
                                     bool                   maximizing)
{
    Interval value = {-WIN, WIN, false};
    if (entry->score_kind != XQ_SEARCH_SCORE_UPPER_BOUND)
        value.lower = entry->score;
    if (entry->score_kind != XQ_SEARCH_SCORE_LOWER_BOUND)
        value.upper = entry->score;
    if (!maximizing)
    {
        int lower   = value.lower;
        value.lower = -value.upper;
        value.upper = -lower;
    }
    return value;
}

/**
 * @brief Classifies a completed node's proven interval and attempts to cache its result.
 *
 * Skips cancelled results and disabled caching. In root perspective, equal endpoints produce an
 * exact score; otherwise upper <= alpha produces an upper-bound entry using value.upper, and lower
 * >= beta produces a lower-bound entry using value.lower. An interval satisfying neither cutoff
 * condition is not stored. Exact classification takes precedence over cutoffs.
 *
 * Parallel search inherits a window that can tighten during execution, so classification uses the
 * proven interval rather than a scalar score compared with an original window. The stored score is
 * always an interval endpoint, never an ancestor's alpha or beta threshold.
 *
 * Converts root-perspective scores to the table's side-to-move perspective before writing. MAX
 * nodes need no conversion; MIN nodes negate the score and exchange upper/lower bound kinds,
 * leaving exact values exact. Delegates depth handling, bucket acquisition, replacement and
 * statistics updates to parallel_tt_store(), which may decline the optional write.
 *
 * The caller must finish its own work and wait for all helpers accessing this node before supplying
 * the final interval and selected move. This function does not wait for completion; its relaxed
 * window loads rely on that existing synchronization and the node being quiescent.
 *
 * @param search    Non-null search state containing the optional borrowed table.
 * @param node      Non-null, completed node whose helper tasks have finished accessing it.
 * @param key       Hash of the node's position, including the side to move.
 * @param value     Final root-perspective interval; cancelled results are ignored.
 * @param best_move Move selected from a valid child result, retained as an ordering hint.
 * @param stats     Executing thread's private cache counters, forwarded to the store operation.
 */
static void parallel_tt_store_node(Search               *search,
                                   const Node           *node,
                                   uint64_t              key,
                                   Interval              value,
                                   XqMove                best_move,
                                   XqTranspositionStats *stats)
{
    XqSearchScoreKind kind;
    int               score;
    if (value.cancelled || search->options.transposition_table == NULL)
        return;
    if (value.lower == value.upper)
    {
        kind  = XQ_SEARCH_SCORE_EXACT;
        score = value.lower;
    }
    else if (value.upper <= atomic_load_explicit(&node->alpha, memory_order_relaxed))
    {
        kind  = XQ_SEARCH_SCORE_UPPER_BOUND;
        score = value.upper;
    }
    else if (value.lower >= atomic_load_explicit(&node->beta, memory_order_relaxed))
    {
        kind  = XQ_SEARCH_SCORE_LOWER_BOUND;
        score = value.lower;
    }
    else
        return;
    if (!node->maximizing)
    {
        score = -score;
        if (kind == XQ_SEARCH_SCORE_UPPER_BOUND)
            kind = XQ_SEARCH_SCORE_LOWER_BOUND;
        else if (kind == XQ_SEARCH_SCORE_LOWER_BOUND)
            kind = XQ_SEARCH_SCORE_UPPER_BOUND;
    }
    parallel_tt_store(search->options.transposition_table, key, node->depth, score, kind, best_move,
                      stats);
}

/**
 * @brief Atomically updates a shared bound toward its maximum or minimum.
 *
 * @param bound     Atomic score, accumulated bound or window threshold; must not be null.
 * @param candidate Value to incorporate without overwriting a stronger concurrent update.
 * @param increase  True for maximum, false for minimum.
 * @return          The candidate successfully published, or an already stronger observed bound.
 */
static int tighten_bound(atomic_int *bound,
                         int         candidate,
                         bool        increase)
{
    int current = atomic_load_explicit(bound, memory_order_relaxed);
    while (increase ? candidate > current : candidate < current)
    {
        if (atomic_compare_exchange_weak_explicit(bound, &current, candidate, memory_order_relaxed,
                                                  memory_order_relaxed))
            return candidate;
    }
    return current;
}

/**
 * @brief Refreshes the window and publishes completion or cancellation without taking any lock.
 *
 * Scans strict ancestors for completion and proven thresholds, then tightens atomic alpha/beta with
 * CAS. Bounds and thresholds only tighten, so mixed or stale observations remain conservative: they
 * can cause extra work but cannot justify an invalid cutoff. The node's own proven bounds are never
 * replaced by inherited thresholds.
 *
 * If an ancestor has stopped, proposes NODE_CANCELLED. Otherwise an exact interval or a cutoff
 * proposes NODE_FINISHED. One CAS changes NODE_RUNNING to the chosen terminal state; the first
 * completion wins, and later checks cannot change a valid result into a cancelled result or vice
 * versa. There is no separate finished/cancelled publication window and no locked confirmation.
 * State is only a stop marker, not a publication mechanism for result data, so its loads and CAS
 * use relaxed ordering. Results are copied only after helper-completion synchronization or before
 * sharing.
 *
 * This function makes no callbacks and sends no semaphore notifications. Task, shutdown and helper
 * completion notifications are handled by the scheduler. Running descendants observe ancestor
 * completion at their checkpoints.
 *
 * @param node Node to check and update; must not be null. The node and its parent chain must remain
 *             alive throughout the call. Their non-atomic metadata must already be initialized.
 * @return     True if the node has stopped, including cancellation; false if no stop was observed.
 *             A true result does not mean helpers have exited: the owner must still wait for them
 *             before copying the result or releasing the node.
 */
static bool check_node(Node *node)
{
    const Node *ancestor;
    int         alpha = -INFINITY_SCORE, beta = INFINITY_SCORE;
    int         lower, upper, desired, expected = NODE_RUNNING;
    bool        cancelled = false;
    if (atomic_load_explicit(&node->state, memory_order_relaxed) != NODE_RUNNING)
        return true;
    for (ancestor = node->parent; ancestor != NULL; ancestor = ancestor->parent)
    {
        int bound;
        if (atomic_load_explicit(&ancestor->state, memory_order_relaxed) != NODE_RUNNING)
            cancelled = true;
        if (ancestor->maximizing)
        {
            bound = atomic_load_explicit(&ancestor->value.lower, memory_order_relaxed);
            if (bound > alpha)
                alpha = bound;
        }
        else
        {
            bound = atomic_load_explicit(&ancestor->value.upper, memory_order_relaxed);
            if (bound < beta)
                beta = bound;
        }
    }
    alpha = tighten_bound(&node->alpha, alpha, true);
    beta  = tighten_bound(&node->beta, beta, false);
    lower = atomic_load_explicit(&node->value.lower, memory_order_relaxed);
    upper = atomic_load_explicit(&node->value.upper, memory_order_relaxed);
    if (cancelled)
        desired = NODE_CANCELLED;
    else if (lower == upper || upper <= alpha || lower >= beta)
        desired = NODE_FINISHED;
    else
        return atomic_load_explicit(&node->state, memory_order_relaxed) != NODE_RUNNING;

    /* Failure means another checker already published an immutable terminal state. */
    (void)atomic_compare_exchange_strong_explicit(&node->state, &expected, desired,
                                                  memory_order_relaxed, memory_order_relaxed);
    return true;
}

/**
 * @brief Publishes one child result and aggregates its interval without acquiring a lock.
 *
 * A MAX node's score is the maximum child score, so its lower and upper bounds are respectively the
 * maximum child lower bound and the maximum child upper bound. Symmetrically, a MIN node takes the
 * minimum of each bound. All scores use the root side's perspective.
 *
 * Writes the exclusively owned child slot, then publishes its index with a release CAS if it
 * improves the best bound. Acquiring best_index before inspecting another slot makes its contents
 * visible. Slots are never rewritten; ties retain the first successfully published best index.
 *
 * CAS updates the MAX lower bound or MIN upper bound, and the opposite bound accumulated in
 * submitted_bound (initially -WIN for MAX, WIN for MIN). Only after these updates does an acq_rel
 * fetch-add publish this submission. The last submitter acquires all preceding submissions through
 * the counter's RMW chain, then publishes the final MAX upper bound or MIN lower bound. Reading
 * submitted_bound before that acquire would risk publishing an incomplete aggregate.
 *
 * Each child must be submitted exactly once; cancelled results must be excluded. The owner's and
 * helper's disjoint batches ensure exclusive slot ownership. A completed node may still accept
 * in-flight valid submissions. This function does not check completion or change alpha/beta.
 *
 * @param node  Node with a non-empty move list and initialized aggregation state; must not be null.
 *              Keep it alive until all concurrent submissions have returned.
 * @param index Child slot exclusively owned by this caller, in [0, node->moves.count).
 * @param child Newly submitted valid child interval, not previously included in the aggregation.
 */
static void aggregate(Node    *node,
                      int      index,
                      Interval child)
{
    int best, submitted;
    assert(!child.cancelled && index >= 0 && index < node->moves.count);
    node->children[index] = child;
    best                  = atomic_load_explicit(&node->best_index, memory_order_acquire);
    while (best < 0 || (node->maximizing && child.lower > node->children[best].lower) ||
           (!node->maximizing && child.upper < node->children[best].upper))
    {
        /* A failed CAS acquires the newly observed best slot before the next comparison. */
        if (atomic_compare_exchange_weak_explicit(&node->best_index, &best, index,
                                                  memory_order_acq_rel, memory_order_acquire))
            break;
    }

    if (node->maximizing)
    {
        (void)tighten_bound(&node->value.lower, child.lower, true);
        (void)tighten_bound(&node->submitted_bound, child.upper, true);
    }
    else
    {
        (void)tighten_bound(&node->value.upper, child.upper, false);
        (void)tighten_bound(&node->submitted_bound, child.lower, false);
    }
    submitted = atomic_fetch_add_explicit(&node->submitted_count, 1, memory_order_acq_rel) + 1;
    assert(submitted <= node->moves.count);
    if (submitted == node->moves.count)
    {
        int bound = atomic_load_explicit(&node->submitted_bound, memory_order_relaxed);
        if (node->maximizing)
            (void)tighten_bound(&node->value.upper, bound, false);
        else
            (void)tighten_bound(&node->value.lower, bound, true);
    }
}

/**
 * @brief Retains root moves in descending score order for the next iteration.
 *
 * Copies the root's move list into search->root_moves and stably sorts it by descending child upper
 * bound. All scores use the root side's perspective, so larger values are searched first. A child
 * pruned against the root's alpha may supply only an upper bound; its lower bound can remain
 * unknown. The upper bound therefore supplies an ordering hint, not necessarily an exact
 * evaluation. The temporary scores array moves in step with the copied moves during sorting.
 *
 * Equal scores retain their input order during insertion sort. Finally promotes the selected best
 * move so a tied upper bound cannot precede the move that actually proves the root score. This
 * promotion preserves the relative order of all other moves. best always indexes the original
 * node->moves list, which is not modified by this function.
 *
 * Called only by the root owner after all helper tasks have completed and their results are
 * visible. Requires exclusive access to search->root_moves; no atomic operations are needed here.
 * Updates only the retained move order, not the node's results or the transposition table. The next
 * iteration creates fresh bounds and child results, so these hints cannot justify pruning. If a
 * proven win stopped the root early, unsubmitted children retain unknown upper bounds; the
 * resulting order is unused because the search driver does not start another iteration.
 *
 * @param search Search state receiving the next iteration's root move order; must not be null.
 * @param node   Finished MAX root with initialized child slots and no active helpers; must not be
 *               null and remains unchanged.
 * @param best   Index of the selected, submitted child in node->moves, in [0, node->moves.count).
 */
static void retain_root_order(Search     *search,
                              const Node *node,
                              int         best)
{
    int scores[XQ_MAX_MOVES];
    int i;
    /* Keep the original move-to-child mapping intact while sorting a separate copy. */
    search->root_moves = node->moves;
    for (i = 0; i < node->moves.count; ++i)
        scores[i] = node->children[i].upper;
    /* Shift only strictly smaller scores, preserving the input order for ties. */
    for (i = 1; i < search->root_moves.count; ++i)
    {
        XqMove move  = search->root_moves.moves[i];
        int    score = scores[i];
        int    j     = i;
        while (j > 0 && scores[j - 1] < score)
        {
            search->root_moves.moves[j] = search->root_moves.moves[j - 1];
            scores[j]                   = scores[j - 1];
            --j;
        }
        search->root_moves.moves[j] = move;
        scores[j]                   = score;
    }
    /* A bound tied with the best score is not necessarily an equally good move. */
    (void)parallel_prioritize_move(&search->root_moves, node->moves.moves[best]);
}

static Interval search_node(Search *,
                            const XqPosition *,
                            Node *,
                            unsigned,
                            XqMove *,
                            const XqMove *,
                            int,
                            ParallelPv *,
                            XqTranspositionStats *);

/**
 * @brief Searches a disjoint batch of child moves without an explicit scheduling mutex.
 *
 * Copies the position for each move and recursively searches it. Valid child intervals are
 * submitted once through lock-free aggregate(); cancelled results are excluded. A completed node
 * can still accept valid in-flight submissions. check_node() then refreshes its window and
 * atomically publishes any completion or cancellation. Only the matched PV move at index zero
 * inherits the hint suffix. The batch retains its best child's variation using the same bound
 * comparison and strict tie rule as aggregate(). Within a batch submissions are sequential, so a
 * final global best_index must also be its batch's retained best_index. The other batch never reads
 * or writes this batch's PV while it runs.
 *
 * @param search Shared search state; must not be null.
 * @param node   Parent with generated moves and positive depth; keep it and its parent chain alive.
 * @param range  Batch indices, with 0 <= begin <= end <= node->moves.count and step > 0. Original
 *               move indices are retained for child results and PV hints.
 * @param batch  Exclusively owned PV output, initialized with best_index == -1 before dispatch.
 * @param stats  Cache counters owned by the executing thread, not by the node's owner.
 */
static void search_range(Search               *search,
                         Node                 *node,
                         SearchRange           range,
                         BatchPv              *batch,
                         XqTranspositionStats *stats)
{
    int i;
    for (i = range.begin; i < range.end; i += range.step)
    {
        XqPosition    child_position;
        ParallelPv    child_pv         = {0};
        const XqMove *child_hint       = NULL;
        int           child_hint_count = 0;
        Interval      child;
        if (check_node(node))
            break;

        if (i == 0 && node->pv_hint_count > 1)
        {
            child_hint       = node->pv_hint + 1;
            child_hint_count = node->pv_hint_count - 1;
        }
        child_position = node->position;
        xq_position_make_move(&child_position, node->moves.moves[i]);
        child = search_node(search, &child_position, node, node->depth - 1, NULL, child_hint,
                            child_hint_count, &child_pv, stats);

        if (!child.cancelled)
        {
            int best = batch->best_index;
            if (best < 0 || (node->maximizing && child.lower > node->children[best].lower) ||
                (!node->maximizing && child.upper < node->children[best].upper))
            {
                batch->best_index = i;
                batch->child_pv   = child_pv;
            }
            aggregate(node, i, child);
            if (search->progress != NULL && node->parent == NULL)
            {
                XqSearchProgress update = {
                    XQ_SEARCH_ROOT_MOVE_COMPLETED, node->depth,
                    (unsigned)atomic_load_explicit(&node->submitted_count, memory_order_relaxed),
                    (unsigned)node->moves.count};
                search->progress(&update, search->progress_user);
            }
        }
        (void)check_node(node);
    }
}

/**
 * @brief Runs an auxiliary thread's task loop until the search pool shuts down.
 *
 * Always consumes a task notification before inspecting shutdown or acquiring WORKER_READY. The
 * notification is retained if the owner dispatches before this thread starts waiting. READY
 * publishes all task fields; RESERVED alone does not permit reading them. Shutdown is published
 * only when no tasks remain (or before any were dispatched) and has its own notification.
 *
 * Runs search_range(), including all nested helper waits, then clears the borrowed task fields,
 * publishes task completion and returns itself to IDLE without waiting for the owner. Another
 * dispatcher may immediately overwrite the task fields, so the old completion is held locally while
 * notifying its owner and releasing the worker's reference. No old task fields or borrowed nodes
 * are accessed after publishing IDLE. Keep Worker and Search alive until the thread is joined.
 *
 * @param argument Pointer to an initialized `Worker` belonging to the search pool; must not be
 *                 null.
 * @return         `NULL` when the thread exits; search scores are submitted directly to nodes.
 */
static void *worker_main(void *argument)
{
    Worker *worker = argument;
    Search *search = worker->search;
    for (;;)
    {
        TaskCompletion *completion;
        search_semaphore_wait(&worker->task_available);
        if (atomic_load_explicit(&search->shutdown, memory_order_acquire))
            break;
        if (atomic_load_explicit(&worker->state, memory_order_acquire) != WORKER_READY)
            abort();
        completion = worker->completion;
        search_range(search, worker->node, worker->range, &worker->node->helper_pv,
                     &worker->cache_stats);
        worker->node       = NULL;
        worker->range      = (SearchRange){0, 0, 1};
        worker->completion = NULL;
        atomic_store_explicit(&completion->finished, 1, memory_order_release);
        atomic_store_explicit(&worker->state, WORKER_IDLE, memory_order_release);
        /*
         * Only the captured completion may be used now: worker task fields may already be reused.
         */
        search_semaphore_post(&completion->notification);
        task_completion_release(completion);
    }
    return NULL;
}

/**
 * @brief Assigns a node's helper batch and returns the owner's range in the ordered move list.
 *
 * Called once after all move ordering and before searching any child. With fewer than two moves, no
 * idle helper, or a zero split percentage under CONTIGUOUS, returns {0, count, 1} without
 * dispatching a task, leaving all moves for the current thread to search serially.
 *
 * Otherwise, reserves the first idle worker. CONTIGUOUS assigns the last `ceil(count *
 * split_percent / 100)` moves, limited to `1..count-1`, with step 1 for both batches. INTERLEAVED
 * ignores the percentage (including zero): the owner searches {0, count, 2} and the helper searches
 * {1, count, 2}. Both strategies produce non-empty, disjoint batches covering all original indices.
 * Dispatch does not wait for a first child result.
 *
 * Acquires IDLE with a CAS to RESERVED, ensuring exclusive ownership of task fields and acquiring
 * the previous worker release. Creates a per-task completion, stores the borrowed node and batch
 * range, release-publishes READY, and posts a task notification. The owner waits on this completion
 * even after pruning; the worker can return to IDLE independently. If completion allocation or
 * semaphore initialization fails, releases the reservation and keeps the entire batch serial.
 *
 * Only already-created, idle workers receive tasks; there is no pending-task queue. If reservation
 * fails, the current thread keeps searching instead of waiting for a worker. Thus nested splits
 * cannot create queued tasks that depend on workers already blocked waiting for those tasks.
 *
 * The node must have no active helper or previously claimed moves and must remain alive until its
 * assigned helper has completed the batch. This function schedules work but does not search moves
 * or wait for the helper to finish.
 *
 * @param search  Shared search state with validated split options and an initialized worker pool;
 *                must not be null.
 * @param node    Node with a generated move list ready for its initial batch assignment; must not
 *                be null.
 * @param pending Required owner-local output; receives this task's completion, or NULL if not
 *                split.
 * @return        The owner's batch range, or {0, count, 1} if no split occurs.
 */
static SearchRange split_node(Search          *search,
                              Node            *node,
                              TaskCompletion **pending)
{
    unsigned    i;
    int         count = node->moves.count;
    SearchRange owner = {0, count, 1};
    *pending          = NULL;
    if (count < 2 || (search->options.split_strategy == XQ_PARALLEL_SPLIT_CONTIGUOUS &&
                      search->options.split_percent == 0))
        return owner;
    for (i = 0; i < search->worker_count; ++i)
    {
        Worker *worker   = &search->workers[i];
        int     expected = WORKER_IDLE;
        if (atomic_compare_exchange_strong_explicit(&worker->state, &expected, WORKER_RESERVED,
                                                    memory_order_acquire, memory_order_relaxed))
        {
            TaskCompletion *completion = task_completion_create();
            SearchRange     helper;
            if (completion == NULL)
            {
                atomic_store_explicit(&worker->state, WORKER_IDLE, memory_order_release);
                return owner;
            }
            if (search->options.split_strategy == XQ_PARALLEL_SPLIT_INTERLEAVED)
            {
                owner.step = 2;
                helper     = (SearchRange){1, count, 2};
            }
            else
            {
                int assigned = (count * (int)search->options.split_percent + 99) / 100;
                if (assigned >= count)
                    assigned = count - 1;
                owner.end = count - assigned;
                helper    = (SearchRange){owner.end, count, 1};
            }
            worker->node       = node;
            worker->range      = helper;
            worker->completion = completion;
            *pending           = completion;
            atomic_store_explicit(&worker->state, WORKER_READY, memory_order_release);
            search_semaphore_post(&worker->task_available);
            /* The worker may already have cleared its task fields; return the local copy. */
            return owner;
        }
    }
    return owner;
}

/**
 * @brief Searches a subtree and returns its proven score interval or a cancellation marker.
 *
 * Creates a local `Node` with a copy of `position`, an initially unknown interval `[-WIN, WIN]`,
 * and a link to `parent`. A node is MAX when its side to move equals `search->root_color` and MIN
 * otherwise; all scores retain the root side's perspective without negation or window flipping.
 * Uses lock-free `check_node()` to refresh inherited bounds and publish completion or cancellation
 * at search checkpoints. Scheduling uses atomic worker states and retained semaphore notifications.
 *
 * The root generates legal moves so even a forced loss returns a legal choice. Descendants search
 * pseudo-legal moves: if the previous mover's king is attacked, that move loses immediately,
 * including at depth zero and when both kings are attacked. This is checked before evaluation or
 * move generation, following the king-capture rule used by the built-in engine's quiescence.
 *
 * Checks terminal positions before the depth limit. A missing king loses for its side. At depth
 * zero, tests pseudo-legal moves only until the first legal continuation is found, preserving mate
 * and stalemate detection without constructing a full legal list. A non-terminal leaf is evaluated
 * using `search->evaluate` and clamped to `[-STATIC_LIMIT, STATIC_LIMIT]`. At positive depth, a
 * node with no legal continuation loses because every pseudo-legal child loses (or its move list is
 * empty). Terminal scores are `-WIN` or `WIN` from the root perspective; accepted terminal and leaf
 * scores produce exact intervals `[score, score]`.
 *
 * After terminal and depth-zero checks, an optional table supplies a hash move and may resolve a
 * non-root node with a sufficiently deep exact value or cutoff bound. Bounds remain intervals; the
 * root uses only the move hint in its first iteration; later iterations reuse the previous root
 * ordering. Cached deeper results may change fixed-depth scores. Exact cache cutoffs reconstruct a
 * PV prefix from guarded snapshots; bound-only cutoffs return an empty PV.
 *
 * Continuing nodes apply stable capture ordering (except a root reusing its prior ordering), then
 * promote the cache move and finally the matched previous-iteration PV move. Hints are matched by
 * from/to and only the matching child inherits their suffix. All ordering and hint metadata is
 * finalized before splitting. Leaf and cache-cutoff paths skip this sorting work.
 *
 * For a continuing internal node, initializes all child intervals as unknown and calls
 * `split_node()` once to assign a batch using the configured strategy if a helper is available.
 * Searches its retained range using `search_range()`, which submits results and checks bounds.
 * After its own batch ends, consumes the task's completion notification and acquires its finished
 * flag, releases its reference, and checks the node again. The helper may already have another
 * task. Even a finished or cancelled node must wait for this handshake to protect its stack
 * lifetime.
 *
 * A valid cutoff result retains the node's proven interval rather than substituting an ancestor
 * threshold. If ancestor completion causes cancellation before a normal completion is published,
 * returns an interval marked `cancelled`, which callers must exclude from score aggregation. The
 * root has no ancestor thresholds and returns a collapsed interval. For a searched root with a
 * selected child, retains root ordering and writes `best_move` if requested; the child's proven
 * lower bound witnesses the root score. Early returns leave that output unchanged. Builds pv_out
 * from the final best_index's batch only after helper synchronization. No PV is retained for a
 * cancelled result; all PVs are ordering hints and never replace proven score intervals.
 *
 * No explicit scheduling mutex is used. Running operations are not interrupted asynchronously, so
 * ancestor changes are observed at later checkpoints. The input position is read-only, and its
 * parent chain must remain valid throughout the call. Custom evaluation callbacks must support
 * concurrent invocation.
 *
 * @param search        Initialized shared search state; must not be null.
 * @param position      Position to search; must not be null and must remain unchanged during the
 *                      call.
 * @param parent        Parent node, or `NULL` for the root.
 * @param depth         Remaining search depth; `0` evaluates a non-terminal leaf.
 * @param best_move     Optional root move output; pass `NULL` for recursive child searches.
 * @param pv_hint       Read-only previous-iteration PV, or NULL for an unmatched path. Its storage
 *                      must outlive this call and all helpers and must not overlap pv_out.
 * @param pv_hint_count Number of available hint moves, in 0..PARALLEL_MAX_PV_MOVES.
 * @param pv_out        Required caller-owned output, cleared on entry; terminal/leaf/cancelled
 *                      results leave it empty. Bound cutoffs may truncate a searched line.
 * @param stats         Cache counters owned by the executing thread throughout this recursion.
 * @return              Proven interval, exact or bounded, if `cancelled` is `false`; otherwise a
 *                      cancellation marker whose score bounds must not be used.
 */
static Interval search_node(Search               *search,
                            const XqPosition     *position,
                            Node                 *parent,
                            unsigned              depth,
                            XqMove               *best_move,
                            const XqMove         *pv_hint,
                            int                   pv_hint_count,
                            ParallelPv           *pv_out,
                            XqTranspositionStats *stats)
{
    Node            node = {0};
    TaskCompletion *pending;
    SearchRange     owner_range;
    Interval        value;
    XqMove          hash_move = {0};
    uint64_t        key       = 0;
    int             score     = 0, i;
    bool            leaf = false, has_hash_move = false;
    pv_out->count            = 0;
    node.parent              = parent;
    node.position            = *position;
    node.depth               = depth;
    node.maximizing          = position->side_to_move == search->root_color;
    node.owner_pv.best_index = node.helper_pv.best_index = -1;
    atomic_init(&node.submitted_count, 0);
    atomic_init(&node.submitted_bound, node.maximizing ? -WIN : WIN);
    atomic_init(&node.value.lower, -WIN);
    atomic_init(&node.value.upper, WIN);
    atomic_init(&node.state, NODE_RUNNING);
    atomic_init(&node.best_index, -1);
    atomic_init(&node.alpha, -INFINITY_SCORE);
    atomic_init(&node.beta, INFINITY_SCORE);

    if (check_node(&node))
        return node_value(&node);

    /*
     * Check terminal positions before the depth limit: even at depth zero, checkmate must not be
     * treated as an ordinary static evaluation.
     */
    if (xq_position_king_square(position, search->root_color) == XQ_NO_SQUARE)
    {
        score = -WIN;
        leaf  = true;
    }
    else if (xq_position_king_square(position, xq_color_opponent(search->root_color)) ==
             XQ_NO_SQUARE)
    {
        score = WIN;
        leaf  = true;
    }
    else if (parent != NULL &&
             xq_position_in_check(position, xq_color_opponent(position->side_to_move)))
    {
        /*
         * The previous pseudo-legal move exposed its own king. The current side wins even if its
         * own king is also attacked; do not let a leaf evaluation hide the illegal move.
         */
        score = node.maximizing ? WIN : -WIN;
        leaf  = true;
    }
    else
    {
        if (parent == NULL)
        {
            if (search->root_moves.count > 0)
                node.moves = search->root_moves;
            else
                xq_generate_legal(position, &node.moves);
        }
        else
            xq_generate_pseudo_legal(position, &node.moves);
        if (node.moves.count == 0)
        {
            score = node.maximizing ? -WIN : WIN;
            leaf  = true;
        }
        else if (depth == 0)
        {
            /*
             * A non-empty pseudo-legal list can still be mate or stalemate. Only existence of a
             * legal move matters here, so stop at the first one rather than filtering the list.
             */
            score = node.maximizing ? -WIN : WIN;
            for (i = 0; i < node.moves.count; ++i)
            {
                XqPosition next = *position;
                if (xq_position_make_move(&next, node.moves.moves[i]) &&
                    !xq_position_in_check(&next, position->side_to_move))
                {
                    score = search->evaluate(position, search->root_color, search->evaluate_user);
                    if (score > STATIC_LIMIT)
                        score = STATIC_LIMIT;
                    if (score < -STATIC_LIMIT)
                        score = -STATIC_LIMIT;
                    break;
                }
            }
            leaf = true;
        }
    }

    if (check_node(&node))
        return node_value(&node);
    if (leaf)
    {
        /* The node has not been shared; no helper can access it yet. */
        atomic_store_explicit(&node.value.lower, score, memory_order_relaxed);
        atomic_store_explicit(&node.value.upper, score, memory_order_relaxed);
        (void)check_node(&node);
        return node_value(&node);
    }
    if (search->options.transposition_table != NULL)
    {
        ParallelTtEntry cached;
        bool            hit   = false;
        int             alpha = atomic_load_explicit(&node.alpha, memory_order_relaxed);
        int             beta  = atomic_load_explicit(&node.beta, memory_order_relaxed);
        key                   = xq_position_hash(position);
        if ((parent != NULL || search->root_moves.count == 0) &&
            parallel_tt_probe(search->options.transposition_table, key, depth,
                              node.maximizing ? alpha : -beta, node.maximizing ? beta : -alpha,
                              parent == NULL, &cached, &hit, stats))
        {
            value = parallel_tt_interval(&cached, node.maximizing);
            /*
             * This node has not been shared. check_node still gives ancestor cancellation priority.
             */
            atomic_store_explicit(&node.value.lower, value.lower, memory_order_relaxed);
            atomic_store_explicit(&node.value.upper, value.upper, memory_order_relaxed);
            if (cached.score_kind == XQ_SEARCH_SCORE_EXACT)
                parallel_pv_from_tt(search->options.transposition_table, position, depth, &cached,
                                    pv_out);
            (void)check_node(&node);
            value = node_value(&node);
            if (value.cancelled)
                pv_out->count = 0;
            return value;
        }
        if (hit)
        {
            hash_move     = cached.best_move;
            has_hash_move = true;
        }
    }
    if (parent != NULL || search->root_moves.count == 0)
        parallel_order_moves(position, &node.moves);
    if (has_hash_move)
        (void)parallel_prioritize_move(&node.moves, hash_move);
    if (pv_hint != NULL && pv_hint_count > 0 && parallel_prioritize_move(&node.moves, pv_hint[0]))
    {
        node.pv_hint       = pv_hint;
        node.pv_hint_count = pv_hint_count;
    }
    for (i = 0; i < node.moves.count; ++i)
        node.children[i] = (Interval){-WIN, WIN, false};
    if (search->progress != NULL && parent == NULL)
    {
        XqSearchProgress update = {XQ_SEARCH_ITERATION_STARTED, depth, 0,
                                   (unsigned)node.moves.count};
        search->progress(&update, search->progress_user);
    }
    owner_range = split_node(search, &node, &pending);

    search_range(search, &node, owner_range, &node.owner_pv, stats);

    /*
     * Node completion does not release the borrowed node: always consume the helper's notification.
     * Helpers observe cancellation themselves. A notification posted early remains available.
     */
    if (pending != NULL)
    {
        search_semaphore_wait(&pending->notification);
        if (!atomic_load_explicit(&pending->finished, memory_order_acquire))
            abort();
        task_completion_release(pending);
    }

    /*
     * This node is now quiescent; acquiring finished makes all helper result submissions visible.
     */
    (void)check_node(&node);
    assert(atomic_load_explicit(&node.state, memory_order_relaxed) != NODE_RUNNING);
    value = node_value(&node);
    if (search->progress != NULL && parent == NULL)
    {
        XqSearchProgress update = {
            XQ_SEARCH_ITERATION_FINISHED, depth,
            (unsigned)atomic_load_explicit(&node.submitted_count, memory_order_relaxed),
            (unsigned)node.moves.count};
        search->progress(&update, search->progress_user);
    }
    if (!value.cancelled)
    {
        /* Helper-completion synchronization already makes the selected child slot visible. */
        int best = atomic_load_explicit(&node.best_index, memory_order_relaxed);
        if (best >= 0)
        {
            const BatchPv *batch =
                node.owner_pv.best_index == best ? &node.owner_pv : &node.helper_pv;
            assert(batch->best_index == best);
            parallel_build_pv(pv_out, node.moves.moves[best], &batch->child_pv);
            if (parent == NULL)
                retain_root_order(search, &node, best);
            if (best_move != NULL)
            {
                assert(node.children[best].lower == value.lower);
                *best_move = node.moves.moves[best];
            }
            parallel_tt_store_node(search, &node, key, value, node.moves.moves[best], stats);
        }
    }
    return value;
}

/**
 * @brief Shuts down the helper pool, joins created threads, and releases search resources.
 *
 * Release-publishes shutdown and posts one retained task notification per created thread. Joins all
 * created threads before destroying their task semaphores. initialized_worker_count separately
 * tracks initialized task semaphores, including a worker whose pthread_create failed. Task-local
 * completions are released independently; joining also waits for workers' final reference releases.
 *
 * Call from outside the pool only after every task completion has been consumed, or before any
 * tasks were dispatched. This is not asynchronous search cancellation. shutdown must be
 * initialized; zero counts and a NULL workers pointer are allowed. Keep Search alive until this
 * call returns.
 *
 * @param search Search state whose pool resources are to be released exactly once; must not be
 *               null.
 */
static void stop_pool(Search *search)
{
    unsigned i;
    atomic_store_explicit(&search->shutdown, 1, memory_order_release);
    for (i = 0; i < search->worker_count; ++i)
        search_semaphore_post(&search->workers[i].task_available);
    for (i = 0; i < search->worker_count; ++i)
        if (pthread_join(search->workers[i].thread, NULL) != 0)
            abort();
    for (i = 0; i < search->worker_count; ++i)
        parallel_tt_add_stats(&search->cache_stats, &search->workers[i].cache_stats);
    for (i = 0; i < search->initialized_worker_count; ++i)
        search_semaphore_destroy(&search->workers[i].task_available);
    free(search->workers);
}
#else
/* Keep the public cache API linkable where parallel search itself is unsupported. */
XqParallelTranspositionTable *xq_parallel_transposition_table_create(void)
{
    return NULL;
}

void xq_parallel_transposition_table_clear(XqParallelTranspositionTable *table)
{
    (void)table;
}

void xq_parallel_transposition_table_destroy(XqParallelTranspositionTable *table)
{
    (void)table;
}

void xq_parallel_transposition_table_reset_stats(XqParallelTranspositionTable *table)
{
    (void)table;
}

void xq_parallel_transposition_table_get_stats(const XqParallelTranspositionTable *table,
                                               XqTranspositionStats               *stats)
{
    (void)table;
    if (stats != NULL)
        memset(stats, 0, sizeof(*stats));
}
#endif

/**
 * @brief Searches a position with iterative deepening using a per-call pool of helper threads.
 *
 * Copies the supplied options, or uses `xq_parallel_options_default()` when `options` is null.
 * Depth and total thread count must be at least one, the split percentage must be in 0..100, and
 * split_strategy must be CONTIGUOUS or INTERLEAVED. The calling thread searches the root; on POSIX
 * platforms, creates `thread_count - 1` helpers before starting the search and reuses them across
 * iterations from depth 1 through options.depth. Completed root scores order the next iteration
 * even without a table. Internal nodes and the first root iteration use the built-in capture
 * ordering, with cache and then previous-PV moves promoted to the front. PV hints are retained for
 * up to 64 moves and passed only along matching paths; they may end early at terminal positions or
 * cache bounds/misses/contention. This does not limit search depth or alter score-bound semantics.
 * An enabled table retains entries and advances its generation each iteration. Stops at the maximum
 * depth, a terminal root, or a proven winning move, as in the built-in search. Each iteration waits
 * for all its helper work before the next begins. CONTIGUOUS assigns a tail batch by percentage;
 * zero disables splitting, not helper creation. INTERLEAVED assigns sorted indices 0,2,... to the
 * owner and 1,3,... to its helper, ignoring the percentage even when zero. Both strategies split
 * only when a helper is idle and keep all moves serial otherwise. The search is synchronous, has no
 * deadline or quiescence search, and does not use the engine adapter's cache or history.
 *
 * All scores use the root side-to-move's perspective. A null evaluator selects
 * `xq_engine_default_static_evaluate`; ordinary leaf scores are clamped to [-STATIC_LIMIT,
 * STATIC_LIMIT], while terminal wins and losses receive WIN and -WIN. Terminal positions are
 * checked before the depth limit, and having no legal moves is a loss even without check. On
 * success, returns the last iteration's root value and a best move when available. Without caching,
 * the value is exact for that iteration's depth; a proven win may stop before the requested
 * maximum. An enabled table may reuse results from greater depths. Terminal roots also succeed but
 * have `move_available == false`; callers must check that flag before using `best_move`.
 * Equal-score move selection may depend on thread scheduling.
 *
 * Evaluators may be called concurrently; the callback and its user data must support this and must
 * not modify positions. Scheduling uses atomic states and semaphores, without explicit mutexes.
 * Each split owns a separate completion notification, allowing its worker to become available
 * before the owner collects the result. Failure to create a completion falls back to serial work
 * for that node; failure to create the initial worker pool returns a resource error.
 *
 * Zeroes a non-null `result` before validating arguments, leaving it zeroed on failure. On success,
 * fills the move and score. Cache counters accumulate on the optional borrowed table; PV cache
 * reconstruction does not contribute to them. No trace, timing or other search counters are
 * collected. Joins all created helpers and releases initialized resources before returning,
 * including after partial thread-creation failure. The input position is never modified, and no
 * helper remains running after the call.
 *
 * @param pos                           Root position; must not be null and must remain valid and
 *                                      unchanged during the call. Its side to move must be XQ_RED
 *                                      or XQ_BLACK.
 * @param options                       Optional search configuration, copied on entry; null selects
 *                                      defaults.
 * @param evaluate                      Optional static evaluator; null selects the built-in
 *                                      evaluator.
 * @param evaluate_user                 Opaque user data passed unchanged to the evaluator; must
 *                                      remain valid for the duration of any callback using it.
 * @param result                        Required output record, initialized even when the search
 *                                      fails.
 * @retval XQ_PARALLEL_OK               Search completed, including terminal root positions.
 * @retval XQ_PARALLEL_INVALID_ARGUMENT A required pointer is null, an option is out of range, or
 *                                      the root side to move is invalid.
 * @retval XQ_PARALLEL_RESOURCE_ERROR   Pool semaphore initialization, worker allocation, or thread
 *                                      creation failed on a supported platform.
 * @retval XQ_PARALLEL_UNSUPPORTED      Arguments are valid, but the build targets Windows.
 */
XqParallelStatus xq_engine_parallel_search(const XqPosition        *pos,
                                           const XqParallelOptions *options,
                                           XqEvaluateFn             evaluate,
                                           void                    *evaluate_user,
                                           XqParallelResult        *result)
{
    return xq_engine_parallel_search_with_progress(pos, options, evaluate, evaluate_user, result,
                                                   NULL, NULL);
}

/**
 * @brief Runs parallel iterative deepening with optional root progress notifications.
 *
 * Uses the same search semantics as xq_engine_parallel_search(): copies options or selects the
 * defaults, creates a helper pool once, and searches depths 1 through the configured maximum. Stops
 * early for a terminal root or a proven winning move. The search has no deadline or quiescence
 * extension. Root ordering, PV hints and an optional borrowed transposition table are retained
 * across iterations; cached results may come from greater search depths.
 *
 * For each searched root iteration, the calling thread emits STARTED before dispatching work and
 * FINISHED after all root callbacks and helper work have completed. FINISHED may describe a partial
 * iteration when a winning move permits an early exit. ROOT_MOVE_COMPLETED events report valid
 * submitted child results; cancelled children are excluded. Terminal roots and argument, platform
 * or initial resource failures emit no iteration events.
 *
 * Root-move callbacks execute synchronously on the thread submitting the result, which may be a
 * helper. They can overlap and arrive out of count order; observers must synchronize shared state
 * and retain the maximum completed count within each iteration. Counts reset at STARTED and measure
 * root moves, not total work or remaining time. All callbacks finish before return. A null progress
 * callback disables notifications without changing the search algorithm.
 *
 * Zeroes a non-null result before argument validation. On success, score is the last iteration's
 * value from the root side's perspective; inspect move_available before using best_move.
 * Equal-score choices may depend on scheduling. The input position is never modified. All created
 * helpers are joined and initialized pool resources released, including on failure. The caller
 * retains ownership of evaluator data, observer data and the optional table; the table must not be
 * accessed externally until this call returns.
 *
 * @param pos                           Required root position, valid and unchanged throughout the
 *                                      call; its side to move must be XQ_RED or XQ_BLACK.
 * @param options                       Optional configuration copied on entry; null selects
 *                                      defaults. Depth and thread count must be positive, and
 *                                      split_percent must be in 0..100 even for INTERLEAVED, which
 *                                      ignores it. split_strategy must be CONTIGUOUS or
 *                                      INTERLEAVED.
 * @param evaluate                      Optional static evaluator; null selects the built-in
 *                                      evaluator. Must support concurrent calls and must not modify
 *                                      positions. Cached evaluations must be deterministic for a
 *                                      given position and perspective.
 * @param evaluate_user                 Borrowed data passed unchanged to evaluate; may be null.
 *                                      Referenced data must remain valid and support concurrent
 *                                      evaluator access until return.
 * @param result                        Required output record; initialized even when the search
 *                                      fails. Terminal roots return success with move_available set
 *                                      to false.
 * @param progress                      Optional root observer; must support concurrent root-move
 *                                      callbacks. Snapshots are borrowed only for the duration of
 *                                      each callback. Keep callbacks short and do not mutate or
 *                                      reenter the search or its state.
 * @param progress_user                 Borrowed pointer passed unchanged to progress; may be null
 *                                      and is ignored when progress is null. Keep referenced data
 *                                      valid until return.
 * @retval XQ_PARALLEL_OK               Search completed, including terminal root positions.
 * @retval XQ_PARALLEL_INVALID_ARGUMENT A required pointer is null, an option is out of range, or
 *                                      the root side to move is invalid.
 * @retval XQ_PARALLEL_RESOURCE_ERROR   Initial worker allocation, semaphore initialization or
 *                                      thread creation failed on a supported platform.
 * @retval XQ_PARALLEL_UNSUPPORTED      Arguments are valid, but the build targets Windows.
 */
XqParallelStatus xq_engine_parallel_search_with_progress(const XqPosition        *pos,
                                                         const XqParallelOptions *options,
                                                         XqEvaluateFn             evaluate,
                                                         void                    *evaluate_user,
                                                         XqParallelResult        *result,
                                                         XqSearchProgressFn       progress,
                                                         void                    *progress_user)
{
    XqParallelOptions effective = options != NULL ? *options : xq_parallel_options_default();
    if (result != NULL)
        memset(result, 0, sizeof(*result));
    if (pos == NULL || result == NULL || effective.depth == 0 || effective.thread_count == 0 ||
        effective.split_percent > 100 ||
        (effective.split_strategy != XQ_PARALLEL_SPLIT_CONTIGUOUS &&
         effective.split_strategy != XQ_PARALLEL_SPLIT_INTERLEAVED) ||
        (pos->side_to_move != XQ_RED && pos->side_to_move != XQ_BLACK))
        return XQ_PARALLEL_INVALID_ARGUMENT;
#ifdef _WIN32
    (void)evaluate;
    (void)evaluate_user;
    (void)progress;
    (void)progress_user;
    return XQ_PARALLEL_UNSUPPORTED;
#else
    {
        Search     search      = {0};
        ParallelPv previous_pv = {0};
        Interval   value;
        XqMove     best        = {0};
        unsigned   i, current_depth;
        atomic_init(&search.shutdown, 0);
        search.options       = effective;
        search.progress      = progress;
        search.progress_user = progress_user;
        search.root_color    = pos->side_to_move;
        search.evaluate      = evaluate != NULL ? evaluate : xq_engine_default_static_evaluate;
        search.evaluate_user = evaluate_user;
        if (effective.thread_count > 1)
        {
            search.workers = calloc(effective.thread_count - 1, sizeof(*search.workers));
            if (search.workers == NULL)
            {
                stop_pool(&search);
                return XQ_PARALLEL_RESOURCE_ERROR;
            }
        }
        for (i = 0; i + 1 < effective.thread_count; ++i)
        {
            Worker *worker = &search.workers[i];
            int     error;
            worker->search = &search;
            atomic_init(&worker->state, WORKER_IDLE);
            if (!search_semaphore_init(&worker->task_available))
            {
                stop_pool(&search);
                return XQ_PARALLEL_RESOURCE_ERROR;
            }
            ++search.initialized_worker_count;
            error = pthread_create(&worker->thread, NULL, worker_main, worker);
            if (error != 0)
            {
                stop_pool(&search);
                return XQ_PARALLEL_RESOURCE_ERROR;
            }
            ++search.worker_count;
        }
        for (current_depth = 1;; ++current_depth)
        {
            ParallelPv current_pv = {0};
            parallel_tt_new_iteration(effective.transposition_table, search.evaluate,
                                      search.evaluate_user, search.root_color);
            best  = (XqMove){0};
            value = search_node(&search, pos, NULL, current_depth, &best, previous_pv.moves,
                                previous_pv.count, &current_pv, &search.cache_stats);
            assert(!value.cancelled && value.lower == value.upper);
            /* All helper tasks have completed; no thread still borrows previous_pv. */
            previous_pv            = current_pv;
            result->score          = value.lower;
            result->best_move      = best;
            result->move_available = best.from != best.to;
            /* Check before incrementing so even UINT_MAX cannot wrap the iteration depth. */
            if (!result->move_available || result->score == WIN || current_depth == effective.depth)
                break;
        }
        stop_pool(&search);
        if (effective.transposition_table != NULL)
            parallel_tt_add_stats(&effective.transposition_table->stats, &search.cache_stats);
        return XQ_PARALLEL_OK;
    }
#endif
}
