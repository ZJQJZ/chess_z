#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#elif !defined(_WIN32_WINNT)
#define _WIN32_WINNT 0x0600
#endif
#include "search_sampling_internal.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <stdatomic.h>
#endif

struct XqSamplingThread
{
    XqSearchSampler *sampler;
    XqSearchProgressFn progress;
    void *user;
    XqMove path[XQ_SEARCH_SAMPLE_MAX_MOVES];
    uint64_t normal_ply, quiescence_ply;
    uint64_t last_request, sequence;
    unsigned thread_id, iteration_depth;
    XqSearchThreadState state;
};

struct XqSearchSampler
{
#ifdef _WIN32
    volatile LONG64 request;
#else
    atomic_uint_fast64_t request;
#endif
    uint64_t id;
    unsigned capacity;
    XqSamplingThread *threads;
};

#ifdef _WIN32
static volatile LONG64 next_search_id;
#else
static atomic_uint_fast64_t next_search_id = ATOMIC_VAR_INIT(0);
#endif

static uint64_t sample_clock_ms(void)
{
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;
    return (uint64_t)now.tv_sec * UINT64_C(1000) + (uint64_t)now.tv_nsec / 1000000;
#endif
}

static uint64_t current_request(XqSearchSampler *sampler)
{
#ifdef _WIN32
    return (uint64_t)InterlockedCompareExchange64(&sampler->request, 0, 0);
#else
    return atomic_load_explicit(&sampler->request, memory_order_relaxed);
#endif
}

XqSearchSampler *xq_search_sampler_create(unsigned thread_capacity)
{
    if (thread_capacity == 0)
        return NULL;
    XqSearchSampler *sampler = calloc(1, sizeof(*sampler));
    if (sampler == NULL)
        return NULL;
    sampler->threads = calloc(thread_capacity, sizeof(*sampler->threads));
    if (sampler->threads == NULL)
    {
        free(sampler);
        return NULL;
    }
#ifdef _WIN32
    sampler->id = (uint64_t)InterlockedIncrement64(&next_search_id);
#else
    atomic_init(&sampler->request, 0);
    sampler->id = atomic_fetch_add_explicit(&next_search_id, 1, memory_order_relaxed) + 1;
#endif
    sampler->capacity = thread_capacity;
    for (unsigned i = 0; i < thread_capacity; ++i)
    {
        sampler->threads[i].sampler = sampler;
        sampler->threads[i].thread_id = i;
    }
    return sampler;
}

void xq_search_sampler_destroy(XqSearchSampler *sampler)
{
    if (sampler != NULL)
    {
        free(sampler->threads);
        free(sampler);
    }
}

void xq_search_sampler_request(XqSearchSampler *sampler)
{
    if (sampler == NULL)
        return;
#ifdef _WIN32
    InterlockedIncrement64(&sampler->request);
#else
    (void)atomic_fetch_add_explicit(&sampler->request, 1, memory_order_relaxed);
#endif
}

uint64_t xq_search_sampler_id(const XqSearchSampler *sampler)
{
    return sampler == NULL ? 0 : sampler->id;
}

unsigned xq_search_sampler_capacity(const XqSearchSampler *sampler)
{
    return sampler == NULL ? 0 : sampler->capacity;
}

XqSamplingThread *xq_sampling_thread(XqSearchSampler *sampler, unsigned index)
{
    return sampler != NULL && index < sampler->capacity ? &sampler->threads[index] : NULL;
}

static void emit(XqSamplingThread *thread, XqSearchProgressEvent event, uint64_t request)
{
    if (thread->progress == NULL)
        return;
    uint64_t count = thread->normal_ply + thread->quiescence_ply;
    XqSearchSample sample = {
        .search_id = thread->sampler->id,
        .request_id = request,
        .sequence = ++thread->sequence,
        .timestamp_ms = sample_clock_ms(),
        .iteration_depth = thread->iteration_depth,
        .thread_id = thread->thread_id,
        .state = thread->state,
        .normal_ply = thread->normal_ply,
        .quiescence_ply = thread->quiescence_ply,
        .path_length = count > XQ_SEARCH_SAMPLE_MAX_MOVES ? XQ_SEARCH_SAMPLE_MAX_MOVES : (size_t)count,
        .path = thread->path,
        .truncated = count > XQ_SEARCH_SAMPLE_MAX_MOVES
    };
    if (event == XQ_SEARCH_THREAD_STATE)
    {
        sample.path = NULL;
        sample.path_length = 0;
    }
    XqSearchProgress update = {event, thread->iteration_depth, 0, 0, &sample};
    thread->progress(&update, thread->user);
}

void xq_sampling_state(XqSamplingThread *thread, XqSearchThreadState state)
{
    if (thread == NULL || thread->state == state)
        return;
    thread->state = state;
    emit(thread, XQ_SEARCH_THREAD_STATE, current_request(thread->sampler));
}

void xq_sampling_register(XqSamplingThread *thread, XqSearchProgressFn progress, void *user)
{
    if (thread == NULL)
        return;
    thread->normal_ply = thread->quiescence_ply = 0;
    thread->iteration_depth = 0;
    thread->progress = progress;
    thread->user = user;
    thread->state = XQ_SEARCH_THREAD_IDLE;
    emit(thread, XQ_SEARCH_THREAD_STATE, current_request(thread->sampler));
}

void xq_sampling_begin(XqSamplingThread *thread, unsigned iteration_depth,
                       XqSearchProgressFn progress, void *user)
{
    if (thread == NULL)
        return;
    thread->normal_ply = thread->quiescence_ply = 0;
    thread->iteration_depth = iteration_depth;
    thread->progress = progress;
    thread->user = user;
    /* Iteration boundaries must remain visible even if a preceding early return
     * left the same logical state. They also synchronize the new depth metadata. */
    thread->state = XQ_SEARCH_THREAD_SEARCH;
    emit(thread, XQ_SEARCH_THREAD_STATE, current_request(thread->sampler));
}

void xq_sampling_inherit(XqSamplingThread *destination, const XqSamplingThread *source)
{
    if (destination == NULL || source == NULL)
        return;
    uint64_t count = source->normal_ply + source->quiescence_ply;
    size_t length = count > XQ_SEARCH_SAMPLE_MAX_MOVES ? XQ_SEARCH_SAMPLE_MAX_MOVES : (size_t)count;
    memcpy(destination->path, source->path, length * sizeof(*source->path));
    destination->normal_ply = source->normal_ply;
    destination->quiescence_ply = source->quiescence_ply;
    destination->iteration_depth = source->iteration_depth;
    destination->progress = source->progress;
    destination->user = source->user;
    /* last_request and sequence belong to the physical thread, not the task. */
}

void xq_sampling_push(XqSamplingThread *thread, XqMove move, bool quiescence)
{
    uint64_t count = thread->normal_ply + thread->quiescence_ply;
    if (count < XQ_SEARCH_SAMPLE_MAX_MOVES)
        thread->path[count] = move;
    if (quiescence)
        ++thread->quiescence_ply;
    else
        ++thread->normal_ply;
}

void xq_sampling_pop(XqSamplingThread *thread, bool quiescence)
{
    if (quiescence)
        --thread->quiescence_ply;
    else
        --thread->normal_ply;
}

void xq_sampling_checkpoint(XqSamplingThread *thread, bool quiescence)
{
    if (thread->state == XQ_SEARCH_THREAD_IDLE ||
        thread->state == XQ_SEARCH_THREAD_WAIT_HELPERS)
        return;
    uint64_t request = current_request(thread->sampler);
    thread->state = quiescence ? XQ_SEARCH_THREAD_QUIESCENCE : XQ_SEARCH_THREAD_SEARCH;
    if (request == thread->last_request)
        return;
    thread->last_request = request;
    emit(thread, XQ_SEARCH_PATH_SAMPLE, request);
}
