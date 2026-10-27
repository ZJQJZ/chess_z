#ifndef XIANGQI_SEARCH_SAMPLING_INTERNAL_H
#define XIANGQI_SEARCH_SAMPLING_INTERNAL_H

#include "xiangqi/engine.h"

typedef struct XqSamplingThread XqSamplingThread;

XqSamplingThread *xq_sampling_thread(XqSearchSampler *sampler, unsigned index);
/* Bind a newly available physical thread before publishing any work to it. The
 * initial IDLE event is emitted even though calloc already initialized its state. */
void xq_sampling_register(XqSamplingThread *thread, XqSearchProgressFn progress, void *user);
void xq_sampling_begin(XqSamplingThread *thread, unsigned iteration_depth,
                       XqSearchProgressFn progress, void *user);
/* Only the source's owning thread may inherit its path into a reserved idle
 * helper. The worker reservation publishes the destination before execution. */
void xq_sampling_inherit(XqSamplingThread *destination, const XqSamplingThread *source);
void xq_sampling_push(XqSamplingThread *thread, XqMove move, bool quiescence);
void xq_sampling_pop(XqSamplingThread *thread, bool quiescence);
void xq_sampling_checkpoint(XqSamplingThread *thread, bool quiescence);
void xq_sampling_state(XqSamplingThread *thread, XqSearchThreadState state);

#endif
