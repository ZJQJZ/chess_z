#ifndef XIANGQI_SEARCH_SAMPLING_H
#define XIANGQI_SEARCH_SAMPLING_H

#include "xiangqi/types.h"
#include <stddef.h>

#define XQ_SEARCH_SAMPLE_MAX_MOVES 1024

typedef struct XqSearchSampler XqSearchSampler;

typedef enum XqSearchThreadState
{
    XQ_SEARCH_THREAD_IDLE,
    XQ_SEARCH_THREAD_SEARCH,
    XQ_SEARCH_THREAD_QUIESCENCE,
    XQ_SEARCH_THREAD_WAIT_HELPERS
} XqSearchThreadState;

/* Borrowed only for the duration of a progress callback. These are cooperative
 * logical-path samples, not OS CPU-time measurements or node visit counts.
 * sequence orders both state events and path replies within one physical thread.
 * A newly registered thread emits IDLE with iteration_depth == 0; each iteration
 * of the calling thread is bounded by SEARCH/IDLE. State events have no path.
 * timestamp_ms uses the monotonic clock; zero means the clock was unavailable. */
typedef struct XqSearchSample
{
    uint64_t search_id;
    uint64_t request_id;
    uint64_t sequence;
    uint64_t timestamp_ms;
    unsigned iteration_depth;
    unsigned thread_id;
    XqSearchThreadState state;
    uint64_t normal_ply;
    uint64_t quiescence_ply;
    size_t path_length;
    const XqMove *path;
    bool truncated;
} XqSearchSample;

/* A controller belongs to one search invocation. request() and the immutable
 * id/capacity accessors may run concurrently with that search.
 * Destroy after search and observer have exited.
 * Capacity includes the calling thread; allocation failure returns NULL. */
XqSearchSampler *xq_search_sampler_create(unsigned thread_capacity);
void xq_search_sampler_destroy(XqSearchSampler *sampler);
void xq_search_sampler_request(XqSearchSampler *sampler);
uint64_t xq_search_sampler_id(const XqSearchSampler *sampler);
unsigned xq_search_sampler_capacity(const XqSearchSampler *sampler);

#endif
