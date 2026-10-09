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

typedef struct XqParallelOptions
{
    unsigned depth;          /* >= 1; default 3. No deadline or quiescence. */
    unsigned thread_count;   /* >= 1, includes calling thread; default 2. */
    unsigned split_percent;  /* 0..100; zero disables splitting; default 50. */
} XqParallelOptions;

typedef struct XqParallelResult
{
    bool move_available;
    XqMove best_move;
    int score; /* Exact root-side value on OK, including terminal roots. */
} XqParallelResult;

XqParallelOptions xq_parallel_options_default(void);
const char *xq_parallel_status_text(XqParallelStatus status);

/* pos is never modified. NULL options selects defaults; NULL evaluate uses the
 * built-in evaluator. The evaluator and its user data must support concurrent
 * calls and must not modify positions. No adapter cache/history/search is used.
 * No trace or search statistics are collected. Results are initialized even on failure.
 * Equal-score choices may depend on scheduling. POSIX pthread implementation requiring
 * always-lock-free atomic int; Windows returns UNSUPPORTED. */
XqParallelStatus xq_engine_parallel_search(const XqPosition *pos,
    const XqParallelOptions *options, XqEvaluateFn evaluate, void *evaluate_user,
    XqParallelResult *result);

#endif
