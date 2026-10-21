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
#define CLI_SAMPLE_ITERATION_BYTES (64u * 1024u * 1024u)
#endif
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#define NO_NODE UINT32_MAX
#define RECENT_MS 5000u

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

typedef struct ReportIteration
{
    unsigned depth;
    ReportNode *nodes;
    size_t count, capacity;
    RecentSample *recent;
    ReportThread *threads;
    size_t recent_head, recent_count, recent_capacity;
    size_t allocated_bytes;
    uint64_t samples, truncated_samples, omitted_samples, window_overwritten;
    uint64_t max_normal_ply, max_quiescence_ply;
    bool incomplete;
    struct ReportIteration *next;
} ReportIteration;

struct CliSampleReport
{
    ReportIteration *iterations;
    unsigned threads, max_depth, finished_depth;
    uint64_t search_id, interval_ms, budget_ms, last_checkpoint;
    uint64_t total_samples, allocation_dropped;
    char directory[PATH_MAX], fen[160], mode[64];
    bool files_enabled, allocation_warning, incomplete;
    CliSampleCounters counters;
};

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
            ",\"path_limit\":%u,\"iteration_memory_limit_bytes\":%u"
            ",\"recent_window_ms\":%u,\"finished\":%s,\"valid_samples\":%" PRIu64
            ",\"incomplete\":%s,",
            report->threads, report->interval_ms, report->max_depth, report->budget_ms,
            XQ_SEARCH_SAMPLE_MAX_MOVES, (unsigned)CLI_SAMPLE_ITERATION_BYTES, RECENT_MS,
            finished ? "true" : "false", report->total_samples,
            report->incomplete || report->counters.dropped || report->counters.overwritten ||
            report->counters.boundaries_dropped || report->allocation_dropped ? "true" : "false");
    write_counters(file, report);
    fputs(",\"directory_timestamp_timezone\":\"UTC\",\"sample_timestamp_clock\":\"monotonic_ms\""
          ",\"meaning\":\"cooperative logical-path sample counts; not CPU percentages or node visits\"}\n", file);
    close_report(report, file, temporary, destination);
}

static void allocation_warning(CliSampleReport *report)
{
    if (!report->allocation_warning)
        fputs("\nwarning: search sampling statistics capacity exhausted; reports may be incomplete\n", stderr);
    report->allocation_warning = true;
    report->incomplete = true;
}

static ReportIteration *find_iteration(CliSampleReport *report, unsigned depth, bool create)
{
    ReportIteration **cursor = &report->iterations;
    while (*cursor && (*cursor)->depth != depth) cursor = &(*cursor)->next;
    if (*cursor || !create) return *cursor;
    ReportIteration *iteration = calloc(1, sizeof(*iteration));
    if (!iteration) { allocation_warning(report); return NULL; }
    iteration->depth = depth;
    size_t budget = CLI_SAMPLE_ITERATION_BYTES;
    iteration->allocated_bytes = sizeof(*iteration);
    size_t reserve = budget > sizeof(*iteration) ? budget - sizeof(*iteration) : 0;
    if (report->threads <= reserve / sizeof(ReportThread))
        iteration->threads = calloc(report->threads, sizeof(*iteration->threads));
    if (iteration->threads)
        iteration->allocated_bytes += (size_t)report->threads * sizeof(*iteration->threads);
    else iteration->incomplete = true;
    reserve = budget > iteration->allocated_bytes ? budget - iteration->allocated_bytes : 0;
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
    size_t node_capacity = 64;
    reserve = budget > iteration->allocated_bytes ? budget - iteration->allocated_bytes : 0;
    if (node_capacity > reserve / sizeof(ReportNode)) node_capacity = reserve / sizeof(ReportNode);
    if (node_capacity) iteration->nodes = calloc(node_capacity, sizeof(*iteration->nodes));
    if (!iteration->nodes)
    {
        free(iteration->recent);
        free(iteration->threads);
        free(iteration);
        allocation_warning(report);
        return NULL;
    }
    iteration->capacity = node_capacity;
    iteration->allocated_bytes += node_capacity * sizeof(*iteration->nodes);
    iteration->count = 1;
    iteration->nodes[0].parent = iteration->nodes[0].child = iteration->nodes[0].sibling = NO_NODE;
    iteration->nodes[0].kind = REPORT_ROOT;
    if (iteration->incomplete) allocation_warning(report);
    *cursor = iteration;
    return iteration;
}

static uint32_t child_node(ReportIteration *iteration, uint32_t parent,
                            ReportNodeKind kind, XqMove move)
{
    uint32_t index = iteration->nodes[parent].child;
    for (; index != NO_NODE; index = iteration->nodes[index].sibling)
    {
        const ReportNode *node = &iteration->nodes[index];
        if (node->kind == kind && (kind == REPORT_Q ||
            (node->move.from == move.from && node->move.to == move.to &&
             node->move.piece == move.piece && node->move.captured == move.captured))) return index;
    }
    if (iteration->count == iteration->capacity)
    {
        size_t room = CLI_SAMPLE_ITERATION_BYTES > iteration->allocated_bytes ?
            CLI_SAMPLE_ITERATION_BYTES - iteration->allocated_bytes : 0;
        size_t added = iteration->capacity;
        if (added > room / sizeof(ReportNode)) added = room / sizeof(ReportNode);
        if (!added) return NO_NODE;
        size_t capacity = iteration->capacity + added;
        ReportNode *nodes = realloc(iteration->nodes, capacity * sizeof(*nodes));
        if (!nodes) return NO_NODE;
        iteration->nodes = nodes;
        iteration->capacity = capacity;
        iteration->allocated_bytes += added * sizeof(*nodes);
    }
    index = (uint32_t)iteration->count++;
    ReportNode *node = &iteration->nodes[index];
    memset(node, 0, sizeof(*node));
    node->parent = parent;
    node->child = NO_NODE;
    node->sibling = iteration->nodes[parent].child;
    node->kind = kind;
    node->move = move;
    iteration->nodes[parent].child = index;
    return index;
}

static void remove_recent(ReportIteration *iteration)
{
    RecentSample *sample = &iteration->recent[iteration->recent_head];
    ReportNode *leaf = &iteration->nodes[sample->leaf];
    if (leaf->recent_self) --leaf->recent_self;
    for (uint32_t node = sample->leaf; node != NO_NODE; node = iteration->nodes[node].parent)
        if (iteration->nodes[node].recent) --iteration->nodes[node].recent;
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

void cli_sample_report_sample(CliSampleReport *report, const XqSearchSample *sample)
{
    if (!report) return;
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
    ++iteration->nodes[node].self;
    if (omitted)
    {
        ++iteration->nodes[node].omitted_self;
        ++iteration->omitted_samples;
        iteration->incomplete = true;
        allocation_warning(report);
    }
    else if (sample->truncated)
    {
        ++iteration->nodes[node].truncated_self;
        iteration->incomplete = true;
    }
    for (uint32_t prefix = node; prefix != NO_NODE; prefix = iteration->nodes[prefix].parent)
        ++iteration->nodes[prefix].inclusive;
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
    ++iteration->nodes[node].recent_self;
    for (uint32_t prefix = node; prefix != NO_NODE; prefix = iteration->nodes[prefix].parent)
        ++iteration->nodes[prefix].recent;
}

static void folded_path(FILE *file, const ReportIteration *iteration, uint32_t leaf)
{
    uint32_t stack[XQ_SEARCH_SAMPLE_MAX_MOVES + 2];
    size_t count = 0;
    for (uint32_t node = leaf; node && node != NO_NODE && count < sizeof(stack) / sizeof(*stack);
         node = iteration->nodes[node].parent) stack[count++] = node;
    fprintf(file, "depth-%u", iteration->depth);
    while (count)
    {
        const ReportNode *node = &iteration->nodes[stack[--count]];
        if (node->kind == REPORT_Q) fputs(";[quiescence]", file);
        else
        {
            char move[8];
            fprintf(file, ";%s", xq_move_to_string(node->move, move, sizeof(move)));
        }
    }
}

static void write_iteration(CliSampleReport *report, ReportIteration *iteration, bool finished)
{
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
        const ReportNode *node = &iteration->nodes[i];
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
    fputs("]}\n", file);
    if (!close_report(report, file, temporary, destination)) return;
    snprintf(name, sizeof(name), "depth-%u.folded", iteration->depth);
    file = open_report(report, name, temporary, destination);
    if (!file) return;
    for (size_t i = 0; i < iteration->count; ++i)
    {
        const ReportNode *node = &iteration->nodes[i];
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
}

static void free_iteration(ReportIteration *iteration)
{
    free(iteration->nodes);
    free(iteration->recent);
    free(iteration->threads);
    free(iteration);
}

void cli_sample_report_boundary(CliSampleReport *report, unsigned depth, bool finished)
{
    if (!report) return;
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
        if (current_depth <= depth) cli_sample_report_boundary(report, current_depth, true);
    }
}

void cli_sample_report_tick(CliSampleReport *report, uint64_t now_ms,
                            const CliSampleCounters *counters)
{
    if (!report) return;
    report->counters = *counters;
    if (!report->last_checkpoint) report->last_checkpoint = now_ms;
    if (now_ms >= report->last_checkpoint && now_ms - report->last_checkpoint >= 5000)
    {
        for (ReportIteration *it = report->iterations; it; it = it->next) write_iteration(report, it, false);
        write_session(report, false);
        report->last_checkpoint = now_ms;
    }
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
        for (uint32_t child = iteration->nodes[parent].child; child != NO_NODE;
             child = iteration->nodes[child].sibling)
            if (iteration->nodes[child].recent && (best == NO_NODE ||
                iteration->nodes[child].recent > iteration->nodes[best].recent)) best = child;
        if (best == NO_NODE) break;
        const ReportNode *node = &iteration->nodes[best];
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
    free(report);
}
