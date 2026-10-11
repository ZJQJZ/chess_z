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
 * splitting is possible. The search has no time limit.
 *
 * @return The default parallel-search configuration.
 */
XqParallelOptions xq_parallel_options_default(void)
{
    XqParallelOptions options = {3, 2, 50};
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

/* Notifications persist until consumed, including when posted before a wait. These wrappers avoid
 * explicit scheduling mutexes; the platform semaphore implementation need not be lock-free. */
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
 * Returning does not identify the event: the caller must acquire the corresponding worker state or
 * shutdown flag before deciding how to proceed. This function does not change those states.
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
 * The caller must ensure all users have finished and no further waits or posts can occur. This
 * function does not wake waiters or join threads; pool teardown joins workers before calling it.
 * Call exactly once per successful initialization, including cleanup after partial pool creation.
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

/* Keep node updates and worker-state transitions genuinely lock-free on supported targets rather
 * than allowing a library-backed atomic int to silently acquire an internal lock. */
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "parallel search requires lock-free atomic int");

/* The owner keeps this stack-allocated node and its parent chain alive until its helper completes.
 * Position, moves, depth, parent and maximizing are immutable once shared. Bounds, window and state
 * use atomic access, as do submission counters and best_index. Each child slot has one writer and
 * is immutable after publication through best_index. Helper completion is acquired through its
 * worker state after consuming the completion notification, before the owner returns. State
 * combines finished/cancelled in one atomic transition so readers never see a partial stop.
 */
typedef struct Node
{
    struct Node *parent;
    XqPosition position;
    XqMoveList moves;
    unsigned depth;
    bool maximizing;
    AtomicInterval value;
    Interval children[XQ_MAX_MOVES];
    atomic_int submitted_count;
    atomic_int submitted_bound; /* MAX: maximum submitted upper bound; MIN: minimum lower bound. */
    atomic_int best_index;
    atomic_int alpha, beta;
    atomic_int state;
} Node;

typedef enum WorkerState
{
    WORKER_IDLE,
    WORKER_RESERVED,
    WORKER_READY,
    WORKER_DONE
} WorkerState;

typedef struct Worker
{
    struct Search *search;
    pthread_t thread;
    atomic_int state;
    SearchSemaphore task_available;
    SearchSemaphore task_done;
    /* Written by the reserving owner, published through READY, cleared before DONE. The owner
     * consumes task_done before publishing IDLE, so another task cannot steal its notification. */
    Node *node;
    int begin, end;
} Worker;

typedef struct Search
{
    Worker *workers;
    unsigned worker_count;
    unsigned initialized_worker_count; /* Workers with both semaphores initialized. */
    atomic_int shutdown;
    XqParallelOptions options;
    XqColor root_color;
    XqEvaluateFn evaluate;
    void *evaluate_user;
} Search;

/**
 * @brief Copies a completed node's result after all of its helper work has exited.
 *
 * Completion alone does not freeze the bounds: an in-flight helper may still submit a valid child
 * result. Unless the node has never been shared, the owner must consume its helper's completion
 * notification and acquire WORKER_DONE before calling this function. That synchronization (or
 * exclusive ownership) makes the result visible; these loads need only relaxed ordering.
 *
 * @param node Completed, quiescent node; must not be null and must remain alive during the call.
 * @return     Plain interval whose cancelled flag is derived from the single completion state.
 */
static Interval node_value(const Node *node)
{
    int state = atomic_load_explicit(&node->state, memory_order_relaxed);
    Interval value = {atomic_load_explicit(&node->value.lower, memory_order_relaxed),
                      atomic_load_explicit(&node->value.upper, memory_order_relaxed),
                      state == NODE_CANCELLED};
    assert(state != NODE_RUNNING);
    return value;
}

/**
 * @brief Atomically updates a shared bound toward its maximum or minimum.
 *
 * @param bound     Atomic score, accumulated bound or window threshold; must not be null.
 * @param candidate Value to incorporate without overwriting a stronger concurrent update.
 * @param increase  True for maximum, false for minimum.
 * @return          The candidate successfully published, or an already stronger observed bound.
 */
static int tighten_bound(atomic_int *bound, int candidate, bool increase)
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
    int alpha = -INFINITY_SCORE, beta = INFINITY_SCORE;
    int lower, upper, desired, expected = NODE_RUNNING;
    bool cancelled = false;
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
    beta = tighten_bound(&node->beta, beta, false);
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
static void aggregate(Node *node, int index, Interval child)
{
    int best, submitted;
    assert(!child.cancelled && index >= 0 && index < node->moves.count);
    node->children[index] = child;
    best = atomic_load_explicit(&node->best_index, memory_order_acquire);
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

static Interval search_node(Search *, const XqPosition *, Node *, unsigned, XqMove *);

/**
 * @brief Searches a disjoint batch of child moves without an explicit scheduling mutex.
 *
 * Copies the position for each move and recursively searches it. Valid child intervals are
 * submitted once through lock-free aggregate(); cancelled results are excluded. A completed node
 * can still accept valid in-flight submissions. check_node() then refreshes its window and
 * atomically publishes any completion or cancellation.
 *
 * @param search Shared search state; must not be null.
 * @param node   Parent with generated moves and positive depth; keep it and its parent chain alive.
 * @param begin  Inclusive start index of this thread's batch.
 * @param end    Exclusive end index, with 0 <= begin <= end <= node->moves.count.
 */
static void search_range(Search *search, Node *node, int begin, int end)
{
    int i;
    for (i = begin; i < end; ++i)
    {
        XqPosition child_position;
        Interval child;
        if (check_node(node))
            break;

        child_position = node->position;
        xq_position_make_move(&child_position, node->moves.moves[i]);
        child = search_node(search, &child_position, node, node->depth - 1, NULL);

        if (!child.cancelled)
            aggregate(node, i, child);
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
 * Runs search_range(), including all nested helper waits, then clears the borrowed task fields and
 * release-publishes WORKER_DONE before notifying the owner. The owner acquires DONE after consuming
 * task_done and alone returns the worker to IDLE. Until then it cannot be reassigned. No task
 * fields or borrowed nodes are accessed after publishing DONE, until a new task arrives. Keep the
 * Worker and Search records alive until the thread is joined.
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
        search_semaphore_wait(&worker->task_available);
        if (atomic_load_explicit(&search->shutdown, memory_order_acquire))
            break;
        if (atomic_load_explicit(&worker->state, memory_order_acquire) != WORKER_READY)
            abort();
        search_range(search, worker->node, worker->begin, worker->end);
        worker->node = NULL;
        worker->begin = worker->end = 0;
        atomic_store_explicit(&worker->state, WORKER_DONE, memory_order_release);
        search_semaphore_post(&worker->task_done);
    }
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
 * Acquires IDLE with a CAS to RESERVED, ensuring exclusive ownership of task fields and acquiring
 * the previous owner's release. Stores the borrowed node and batch range, release-publishes READY,
 * and posts a task notification. The returned helper must be waited for even after pruning; only
 * its owner may consume the completion notification and release it back to IDLE.
 *
 * Only already-created, idle workers receive tasks; there is no pending-task queue. If reservation
 * fails, the current thread keeps searching instead of waiting for a worker. Thus nested splits
 * cannot create queued tasks that depend on workers already blocked waiting for those tasks.
 *
 * The node must have no active helper or previously claimed moves and must remain alive until its
 * assigned helper has completed the batch. This function schedules work but does not search moves
 * or wait for the helper to finish.
 *
 * @param search Shared search state with validated split options and an initialized worker pool;
 *               must not be null.
 * @param node   Node with a generated move list ready for its initial batch assignment; must not be
 *               null.
 * @param helper Required owner-local output; receives the reserved worker, or NULL if not split.
 * @return       Exclusive end index of the current thread's batch. Equals `node->moves.count` if no
 *               split occurs; otherwise also equals the helper's inclusive start index.
 */
static int split_node(Search *search, Node *node, Worker **helper)
{
    unsigned i;
    int count = node->moves.count;
    *helper = NULL;
    if (count < 2 || search->options.split_percent == 0)
        return count;
    for (i = 0; i < search->worker_count; ++i)
    {
        Worker *worker = &search->workers[i];
        int expected = WORKER_IDLE;
        if (atomic_compare_exchange_strong_explicit(&worker->state, &expected, WORKER_RESERVED,
                                                    memory_order_acquire, memory_order_relaxed))
        {
            int assigned = (count * (int)search->options.split_percent + 99) / 100;
            int begin;
            if (assigned >= count)
                assigned = count - 1;
            begin = count - assigned;
            worker->node = node;
            worker->begin = begin;
            worker->end = count;
            *helper = worker;
            atomic_store_explicit(&worker->state, WORKER_READY, memory_order_release);
            search_semaphore_post(&worker->task_available);
            /* The worker may already have cleared its task fields; return the local copy. */
            return begin;
        }
    }
    return count;
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
 * For a continuing internal node, initializes all child intervals as unknown and calls
 * `split_node()` once to assign a tail batch if a helper is available. Searches the retained front
 * batch using `search_range()`, which submits child results and checks updated bounds. After its
 * own batch ends, consumes any helper's completion notification and acquires DONE, then releases
 * that worker back to IDLE and checks the node again. Even a finished or cancelled node must wait
 * for this handshake, because the helper borrows this stack-allocated node and may still access it.
 *
 * A valid cutoff result retains the node's proven interval rather than substituting an ancestor
 * threshold. If ancestor completion causes cancellation before a normal completion is published,
 * returns an interval marked `cancelled`, which callers must exclude from score aggregation. The
 * root has no ancestor thresholds and returns an exact score. For a searched root with a selected
 * child, writes `best_move` if requested; the child's proven lower bound witnesses the root score.
 * Early returns leave that output unchanged.
 *
 * No explicit scheduling mutex is used. Running operations are not interrupted asynchronously, so
 * ancestor changes are observed at later checkpoints. The input position is read-only, and its
 * parent chain must remain valid throughout the call. Custom evaluation callbacks must support
 * concurrent invocation.
 *
 * @param search    Initialized shared search state; must not be null.
 * @param position  Position to search; must not be null and must remain unchanged during the call.
 * @param parent    Parent node, or `NULL` for the root.
 * @param depth     Remaining search depth; `0` evaluates a non-terminal leaf.
 * @param best_move Optional root move output; pass `NULL` for recursive child searches.
 * @return          Proven interval, exact or bounded, if `cancelled` is `false`; otherwise a
 *                  cancellation marker whose score bounds must not be used.
 */
static Interval search_node(Search *search, const XqPosition *position, Node *parent,
                            unsigned depth, XqMove *best_move)
{
    Node node = {0};
    Worker *helper;
    int score = 0, end, i;
    bool leaf = false;
    node.parent = parent;
    node.position = *position;
    node.depth = depth;
    node.maximizing = position->side_to_move == search->root_color;
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
    else if (parent != NULL &&
             xq_position_in_check(position, xq_color_opponent(position->side_to_move)))
    {
        /* The previous pseudo-legal move exposed its own king. The current side wins even if its
         * own king is also attacked; do not let a leaf evaluation hide the illegal move. */
        score = node.maximizing ? WIN : -WIN;
        leaf = true;
    }
    else
    {
        if (parent == NULL)
            xq_generate_legal(position, &node.moves);
        else
            xq_generate_pseudo_legal(position, &node.moves);
        if (node.moves.count == 0)
        {
            score = node.maximizing ? -WIN : WIN;
            leaf = true;
        }
        else if (depth == 0)
        {
            /* A non-empty pseudo-legal list can still be mate or stalemate. Only existence of a
             * legal move matters here, so stop at the first one rather than filtering the list. */
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
    for (i = 0; i < node.moves.count; ++i)
        node.children[i] = (Interval){-WIN, WIN, false};
    end = split_node(search, &node, &helper);

    search_range(search, &node, 0, end);

    /* Node completion does not release the borrowed node: always consume the helper's notification.
     * Helpers observe cancellation themselves. A notification posted early remains available. */
    if (helper != NULL)
    {
        search_semaphore_wait(&helper->task_done);
        if (atomic_load_explicit(&helper->state, memory_order_acquire) != WORKER_DONE)
            abort();
        /* Only this owner can recycle the worker, after consuming this task's completion token. */
        atomic_store_explicit(&helper->state, WORKER_IDLE, memory_order_release);
    }

    /* This node is now quiescent; acquiring DONE makes all helper result submissions visible. */
    (void)check_node(&node);
    assert(atomic_load_explicit(&node.state, memory_order_relaxed) != NODE_RUNNING);
    if (best_move != NULL)
    {
        /* Helper-completion synchronization already makes the selected child slot visible. */
        int best = atomic_load_explicit(&node.best_index, memory_order_relaxed);
        if (best >= 0)
        {
            assert(node.children[best].lower ==
                   atomic_load_explicit(&node.value.lower, memory_order_relaxed));
            *best_move = node.moves.moves[best];
        }
    }
    return node_value(&node);
}

/**
 * @brief Shuts down the helper pool, joins created threads, and releases search resources.
 *
 * Release-publishes shutdown and posts one retained task notification per created thread. Joins all
 * created threads before destroying any semaphores. initialized_worker_count separately tracks
 * complete semaphore pairs, including a worker whose pthread_create failed. A partially initialized
 * pair is cleaned up by its initializer before this function is called.
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
    for (i = 0; i < search->initialized_worker_count; ++i)
    {
        search_semaphore_destroy(&search->workers[i].task_done);
        search_semaphore_destroy(&search->workers[i].task_available);
    }
    free(search->workers);
}
#endif

/**
 * @brief Searches a position to a fixed depth using a per-call pool of helper threads.
 *
 * Copies the supplied options, or uses `xq_parallel_options_default()` when `options` is null.
 * Depth and total thread count must be at least one, and the split percentage must be in 0..100.
 * The calling thread searches the root; on POSIX platforms, creates `thread_count - 1` helpers
 * before starting the search. A split percentage of zero disables task splitting, not helper
 * creation. The search is synchronous, has no deadline or quiescence search, and does not use the
 * engine adapter's cache or history.
 *
 * All scores use the root side-to-move's perspective. A null evaluator selects
 * `xq_engine_default_static_evaluate`; ordinary leaf scores are clamped to [-STATIC_LIMIT,
 * STATIC_LIMIT], while terminal wins and losses receive WIN and -WIN. Terminal positions are
 * checked before the depth limit, and having no legal moves is a loss even without check. On
 * success, returns the exact root value for the requested depth and a best move when available.
 * Terminal roots also succeed but have `move_available == false`; callers must check that flag
 * before using `best_move`. Equal-score move selection may depend on thread scheduling.
 *
 * Evaluators may be called concurrently; the callback and its user data must support this and must
 * not modify positions. Scheduling uses atomic states and semaphores, without explicit mutexes.
 *
 * Zeroes a non-null `result` before validating arguments, leaving it zeroed on failure. On success,
 * fills the move and score. No trace, timing or search counters are collected. Joins all created
 * helpers and releases initialized resources before returning, including after partial
 * thread-creation failure. The input position is never modified, and no helper remains running
 * after the call.
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
 * @retval XQ_PARALLEL_RESOURCE_ERROR   Semaphore initialization, worker allocation, or thread
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
        XqMove best = {0};
        unsigned i;
        atomic_init(&search.shutdown, 0);
        search.options = effective;
        search.root_color = pos->side_to_move;
        search.evaluate = evaluate != NULL ? evaluate : xq_engine_default_static_evaluate;
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
            int error;
            worker->search = &search;
            atomic_init(&worker->state, WORKER_IDLE);
            if (!search_semaphore_init(&worker->task_available))
            {
                stop_pool(&search);
                return XQ_PARALLEL_RESOURCE_ERROR;
            }
            if (!search_semaphore_init(&worker->task_done))
            {
                search_semaphore_destroy(&worker->task_available);
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
        value = search_node(&search, pos, NULL, effective.depth, &best);
        assert(!value.cancelled && value.lower == value.upper);
        result->score = value.lower;
        result->best_move = best;
        result->move_available = best.from != best.to;
        stop_pool(&search);
        return XQ_PARALLEL_OK;
    }
#endif
}
