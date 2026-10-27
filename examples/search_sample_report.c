#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "search_sample_report.h"
#include "xiangqi/movegen.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifdef _WIN32
#include <direct.h>
#include <process.h>
#include <windows.h>
#define sample_mkdir(path) _mkdir(path)
#define sample_pid() _getpid()
#else
#include <unistd.h>
#define sample_mkdir(path) mkdir(path, 0777)
#define sample_pid() getpid()
#endif

#ifndef CLI_SAMPLE_ITERATION_BYTES
#define CLI_SAMPLE_ITERATION_BYTES (512 * 1024u * 1024u)
#endif
#ifndef CLI_SAMPLE_NODE_PAGE_NODES
#define CLI_SAMPLE_NODE_PAGE_NODES 1024u
#endif
_Static_assert(CLI_SAMPLE_NODE_PAGE_NODES > 0 && CLI_SAMPLE_NODE_PAGE_NODES <= UINT32_MAX,
               "statistics node pages must fit the node ID range");
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#define NO_NODE UINT32_MAX
#define RECENT_MS 5000u
#ifndef CLI_SAMPLE_TIMELINE_BUFFER_BYTES
#define CLI_SAMPLE_TIMELINE_BUFFER_BYTES (4 * 1024u * 1024u)
#endif
/* Full dictionaries and folded paths can be hundreds of MiB. Keep their
 * periodic writer occupancy near 10% or less; finished iterations bypass this
 * pacing and always publish their complete dictionary and chunk manifest. */
#ifndef CLI_SAMPLE_CHECKPOINT_MIN_MS
#define CLI_SAMPLE_CHECKPOINT_MIN_MS UINT64_C(5000)
#endif
#ifndef CLI_SAMPLE_CHECKPOINT_LARGE_NODES
#define CLI_SAMPLE_CHECKPOINT_LARGE_NODES (512 * 1024u)
#endif
#ifndef CLI_SAMPLE_CHECKPOINT_LARGE_MS
#define CLI_SAMPLE_CHECKPOINT_LARGE_MS UINT64_C(60000)
#endif
#ifndef CLI_SAMPLE_CHECKPOINT_WRITE_MULTIPLIER
#define CLI_SAMPLE_CHECKPOINT_WRITE_MULTIPLIER UINT64_C(10)
#endif

typedef enum ReportNodeKind { REPORT_ROOT, REPORT_MOVE, REPORT_Q, REPORT_QMOVE } ReportNodeKind;
typedef struct ReportNode
{
    uint32_t parent, child, sibling;
    XqMove move;
    uint64_t inclusive, self, truncated_self, omitted_self;
    uint64_t recent, recent_self;
    ReportNodeKind kind;
} ReportNode;

typedef struct RecentSample
{
    uint64_t timestamp_ms, quiescence_ply;
    uint32_t leaf;
} RecentSample;

typedef struct ReportThread
{
    uint64_t samples, max_quiescence_ply, last_timestamp_ms;
} ReportThread;

typedef struct TimelineState
{
    uint64_t time_ms, sequence;
    XqSearchThreadState state;
    bool known, valid_time;
} TimelineState;

typedef enum TimelineKind { TIMELINE_SAMPLE, TIMELINE_STATE, TIMELINE_GAP } TimelineKind;
typedef struct TimelineRecord
{
    uint64_t time_ms, request_time_ms, sequence, request_id;
    uint64_t normal_ply, quiescence_ply, lost_count;
    unsigned depth, thread_id;
    uint32_t node_id;
    XqSearchThreadState state;
    TimelineKind kind;
    bool valid_time, valid_request_time, truncated, omitted;
} TimelineRecord;

typedef struct TimelineChunk
{
    uint64_t start_ms, end_ms;
    size_t records;
    unsigned number;
} TimelineChunk;

typedef struct ReportIteration
{
    unsigned depth;
    ReportNode **node_pages;
    size_t page_count, page_capacity;
    size_t count, capacity;
    RecentSample *recent;
    ReportThread *threads;
    size_t recent_head, recent_count, recent_capacity;
    size_t allocated_bytes, memory_limit;
    const char *capacity_reason;
    uint64_t samples, truncated_samples, omitted_samples, window_overwritten;
    uint64_t max_normal_ply, max_quiescence_ply;
    bool incomplete;
    TimelineState *initial_states;
    TimelineChunk *chunks;
    size_t chunk_count, chunk_capacity;
    uint64_t timeline_start, timeline_end, timeline_last_observed, state_dropped;
    bool timeline_started, timeline_ended, timeline_incomplete;
    bool timeline_start_known, timeline_end_known;
    struct ReportIteration *next;
} ReportIteration;

struct CliSampleReport
{
    ReportIteration *iterations;
    unsigned threads, max_depth, finished_depth;
    uint64_t search_id, interval_ms, budget_ms, last_checkpoint;
    uint64_t last_timeline_flush, checkpoint_write_ms;
    uint64_t total_samples, allocation_dropped;
    size_t statistics_limit;
    char directory[PATH_MAX], fen[160], mode[64];
    bool files_enabled, allocation_warning, incomplete;
    CliSampleCounters counters;
    bool timeline_requested, timeline_enabled, timeline_warning, timeline_incomplete;
    uint64_t started_ms, timeline_limit, timeline_bytes, timeline_cutoff, timeline_state_dropped;
    uint64_t boundary_floor_ms;
    unsigned boundary_floor_depth;
    const char *timeline_stop_reason;
    TimelineState *states;
    TimelineRecord *timeline_records;
    size_t timeline_count, timeline_capacity;
};

static void write_iteration(CliSampleReport *report, ReportIteration *iteration, bool finished);
static bool flush_timeline_iteration(CliSampleReport *report, ReportIteration *iteration);

static uint64_t report_clock_ms(void)
{
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (uint64_t)now.tv_sec * UINT64_C(1000) + (uint64_t)now.tv_nsec / UINT64_C(1000000);
#endif
}

static uint64_t elapsed_write_ms(uint64_t started)
{
    uint64_t ended = report_clock_ms();
    return started && ended >= started ? ended - started : 0;
}

static uint64_t checkpoint_period_ms(const CliSampleReport *report)
{
    uint64_t period = CLI_SAMPLE_CHECKPOINT_MIN_MS;
    for (const ReportIteration *it = report->iterations; it; it = it->next)
        if (it->count >= CLI_SAMPLE_CHECKPOINT_LARGE_NODES && period < CLI_SAMPLE_CHECKPOINT_LARGE_MS)
            period = CLI_SAMPLE_CHECKPOINT_LARGE_MS;
    uint64_t factor = CLI_SAMPLE_CHECKPOINT_WRITE_MULTIPLIER;
    uint64_t paced = factor && report->checkpoint_write_ms > UINT64_MAX / factor ?
        UINT64_MAX : report->checkpoint_write_ms * factor;
    return paced > period ? paced : period;
}

static const char *state_name(XqSearchThreadState state)
{
    switch (state)
    {
        case XQ_SEARCH_THREAD_IDLE: return "idle";
        case XQ_SEARCH_THREAD_SEARCH: return "search";
        case XQ_SEARCH_THREAD_QUIESCENCE: return "quiescence";
        case XQ_SEARCH_THREAD_WAIT_HELPERS: return "wait_helpers";
    }
    return "unknown";
}

static void disable_timeline(CliSampleReport *report, const char *reason)
{
    if (!report->timeline_requested) return;
    report->timeline_enabled = false;
    report->timeline_incomplete = true;
    if (!report->timeline_stop_reason) report->timeline_stop_reason = reason;
    for (ReportIteration *it = report->iterations; it; it = it->next)
        it->timeline_incomplete = true;
    if (!report->timeline_warning)
        fprintf(stderr, "\nwarning: search sampling timeline stopped (%s); search and hotspot statistics continue\n", reason);
    report->timeline_warning = true;
    report->timeline_count = 0;
}

static bool relative_time(const CliSampleReport *report, uint64_t timestamp, uint64_t *relative)
{
    if (!timestamp || timestamp < report->started_ms) return false;
    *relative = timestamp - report->started_ms;
    return true;
}

static void json_string(FILE *file, const char *text)
{
    fputc('"', file);
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p)
    {
        if (*p == '"' || *p == '\\') fprintf(file, "\\%c", *p);
        else if (*p < 32) fprintf(file, "\\u%04x", (unsigned)*p);
        else fputc(*p, file);
    }
    fputc('"', file);
}

static bool mkdir_tree(const char *path)
{
    char buffer[PATH_MAX];
    size_t length = strlen(path);
    if (length == 0 || length >= sizeof(buffer)) return false;
    memcpy(buffer, path, length + 1);
    for (size_t i = 1; i <= length; ++i)
    {
        if (buffer[i] != '/' && buffer[i] != '\0'
#ifdef _WIN32
            && buffer[i] != '\\'
#endif
           ) continue;
#ifdef _WIN32
        if (i == 2 && buffer[1] == ':') continue;
#endif
        char saved = buffer[i];
        buffer[i] = '\0';
        if (sample_mkdir(buffer) != 0 && errno != EEXIST) return false;
        buffer[i] = saved;
    }
    return true;
}

static void disable_files(CliSampleReport *report)
{
    if (report->files_enabled)
        fprintf(stderr, "\nwarning: cannot write search sampling reports in %s; file output disabled\n",
                report->directory);
    report->files_enabled = false;
}

static FILE *open_report(CliSampleReport *report, const char *name,
                         char *temporary, char *destination)
{
    if (!report->files_enabled) return NULL;
    int final_length = snprintf(destination, PATH_MAX, "%s/%s", report->directory, name);
    int temp_length = snprintf(temporary, PATH_MAX, "%s/%s.tmp", report->directory, name);
    if (final_length < 0 || final_length >= PATH_MAX || temp_length < 0 || temp_length >= PATH_MAX)
    {
        disable_files(report);
        return NULL;
    }
    FILE *file = fopen(temporary, "w");
    if (!file) disable_files(report);
    return file;
}

static bool close_report(CliSampleReport *report, FILE *file,
                          const char *temporary, const char *destination)
{
    bool okay = ferror(file) == 0;
    if (fclose(file) != 0) okay = false;
#ifdef _WIN32
    if (okay && MoveFileExA(temporary, destination, MOVEFILE_REPLACE_EXISTING)) return true;
#else
    if (okay && rename(temporary, destination) == 0) return true;
#endif
    disable_files(report);
    return false;
}

static void write_counters(FILE *file, const CliSampleReport *report)
{
    fprintf(file, "\"slot_overwritten\":%" PRIu64 ",\"dropped\":%" PRIu64
            ",\"delayed\":%" PRIu64 ",\"duplicates\":%" PRIu64
            ",\"boundaries_dropped\":%" PRIu64 ",\"allocation_dropped\":%" PRIu64,
            report->counters.overwritten, report->counters.dropped,
            report->counters.delayed, report->counters.duplicates,
            report->counters.boundaries_dropped, report->allocation_dropped);
}

static void write_session(CliSampleReport *report, bool finished)
{
    char temporary[PATH_MAX], destination[PATH_MAX];
    FILE *file = open_report(report, "session.json", temporary, destination);
    if (!file) return;
    fprintf(file, "{\"format_version\":1,\"search_id\":%" PRIu64 ",\"initial_fen\":", report->search_id);
    json_string(file, report->fen);
    fputs(",\"search_mode\":", file);
    json_string(file, report->mode);
    fprintf(file, ",\"thread_capacity\":%u,\"sample_interval_ms\":%" PRIu64
            ",\"max_depth\":%u,\"time_limit_ms\":%" PRIu64
            ",\"path_limit\":%u,\"iteration_memory_limit_bytes\":%" PRIu64
            ",\"recent_window_ms\":%u,\"finished\":%s,\"valid_samples\":%" PRIu64
            ",\"incomplete\":%s,",
            report->threads, report->interval_ms, report->max_depth, report->budget_ms,
            XQ_SEARCH_SAMPLE_MAX_MOVES, (uint64_t)report->statistics_limit, RECENT_MS,
            finished ? "true" : "false", report->total_samples,
            report->incomplete || report->counters.dropped || report->counters.overwritten ||
            report->counters.boundaries_dropped || report->allocation_dropped ? "true" : "false");
    write_counters(file, report);
    if (report->timeline_requested)
    {
        fprintf(file, ",\"timeline_version\":1,\"timeline_byte_limit\":%" PRIu64
                ",\"timeline_bytes\":%" PRIu64 ",\"timeline_state_dropped\":%" PRIu64
                ",\"timeline_incomplete\":%s,\"timeline_stop_reason\":",
                report->timeline_limit, report->timeline_bytes, report->timeline_state_dropped,
                report->timeline_incomplete ? "true" : "false");
        if (report->timeline_stop_reason) json_string(file, report->timeline_stop_reason);
        else fputs("null", file);
    }
    fputs(",\"directory_timestamp_timezone\":\"UTC\",\"sample_timestamp_clock\":\"monotonic_ms\""
          ",\"meaning\":\"cooperative logical-path sample counts; not CPU percentages or node visits\"}\n", file);
    close_report(report, file, temporary, destination);
}

static void statistics_warning(CliSampleReport *report, const char *reason)
{
    if (!report->allocation_warning)
    {
        if (reason && strcmp(reason, "statistics_limit") == 0)
            fprintf(stderr, "\nwarning: search sampling statistics memory limit reached (%zu bytes); "
                    "use --sample-statistics-max-mib 0 to remove the limit; reports may be incomplete\n",
                    report->statistics_limit);
        else if (reason && strcmp(reason, "node_id_limit") == 0)
            fputs("\nwarning: search sampling statistics node ID limit reached; reports may be incomplete\n", stderr);
        else
            fputs("\nwarning: cannot allocate search sampling statistics; reports may be incomplete\n", stderr);
    }
    report->allocation_warning = true;
    report->incomplete = true;
}

static void allocation_warning(CliSampleReport *report)
{
    statistics_warning(report, "allocation");
}

void cli_sample_report_set_statistics_limit(CliSampleReport *report, size_t byte_limit)
{
    /* The report owner must not be running yet. An existing iteration's page
     * geometry and budget remain immutable throughout its lifetime. */
    if (!report || report->iterations || report->total_samples || report->finished_depth) return;
    report->statistics_limit = byte_limit;
    write_session(report, false);
}

static size_t statistics_room(const ReportIteration *iteration)
{
    size_t limit = iteration->memory_limit ? iteration->memory_limit : SIZE_MAX;
    return limit > iteration->allocated_bytes ? limit - iteration->allocated_bytes : 0;
}

static ReportNode *node_at(const ReportIteration *iteration, size_t index)
{
    return &iteration->node_pages[index / CLI_SAMPLE_NODE_PAGE_NODES][index % CLI_SAMPLE_NODE_PAGE_NODES];
}

static bool append_node_page(ReportIteration *iteration)
{
    const size_t page_nodes = CLI_SAMPLE_NODE_PAGE_NODES;
    if (iteration->capacity >= UINT32_MAX)
    {
        iteration->capacity_reason = "node_id_limit";
        return false;
    }
    /* A shortened final page consumes the remaining positive budget. Never
     * append beyond it: index/page_nodes must retain its fixed-page meaning. */
    if (iteration->capacity % page_nodes)
    {
        iteration->capacity_reason = "statistics_limit";
        return false;
    }
    size_t directory_capacity = iteration->page_capacity;
    size_t directory_bytes = 0;
    size_t room = statistics_room(iteration);
    if (iteration->page_count == directory_capacity)
    {
        if (iteration->page_count >= SIZE_MAX / sizeof(*iteration->node_pages))
        {
            iteration->capacity_reason = "allocation";
            return false;
        }
        size_t required = iteration->page_count + 1;
        directory_capacity = directory_capacity && directory_capacity <= SIZE_MAX / 2 ?
                             directory_capacity * 2 : required;
        if (directory_capacity < required || directory_capacity > SIZE_MAX / sizeof(*iteration->node_pages))
            directory_capacity = required;
        directory_bytes = (directory_capacity - iteration->page_capacity) * sizeof(*iteration->node_pages);
        /* A tight explicit budget can buy just one directory entry. */
        if (directory_bytes > room || room - directory_bytes < sizeof(ReportNode))
        {
            directory_capacity = required;
            directory_bytes = sizeof(*iteration->node_pages);
        }
    }
    if (directory_bytes > room || room - directory_bytes < sizeof(ReportNode))
    {
        iteration->capacity_reason = iteration->memory_limit ? "statistics_limit" : "allocation";
        return false;
    }
    size_t added = (room - directory_bytes) / sizeof(ReportNode);
    if (added > page_nodes) added = page_nodes;
    if (added > (size_t)UINT32_MAX - iteration->capacity) added = (size_t)UINT32_MAX - iteration->capacity;
    ReportNode *page = calloc(added, sizeof(*page));
    if (!page)
    {
        iteration->capacity_reason = "allocation";
        return false;
    }
    if (directory_bytes)
    {
        ReportNode **pages = realloc(iteration->node_pages, directory_capacity * sizeof(*pages));
        if (!pages)
        {
            free(page);
            iteration->capacity_reason = "allocation";
            return false;
        }
        iteration->node_pages = pages;
        iteration->page_capacity = directory_capacity;
    }
    iteration->node_pages[iteration->page_count++] = page;
    iteration->capacity += added;
    iteration->allocated_bytes += directory_bytes + added * sizeof(*page);
    return true;
}

void cli_sample_report_allocation_failed(CliSampleReport *report)
{
    if (!report) return;
    ++report->allocation_dropped;
    allocation_warning(report);
    if (report->timeline_requested) disable_timeline(report, "allocation");
}

static ReportIteration *find_iteration(CliSampleReport *report, unsigned depth, bool create)
{
    ReportIteration **cursor = &report->iterations;
    while (*cursor && (*cursor)->depth != depth) cursor = &(*cursor)->next;
    if (*cursor || !create) return *cursor;
    ReportIteration *iteration = calloc(1, sizeof(*iteration));
    if (!iteration)
    {
        allocation_warning(report);
        if (report->timeline_requested) disable_timeline(report, "allocation");
        return NULL;
    }
    iteration->depth = depth;
    iteration->memory_limit = report->statistics_limit;
    if (report->timeline_requested)
    {
        /* When an iteration's SEARCH event is lost, start at the latest reliable
         * lower bound from an earlier iteration, rather than implying that zero
         * is its actual start. Explicit metadata keeps this range provisional. */
        if (depth > report->boundary_floor_depth)
            iteration->timeline_start = iteration->timeline_end = report->boundary_floor_ms;
        iteration->initial_states = calloc(report->threads, sizeof(*iteration->initial_states));
        if (iteration->initial_states && report->states)
            memcpy(iteration->initial_states, report->states, report->threads * sizeof(*report->states));
        else disable_timeline(report, "allocation");
        iteration->timeline_incomplete = report->timeline_stop_reason != NULL;
    }
    iteration->allocated_bytes = sizeof(*iteration);
    size_t reserve = statistics_room(iteration);
    if (report->threads <= reserve / sizeof(ReportThread))
        iteration->threads = calloc(report->threads, sizeof(*iteration->threads));
    if (iteration->threads)
        iteration->allocated_bytes += (size_t)report->threads * sizeof(*iteration->threads);
    else iteration->incomplete = true;
    reserve = statistics_room(iteration);
    size_t wanted = (size_t)report->threads;
    uint64_t per_thread = RECENT_MS / report->interval_ms + 2;
    if (per_thread > SIZE_MAX / (wanted ? wanted : 1)) wanted = SIZE_MAX;
    else wanted *= (size_t)per_thread;
    size_t window_limit = reserve / 8 / sizeof(RecentSample);
    if (wanted > window_limit) wanted = window_limit;
    if (wanted)
    {
        iteration->recent = calloc(wanted, sizeof(*iteration->recent));
        if (iteration->recent)
        {
            iteration->recent_capacity = wanted;
            iteration->allocated_bytes += wanted * sizeof(*iteration->recent);
        }
        else iteration->incomplete = true;
    }
    else iteration->incomplete = true;
    if (!append_node_page(iteration))
    {
        const char *reason = iteration->capacity_reason;
        free(iteration->recent);
        free(iteration->threads);
        free(iteration->initial_states);
        free(iteration->node_pages);
        free(iteration);
        statistics_warning(report, reason);
        if (report->timeline_requested) disable_timeline(report, reason ? reason : "allocation");
        return NULL;
    }
    iteration->count = 1;
    ReportNode *root = node_at(iteration, 0);
    root->parent = root->child = root->sibling = NO_NODE;
    root->kind = REPORT_ROOT;
    if (iteration->incomplete) allocation_warning(report);
    *cursor = iteration;
    return iteration;
}

static uint32_t child_node(ReportIteration *iteration, uint32_t parent,
                            ReportNodeKind kind, XqMove move)
{
    uint32_t index = node_at(iteration, parent)->child;
    for (; index != NO_NODE; index = node_at(iteration, index)->sibling)
    {
        const ReportNode *node = node_at(iteration, index);
        if (node->kind == kind && (kind == REPORT_Q ||
            (node->move.from == move.from && node->move.to == move.to &&
             node->move.piece == move.piece && node->move.captured == move.captured))) return index;
    }
    if (iteration->count >= UINT32_MAX)
    {
        iteration->capacity_reason = "node_id_limit";
        return NO_NODE;
    }
    if (iteration->count == iteration->capacity && !append_node_page(iteration)) return NO_NODE;
    index = (uint32_t)iteration->count++;
    ReportNode *node = node_at(iteration, index);
    memset(node, 0, sizeof(*node));
    node->parent = parent;
    node->child = NO_NODE;
    node->sibling = node_at(iteration, parent)->child;
    node->kind = kind;
    node->move = move;
    node_at(iteration, parent)->child = index;
    return index;
}

static void remove_recent(ReportIteration *iteration)
{
    RecentSample *sample = &iteration->recent[iteration->recent_head];
    ReportNode *leaf = node_at(iteration, sample->leaf);
    if (leaf->recent_self) --leaf->recent_self;
    for (uint32_t node = sample->leaf; node != NO_NODE; node = node_at(iteration, node)->parent)
        if (node_at(iteration, node)->recent) --node_at(iteration, node)->recent;
    iteration->recent_head = (iteration->recent_head + 1) % iteration->recent_capacity;
    --iteration->recent_count;
}

static void expire_recent(ReportIteration *iteration, uint64_t now)
{
    while (iteration->recent_count)
    {
        uint64_t time = iteration->recent[iteration->recent_head].timestamp_ms;
        if (now < time || now - time <= RECENT_MS) break;
        remove_recent(iteration);
    }
}

static void append_timeline(CliSampleReport *report, const TimelineRecord *record);

void cli_sample_report_sample_timed(CliSampleReport *report, const XqSearchSample *sample,
                                    uint64_t request_timestamp_ms)
{
    if (!report || !sample || (report->timeline_requested &&
        sample->iteration_depth <= report->finished_depth)) return;
    ReportIteration *iteration = find_iteration(report, sample->iteration_depth, true);
    if (!iteration) { ++report->allocation_dropped; return; }
    ++iteration->samples;
    ++report->total_samples;
    if (sample->truncated)
    {
        ++iteration->truncated_samples;
        report->incomplete = true;
    }
    if (iteration->threads && sample->thread_id < report->threads)
    {
        ReportThread *thread = &iteration->threads[sample->thread_id];
        ++thread->samples;
        if (sample->quiescence_ply > thread->max_quiescence_ply)
            thread->max_quiescence_ply = sample->quiescence_ply;
        if (sample->timestamp_ms > thread->last_timestamp_ms)
            thread->last_timestamp_ms = sample->timestamp_ms;
    }
    if (sample->normal_ply > iteration->max_normal_ply) iteration->max_normal_ply = sample->normal_ply;
    if (sample->quiescence_ply > iteration->max_quiescence_ply)
        iteration->max_quiescence_ply = sample->quiescence_ply;
    uint32_t node = 0;
    bool omitted = false, phase_added = false;
    for (size_t i = 0; i <= sample->path_length; ++i)
    {
        bool in_q = i >= sample->normal_ply && sample->state == XQ_SEARCH_THREAD_QUIESCENCE;
        if (in_q && !phase_added)
        {
            XqMove unused = {0};
            uint32_t next = child_node(iteration, node, REPORT_Q, unused);
            if (next == NO_NODE) { omitted = true; break; }
            node = next;
            phase_added = true;
        }
        if (i == sample->path_length) break;
        uint32_t next = child_node(iteration, node, in_q ? REPORT_QMOVE : REPORT_MOVE, sample->path[i]);
        if (next == NO_NODE) { omitted = true; break; }
        node = next;
    }
    ++node_at(iteration, node)->self;
    if (omitted)
    {
        ++node_at(iteration, node)->omitted_self;
        ++iteration->omitted_samples;
        iteration->incomplete = true;
        statistics_warning(report, iteration->capacity_reason);
    }
    else if (sample->truncated)
    {
        ++node_at(iteration, node)->truncated_self;
        iteration->incomplete = true;
    }
    for (uint32_t prefix = node; prefix != NO_NODE; prefix = node_at(iteration, prefix)->parent)
        ++node_at(iteration, prefix)->inclusive;
    if (report->timeline_enabled)
    {
        TimelineRecord record = {0};
        record.kind = TIMELINE_SAMPLE;
        record.depth = sample->iteration_depth;
        record.thread_id = sample->thread_id;
        record.sequence = sample->sequence;
        record.request_id = sample->request_id;
        record.state = sample->state;
        record.node_id = node;
        record.normal_ply = sample->normal_ply;
        record.quiescence_ply = sample->quiescence_ply;
        record.truncated = sample->truncated;
        record.omitted = omitted;
        record.valid_time = relative_time(report, sample->timestamp_ms, &record.time_ms);
        record.valid_request_time = relative_time(report, request_timestamp_ms, &record.request_time_ms) &&
            record.valid_time && record.request_time_ms <= record.time_ms;
        if (report->states && sample->thread_id < report->threads)
        {
            TimelineState *state = &report->states[sample->thread_id];
            if (state->valid_time && record.valid_time && record.time_ms < state->time_ms)
                record.valid_time = false;
            state->sequence = sample->sequence;
            /* A path point does not prove that no state transitions were lost
             * after it in the same overflowing burst. Only an explicit state
             * event may recover a previously unknown status interval. */
            if (state->known) state->state = sample->state;
            state->known = state->known && record.valid_time;
            if (record.valid_time)
            {
                state->time_ms = record.time_ms;
                state->valid_time = true;
            }
        }
        /* Clock validation above can invalidate a superficially plausible
         * request/response pair; never serialize a request time for a null point. */
        if (!record.valid_time) record.valid_request_time = false;
        if (record.valid_time)
        {
            if (record.time_ms > iteration->timeline_end) iteration->timeline_end = record.time_ms;
            if (record.time_ms > iteration->timeline_last_observed) iteration->timeline_last_observed = record.time_ms;
        }
        if (!record.valid_time || sample->truncated || omitted)
            report->timeline_incomplete = iteration->timeline_incomplete = true;
        append_timeline(report, &record);
    }
    if (!iteration->recent_capacity) return;
    expire_recent(iteration, sample->timestamp_ms);
    if (iteration->recent_count == iteration->recent_capacity)
    {
        remove_recent(iteration);
        ++iteration->window_overwritten;
        iteration->incomplete = true;
        report->incomplete = true;
    }
    /* Different worker callbacks may reach the collector in a different timestamp
     * order. Keep the bounded ring ordered so its five-second expiry is exact. */
    size_t insertion = iteration->recent_count;
    while (insertion)
    {
        size_t previous = (iteration->recent_head + insertion - 1) % iteration->recent_capacity;
        if (iteration->recent[previous].timestamp_ms <= sample->timestamp_ms) break;
        size_t next = (iteration->recent_head + insertion) % iteration->recent_capacity;
        iteration->recent[next] = iteration->recent[previous];
        --insertion;
    }
    size_t slot = (iteration->recent_head + insertion) % iteration->recent_capacity;
    ++iteration->recent_count;
    iteration->recent[slot] = (RecentSample){sample->timestamp_ms, sample->quiescence_ply, node};
    ++node_at(iteration, node)->recent_self;
    for (uint32_t prefix = node; prefix != NO_NODE; prefix = node_at(iteration, prefix)->parent)
        ++node_at(iteration, prefix)->recent;
}

void cli_sample_report_sample(CliSampleReport *report, const XqSearchSample *sample)
{
    cli_sample_report_sample_timed(report, sample, 0);
}

static void folded_path(FILE *file, const ReportIteration *iteration, uint32_t leaf)
{
    uint32_t stack[XQ_SEARCH_SAMPLE_MAX_MOVES + 2];
    size_t count = 0;
    for (uint32_t node = leaf; node && node != NO_NODE && count < sizeof(stack) / sizeof(*stack);
         node = node_at(iteration, node)->parent) stack[count++] = node;
    fprintf(file, "depth-%u", iteration->depth);
    while (count)
    {
        const ReportNode *node = node_at(iteration, stack[--count]);
        if (node->kind == REPORT_Q) fputs(";[quiescence]", file);
        else
        {
            char move[8];
            fprintf(file, ";%s", xq_move_to_string(node->move, move, sizeof(move)));
        }
    }
}

static void json_time(FILE *file, bool valid, uint64_t value)
{
    if (valid) fprintf(file, "%" PRIu64, value);
    else fputs("null", file);
}

static void write_timeline_record(FILE *file, const TimelineRecord *record)
{
    const char *type = record->kind == TIMELINE_SAMPLE ? "sample" :
        record->kind == TIMELINE_STATE ? "state" : "gap";
    fprintf(file, "{\"type\":\"%s\",\"time_ms\":", type);
    json_time(file, record->valid_time, record->time_ms);
    fprintf(file, ",\"thread_id\":%u,\"sequence\":%" PRIu64,
            record->thread_id, record->sequence);
    if (record->kind == TIMELINE_GAP)
        fprintf(file, ",\"lost_count\":%" PRIu64, record->lost_count);
    else
    {
        fputs(",\"state\":", file);
        json_string(file, record->valid_time ? state_name(record->state) : "unknown");
        if (record->kind == TIMELINE_SAMPLE)
        {
            fputs(",\"request_time_ms\":", file);
            json_time(file, record->valid_request_time, record->request_time_ms);
            fprintf(file, ",\"request_id\":%" PRIu64 ",\"node_id\":%u"
                    ",\"normal_ply\":%" PRIu64 ",\"quiescence_ply\":%" PRIu64
                    ",\"truncated\":%s,\"path_omitted\":%s",
                    record->request_id, record->node_id, record->normal_ply,
                    record->quiescence_ply, record->truncated ? "true" : "false",
                    record->omitted ? "true" : "false");
        }
    }
    fputc('}', file);
}

static bool flush_timeline_iteration(CliSampleReport *report, ReportIteration *iteration)
{
    if (!report->timeline_enabled || !report->timeline_count) return true;
    size_t count = 0;
    uint64_t start = UINT64_MAX, end = 0;
    for (size_t i = 0; i < report->timeline_count; ++i)
    {
        const TimelineRecord *record = &report->timeline_records[i];
        if (record->depth != iteration->depth) continue;
        ++count;
        if (record->valid_time)
        {
            if (record->time_ms < start) start = record->time_ms;
            if (record->time_ms > end) end = record->time_ms;
        }
    }
    if (!count) return true;
    if (start == UINT64_MAX) start = end = iteration->timeline_start;
    if (!report->files_enabled) { disable_timeline(report, "write_failed"); return false; }
    if (iteration->chunk_count == iteration->chunk_capacity)
    {
        size_t capacity = iteration->chunk_capacity ? iteration->chunk_capacity * 2 : 8;
        TimelineChunk *chunks = realloc(iteration->chunks, capacity * sizeof(*chunks));
        if (!chunks) { disable_timeline(report, "allocation"); return false; }
        iteration->chunks = chunks;
        iteration->chunk_capacity = capacity;
    }
    char name[96], temporary[PATH_MAX], destination[PATH_MAX];
    unsigned number = (unsigned)iteration->chunk_count + 1;
    snprintf(name, sizeof(name), "depth-%u-timeline-%06u.json", iteration->depth, number);
    int a = snprintf(destination, sizeof(destination), "%s/%s", report->directory, name);
    int b = snprintf(temporary, sizeof(temporary), "%s/%s.tmp", report->directory, name);
    if (a < 0 || a >= PATH_MAX || b < 0 || b >= PATH_MAX)
    { disable_timeline(report, "write_failed"); return false; }
    FILE *file = fopen(temporary, "w");
    if (!file) { disable_timeline(report, "write_failed"); return false; }
    fprintf(file, "{\"timeline_version\":1,\"search_id\":%" PRIu64
            ",\"iteration_depth\":%u,\"start_ms\":%" PRIu64 ",\"end_ms\":%" PRIu64 ",\"records\":[",
            report->search_id, iteration->depth, start, end);
    size_t written = 0;
    for (size_t i = 0; i < report->timeline_count; ++i)
    {
        const TimelineRecord *record = &report->timeline_records[i];
        if (record->depth != iteration->depth) continue;
        if (written++) fputc(',', file);
        write_timeline_record(file, record);
    }
    fputs("]}\n", file);
    long bytes = ftell(file);
    bool okay = ferror(file) == 0 && bytes >= 0;
    if (fclose(file) != 0) okay = false;
    bool overflow = okay && (uint64_t)bytes > UINT64_MAX - report->timeline_bytes;
    bool over_limit = okay && report->timeline_limit != 0 &&
        (report->timeline_bytes > report->timeline_limit ||
         (uint64_t)bytes > report->timeline_limit - report->timeline_bytes);
    if (!okay || over_limit || overflow)
    {
        remove(temporary);
        disable_timeline(report, overflow ? "byte_count_overflow" : over_limit ? "byte_limit" : "write_failed");
        return false;
    }
#ifdef _WIN32
    okay = MoveFileExA(temporary, destination, MOVEFILE_REPLACE_EXISTING) != 0;
#else
    okay = rename(temporary, destination) == 0;
#endif
    if (!okay)
    {
        remove(temporary);
        disable_timeline(report, "write_failed");
        return false;
    }
    report->timeline_bytes += (uint64_t)bytes;
    if (end > report->timeline_cutoff) report->timeline_cutoff = end;
    iteration->chunks[iteration->chunk_count++] = (TimelineChunk){start, end, count, number};
    size_t kept = 0;
    for (size_t i = 0; i < report->timeline_count; ++i)
        if (report->timeline_records[i].depth != iteration->depth)
            report->timeline_records[kept++] = report->timeline_records[i];
    report->timeline_count = kept;
    return true;
}

static void append_timeline(CliSampleReport *report, const TimelineRecord *record)
{
    if (!report->timeline_enabled) return;
    if (report->timeline_count == report->timeline_capacity)
    {
        /* Chunks are immutable and may be persisted before the next dictionary
         * checkpoint. They stay unreferenced by the previous manifest until a
         * full JSON snapshot atomically publishes all matching node IDs. This
         * must not trigger a full multi-MiB dictionary rewrite per buffer. */
        for (ReportIteration *it = report->iterations; it && report->timeline_enabled; it = it->next)
            flush_timeline_iteration(report, it);
    }
    if (!report->timeline_enabled) return;
    if (report->timeline_count == report->timeline_capacity)
    { disable_timeline(report, "allocation"); return; }
    report->timeline_records[report->timeline_count++] = *record;
}

static void write_timeline_manifest(FILE *file, const CliSampleReport *report,
                                     const ReportIteration *iteration, bool finished)
{
    fprintf(file, ",\"timeline_version\":1,\"timeline\":{\"start_ms\":%" PRIu64
            ",\"end_ms\":%" PRIu64 ",\"finished\":%s,\"incomplete\":%s,\"stop_reason\":",
            iteration->timeline_start, iteration->timeline_end, finished ? "true" : "false",
            iteration->timeline_incomplete || report->timeline_stop_reason ||
            !iteration->timeline_start_known || (finished && !iteration->timeline_end_known) ? "true" : "false");
    if (report->timeline_stop_reason) json_string(file, report->timeline_stop_reason);
    else fputs("null", file);
    fprintf(file, ",\"start_known\":%s,\"end_known\":%s,\"boundary_unknown\":%s",
            iteration->timeline_start_known ? "true" : "false",
            iteration->timeline_end_known ? "true" : "false",
            !iteration->timeline_start_known || (finished && !iteration->timeline_end_known) ? "true" : "false");
    fputs(",\"cutoff_ms\":", file);
    bool unknown_end = finished && !iteration->timeline_end_known;
    if (report->timeline_stop_reason || unknown_end)
    {
        uint64_t cutoff = report->timeline_stop_reason ? report->timeline_cutoff : iteration->timeline_last_observed;
        if (unknown_end && iteration->timeline_last_observed < cutoff) cutoff = iteration->timeline_last_observed;
        fprintf(file, "%" PRIu64, cutoff);
    }
    else fputs("null", file);
    fprintf(file, ",\"state_dropped\":%" PRIu64 ",\"initial_states\":[", iteration->state_dropped);
    for (unsigned thread = 0; thread < report->threads; ++thread)
    {
        if (thread) fputc(',', file);
        const TimelineState *state = iteration->initial_states ? &iteration->initial_states[thread] : NULL;
        bool known = iteration->timeline_start_known && state && state->known && state->valid_time &&
            state->time_ms <= iteration->timeline_start;
        fprintf(file, "{\"thread_id\":%u,\"state\":", thread);
        json_string(file, known ? state_name(state->state) : "unknown");
        fputs(",\"time_ms\":", file);
        json_time(file, known, state ? state->time_ms : 0);
        fprintf(file, ",\"sequence\":%" PRIu64 "}", state ? state->sequence : 0);
    }
    fputs("],\"chunks\":[", file);
    for (size_t i = 0; i < iteration->chunk_count; ++i)
    {
        const TimelineChunk *chunk = &iteration->chunks[i];
        if (i) fputc(',', file);
        fprintf(file, "{\"file\":\"depth-%u-timeline-%06u.json\",\"start_ms\":%" PRIu64
                ",\"end_ms\":%" PRIu64 ",\"records\":%zu}", iteration->depth, chunk->number,
                chunk->start_ms, chunk->end_ms, chunk->records);
    }
    fputs("]}", file);
}

bool cli_sample_report_enable_timeline(CliSampleReport *report, uint64_t started_ms,
                                       uint64_t byte_limit)
{
    if (!report || report->timeline_requested) return false;
    report->timeline_requested = true;
    report->started_ms = started_ms;
    report->timeline_limit = byte_limit;
    report->timeline_capacity = CLI_SAMPLE_TIMELINE_BUFFER_BYTES / sizeof(TimelineRecord);
    report->states = calloc(report->threads, sizeof(*report->states));
    report->timeline_records = calloc(report->timeline_capacity, sizeof(*report->timeline_records));
    if (!report->states || !report->timeline_records || !report->timeline_capacity)
    { disable_timeline(report, "allocation"); return false; }
    if (!report->files_enabled)
    { disable_timeline(report, "write_failed"); return false; }
    report->timeline_enabled = true;
    write_session(report, false);
    return true;
}

void cli_sample_report_timeline_state(CliSampleReport *report, const XqSearchSample *sample)
{
    if (!report || !report->timeline_requested || !sample || sample->thread_id >= report->threads) return;
    TimelineRecord record = {0};
    record.kind = TIMELINE_STATE;
    record.depth = sample->iteration_depth;
    record.thread_id = sample->thread_id;
    record.sequence = sample->sequence;
    record.state = sample->state;
    record.valid_time = relative_time(report, sample->timestamp_ms, &record.time_ms);
    TimelineState *state = report->states ? &report->states[sample->thread_id] : NULL;
    if (state && state->valid_time && record.valid_time && record.time_ms < state->time_ms)
        record.valid_time = false;
    if (record.depth && record.depth > report->finished_depth)
    {
        ReportIteration *iteration = find_iteration(report, record.depth, true);
        if (iteration)
        {
            if (record.thread_id == 0 && record.state == XQ_SEARCH_THREAD_SEARCH && !iteration->timeline_started)
            {
                iteration->timeline_started = true;
                /* SEARCH also marks recovery from WAIT_HELPERS. Without a
                 * contiguous preceding IDLE, a dropped begin cannot safely be
                 * reconstructed from the first later SEARCH event. */
                bool authoritative = state && state->known && state->state == XQ_SEARCH_THREAD_IDLE &&
                    state->sequence != UINT64_MAX && state->sequence + 1 == record.sequence;
                if (record.valid_time && authoritative)
                {
                    iteration->timeline_start = record.time_ms;
                    if (record.time_ms > iteration->timeline_end) iteration->timeline_end = record.time_ms;
                    iteration->timeline_start_known = true;
                }
                else report->timeline_incomplete = iteration->timeline_incomplete = true;
            }
            if (record.thread_id == 0 && record.state == XQ_SEARCH_THREAD_IDLE)
            {
                iteration->timeline_ended = true;
                if (record.valid_time && record.time_ms >= iteration->timeline_start &&
                    record.time_ms >= iteration->timeline_last_observed)
                {
                    /* A tick may already have extended a running checkpoint
                     * beyond this delayed event. The root's trustworthy IDLE
                     * is the actual end, not another lower-bound estimate. */
                    iteration->timeline_end = record.time_ms;
                    iteration->timeline_end_known = true;
                }
                else
                {
                    iteration->timeline_end_known = false;
                    report->timeline_incomplete = iteration->timeline_incomplete = true;
                }
            }
            if (record.valid_time)
            {
                if (record.time_ms > iteration->timeline_end) iteration->timeline_end = record.time_ms;
                if (record.time_ms > iteration->timeline_last_observed)
                    iteration->timeline_last_observed = record.time_ms;
            }
            if (!record.valid_time) report->timeline_incomplete = iteration->timeline_incomplete = true;
            append_timeline(report, &record);
        }
    }
    if (record.thread_id == 0 && record.state == XQ_SEARCH_THREAD_IDLE && record.valid_time &&
        record.time_ms >= report->boundary_floor_ms)
    {
        report->boundary_floor_ms = record.time_ms;
        report->boundary_floor_depth = record.depth;
    }
    if (state)
    {
        state->state = sample->state;
        state->sequence = sample->sequence;
        state->known = record.valid_time;
        if (record.valid_time)
        {
            state->valid_time = true;
            state->time_ms = record.time_ms;
        }
    }
}

void cli_sample_report_timeline_gap(CliSampleReport *report, const XqSearchSample *lost,
                                    uint64_t lost_count)
{
    if (!report || !report->timeline_requested || !lost || lost->thread_id >= report->threads) return;
    report->timeline_state_dropped += lost_count;
    report->timeline_incomplete = true;
    if (report->states) report->states[lost->thread_id].known = false;
    uint64_t lost_time;
    if (lost->thread_id == 0 && relative_time(report, lost->timestamp_ms, &lost_time) &&
        lost_time >= report->boundary_floor_ms)
    {
        /* Only later iterations may use this lower bound: the lost event can be
         * in the middle of its own iteration. */
        report->boundary_floor_ms = lost_time;
        report->boundary_floor_depth = lost->iteration_depth;
    }
    if (!lost->iteration_depth || lost->iteration_depth <= report->finished_depth) return;
    ReportIteration *iteration = find_iteration(report, lost->iteration_depth, true);
    if (!iteration) return;
    iteration->state_dropped += lost_count;
    report->timeline_incomplete = iteration->timeline_incomplete = true;
    TimelineRecord record = {0};
    record.kind = TIMELINE_GAP;
    record.depth = lost->iteration_depth;
    record.thread_id = lost->thread_id;
    record.sequence = lost->sequence;
    record.lost_count = lost_count;
    record.valid_time = relative_time(report, lost->timestamp_ms, &record.time_ms);
    if (record.valid_time)
    {
        if (record.time_ms > iteration->timeline_end) iteration->timeline_end = record.time_ms;
        if (record.time_ms > iteration->timeline_last_observed) iteration->timeline_last_observed = record.time_ms;
    }
    append_timeline(report, &record);
}

static void write_iteration(CliSampleReport *report, ReportIteration *iteration, bool finished)
{
    uint64_t write_started = report_clock_ms();
    if (report->timeline_requested)
    {
        if (!iteration->timeline_start_known || (finished && !iteration->timeline_end_known))
            report->timeline_incomplete = iteration->timeline_incomplete = true;
        flush_timeline_iteration(report, iteration);
    }
    char temporary[PATH_MAX], destination[PATH_MAX], name[64];
    snprintf(name, sizeof(name), "depth-%u.json", iteration->depth);
    FILE *file = open_report(report, name, temporary, destination);
    if (!file) return;
    fprintf(file, "{\"format_version\":1,\"search_id\":%" PRIu64 ",\"iteration_depth\":%u"
            ",\"finished\":%s,\"valid_samples\":%" PRIu64 ",\"truncated_samples\":%" PRIu64
            ",\"unrecorded_path_samples\":%" PRIu64 ",\"window_overwritten\":%" PRIu64
            ",\"incomplete\":%s,\"max_normal_ply\":%" PRIu64 ",\"max_quiescence_ply\":%" PRIu64
            ",\"statistics_bytes\":%zu,\"session_counters\":{",
            report->search_id, iteration->depth, finished ? "true" : "false", iteration->samples,
            iteration->truncated_samples, iteration->omitted_samples, iteration->window_overwritten,
            iteration->incomplete || report->counters.dropped || report->counters.overwritten ||
            report->counters.boundaries_dropped || report->allocation_dropped ? "true" : "false",
            iteration->max_normal_ply, iteration->max_quiescence_ply, iteration->allocated_bytes);
    write_counters(file, report);
    fputs("},\"thread_statistics_available\":", file);
    fputs(iteration->threads ? "true" : "false", file);
    fputs(",\"threads\":[", file);
    for (unsigned i = 0; iteration->threads && i < report->threads; ++i)
    {
        if (i) fputc(',', file);
        const ReportThread *thread = &iteration->threads[i];
        fprintf(file, "{\"thread_id\":%u,\"valid_samples\":%" PRIu64
                ",\"max_quiescence_ply\":%" PRIu64 ",\"last_timestamp_ms\":%" PRIu64 "}",
                i, thread->samples, thread->max_quiescence_ply, thread->last_timestamp_ms);
    }
    fputs("],\"nodes\":[", file);
    for (size_t i = 0; i < iteration->count; ++i)
    {
        const ReportNode *node = node_at(iteration, i);
        if (i) fputc(',', file);
        fprintf(file, "{\"id\":%zu,\"parent\":", i);
        if (node->parent == NO_NODE) fputs("null", file);
        else fprintf(file, "%u", node->parent);
        fputs(",\"kind\":", file);
        json_string(file, node->kind == REPORT_ROOT ? "root" : node->kind == REPORT_Q ? "quiescence" :
                    node->kind == REPORT_QMOVE ? "quiescence_move" : "normal_move");
        fputs(",\"move\":", file);
        if (node->kind == REPORT_ROOT || node->kind == REPORT_Q) fputs("null", file);
        else { char move[8]; json_string(file, xq_move_to_string(node->move, move, sizeof(move))); }
        fprintf(file, ",\"inclusive\":%" PRIu64 ",\"self\":%" PRIu64
                ",\"truncated_self\":%" PRIu64 ",\"unrecorded_self\":%" PRIu64 "}",
                node->inclusive, node->self, node->truncated_self, node->omitted_self);
    }
    fputs("]", file);
    if (report->timeline_requested) write_timeline_manifest(file, report, iteration, finished);
    fputs("}\n", file);
    if (!close_report(report, file, temporary, destination)) return;
    snprintf(name, sizeof(name), "depth-%u.folded", iteration->depth);
    file = open_report(report, name, temporary, destination);
    if (!file) return;
    for (size_t i = 0; i < iteration->count; ++i)
    {
        const ReportNode *node = node_at(iteration, i);
        uint64_t complete = node->self - node->truncated_self - node->omitted_self;
        if (complete) { folded_path(file, iteration, (uint32_t)i); fprintf(file, " %" PRIu64 "\n", complete); }
        if (node->truncated_self)
        {
            folded_path(file, iteration, (uint32_t)i);
            fprintf(file, ";[path-truncated] %" PRIu64 "\n", node->truncated_self);
        }
        if (node->omitted_self)
        {
            folded_path(file, iteration, (uint32_t)i);
            fprintf(file, ";[statistics-capacity] %" PRIu64 "\n", node->omitted_self);
        }
    }
    close_report(report, file, temporary, destination);
    report->checkpoint_write_ms = elapsed_write_ms(write_started);
}

static void free_iteration(ReportIteration *iteration)
{
    for (size_t i = 0; i < iteration->page_count; ++i) free(iteration->node_pages[i]);
    free(iteration->node_pages);
    free(iteration->recent);
    free(iteration->threads);
    free(iteration->initial_states);
    free(iteration->chunks);
    free(iteration);
}

void cli_sample_report_boundary(CliSampleReport *report, unsigned depth, bool finished)
{
    if (!report || report->timeline_requested) return;
    ReportIteration *iteration = find_iteration(report, depth, true);
    if (!finished) return;
    if (depth > report->finished_depth) report->finished_depth = depth;
    if (!iteration) return;
    write_iteration(report, iteration, true);
    ReportIteration **cursor = &report->iterations;
    while (*cursor && *cursor != iteration) cursor = &(*cursor)->next;
    if (*cursor) *cursor = iteration->next;
    free_iteration(iteration);
}

void cli_sample_report_complete_through(CliSampleReport *report, unsigned depth)
{
    if (!report) return;
    ReportIteration *iteration = report->iterations;
    while (iteration)
    {
        unsigned current_depth = iteration->depth;
        iteration = iteration->next;
        if (current_depth <= depth)
        {
            if (!report->timeline_requested) cli_sample_report_boundary(report, current_depth, true);
            else
            {
                ReportIteration **cursor = &report->iterations;
                while (*cursor && (*cursor)->depth != current_depth) cursor = &(*cursor)->next;
                if (*cursor)
                {
                    ReportIteration *completed = *cursor;
                    write_iteration(report, completed, true);
                    *cursor = completed->next;
                    free_iteration(completed);
                }
                if (current_depth > report->finished_depth) report->finished_depth = current_depth;
            }
        }
    }
    if (report->timeline_requested && depth > report->finished_depth)
        report->finished_depth = depth;
}

void cli_sample_report_tick(CliSampleReport *report, uint64_t now_ms,
                            const CliSampleCounters *counters)
{
    if (!report) return;
    report->counters = *counters;
    if (report->timeline_requested)
        for (ReportIteration *it = report->iterations; it; it = it->next)
            if (!it->timeline_end_known)
            {
                uint64_t relative;
                if (relative_time(report, now_ms, &relative) && relative >= it->timeline_start &&
                    relative > it->timeline_end) it->timeline_end = relative;
            }
    if (!report->last_checkpoint) report->last_checkpoint = now_ms;
    if (!report->last_timeline_flush) report->last_timeline_flush = now_ms;
    bool flush_due = now_ms >= report->last_timeline_flush &&
        now_ms - report->last_timeline_flush >= CLI_SAMPLE_CHECKPOINT_MIN_MS;
    if (flush_due)
    {
        /* Persist bounded timeline buffers at the original cadence even when
         * a large dictionary is deliberately checkpointed less frequently. */
        for (ReportIteration *it = report->iterations; it && report->timeline_enabled; it = it->next)
            flush_timeline_iteration(report, it);
        report->last_timeline_flush = now_ms;
    }
    if (now_ms >= report->last_checkpoint &&
        now_ms - report->last_checkpoint >= checkpoint_period_ms(report))
    {
        uint64_t write_started = report_clock_ms();
        for (ReportIteration *it = report->iterations; it; it = it->next) write_iteration(report, it, false);
        write_session(report, false);
        report->checkpoint_write_ms = elapsed_write_ms(write_started);
        /* Schedule from completion rather than from before a slow write. The
         * caller's monotonic timestamps may already be queued, so add only the
         * measured duration instead of replacing their clock with wall time. */
        report->last_checkpoint = UINT64_MAX - now_ms < report->checkpoint_write_ms ?
            UINT64_MAX : now_ms + report->checkpoint_write_ms;
    }
    else if (flush_due) write_session(report, false);
}

void cli_sample_report_summary(CliSampleReport *report, unsigned depth, uint64_t now_ms,
                               CliSampleSummary *summary)
{
    memset(summary, 0, sizeof(*summary));
    if (!report) return;
    summary->incomplete = report->incomplete || report->allocation_dropped ||
        report->counters.overwritten || report->counters.dropped ||
        report->counters.boundaries_dropped;
    ReportIteration *iteration = find_iteration(report, depth, false);
    if (!iteration) return;
    expire_recent(iteration, now_ms);
    summary->recent_samples = iteration->recent_count;
    summary->incomplete = summary->incomplete || iteration->incomplete;
    for (size_t i = 0; i < iteration->recent_count; ++i)
    {
        const RecentSample *sample = &iteration->recent[(iteration->recent_head + i) % iteration->recent_capacity];
        if (sample->quiescence_ply > summary->max_quiescence_ply)
            summary->max_quiescence_ply = sample->quiescence_ply;
    }
    uint32_t parent = 0;
    size_t used = 0;
    /* Follow the hottest sampled prefix; a short terminal summary stays readable. */
    for (unsigned step = 0; step < 8; ++step)
    {
        uint32_t best = NO_NODE;
        for (uint32_t child = node_at(iteration, parent)->child; child != NO_NODE;
             child = node_at(iteration, child)->sibling)
            if (node_at(iteration, child)->recent && (best == NO_NODE ||
                node_at(iteration, child)->recent > node_at(iteration, best)->recent)) best = child;
        if (best == NO_NODE) break;
        const ReportNode *node = node_at(iteration, best);
        char move[8];
        const char *label = node->kind == REPORT_Q ? "Q" : xq_move_to_string(node->move, move, sizeof(move));
        int written = snprintf(summary->hot_path + used, sizeof(summary->hot_path) - used,
                                "%s%s", used ? "/" : "", label);
        if (written < 0 || (size_t)written >= sizeof(summary->hot_path) - used) break;
        used += (size_t)written;
        parent = best;
    }
}

CliSampleReport *cli_sample_report_create(const char *output_dir,
                                        const XqPosition *position, const char *mode,
                                        uint64_t search_id, unsigned thread_capacity,
                                        uint64_t interval_ms, unsigned max_depth,
                                        uint64_t time_limit_ms)
{
    CliSampleReport *report = calloc(1, sizeof(*report));
    if (!report) return NULL;
    report->statistics_limit = (size_t)CLI_SAMPLE_ITERATION_BYTES;
    report->search_id = search_id;
    report->threads = thread_capacity;
    report->interval_ms = interval_ms ? interval_ms : 20;
    report->max_depth = max_depth;
    report->budget_ms = time_limit_ms;
    snprintf(report->mode, sizeof(report->mode), "%s", mode ? mode : "unknown");
    if (!position || !xq_position_to_fen(position, report->fen, sizeof(report->fen)))
        strcpy(report->fen, "unavailable");
    if (!output_dir) output_dir = "build/search_samples";
    bool okay = mkdir_tree(output_dir);
    time_t seconds = time(NULL);
    struct tm value;
#ifdef _WIN32
    bool valid_time = gmtime_s(&value, &seconds) == 0;
#else
    bool valid_time = gmtime_r(&seconds, &value) != NULL;
#endif
    char timestamp[32] = "unknown-time";
    if (valid_time) strftime(timestamp, sizeof(timestamp), "%Y%m%d-%H%M%S", &value);
    if (okay)
    {
        bool created = false;
        for (unsigned attempt = 0; attempt < 100; ++attempt)
        {
            int length = snprintf(report->directory, sizeof(report->directory), "%s/search-%s-%ld-%" PRIu64 "-%u",
                                  output_dir, timestamp, (long)sample_pid(), search_id, attempt);
            if (length < 0 || (size_t)length >= sizeof(report->directory)) break;
            if (sample_mkdir(report->directory) == 0) { created = true; break; }
            if (errno != EEXIST) break;
        }
        okay = created;
    }
    if (!okay)
    {
        fprintf(stderr, "warning: cannot create search sampling report directory in %s; file output disabled\n", output_dir);
        report->files_enabled = false;
    }
    else
    {
        report->files_enabled = true;
        write_session(report, false);
        fprintf(stderr, "search sampling reports: %s\n", report->directory);
    }
    return report;
}

void cli_sample_report_finish(CliSampleReport *report, const CliSampleCounters *counters)
{
    if (!report) return;
    report->counters = *counters;
    while (report->iterations)
    {
        ReportIteration *iteration = report->iterations;
        report->iterations = iteration->next;
        write_iteration(report, iteration, true);
        free_iteration(iteration);
    }
    write_session(report, true);
    free(report->states);
    free(report->timeline_records);
    free(report);
}
