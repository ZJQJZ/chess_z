#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "xiangqi/parallel_search.h"
#include "xiangqi/movegen.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief Creates a default configuration for the parallel minimax search.
 *
 * The default configuration searches to a fixed depth of 3 plies, uses 2 threads including the
 * calling thread, and assigns 50 percent of a node's children to an available helper thread when
 * splitting is possible. Event tracing is disabled, with both the callback and its user data set to
 * null. The search has no time limit.
 *
 * @return The default parallel-search configuration.
 */
XqParallelOptions xq_parallel_options_default(void)
{
    XqParallelOptions options = {3, 2, 50, NULL, NULL};
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
#include <time.h>

enum
{
    WIN = 30000,
    STATIC_LIMIT = 28999,
    INFINITY_SCORE = 30001
};

typedef struct Interval
{
    int lower, upper;
    bool cancelled; /* A cancelled result is not a score and must not be included in the parent's
                       interval aggregation. */
} Interval;

/* The node is owned by the thread that creates it; helper threads only borrow it. The owner must
 * wait for helpers to finish using the node before returning. Fields such as position, moves and
 * depth remain unchanged after the node is shared; bounds, child results and completion state are
 * protected by the shared search mutex. */
typedef struct Node
{
    struct Node *parent;
    XqPosition position;
    XqMoveList moves;
    XqMove incoming;
    unsigned depth;
    uint64_t id;
    bool maximizing;
    Interval value;
    Interval children[XQ_MAX_MOVES];
    int best_index;
    int alpha, beta;
    uint64_t alpha_source, beta_source;
    bool finished;
    bool helper_active;
} Node;

typedef struct Worker
{
    struct Search *search;
    pthread_t thread;
    unsigned id;
    bool busy; /* Reserved under the mutex; includes assigned tasks that have not started yet. */
    Node *node;
    int begin, end;
} Worker;

typedef struct Search
{
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    Worker *workers;
    unsigned worker_count;
    bool shutdown;
    XqParallelOptions options;
    XqParallelResult stats;
    XqColor root_color;
    XqEvaluateFn evaluate;
    void *evaluate_user;
    uint64_t next_node, next_event;
} Search;

#ifdef XQ_PARALLEL_TESTING
/* Test barriers run only after the mutex is released; these hooks are absent from production
 * builds. */
extern void xq_parallel_test_checkpoint(const XqParallelEvent *, const XqPosition *);
extern int xq_parallel_test_create(pthread_t *, void *(*)(void *), void *);
#endif

/**
 * @brief Reads the monotonic wall clock and converts it to integer milliseconds.
 *
 * Uses `clock_gettime(CLOCK_MONOTONIC)` to measure elapsed search time, including time spent
 * waiting for helper threads. The clock is unaffected by changes to the system date or time zone.
 * Fractional milliseconds are discarded when converting seconds and nanoseconds.
 *
 * @return Milliseconds elapsed since an unspecified fixed reference point, or `-1` if the clock
 *         cannot be read. This is not calendar time; subtract two readings to measure duration.
 */
static int64_t wall_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return -1;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

/**
 * @brief Copies a search node's current state into an event record.
 *
 * The snapshot contains the node and parent IDs, remaining depth, MAX/MIN role, proven score
 * interval, and the currently stored alpha-beta window with its ancestor source IDs. All scores use
 * the root side's perspective. The function copies these values without refreshing the window.
 *
 * For a non-root node, `move` is the incoming move from its parent and `move_available` is `true`.
 * The root has no incoming move, so `move_available` is `false` and `move` must be ignored.
 * `emit()` may subsequently replace these fields with a child move for a `dispatch` or `submit`
 * event.
 *
 * The caller must hold the search mutex so the shared node state remains consistent while copied.
 * The node is left unchanged. The event `sequence` remains zero until assigned by `emit()`; `kind`
 * is a borrowed string pointer, while the other fields are copied by value.
 *
 * @param node   Node whose state is recorded; must not be null.
 * @param worker ID of the worker producing the event, which need not own the node.
 * @param kind   Event type string; must remain valid while the returned event is used.
 * @return       Event snapshot by value, with `sequence` initialized to zero.
 */
static XqParallelEvent event_snapshot(const Node *node, unsigned worker, const char *kind)
{
    XqParallelEvent event = {0};
    event.kind = kind;
    event.node_id = node->id;
    event.parent_id = node->parent != NULL ? node->parent->id : 0;
    event.worker_id = worker;
    event.depth = node->depth;
    event.maximizing = node->maximizing;
    event.move_available = node->parent != NULL;
    event.move = node->incoming;
    event.lower = node->value.lower;
    event.upper = node->value.upper;
    event.alpha = node->alpha;
    event.beta = node->beta;
    event.alpha_source = node->alpha_source;
    event.beta_source = node->beta_source;
    return event;
}

/**
 * @brief Creates a numbered search event and delivers it to the configured trace callback.
 *
 * Copies the node's current state using `event_snapshot()`, then increments the shared event
 * counter and assigns the new value to the event `sequence`. This counter advances even when no
 * trace callback is configured.
 *
 * If `child` is non-null, its move is copied into the event and `move_available` is set to `true`,
 * replacing the snapshot's incoming move. This associates `dispatch` and `submit` events with a
 * child move, including events at the root. Otherwise, the snapshot's move fields are retained.
 *
 * The caller must hold `search->mutex` throughout this call. The trace callback runs synchronously
 * under that mutex and receives `trace_user` unchanged; it must not wait for search work or reenter
 * this search. The event is local to this function, so a callback retaining it must copy the record
 * rather than retain its address. The `kind` string is borrowed and is not copied.
 *
 * @param search Search state containing the event counter and trace configuration; must not be
 *               null.
 * @param node   Node associated with the event; must not be null and is left unchanged.
 * @param worker ID of the worker recorded for the event, which need not own the node.
 * @param kind   Event type string; must remain valid while the event or a retained copy is used.
 * @param child  Optional child move to associate with the event; null retains the incoming move.
 */
static void emit(Search *search, const Node *node, unsigned worker, const char *kind,
                 const XqMove *child)
{
    XqParallelEvent event = event_snapshot(node, worker, kind);
    event.sequence = ++search->next_event;
    if (child != NULL)
    {
        event.move_available = true;
        event.move = *child;
    }
    if (search->options.trace != NULL)
        search->options.trace(&event, search->options.trace_user);
}

/**
 * @brief Refreshes a node's alpha-beta window from the latest proven ancestor bounds.
 *
 * Walks the parent chain starting at `node->parent`. `alpha` is the maximum proven lower bound
 * among MAX ancestors: those ancestors can choose another MIN child instead of this branch.
 * Symmetrically, `beta` is the minimum proven upper bound among MIN ancestors, which can choose
 * another MAX child. All scores use the root side's perspective.
 *
 * Only strict ancestors contribute. The node's own `value` is its proven score interval, not an
 * inherited search threshold, and is neither read nor modified here. If no ancestor of the
 * corresponding type exists, the bound remains `-INFINITY_SCORE` or `INFINITY_SCORE`, with a source
 * ID of `0`. Equal ancestor bounds retain the nearest ancestor as their source.
 *
 * Always updates `alpha_source` and `beta_source`, even if only the source IDs change. If either
 * bound changes numerically, also updates `alpha` and `beta`, increments
 * `search->stats.bound_updates`, and emits a `refresh` event containing the new window and sources.
 * This function does not itself prune or finish the node.
 *
 * The caller must hold `search->mutex` throughout this call so ancestor bounds are read
 * consistently and the node's window, statistics, and event are updated under the same lock.
 *
 * @param search Shared search state containing the mutex, statistics, and trace configuration; must
 *               not be null.
 * @param node   Node whose window and source IDs are refreshed; must not be null.
 * @param worker ID recorded for a possible `refresh` event, which need not own the node.
 */
static void refresh_window(Search *search, Node *node, unsigned worker)
{
    Node *ancestor;
    int alpha = -INFINITY_SCORE, beta = INFINITY_SCORE;
    uint64_t alpha_source = 0, beta_source = 0;
    for (ancestor = node->parent; ancestor != NULL; ancestor = ancestor->parent)
    {
        if (ancestor->maximizing && ancestor->value.lower > alpha)
        {
            alpha = ancestor->value.lower;
            alpha_source = ancestor->id;
        }
        if (!ancestor->maximizing && ancestor->value.upper < beta)
        {
            beta = ancestor->value.upper;
            beta_source = ancestor->id;
        }
    }
    node->alpha_source = alpha_source;
    node->beta_source = beta_source;
    if (alpha != node->alpha || beta != node->beta)
    {
        node->alpha = alpha;
        node->beta = beta;
        ++search->stats.bound_updates;
        emit(search, node, worker, "refresh", NULL);
    }
}

/**
 * @brief Checks completion using only the node's proven interval and stored search window.
 *
 * Returns immediately if `node->finished` is already `true`, without emitting another event or
 * notification. Otherwise, equal `value.lower` and `value.upper` establish an exact score. If
 * `value.upper <= alpha`, a MAX ancestor already has an alternative at least as good as this branch
 * can achieve. If `value.lower >= beta`, a MIN ancestor already has an alternative no worse for
 * that side. Either inequality permits pruning with a possibly non-exact score interval.
 *
 * On completion, sets `finished`, emits `exact` if the bounds are equal or `cutoff` otherwise, and
 * broadcasts `search->changed`. Leaves the score interval, cancellation marker, and stored
 * alpha-beta window unchanged; ancestor thresholds are never substituted for proven score bounds.
 * If no completion condition holds, leaves the node unchanged and returns `false`.
 *
 * For an unfinished node, the caller must have checked that no ancestor has finished and refreshed
 * the window under `search->mutex`, without releasing the mutex since. Only the node's own interval
 * may have changed in between. This allows `check_node()` to delegate its final check here and
 * `search_range()` to recheck after aggregation without rescanning ancestors. This function does
 * not detect ancestor cancellation, refresh the window, or wait for helpers to release the node.
 *
 * The caller must hold `search->mutex` throughout this call, including the synchronous trace
 * callback and condition-variable notification. Completion does not imply helpers have stopped; the
 * owner must still wait for them before returning and releasing the node's storage.
 *
 * @param search Shared search state containing synchronization and trace configuration; must not be
 *               null.
 * @param node   Node whose interval is checked against its stored window; must not be null.
 * @param worker ID recorded for a completion event, which need not own the node.
 * @return       `true` if the node is finished, including a previously cancelled node; `false` if
 *               search should continue. Inspect `node->value.cancelled` before using its score.
 */
static bool check_interval(Search *search, Node *node, unsigned worker)
{
    if (node->finished)
        return true;
    /* Tightening the window does not by itself make the node's proven interval exact. */
    if (node->value.lower == node->value.upper || node->value.upper <= node->alpha ||
        node->value.lower >= node->beta)
    {
        node->finished = true;
        emit(search, node, worker, node->value.lower == node->value.upper ? "exact" : "cutoff",
             NULL);
        pthread_cond_broadcast(&search->changed);
    }
    return node->finished;
}

/**
 * @brief Checks whether a node can stop after observing the latest ancestor state and bounds.
 *
 * Returns immediately if `node->finished` is already `true`. Otherwise, refreshes the inherited
 * window using `refresh_window()` and checks all strict ancestors. If any ancestor has finished,
 * this branch is no longer needed: sets `finished` and `value.cancelled`, increments
 * `search->stats.cancelled_tasks`, and emits a `cancel` event. A cancelled result must not
 * participate in its parent's score aggregation.
 *
 * If no ancestor has finished, tests the node's own proven interval. Equal `value.lower` and
 * `value.upper` establish an exact score. If `value.upper <= alpha`, a MAX ancestor already has an
 * alternative at least as good as this branch can achieve. Symmetrically, if `value.lower >= beta`,
 * a MIN ancestor already has an alternative no worse for that side. Either inequality permits
 * pruning while retaining the node's valid, possibly non-exact interval. The inherited thresholds
 * are never substituted for the node's own bounds.
 *
 * Marks a newly completed node as `finished`, emits `exact` or `cutoff` as appropriate, and
 * broadcasts `search->changed` on each new completion or cancellation. Waiting threads must recheck
 * their conditions; running descendants observe completion at later checkpoints. This function does
 * not wait for helper tasks or release the node's storage.
 *
 * The caller must hold `search->mutex` throughout this call, including event callbacks and
 * condition-variable notifications.
 *
 * @param search Shared search state containing synchronization, statistics, and trace
 *               configuration; must not be null.
 * @param node   Node to check and possibly mark as finished or cancelled; must not be null.
 * @param worker ID recorded for events produced by this check, which need not own the node.
 * @return       `true` if the node is finished, including cancellation; `false` if search should
 *               continue. Inspect `node->value.cancelled` to distinguish cancellation from a valid
 *               score interval.
 */
static bool check_node(Search *search, Node *node, unsigned worker)
{
    Node *ancestor;
    if (node->finished)
        return true;
    refresh_window(search, node, worker);
    for (ancestor = node->parent; ancestor != NULL; ancestor = ancestor->parent)
        if (ancestor->finished)
        {
            node->finished = true;
            node->value.cancelled = true;
            ++search->stats.cancelled_tasks;
            emit(search, node, worker, "cancel", NULL);
            pthread_cond_broadcast(&search->changed);
            return true;
        }

    return check_interval(search, node, worker);
}

/**
 * @brief Recomputes a node's proven score interval from all stored child intervals.
 *
 * A MAX node's score is the maximum child score, so its lower and upper bounds are respectively the
 * maximum child lower bound and the maximum child upper bound. Symmetrically, a MIN node takes the
 * minimum of each bound. All scores use the root side's perspective.
 *
 * Includes every entry in `children` corresponding to `moves`, even if its result has not yet been
 * submitted. Such entries retain the unknown interval `[-WIN, WIN]`: an unknown child keeps a MAX
 * node's upper bound at `WIN` and a MIN node's lower bound at `-WIN`. An accepted child can still
 * improve a MAX node's lower bound or a MIN node's upper bound before all children finish.
 *
 * Each stored interval must be valid. The caller must exclude cancelled results before storing
 * them; this function does not inspect `cancelled`. It updates only `node->value.lower` and
 * `node->value.upper`, leaving child intervals, `best_index`, and the inherited `alpha`/`beta`
 * window unchanged. It does not check for completion, emit events, or notify waiting threads.
 *
 * The caller must hold the search mutex while reading child intervals and updating the node.
 *
 * @param node Node with a non-empty move list and initialized child intervals; must not be null.
 */
static void aggregate(Node *node)
{
    int lower = node->maximizing ? -WIN : WIN;
    int upper = lower;
    int i;
    for (i = 0; i < node->moves.count; ++i)
    {
        Interval child = node->children[i];
        if (node->maximizing)
        {
            if (child.lower > lower)
                lower = child.lower;
            if (child.upper > upper)
                upper = child.upper;
        }
        else
        {
            if (child.lower < lower)
                lower = child.lower;
            if (child.upper < upper)
                upper = child.upper;
        }
    }
    node->value.lower = lower;
    node->value.upper = upper;
}

static Interval search_node(Search *, const XqPosition *, Node *, XqMove, unsigned, unsigned,
                            XqMove *);

/**
 * @brief Searches a contiguous batch of child moves and submits useful results to their parent.
 *
 * Processes move indices in `[begin, end)` in generation order. The node's owner and its helper
 * receive disjoint batches. Before each move, checks the node under `search->mutex` and stops the
 * batch if the node has finished or been cancelled. Copies `node->position` into a fresh local
 * board, makes the move, and calls `search_node()` with one less remaining depth. Board operations
 * and recursive search run without holding the search mutex.
 *
 * After each child returns, reacquires the mutex and checks the parent again: another thread may
 * have finished it during the recursive search. Only stores the child's interval if the parent
 * still needs work and `child.cancelled` is `false`. Cancelled or no-longer-needed results are
 * discarded rather than used as scores.
 *
 * Updates `best_index` using child lower bounds for MAX and child upper bounds for MIN. Equal
 * bounds retain the move whose result was accepted first, regardless of when its search started.
 * Calls `aggregate()` to recompute the parent's interval, emits a `submit` event associated with
 * the child move, checks this node's updated interval against its already-refreshed window, and
 * broadcasts `search->changed`. Ancestor state cannot change during this critical section, so
 * there is no need to refresh the window or check ancestors again after aggregation.
 *
 * The caller must not hold `search->mutex` on entry. The node, its parent chain, position, and move
 * list must remain valid throughout this call; the position and move list must remain unchanged.
 * This function does not release the node or mark a helper as idle; the caller manages those
 * lifetimes and scheduling states.
 *
 * @param search Shared search state; must not be null.
 * @param node   Parent node with initialized child intervals and positive remaining depth; must not
 *               be null.
 * @param begin  Inclusive start index, satisfying `0 <= begin <= end`.
 * @param end    Exclusive end index, satisfying `end <= node->moves.count`.
 * @param worker ID of the worker executing this batch, which need not own the node.
 */
static void search_range(Search *search, Node *node, int begin, int end, unsigned worker)
{
    int i;
    for (i = begin; i < end; ++i)
    {
        XqPosition child_position;
        Interval child;
        pthread_mutex_lock(&search->mutex);
        if (check_node(search, node, worker))
        {
            pthread_mutex_unlock(&search->mutex);
            break;
        }
        pthread_mutex_unlock(&search->mutex);

        child_position = node->position;
        xq_position_make_move(&child_position, node->moves.moves[i]);
        child = search_node(search, &child_position, node, node->moves.moves[i], node->depth - 1,
                            worker, NULL);

        pthread_mutex_lock(&search->mutex);
        if (!check_node(search, node, worker) && !child.cancelled)
        {
            node->children[i] = child;
            /* On ties, retain the move whose result was accepted first, rather than the one whose
             * search started first. */
            if (node->best_index < 0 ||
                (node->maximizing && child.lower > node->children[node->best_index].lower) ||
                (!node->maximizing && child.upper < node->children[node->best_index].upper))
                node->best_index = i;
            aggregate(node);
            emit(search, node, worker, "submit", &node->moves.moves[i]);
            (void)check_interval(search, node, worker);
            pthread_cond_broadcast(&search->changed);
        }
        pthread_mutex_unlock(&search->mutex);
    }
}

/**
 * @brief Runs an auxiliary thread's task loop until the search pool shuts down.
 *
 * Interprets `argument` as the thread's `Worker` record and acquires `search->mutex`. While no task
 * is reserved (`worker->busy` is `false`) and `search->shutdown` is `false`, waits on
 * `search->changed`. `pthread_cond_wait()` atomically releases the mutex and begins waiting, then
 * reacquires it before returning. The loop rechecks the state after every wakeup because
 * notifications may concern other workers or be spurious. Shutdown takes priority over dispatch.
 *
 * For an assigned task, borrows `worker->node`, releases the mutex, and calls `search_range()` for
 * the batch `[worker->begin, worker->end)`. The worker remains busy throughout that call, so the
 * scheduler cannot overwrite its task fields. Search results are submitted by `search_range()`
 * rather than returned through this thread entry point.
 *
 * When the batch returns, including early termination due to pruning or cancellation, reacquires
 * the mutex, clears `node->helper_active`, clears `worker->node`, and sets `worker->busy` to
 * `false`. Broadcasts `search->changed` so waiting threads can observe task completion and the
 * worker's availability, then loops to wait for another task. The borrowed node is not freed; its
 * owner must keep it alive until the helper task has released it.
 *
 * On observing `search->shutdown`, releases the mutex and returns, ending this thread. The caller
 * must keep the `Worker` and shared `Search` records alive until the thread is joined.
 *
 * @param argument Pointer to an initialized `Worker` belonging to the search pool; must not be
 *                 null.
 * @return         `NULL` when the thread exits; search scores are submitted directly to nodes.
 */
static void *worker_main(void *argument)
{
    Worker *worker = argument;
    Search *search = worker->search;
    pthread_mutex_lock(&search->mutex);
    for (;;)
    {
        Node *node;
        while (!worker->busy && !search->shutdown)
            pthread_cond_wait(&search->changed, &search->mutex);
        if (search->shutdown)
            break;
        node = worker->node;
        pthread_mutex_unlock(&search->mutex);
        search_range(search, node, worker->begin, worker->end, worker->id);
        pthread_mutex_lock(&search->mutex);
        node->helper_active = false;
        worker->node = NULL;
        worker->busy = false;
        pthread_cond_broadcast(&search->changed);
    }
    pthread_mutex_unlock(&search->mutex);
    return NULL;
}

/**
 * @brief Assigns a node's tail batch to one idle helper and returns the owner's batch end.
 *
 * Called once for a node after generating moves and before searching any child. If there are fewer
 * than two moves, `split_percent` is `0`, or no helper is idle, returns `node->moves.count` without
 * dispatching a task, leaving all moves for the current thread to search serially.
 *
 * Otherwise, reserves the first idle worker and assigns the last `ceil(count * split_percent /
 * 100)` moves, limited to `1..count-1`. The helper processes `[worker->begin, worker->end)`, while
 * the current thread retains `[0, worker->begin)`, ensuring that both batches are non-empty and
 * disjoint. Dispatch does not wait for a first child result.
 *
 * Sets `worker->busy` before notification so another dispatcher cannot reserve the same worker,
 * stores the borrowed node and batch range, and sets `node->helper_active`. Increments
 * `search->stats.splits`, emits a `dispatch` event with the receiving worker's ID and the first
 * move in its batch, and broadcasts `search->changed` to wake waiting workers.
 *
 * Only already-created, idle workers receive tasks; there is no pending-task queue. If reservation
 * fails, the current thread keeps searching instead of waiting for a worker. Thus nested splits
 * cannot create queued tasks that depend on workers already blocked waiting for those tasks.
 *
 * The caller must hold `search->mutex` throughout this call. The node must have no active helper or
 * previously claimed moves and must remain alive until its assigned helper has exited the batch.
 * This function schedules work but does not search moves or wait for the helper to finish.
 *
 * @param search Shared search state with validated split options and an initialized worker pool;
 *               must not be null.
 * @param node   Node with a generated move list ready for its initial batch assignment; must not be
 *               null.
 * @return       Exclusive end index of the current thread's batch. Equals `node->moves.count` if no
 *               split occurs; otherwise also equals the helper's inclusive start index.
 */
static int split_node(Search *search, Node *node)
{
    unsigned i;
    int count = node->moves.count;
    if (count < 2 || search->options.split_percent == 0)
        return count;
    for (i = 0; i < search->worker_count; ++i)
        if (!search->workers[i].busy)
        {
            Worker *worker = &search->workers[i];
            int assigned = (count * (int)search->options.split_percent + 99) / 100;
            if (assigned >= count)
                assigned = count - 1;
            worker->busy = true;
            worker->node = node;
            worker->begin = count - assigned;
            worker->end = count;
            node->helper_active = true;
            ++search->stats.splits;
            emit(search, node, worker->id, "dispatch", &node->moves.moves[worker->begin]);
            pthread_cond_broadcast(&search->changed);
            return worker->begin;
        }
    return count;
}

/**
 * @brief Searches a subtree and returns its proven score interval or a cancellation marker.
 *
 * Creates a local `Node` with a copy of `position`, an initially unknown interval `[-WIN, WIN]`,
 * and a link to `parent`. A node is MAX when its side to move equals `search->root_color` and MIN
 * otherwise; all scores retain the root side's perspective without negation or window flipping.
 * Registers the node and emits `enter`, then uses `check_node()` to observe ancestor completion and
 * refresh inherited bounds at search checkpoints.
 *
 * Checks terminal positions before the depth limit. A missing king loses for its side, and a side
 * with no legal moves loses whether in check or stalemated; terminal scores are `-WIN` or `WIN`
 * from the root perspective. At `depth == 0`, a non-terminal position is evaluated using
 * `search->evaluate` and clamped to `[-STATIC_LIMIT, STATIC_LIMIT]`. Accepted terminal and leaf
 * scores produce exact intervals `[score, score]`.
 *
 * For a continuing internal node, initializes all child intervals as unknown and calls
 * `split_node()` once to assign a tail batch if a helper is available. Searches the retained front
 * batch using `search_range()`, which submits child results and checks updated bounds. After its
 * own batch ends, continues checking the node while waiting for any helper to exit. Even a finished
 * or cancelled node must wait until `helper_active` is `false` before returning, because the helper
 * borrows this stack-allocated node and may still access it.
 *
 * A valid cutoff result retains the node's proven interval rather than substituting an ancestor
 * threshold. If an ancestor has finished, returns an interval marked `cancelled`, which callers
 * must exclude from score aggregation. The root has no ancestor thresholds and returns an exact
 * score. For a searched root with a selected child, writes `best_move` if requested; the child's
 * proven lower bound witnesses the root score. Early returns leave that output unchanged.
 *
 * The caller must not hold `search->mutex` on entry. Shared state, events, and scheduling are
 * protected by that mutex; move generation, evaluation, and recursive computation run without it.
 * Running operations are not interrupted asynchronously, so ancestor changes are observed at later
 * checkpoints. The input position is read-only, and its parent chain must remain valid throughout
 * the call. Custom evaluation callbacks must support concurrent invocation.
 *
 * @param search    Initialized shared search state; must not be null.
 * @param position  Position to search; must not be null and must remain unchanged during the call.
 * @param parent    Parent node, or `NULL` for the root.
 * @param incoming  Move from the parent to this position; ignored as an incoming move at the root.
 * @param depth     Remaining search depth; `0` evaluates a non-terminal leaf.
 * @param worker    ID of the worker executing this call.
 * @param best_move Optional root move output; pass `NULL` for recursive child searches.
 * @return          Proven interval, exact or bounded, if `cancelled` is `false`; otherwise a
 *                  cancellation marker whose score bounds must not be used.
 */
static Interval search_node(Search *search, const XqPosition *position, Node *parent,
                            XqMove incoming, unsigned depth, unsigned worker, XqMove *best_move)
{
    Node node = {0};
    int score = 0, end, i;
    bool leaf = false;
#ifdef XQ_PARALLEL_TESTING
    XqParallelEvent checkpoint;
#endif
    node.parent = parent;
    node.position = *position;
    node.incoming = incoming;
    node.depth = depth;
    node.maximizing = position->side_to_move == search->root_color;
    node.value.lower = -WIN;
    node.value.upper = WIN;
    node.best_index = -1;
    node.alpha = -INFINITY_SCORE;
    node.beta = INFINITY_SCORE;

    pthread_mutex_lock(&search->mutex);
    node.id = ++search->next_node;
    ++search->stats.nodes;
    emit(search, &node, worker, "enter", NULL);
    if (check_node(search, &node, worker))
    {
        pthread_mutex_unlock(&search->mutex);
        return node.value;
    }
#ifdef XQ_PARALLEL_TESTING
    checkpoint = event_snapshot(&node, worker, "checkpoint");
#endif
    pthread_mutex_unlock(&search->mutex);
#ifdef XQ_PARALLEL_TESTING
    xq_parallel_test_checkpoint(&checkpoint, position);
#endif

    /* Check terminal positions before the depth limit: even at depth zero, checkmate must not be
     * treated as an ordinary static evaluation. */
    if (xq_position_king_square(position, search->root_color) == XQ_NO_SQUARE)
    {
        score = -WIN;
        leaf = true;
    }
    else if (xq_position_king_square(position, xq_color_opponent(search->root_color)) ==
             XQ_NO_SQUARE)
    {
        score = WIN;
        leaf = true;
    }
    else
    {
        xq_generate_legal(position, &node.moves);
        if (node.moves.count == 0)
        {
            score = node.maximizing ? -WIN : WIN;
            leaf = true;
        }
        else if (depth == 0)
        {
            score = search->evaluate(position, search->root_color, search->evaluate_user);
            if (score > STATIC_LIMIT)
                score = STATIC_LIMIT;
            if (score < -STATIC_LIMIT)
                score = -STATIC_LIMIT;
            leaf = true;
        }
    }

    pthread_mutex_lock(&search->mutex);
    if (!check_node(search, &node, worker) && leaf)
    {
        node.value.lower = node.value.upper = score;
        (void)check_node(search, &node, worker);
    }
    if (node.finished)
    {
        pthread_mutex_unlock(&search->mutex);
        return node.value;
    }
    for (i = 0; i < node.moves.count; ++i)
        node.children[i] = (Interval){-WIN, WIN, false};
    end = split_node(search, &node);
    pthread_mutex_unlock(&search->mutex);

    search_range(search, &node, 0, end, worker);

    pthread_mutex_lock(&search->mutex);
    while (node.helper_active)
    {
        (void)check_node(search, &node, worker);
        pthread_cond_wait(&search->changed, &search->mutex);
    }
    (void)check_node(search, &node, worker);
    /* Once all children have returned valid intervals, the node either has an exact score or
     * satisfies an ancestor's cutoff threshold. */
    assert(node.finished);
    if (best_move != NULL && node.best_index >= 0)
    {
        assert(node.children[node.best_index].lower == node.value.lower);
        *best_move = node.moves.moves[node.best_index];
    }
    pthread_mutex_unlock(&search->mutex);
    return node.value;
}

/**
 * @brief Shuts down the helper pool, joins created threads, and releases search resources.
 *
 * Sets `search->shutdown` under `search->mutex` and broadcasts `search->changed` so idle workers
 * wake, observe shutdown, and exit their task loops. Releases the mutex before calling
 * `pthread_join()`, allowing those workers to acquire it and complete their exit paths. Thread
 * return values are ignored by passing `NULL` to `pthread_join()`.
 *
 * Joins only the `search->worker_count` successfully created threads. This supports cleanup after
 * partial thread creation as well as normal search completion. A count of `0` skips joining, and
 * `free(search->workers)` also accepts `NULL` if allocation failed or no helpers were needed. After
 * all workers have exited, frees their records, destroys `search->changed`, and finally destroys
 * `search->mutex`. The caller's `Search` record itself is not freed.
 *
 * The caller must be outside the helper threads, must not hold `search->mutex`, and must have
 * successfully initialized both the mutex and condition variable. All search tasks must already be
 * complete, or must not yet have been dispatched; shutdown is not an asynchronous search
 * cancellation mechanism. Keep the shared state alive until this call returns. This cleanup is
 * performed once; the destroyed synchronization objects cannot be reused without reinitialization,
 * and the worker pointer is not reset after freeing it.
 *
 * @param search Search state whose initialized pool resources are to be released; must not be null.
 */
static void stop_pool(Search *search)
{
    unsigned i;
    pthread_mutex_lock(&search->mutex);
    search->shutdown = true;
    pthread_cond_broadcast(&search->changed);
    pthread_mutex_unlock(&search->mutex);
    for (i = 0; i < search->worker_count; ++i)
        pthread_join(search->workers[i].thread, NULL);
    free(search->workers);
    pthread_cond_destroy(&search->changed);
    pthread_mutex_destroy(&search->mutex);
}
#endif

/**
 * @brief Searches a position to a fixed depth using a per-call pool of helper threads.
 *
 * Copies the supplied options, or uses `xq_parallel_options_default()` when `options` is null.
 * Depth and total thread count must be at least one, and the split percentage must be in 0..100.
 * The calling thread searches the root as worker 0; on POSIX platforms, creates `thread_count - 1`
 * helpers before starting the search. A split percentage of zero disables task splitting, not
 * helper creation. The search is synchronous, has no deadline or quiescence search, and does not
 * use the engine adapter's cache or history.
 *
 * All scores use the root side-to-move's perspective. A null evaluator selects
 * `xq_engine_default_static_evaluate`; ordinary leaf scores are clamped to [-STATIC_LIMIT,
 * STATIC_LIMIT], while terminal wins and losses receive WIN and -WIN. Terminal positions are
 * checked before the depth limit, and having no legal moves is a loss even without check. On
 * success, returns the exact root value for the requested depth and a best move when available.
 * Terminal roots also succeed but have `move_available == false`; callers must check that flag
 * before using `best_move`. Equal-score move selection may depend on thread scheduling.
 *
 * Evaluators run outside the search mutex and may be called concurrently; the callback and its user
 * data must support this and must not modify positions. Optional trace callbacks run serially under
 * the search mutex, possibly on different threads, and must not wait for search work or reenter
 * this search. A trace callback retaining an event must copy it.
 *
 * Zeroes a non-null `result` before validating arguments, leaving it zeroed on failure. On success,
 * fills the move, score, configured thread count, and search statistics. Elapsed milliseconds
 * include pool creation and teardown, and are valid only when `elapsed_available` is true; an
 * unavailable clock does not fail the search. Joins all created helpers and releases initialized
 * resources before returning, including after partial thread-creation failure. The input position
 * is never modified, and no helper remains running after the call.
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
 * @retval XQ_PARALLEL_RESOURCE_ERROR   Mutex/condition initialization, worker allocation, or thread
 *                                      creation failed on a supported platform.
 * @retval XQ_PARALLEL_UNSUPPORTED      Arguments are valid, but the build targets Windows.
 */
XqParallelStatus xq_engine_parallel_search(const XqPosition *pos, const XqParallelOptions *options,
                                           XqEvaluateFn evaluate, void *evaluate_user,
                                           XqParallelResult *result)
{
    XqParallelOptions effective = options != NULL ? *options : xq_parallel_options_default();
    if (result != NULL)
        memset(result, 0, sizeof(*result));
    if (pos == NULL || result == NULL || effective.depth == 0 || effective.thread_count == 0 ||
        effective.split_percent > 100 ||
        (pos->side_to_move != XQ_RED && pos->side_to_move != XQ_BLACK))
        return XQ_PARALLEL_INVALID_ARGUMENT;
#ifdef _WIN32
    (void)evaluate;
    (void)evaluate_user;
    return XQ_PARALLEL_UNSUPPORTED;
#else
    {
        Search search = {0};
        Interval value;
        XqMove best = {0}, incoming = {0};
        unsigned i;
        int64_t start = wall_ms(), end;
        search.options = effective;
        search.root_color = pos->side_to_move;
        search.evaluate = evaluate != NULL ? evaluate : xq_engine_default_static_evaluate;
        search.evaluate_user = evaluate_user;
        if (pthread_mutex_init(&search.mutex, NULL) != 0)
            return XQ_PARALLEL_RESOURCE_ERROR;
        if (pthread_cond_init(&search.changed, NULL) != 0)
        {
            pthread_mutex_destroy(&search.mutex);
            return XQ_PARALLEL_RESOURCE_ERROR;
        }
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
            int error;
            worker->search = &search;
            worker->id = i + 1;
#ifdef XQ_PARALLEL_TESTING
            error = xq_parallel_test_create(&worker->thread, worker_main, worker);
#else
            error = pthread_create(&worker->thread, NULL, worker_main, worker);
#endif
            if (error != 0)
            {
                stop_pool(&search);
                return XQ_PARALLEL_RESOURCE_ERROR;
            }
            ++search.worker_count;
        }
        value = search_node(&search, pos, NULL, incoming, effective.depth, 0, &best);
        assert(!value.cancelled && value.lower == value.upper);
        /* All helper tasks have finished; only the calling thread will access the statistics from
         * now on. */
        *result = search.stats;
        result->score = value.lower;
        result->best_move = best;
        result->move_available = best.from != best.to;
        result->thread_count = effective.thread_count;
        stop_pool(&search);
        end = wall_ms();
        result->elapsed_available = start >= 0 && end >= start;
        if (result->elapsed_available)
            result->elapsed_ms = (uint64_t)(end - start);
        return XQ_PARALLEL_OK;
    }
#endif
}
