#ifndef XIANGQI_PARALLEL_SEARCH_H
#define XIANGQI_PARALLEL_SEARCH_H

#include "xiangqi/engine.h"

typedef enum XqParallelStatus
{
    XQ_PARALLEL_OK, /* Includes terminal positions: inspect move_available. */
    XQ_PARALLEL_INVALID_ARGUMENT,
    XQ_PARALLEL_RESOURCE_ERROR,
    XQ_PARALLEL_UNSUPPORTED
} XqParallelStatus;

/* Every score uses the root side's perspective, including MIN-node scores.
 * Node/source ID zero means no node (root parent or unbounded window). */
typedef struct XqParallelEvent
{
    const char *kind;
    uint64_t sequence, node_id, parent_id;
    uint64_t alpha_source, beta_source;
    unsigned worker_id, depth;
    bool maximizing;
    bool move_available;
    XqMove move; /* Incoming move, or the child move for submit/dispatch. */
    int lower, upper, alpha, beta;
} XqParallelEvent;

/* Called serially under the search mutex. Do not block waiting for search work
 * or reenter this search. Copy the event if retaining it after the callback. */
typedef void (*XqParallelTraceFn)(const XqParallelEvent *event, void *user);

typedef struct XqParallelOptions
{
    unsigned depth;          /* >= 1; default 3. No deadline or quiescence. */
    unsigned thread_count;   /* >= 1, includes calling thread; default 2. */
    unsigned split_percent;  /* 0..100; zero disables splitting; default 50. */
    XqParallelTraceFn trace;
    void *trace_user;
} XqParallelOptions;

typedef struct XqParallelResult
{
    bool move_available;
    XqMove best_move;
    int score; /* Exact root-side value on OK, including terminal roots. */
    unsigned thread_count;
    uint64_t nodes, splits, bound_updates, cancelled_tasks;
    bool elapsed_available;
    uint64_t elapsed_ms; /* Includes pool creation and teardown. */
} XqParallelResult;

XqParallelOptions xq_parallel_options_default(void);
const char *xq_parallel_status_text(XqParallelStatus status);

/* pos is never modified. NULL options selects defaults; NULL evaluate uses the
 * built-in evaluator. The evaluator and its user data must support concurrent
 * calls and must not modify positions. No adapter cache/history/search is used.
 * Results are initialized even on failure. Equal-score choices may depend on
 * scheduling. POSIX pthread implementation; Windows returns UNSUPPORTED. */
XqParallelStatus xq_engine_parallel_search(const XqPosition *pos,
    const XqParallelOptions *options, XqEvaluateFn evaluate, void *evaluate_user,
    XqParallelResult *result);

#endif
