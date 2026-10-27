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

/* Create/configure before startup; subsequent access is report-writer-thread
 * owned. Callers serialize access; this module does not take an internal lock. */
CliSampleReport *cli_sample_report_create(const char *output_dir,
                                        const XqPosition *position, const char *mode,
                                        uint64_t search_id, unsigned thread_capacity,
                                        uint64_t interval_ms, unsigned max_depth,
                                        uint64_t time_limit_ms);
/* Call only after create and before starting the report owner. Zero removes
 * the artificial per-iteration statistics budget; actual allocation failures
 * and the node-ID bound still produce explicit incomplete reports. */
void cli_sample_report_set_statistics_limit(CliSampleReport *report, size_t byte_limit);
/* Optional timeline storage; call before the report writer starts. Zero request time
 * means unavailable. All input timestamps use the search monotonic clock.
 * A zero byte_limit disables the artificial timeline-file budget. */
bool cli_sample_report_enable_timeline(CliSampleReport *report, uint64_t started_ms,
                                       uint64_t byte_limit);
/* Record a writer-pipeline allocation failure without requiring another
 * allocation; mark statistics and any requested timeline explicitly incomplete. */
void cli_sample_report_allocation_failed(CliSampleReport *report);
void cli_sample_report_sample_timed(CliSampleReport *report, const XqSearchSample *sample,
                                    uint64_t request_timestamp_ms);
void cli_sample_report_timeline_state(CliSampleReport *report, const XqSearchSample *sample);
void cli_sample_report_timeline_gap(CliSampleReport *report, const XqSearchSample *lost,
                                    uint64_t lost_count);
void cli_sample_report_sample(CliSampleReport *report, const XqSearchSample *sample);
void cli_sample_report_boundary(CliSampleReport *report, unsigned depth, bool finished);
void cli_sample_report_complete_through(CliSampleReport *report, unsigned depth);
/* Timeline chunks flush at least every 5s of ticks or whenever the buffer fills.
 * Full JSON/folded checkpoints are paced by size and previous write duration;
 * new chunks become discoverable together with a matching full node dictionary.
 * Boundary completion and finish always publish a full final snapshot. */
void cli_sample_report_tick(CliSampleReport *report, uint64_t now_ms,
                            const CliSampleCounters *counters);
void cli_sample_report_summary(CliSampleReport *report, unsigned depth, uint64_t now_ms,
                               CliSampleSummary *summary);
void cli_sample_report_finish(CliSampleReport *report, const CliSampleCounters *counters);

#endif
