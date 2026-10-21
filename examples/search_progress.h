#ifndef CLI_SEARCH_PROGRESS_H
#define CLI_SEARCH_PROGRESS_H

#include "xiangqi/engine.h"

typedef struct CliSearchProgress CliSearchProgress;

/* CLI-only display. NULL is accepted by update/finish if allocation failed. */
CliSearchProgress *cli_search_progress_start(unsigned max_depth, uint64_t time_limit_ms);
/* Sampling runs one collector thread even when stderr is not a terminal. */
CliSearchProgress *cli_search_progress_start_sampling(unsigned max_depth, uint64_t time_limit_ms,
                                                     bool show_progress, unsigned thread_capacity,
                                                     uint64_t interval_ms, const char *output_dir,
                                                     const XqPosition *initial_position,
                                                     const char *search_mode);
XqSearchSampler *cli_search_progress_sampler(CliSearchProgress *display);
void cli_search_progress_update(const XqSearchProgress *update, void *user);
/* Call only after the search (and every callback) has returned. stats may describe either engine. */
void cli_search_progress_finish(CliSearchProgress *display, const char *status,
                                const XqSearchStats *stats);

#endif
