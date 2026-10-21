#ifndef CLI_SEARCH_SAMPLE_REPORT_H
#define CLI_SEARCH_SAMPLE_REPORT_H

#include "xiangqi/engine.h"

typedef struct CliSampleReport CliSampleReport;

typedef struct CliSampleCounters
{
    uint64_t overwritten, dropped, delayed, duplicates, boundaries_dropped;
} CliSampleCounters;

typedef struct CliSampleSummary
{
    uint64_t recent_samples, max_quiescence_ply;
    bool incomplete;
    char hot_path[96];
} CliSampleSummary;

/* All functions are collector-thread owned, except create before thread startup. */
CliSampleReport *cli_sample_report_create(const char *output_dir,
                                        const XqPosition *position, const char *mode,
                                        uint64_t search_id, unsigned thread_capacity,
                                        uint64_t interval_ms, unsigned max_depth,
                                        uint64_t time_limit_ms);
void cli_sample_report_sample(CliSampleReport *report, const XqSearchSample *sample);
void cli_sample_report_boundary(CliSampleReport *report, unsigned depth, bool finished);
void cli_sample_report_complete_through(CliSampleReport *report, unsigned depth);
void cli_sample_report_tick(CliSampleReport *report, uint64_t now_ms,
                            const CliSampleCounters *counters);
void cli_sample_report_summary(CliSampleReport *report, unsigned depth, uint64_t now_ms,
                               CliSampleSummary *summary);
void cli_sample_report_finish(CliSampleReport *report, const CliSampleCounters *counters);

#endif
