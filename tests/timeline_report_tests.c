#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include <stdio.h>
#include <stdlib.h>

/* Isolate report-allocation failures without changing engine allocations. */
static int fail_after = -1;
static void *timeline_test_calloc(size_t count, size_t size)
{
    if (fail_after >= 0 && fail_after-- == 0) return NULL;
    return calloc(count, size);
}
static void *timeline_test_realloc(void *pointer, size_t size)
{
    if (fail_after >= 0 && fail_after-- == 0) return NULL;
    return realloc(pointer, size);
}
#define calloc timeline_test_calloc
#define realloc timeline_test_realloc
#include "../examples/search_sample_report.c"
#undef calloc
#undef realloc

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "check failed: %s (%s:%d)\n", #condition, __FILE__, __LINE__); \
    exit(EXIT_FAILURE); } } while (0)

static void state(CliSampleReport *report, unsigned thread, unsigned depth,
                  uint64_t sequence, uint64_t time, XqSearchThreadState value)
{
    XqSearchSample event = {0};
    event.thread_id = thread;
    event.iteration_depth = depth;
    event.sequence = sequence;
    event.timestamp_ms = time;
    event.state = value;
    cli_sample_report_timeline_state(report, &event);
}

static int statistics_storage_case(const char *directory, bool limited)
{
    XqPosition position;
    xq_position_startpos(&position);
    CliSampleReport *report = cli_sample_report_create(directory, &position, "storage-test",
                                                       77, 1, 20, 2, 0);
    CHECK(report != NULL);
    cli_sample_report_set_statistics_limit(report, limited ? 4096 : 0);
    ReportIteration *iteration = find_iteration(report, 1, true);
    CHECK(iteration != NULL);
    if (limited)
    {
        /* More distinct one-step prefixes than a 4KiB statistics budget fits.
         * Totals must remain valid, with every omitted path explicitly counted. */
        for (unsigned i = 0; i < 256; ++i)
        {
            XqMove move = {(uint8_t)(i / 90), (uint8_t)(i % 90), 0, -1};
            XqSearchSample sample = {0};
            sample.search_id = 77;
            sample.thread_id = 0;
            sample.iteration_depth = 1;
            sample.request_id = sample.sequence = i + 1;
            sample.timestamp_ms = 1000 + i * 20;
            sample.state = XQ_SEARCH_THREAD_SEARCH;
            sample.normal_ply = sample.path_length = 1;
            sample.path = &move;
            cli_sample_report_sample(report, &sample);
        }
        CHECK(iteration->allocated_bytes <= 4096);
        CHECK(iteration->omitted_samples > 0 && iteration->incomplete);
        CHECK(iteration->capacity_reason && !strcmp(iteration->capacity_reason, "statistics_limit"));
        CHECK(node_at(iteration, 0)->inclusive == 256);
    }
    else
    {
        /* Several pages and directory expansions must preserve earlier node
         * addresses, IDs and parent links. No live search or large fixture. */
        enum { GROUPS = 32 };
        uint32_t groups[GROUPS];
        ReportNode *root = node_at(iteration, 0);
        for (unsigned i = 0; i < GROUPS; ++i)
        {
            XqMove move = {0, (uint8_t)i, 0, -1};
            groups[i] = child_node(iteration, 0, REPORT_MOVE, move);
            CHECK(groups[i] != NO_NODE);
        }
        size_t target = (size_t)CLI_SAMPLE_NODE_PAGE_NODES * 5 + GROUPS + 11;
        for (size_t i = 0; iteration->count < target; ++i)
        {
            unsigned unique = (unsigned)(i / GROUPS);
            XqMove move = {(uint8_t)(unique / 90), (uint8_t)(unique % 90), 0, -1};
            uint32_t parent = groups[i % GROUPS];
            uint32_t id = child_node(iteration, parent, REPORT_MOVE, move);
            CHECK(id != NO_NODE && node_at(iteration, id)->parent == parent);
        }
        CHECK(node_at(iteration, 0) == root && iteration->page_count >= 5);
        for (size_t i = 1; i < iteration->count; ++i)
            CHECK(node_at(iteration, i)->parent < i);
        CHECK(!iteration->omitted_samples && !iteration->incomplete);
        iteration->max_normal_ply = 2;
    }
    CliSampleCounters counters = {0};
    cli_sample_report_tick(report, 7000, &counters);
    cli_sample_report_boundary(report, 1, true);
    cli_sample_report_finish(report, &counters);
    return EXIT_SUCCESS;
}

int main(int argc, char **argv)
{
    CHECK(argc == 3);
    const char *mode = argv[2];
    if (!strcmp(mode, "statistics_unlimited") || !strcmp(mode, "statistics_budget"))
        return statistics_storage_case(argv[1], !strcmp(mode, "statistics_budget"));
    XqPosition position;
    xq_position_startpos(&position);
    CliSampleReport *report = cli_sample_report_create(argv[1], &position, "timeline-test",
                                                       77, 3, 20, 2, 0);
    CHECK(report != NULL);
    if (!strcmp(mode, "enable_allocation")) fail_after = 0;
    bool enabled = cli_sample_report_enable_timeline(report, 1000,
        !strcmp(mode, "budget") ? 1 : !strcmp(mode, "unlimited") ? 0 : 16 * 1024 * 1024);
    fail_after = -1;
    CHECK(enabled == (strcmp(mode, "enable_allocation") != 0));
    state(report, 0, 0, 1, 1000, XQ_SEARCH_THREAD_IDLE);
    state(report, 1, 0, 1, 1000, XQ_SEARCH_THREAD_IDLE);
    /* Thread 2 deliberately never registers: it must stay unknown. */
    if (!strcmp(mode, "iteration_allocation")) fail_after = 1;
    if (!strcmp(mode, "iteration_struct_allocation")) fail_after = 0;
    if (!strcmp(mode, "iteration_nodes_allocation")) fail_after = 4;
    state(report, 0, 1, 2, 1010, XQ_SEARCH_THREAD_SEARCH);
    fail_after = -1;
    state(report, 1, 1, 2, 1011, XQ_SEARCH_THREAD_SEARCH);
    XqMoveList legal;
    xq_generate_legal(&position, &legal);
    CHECK(legal.count > 0);
    XqSearchSample sample = {0};
    sample.search_id = 77;
    sample.request_id = 1;
    sample.sequence = 3;
    sample.timestamp_ms = 1020;
    sample.iteration_depth = 1;
    sample.thread_id = 1;
    sample.state = XQ_SEARCH_THREAD_SEARCH;
    sample.normal_ply = sample.path_length = 1;
    sample.path = &legal.moves[0];
    cli_sample_report_sample_timed(report, &sample, 1015);
    state(report, 1, 1, 4, 1021, XQ_SEARCH_THREAD_WAIT_HELPERS);
    sample.sequence = 5;
    sample.timestamp_ms = 1023;
    cli_sample_report_timeline_gap(report, &sample, 2);
    state(report, 1, 1, 7, 1025, XQ_SEARCH_THREAD_SEARCH);
    sample.sequence = 8;
    sample.request_id = 2;
    sample.timestamp_ms = 1025;
    sample.state = XQ_SEARCH_THREAD_QUIESCENCE;
    cli_sample_report_sample_timed(report, &sample, 0);
    if (report->timeline_enabled)
    {
        CHECK(report->states[1].known);
        CHECK(report->states[1].state == XQ_SEARCH_THREAD_QUIESCENCE);
        CHECK(report->states[1].sequence == 8);
    }
    /* An invalid/retrograde clock must never be used as a fake duration. */
    state(report, 1, 1, 9, 1001, XQ_SEARCH_THREAD_WAIT_HELPERS);
    if (report->states)
    {
        CHECK(!report->states[1].known);
        CHECK(report->states[1].valid_time && report->states[1].time_ms == 25);
    }
    state(report, 1, 1, 10, 1030, XQ_SEARCH_THREAD_IDLE);
    state(report, 0, 1, 3, 1040, XQ_SEARCH_THREAD_IDLE);
    if (!strcmp(mode, "write_failure"))
    {
        char blocked[PATH_MAX];
        int length = snprintf(blocked, sizeof(blocked), "%s/depth-1-timeline-000001.json.tmp", report->directory);
        CHECK(length > 0 && (size_t)length < sizeof(blocked));
        CHECK(sample_mkdir(blocked) == 0);
    }
    CliSampleCounters counters = {0};
    cli_sample_report_tick(report, 1040, &counters);
    /* Intermediate manifest must be independently readable. */
    for (ReportIteration *iteration = report->iterations; iteration; iteration = iteration->next)
        write_iteration(report, iteration, false);
    cli_sample_report_complete_through(report, 1);
    /* Late root events and late worker samples cannot recreate depth 1. */
    cli_sample_report_boundary(report, 1, false);
    cli_sample_report_sample_timed(report, &sample, 0);
    CHECK(find_iteration(report, 1, false) == NULL);
    state(report, 0, 2, 4, 1050, XQ_SEARCH_THREAD_SEARCH);
    /* A report writer can process a checkpoint before a delayed root end.
     * Its future estimate must not survive the authoritative queued IDLE. */
    cli_sample_report_tick(report, 1150, &counters);
    ReportIteration *second = find_iteration(report, 2, false);
    CHECK(second && second->timeline_end == 150 && !second->timeline_end_known);
    state(report, 0, 2, 5, 1050, XQ_SEARCH_THREAD_IDLE);
    CHECK(second->timeline_end_known && second->timeline_end == 50);
    cli_sample_report_tick(report, 1200, &counters);
    CHECK(second->timeline_end_known && second->timeline_end == 50);
    cli_sample_report_complete_through(report, 2);
    if (!strcmp(mode, "sample_state"))
    {
        state(report, 0, 3, 6, 1060, XQ_SEARCH_THREAD_SEARCH);
        XqSearchSample lost = {0};
        lost.thread_id = 1;
        lost.iteration_depth = 3;
        lost.sequence = 11;
        lost.timestamp_ms = 1061;
        cli_sample_report_timeline_gap(report, &lost, 2);
        sample.iteration_depth = 3;
        sample.sequence = 12;
        sample.request_id = 3;
        sample.timestamp_ms = 1070;
        cli_sample_report_sample_timed(report, &sample, 1065);
        CHECK(!report->states[1].known);
        state(report, 0, 3, 7, 1080, XQ_SEARCH_THREAD_IDLE);
        cli_sample_report_complete_through(report, 3);
        state(report, 0, 4, 8, 1090, XQ_SEARCH_THREAD_SEARCH);
        ReportIteration *fourth = find_iteration(report, 4, false);
        CHECK(fourth && fourth->initial_states && !fourth->initial_states[1].known);
        state(report, 1, 4, 13, 1091, XQ_SEARCH_THREAD_SEARCH);
        CHECK(report->states[1].known);
        sample.iteration_depth = 4;
        sample.sequence = 14;
        sample.request_id = 4;
        sample.timestamp_ms = 1090; /* Behind this thread's last state clock. */
        cli_sample_report_sample_timed(report, &sample, 1089);
        CHECK(!report->states[1].known);
        state(report, 0, 4, 9, 1100, XQ_SEARCH_THREAD_IDLE);
        cli_sample_report_complete_through(report, 4);
    }
    if (!strcmp(mode, "boundaries"))
    {
        /* One overflow can discard lifecycle events for multiple later rounds.
         * Only the earliest lost metadata remains in the collector gap. */
        XqSearchSample lost = {0};
        lost.thread_id = 0;
        lost.iteration_depth = 2;
        lost.sequence = 6;
        lost.timestamp_ms = 1060;
        cli_sample_report_timeline_gap(report, &lost, 4);
        sample.iteration_depth = 3;
        sample.sequence = 11;
        sample.request_id = 3;
        sample.timestamp_ms = 1070;
        cli_sample_report_sample_timed(report, &sample, 1065);
        cli_sample_report_tick(report, 1080, &counters);
        cli_sample_report_complete_through(report, 3);
        /* A later SEARCH can be recovery from WAIT, not the missing begin. */
        state(report, 0, 4, 11, 1090, XQ_SEARCH_THREAD_SEARCH);
        sample.iteration_depth = 4;
        sample.sequence = 12;
        sample.request_id = 4;
        sample.timestamp_ms = 1095;
        cli_sample_report_sample_timed(report, &sample, 1090);
        state(report, 0, 4, 12, 1100, XQ_SEARCH_THREAD_IDLE);
        cli_sample_report_complete_through(report, 4);
        /* A reliable IDLE then restores authoritative boundaries next round. */
        state(report, 0, 5, 13, 1110, XQ_SEARCH_THREAD_SEARCH);
        state(report, 0, 5, 14, 1120, XQ_SEARCH_THREAD_IDLE);
        cli_sample_report_complete_through(report, 5);
        /* A malformed-clock end cannot certify a finished interval. */
        state(report, 0, 6, 15, 1130, XQ_SEARCH_THREAD_SEARCH);
        state(report, 0, 6, 16, 0, XQ_SEARCH_THREAD_IDLE);
        cli_sample_report_tick(report, 1150, &counters);
        cli_sample_report_complete_through(report, 6);
        /* An IDLE predating a retained helper event cannot certify an end,
         * even though its own root-thread clock is not retrograde. */
        state(report, 0, 0, 17, 1160, XQ_SEARCH_THREAD_IDLE);
        state(report, 0, 7, 18, 1170, XQ_SEARCH_THREAD_SEARCH);
        state(report, 1, 7, 13, 1190, XQ_SEARCH_THREAD_SEARCH);
        state(report, 0, 7, 19, 1180, XQ_SEARCH_THREAD_IDLE);
        ReportIteration *seventh = find_iteration(report, 7, false);
        CHECK(seventh && !seventh->timeline_end_known && seventh->timeline_incomplete);
        CHECK(seventh->timeline_end == 190 && seventh->timeline_last_observed == 190);
        cli_sample_report_complete_through(report, 7);
        /* Completion watermarks also protect depths with no retained records. */
        cli_sample_report_complete_through(report, 25);
        sample.iteration_depth = 20;
        sample.sequence = 13;
        sample.request_id = 5;
        sample.timestamp_ms = 1160;
        cli_sample_report_sample_timed(report, &sample, 1155);
        CHECK(find_iteration(report, 20, false) == NULL);
    }
    cli_sample_report_finish(report, &counters);
    puts("timeline report driver passed");
    return EXIT_SUCCESS;
}
