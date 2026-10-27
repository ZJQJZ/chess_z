#ifndef CLI_SEARCH_PROGRESS_H
#define CLI_SEARCH_PROGRESS_H

#include "xiangqi/engine.h"

typedef struct CliSearchProgress CliSearchProgress;

/* CLI-only display. NULL is accepted by update/finish if allocation failed. */
CliSearchProgress *cli_search_progress_start(unsigned max_depth, uint64_t time_limit_ms);
/* Sampling uses an independent request/display thread and a report owner even
 * when stderr is not a terminal. Accepted callbacks are queued with deep-copied
 * paths. A full bounded queue waits for space rather than discarding events;
 * recording backpressure can therefore slow the search. */
CliSearchProgress *cli_search_progress_start_sampling(unsigned max_depth, uint64_t time_limit_ms,
                                                     bool show_progress, unsigned thread_capacity,
                                                     uint64_t interval_ms, const char *output_dir,
                                                     const XqPosition *initial_position,
                                                     const char *search_mode);
CliSearchProgress *cli_search_progress_start_timeline(unsigned max_depth, uint64_t time_limit_ms,
                                                     bool show_progress, unsigned thread_capacity,
                                                     uint64_t interval_ms, const char *output_dir,
                                                     const XqPosition *initial_position,
                                                     const char *search_mode,
                                                     uint64_t timeline_byte_limit);
/* Storage budgets use bytes; zero imposes no artificial limit. Configure them
 * before the request/report threads start. Existing entry points use unlimited
 * statistics storage and retain their explicit timeline-budget argument. */
CliSearchProgress *cli_search_progress_start_sampling_with_limits(
    unsigned max_depth, uint64_t time_limit_ms, bool show_progress,
    unsigned thread_capacity, uint64_t interval_ms, const char *output_dir,
    const XqPosition *initial_position, const char *search_mode,
    bool timeline, uint64_t timeline_byte_limit, size_t statistics_byte_limit);
XqSearchSampler *cli_search_progress_sampler(CliSearchProgress *display);
void cli_search_progress_update(const XqSearchProgress *update, void *user);
/* Call only after the search (and every callback) has returned. stats may describe either engine. */
void cli_search_progress_finish(CliSearchProgress *display, const char *status,
                                const XqSearchStats *stats);

#endif
