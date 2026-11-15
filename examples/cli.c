#include "xiangqi/engine.h"
#include "search_progress.h"
#include "xiangqi/parallel_search.h"
#include "xiangqi/movegen.h"
#include "xiangqi/position.h"
#include "xiangqi/jieqi/parallel_search_jq.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

static const char *search_detail_path = "build/search_detail.txt";

typedef struct CliSearchOutput
{
    bool show_progress;
    bool sample_search;
    bool sample_timeline;
    uint64_t timeline_byte_limit;
    size_t statistics_byte_limit;
    uint64_t sample_interval_ms;
    const char *sample_output_dir;
} CliSearchOutput;

typedef struct CliSearchResult
{
    bool found;
    XqMove best;
    XqSearchStats stats;
    XqParallelStatus status;
} CliSearchResult;

typedef struct CliBatchSummary
{
    size_t processed; /* Successful roots, including positions with no legal move. */
    size_t no_moves;
    uint64_t nodes;
    uint64_t search_elapsed_ms;
    bool elapsed_unavailable;
    XqTranspositionStats cache;
} CliBatchSummary;

typedef enum CliJqRevealMode
{
    CLI_JQ_REVEAL_MANUAL,
    CLI_JQ_REVEAL_RANDOM
} CliJqRevealMode;

/**
 * 返回指定棋子颜色对应的英文名称。
 *
 * @param color 棋子颜色，可为 XQ_RED 或 XQ_BLACK。
 * @return color 为 XQ_RED 时返回 "red"，否则返回 "black"。
 */
static const char *color_name(XqColor color)
{
    return color == XQ_RED ? "red" : "black";
}

static const char *split_strategy_name(XqParallelSplitStrategy strategy)
{
    return strategy == XQ_PARALLEL_SPLIT_INTERLEAVED ? "interleaved" : "contiguous";
}

static const char *score_kind_name(XqSearchScoreKind kind)
{
    switch (kind)
    {
    case XQ_SEARCH_SCORE_EXACT: return "exact";
    case XQ_SEARCH_SCORE_UPPER_BOUND: return "upper_bound";
    case XQ_SEARCH_SCORE_LOWER_BOUND: return "lower_bound";
    }
    return "unknown";
}

/**
 * 打印命令行程序的用法、可用选项以及 FEN 参数的输入提示。
 *
 * @param program 命令行程序名称，通常传入 argv[0]。
 */
static void print_usage(const char *program)
{
    printf("usage: %s [--game xiangqi|jieqi] [--reveal manual|random] [--fen FEN | --fen-file PATH] [--engine red|black] [--depth N] [--time-ms MS] [--tt-file PATH] [--search-detail] [--progress]\n", program);
    printf("\n");
    printf("options:\n");
    printf("  --game xiangqi|jieqi       game rules (default xiangqi)\n");
    printf("  --reveal manual|random     jieqi reveal mode (default manual; jieqi only)\n");
    printf("  --search builtin|parallel  search implementation (default builtin)\n");
    printf("  jieqi: parallel search by default; builtin, TT files and --sample-* are unsupported\n");
    printf("  jieqi manual: append the reveal type to dark moves, e.g. a0a1=R; engine dark moves prompt for a type\n");
    printf("  --threads N                parallel threads including caller (default 2)\n");
    printf("  --split-strategy contiguous|interleaved  parallel split strategy (default contiguous)\n");
    printf("  --split-percent P          contiguous batch percentage, 0..100 (default 50; 0 disables splitting)\n");
    printf("  interleaved: sorted moves are shared round-robin among caller and available helpers; split-percent is ignored\n");
    printf("  interleaved: per-node thread limit is set by PARALLEL_INTERLEAVED_SPLIT_THREADS in src/parallel_search.c (includes caller)\n");
    printf("  parallel: iterative deepening (default max depth 3), in-memory cache, no time limit by default; no TT files\n");
    printf("  %-21s  %s\n", "-f, --fen FEN", "initialize the position from FEN");
    printf("  %-21s  %s\n", "    --fen-file PATH", "search once per FEN line, then exit (no interaction)");
    printf("  %-21s  %s\n", "", "batch: use each FEN's side to move; ignore --engine; reject --fen/--tt-file");
    printf("  %-21s  %s\n", "", "batch: no time limit unless --time-ms is set; clear cache before each FEN");
    printf("  %-21s  %s\n", "-e, --engine COLOR", "choose the engine side: red or black");
    printf("  %-21s  %s\n", "    --depth N", "maximum search depth in plies (builtin default 10; parallel default 3)");
    printf("  %-21s  %s\n", "    --time-ms MS", "default thinking time per engine move (positive integer milliseconds)");
    printf("  %-21s  %s\n", "", "a nearly complete iteration may finish after this budget");
    printf("  %-21s  %s\n", "    --tt-file PATH", "load a saved transposition table before the first search");
    printf("  %-21s  %s\n", "    --progress", "show live per-depth root-move progress on stderr");
    printf("  %-21s  %s\n", "    --sample-search", "sample logical move paths (independent of --progress)");
    printf("  %-21s  %s\n", "    --sample-timeline", "record per-thread time stacks and wait states (implies --sample-search)");
    printf("  %-21s  %s\n", "    --sample-timeline-max-mib N", "timeline file budget per thought; 0 = unlimited (default 0)");
    printf("  %-21s  %s\n", "    --sample-statistics-max-mib N", "statistics memory budget per iteration; 0 = unlimited (default 0)");
    printf("  %-21s  %s\n", "    --sample-interval-ms N", "positive sampling interval in milliseconds (default 20)");
    printf("  %-21s  %s\n", "    --sample-output-dir DIR", "sampling reports directory (default build/search_samples)");
    printf("  %-21s  %s\n", "    --search-detail", "append each engine search to build/search_detail.txt");
    printf("  %-21s  %s\n", "-h, --help", "show this help");
    printf("\n");
    printf("FEN strings containing spaces or jieqi '*' markers must be quoted.\n");
}

/**
 * 将命令行输入的英文颜色名称或缩写解析为棋子颜色。
 *
 * @param text 待解析的字符串，支持 "red"、"r"、"black" 和 "b"。
 * @param color 输出参数；解析成功时写入对应的 XqColor 值。
 * @return 解析成功时返回 true；输入不是受支持的颜色时返回 false。
 */
static bool parse_color(const char *text, XqColor *color)
{
    if (strcmp(text, "red") == 0 || strcmp(text, "r") == 0)
    {
        *color = XQ_RED;
        return true;
    }
    if (strcmp(text, "black") == 0 || strcmp(text, "b") == 0)
    {
        *color = XQ_BLACK;
        return true;
    }
    return false;
}

/**
 * 把用户在命令行输入的走法文本解析成程序内部的 XqMove，并且检查这个走法是不是当前局面的合法走法
 */
static bool parse_move_text(const char *text, const XqMoveList *legal, XqMove *move)
{
    int from_file;
    int from_rank;
    int to_file;
    int to_rank;
    XqSquare from;
    XqSquare to;
    int i;

    if (strlen(text) != 4)
        return false;

    from_file = tolower((unsigned char)text[0]) - 'a';
    from_rank = text[1] - '0';
    to_file = tolower((unsigned char)text[2]) - 'a';
    to_rank = text[3] - '0';

    if (!xq_square_is_valid(from_file, from_rank) || !xq_square_is_valid(to_file, to_rank))
        return false;

    from = xq_square_make(from_file, from_rank);
    to = xq_square_make(to_file, to_rank);

    for (i = 0; i < legal->count; ++i)
        if ((XqSquare)legal->moves[i].from == from && (XqSquare)legal->moves[i].to == to)
        {
            *move = legal->moves[i];
            return true;
        }

    return false;
}

/* Follow explain.c's integer parsing, but require positive depth for an engine move. */
static bool parse_depth(const char *text, unsigned *depth)
{
    char *end;
    long parsed;

    errno = 0;
    parsed = strtol(text, &end, 10);
    if (errno == ERANGE || text == end || parsed <= 0 || parsed > INT_MAX)
        return false;
    while (*end != '\0')
    {
        if (!isspace((unsigned char)*end))
            return false;
        ++end;
    }
    *depth = (unsigned)parsed;
    return true;
}

/* Parse decimal unsigned limits, rejecting overflow and extra arguments. */
static bool parse_unsigned_limit(const char *text, uint64_t *limit)
{
    uint64_t value = 0;

    if (*text < '0' || *text > '9')
        return false;
    while (*text >= '0' && *text <= '9')
    {
        unsigned digit = (unsigned)(*text++ - '0');
        if (value > (UINT64_MAX - digit) / 10)
            return false;
        value = value * 10 + digit;
    }
    while (isspace((unsigned char)*text))
        ++text;
    if (*text != '\0')
        return false;
    *limit = value;
    return true;
}

static bool parse_time_ms(const char *text, uint64_t *time_limit_ms)
{
    return parse_unsigned_limit(text, time_limit_ms) && *time_limit_ms > 0;
}

typedef struct CliMoveInput
{
    XqMove moves[2];
    XqPosition positions[2]; /* Position after each validated move. */
    int count;
    uint64_t time_limit_ms;
} CliMoveInput;

/* Tokenize the input buffer and validate on copies; NULL means success, otherwise an error.
 * The caller initializes time_limit_ms to the default and commits only after full validation. */
static const char *parse_move_input(char *text, const XqPosition *pos,
                                   const XqMoveList *legal, CliMoveInput *parsed)
{
    char *tokens[2];
    int count = 0;
    XqMoveList replies;

    while (*text != '\0')
    {
        while (isspace((unsigned char)*text))
            ++text;
        if (*text == '\0')
            break;
        if (count == 2)
            return "extra input: use one move, a move and time_ms, or exactly two moves";
        tokens[count++] = text;
        while (*text != '\0' && !isspace((unsigned char)*text))
            ++text;
        if (*text != '\0')
            *text++ = '\0';
    }

    if (count == 0 || !parse_move_text(tokens[0], legal, &parsed->moves[0]))
        return "first move is illegal or is not a four-character coordinate move";
    parsed->positions[0] = *pos;
    if (!xq_position_make_move(&parsed->positions[0], parsed->moves[0]))
        return "cannot apply the first move";
    parsed->count = 1;
    if (count == 1)
        return NULL;

    if (isdigit((unsigned char)tokens[1][0]))
    {
        if (!parse_time_ms(tokens[1], &parsed->time_limit_ms))
            return "second item is an invalid time limit; use positive integer milliseconds";
        return NULL;
    }

    xq_generate_legal(&parsed->positions[0], &replies);
    if (replies.count == 0)
        return "first move ends the game; enter it alone without an engine reply";
    if (!parse_move_text(tokens[1], &replies, &parsed->moves[1]))
        return "second item must be a legal four-character engine reply or positive time_ms";
    parsed->positions[1] = parsed->positions[0];
    if (!xq_position_make_move(&parsed->positions[1], parsed->moves[1]))
        return "cannot apply the second move";
    parsed->count = 2;
    return NULL;
}

/**
 * help
 */
static void print_help(bool parallel)
{
    printf("commands:\n");
    printf("  a0a1 [time_ms]  move; optional positive integer time limit for the next engine reply\n");
    printf("                  e.g. a0a1 2000 (2 seconds); omit time to use the startup default\n");
    printf("  a0a1 a9a8       play your move and specify the engine reply, without searching\n");
    printf("                  both moves must be legal; invalid input leaves the whole turn unchanged\n");
    printf("                  exactly two moves; no third move or additional time argument\n");
    printf("  fen             print current FEN\n");
    printf("  moves           print legal moves\n");
    printf("  undo            take back your last move and the automatic or specified engine reply (alias: u)\n");
    printf("  flip            rotate the board display 180 degrees; move coordinates stay unchanged\n");
    if (!parallel)
        printf("  save-tt PATH    save the current transposition table (paths may contain spaces)\n");
    printf("  quit            exit\n");
}

static const char *tt_io_status_text(XqTranspositionIoStatus status)
{
    switch (status)
    {
    case XQ_TT_IO_OK:
        return "success";
    case XQ_TT_IO_FILE_ERROR:
        return "file I/O error (check the path, parent directory and permissions)";
    case XQ_TT_IO_INVALID_FORMAT:
        return "invalid or corrupted transposition table file";
    case XQ_TT_IO_INCOMPATIBLE:
        return "incompatible cache format, engine version or table dimensions";
    case XQ_TT_IO_NO_MEMORY:
        return "not enough memory for the transposition table operation";
    case XQ_TT_IO_INVALID_ARGUMENT:
        return "invalid table or file path";
    }
    return "unknown transposition table error";
}

/* The entire remainder is one path. Optional matching outer quotes are stripped;
 * shell expansion and escape processing are intentionally not performed here. */
static char *parse_save_path(char *text)
{
    char *end;

    while (isspace((unsigned char)*text))
        ++text;
    end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1]))
        --end;
    *end = '\0';
    if (*text == '\'' || *text == '"')
    {
        if (end - text < 2 || end[-1] != *text)
            return NULL;
        ++text;
        *--end = '\0';
    }
    return *text != '\0' ? text : NULL;
}

/**
 * 打印合法走法
 */
static void print_legal_moves(const XqMoveList *legal)
{
    int i;
    char text[8];

    for (i = 0; i < legal->count; ++i)
    {
        printf("%s", xq_move_to_string(legal->moves[i], text, sizeof(text)));
        if ((i + 1) % 12 == 0 || i == legal->count - 1)
            printf("\n");
        else
            printf(" ");
    }
}

/* Logging failures disable only diagnostics, never the game. */
static void flush_search_detail(FILE **log)
{
    if (ferror(*log) || fflush(*log) == EOF)
    {
        fprintf(stderr, "warning: cannot write %s; search logging disabled\n", search_detail_path);
        fclose(*log);
        *log = NULL;
    }
}

static FILE *open_search_detail(const XqPosition *pos, XqColor engine_color,
                                const char *fen_file)
{
    FILE *log;
    char fen[128];
    char timestamp[64] = "unavailable";
    time_t now;
    struct tm *local;
    int directory_result;

#ifdef _WIN32
    directory_result = _mkdir("build");
#else
    directory_result = mkdir("build", 0777);
#endif
    if (directory_result != 0 && errno != EEXIST)
    {
        fprintf(stderr, "warning: cannot create build directory: %s; search logging disabled\n",
                strerror(errno));
        return NULL;
    }
    log = fopen(search_detail_path, "a");
    if (log == NULL)
    {
        fprintf(stderr, "warning: cannot open %s: %s; search logging disabled\n",
                search_detail_path, strerror(errno));
        return NULL;
    }
    now = time(NULL);
    local = now == (time_t)-1 ? NULL : localtime(&now);
    if (local != NULL && strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S %Z", local) == 0)
        strcpy(timestamp, "unavailable");
    if (fen_file != NULL)
        fprintf(log, "\n=== batch FEN search ===\ntimestamp: %s\ninput_file: %s\n"
                     "engine_color: fen_side_to_move\ncache_policy: clear_per_fen\n",
                timestamp, fen_file);
    else
    {
        if (!xq_position_to_fen(pos, fen, sizeof(fen)))
            strcpy(fen, "unavailable");
        fprintf(log, "\n=== new game ===\ntimestamp: %s\ninitial_fen: %s\nengine_color: %s\n",
                timestamp, fen, color_name(engine_color));
    }
    flush_search_detail(&log);
    return log;
}

static void write_search_detail(FILE **log, const XqPosition *pos,
                                const XqSearchLimits *limits, bool found,
                                XqMove best, const XqSearchStats *stats)
{
    FILE *out = *log;
    char fen[128];
    char move[8];
    const XqTranspositionStats *cache = &stats->cache;
    double hit_rate = cache->probes == 0 ? 0.0 : 100.0 * (double)cache->hits / (double)cache->probes;

    if (!xq_position_to_fen(pos, fen, sizeof(fen)))
        strcpy(fen, "unavailable");
    fprintf(out, "\n--- engine search ---\nfullmove_number: %" PRIu64 "\nside_to_move: %s\n"
                 "fen_before: %s\nsuccess: %s\nselected_move: %s\n",
            pos->fullmove_number, color_name(pos->side_to_move), fen,
            found ? "yes" : "no", found ? xq_move_to_string(best, move, sizeof(move)) : "none");
    fprintf(out, "depth_limit: %u\ntime_limit_ms: %" PRIu64 "\ntime_mode: %s\n",
            limits->max_depth, limits->time_limit_ms,
            limits->time_mode == XQ_SEARCH_TIME_CPU ? "cpu" : "monotonic");
    fprintf(out, "stats_available: %s\n", stats->available ? "yes" : "no");
    if (stats->available)
    {
        fprintf(out, "max_started_depth: %u\n", stats->max_started_depth);
        if (stats->max_started_depth == 0 || stats->iteration_total_moves == 0)
            fprintf(out, "iteration_progress: not-started (0/%u)\n",
                    stats->iteration_total_moves);
        else
            fprintf(out, "iteration_progress: %u/%u (%.1f%%)\n",
                    stats->iteration_completed_moves, stats->iteration_total_moves,
                    100.0 * (double)stats->iteration_completed_moves / stats->iteration_total_moves);
        fprintf(out, "completed_depth: %u\nselected_move_depth: %u\n"
                     "nodes: %" PRIu64 "\nstopped: %s\nwinning_move_found: %s\n",
                stats->completed_depth, stats->selected_move_depth,
                stats->nodes, stats->stopped ? "yes" : "no",
                stats->winning_move_found ? "yes" : "no");
    }
    if (stats->elapsed_available)
        fprintf(out, "elapsed_ms: %" PRIu64 "\n", stats->elapsed_ms);
    else
        fprintf(out, "elapsed_ms: unavailable\n");
    fprintf(out, "cache_available: %s\n", stats->cache_available ? "yes" : "no");
    fprintf(out, "cache_probes: %" PRIu64 "\ncache_hits: %" PRIu64 "\ncache_hit_rate: %.2f%%\n"
                 "cache_cutoffs: %" PRIu64 "\ncache_stores: %" PRIu64 "\ncache_replacements: %" PRIu64 "\n",
            cache->probes, cache->hits, hit_rate, cache->cutoffs, cache->stores, cache->replacements);
    flush_search_detail(log);
}

/* A specified engine reply is a real move, but produces no search statistics. */
static void write_manual_engine_move(FILE **log, const XqPosition *before,
                                     const XqPosition *after, XqMove move)
{
    char fen_before[128];
    char fen_after[128];
    char text[8];

    if (!xq_position_to_fen(before, fen_before, sizeof(fen_before)))
        strcpy(fen_before, "unavailable");
    if (!xq_position_to_fen(after, fen_after, sizeof(fen_after)))
        strcpy(fen_after, "unavailable");
    fprintf(*log, "\n--- manual engine move ---\nfullmove_number: %" PRIu64 "\nengine_color: %s\n"
                  "selected_move: %s\nfen_before: %s\nfen_after: %s\n",
            before->fullmove_number, color_name(before->side_to_move),
            xq_move_to_string(move, text, sizeof(text)), fen_before, fen_after);
    flush_search_detail(log);
}

/* Replaying the retained moves also restores the initial FEN's move counters.
 * History and board are committed together; the engine's cache is untouched. */
static bool undo_last_turn(XqPosition *pos, const XqPosition *initial,
                           XqHistory *history, size_t plies)
{
    XqPosition restored = *initial;
    size_t retained;
    size_t i;

    if (history->count <= plies)
        return false;
    retained = history->count - plies;
    for (i = 1; i < retained; ++i)
        if (!xq_position_make_move(&restored, history->entries[i].move))
            return false;
    if (!xq_history_truncate(history, retained))
        return false;
    *pos = restored;
    return true;
}

static void write_parallel_detail(FILE **log, const XqPosition *pos,
                                  const XqParallelOptions *options,
                                  XqParallelStatus status, const XqParallelResult *result)
{
    char fen[128], move[8];
    const XqSearchStats *stats = &result->stats;
    const XqTranspositionStats *cache = &stats->cache;
    double hit_rate = cache->probes == 0 ? 0.0 : 100.0 * (double)cache->hits / (double)cache->probes;

    if (!xq_position_to_fen(pos, fen, sizeof(fen)))
        strcpy(fen, "unavailable");
    fprintf(*log, "\n--- parallel engine search ---\nsearch_mode: parallel\n"
            "fen_before: %s\nstatus: %s\nselected_move: %s\nscore_available: %s\n",
            fen, xq_parallel_status_text(status),
            result->move_available ? xq_move_to_string(result->best_move, move, sizeof(move)) : "none",
            result->score_available ? "yes" : "no");
    if (result->score_available)
        fprintf(*log, "score: %d\nscore_kind: %s\n", result->score,
                score_kind_name(result->score_kind));
    else
        fprintf(*log, "score: unavailable\nscore_kind: unavailable\n");
    fprintf(*log, "depth: %u\ntime_limit_ms: %" PRIu64 "\ntime_mode: monotonic\n"
            "threads: %u\nsplit_strategy: %s\nsplit_percent: %u\n",
            options->depth, options->time_limit_ms, options->thread_count,
            split_strategy_name(options->split_strategy), options->split_percent);
    if (options->split_strategy == XQ_PARALLEL_SPLIT_INTERLEAVED)
        fprintf(*log, "split_percent_note: ignored for interleaved\n");
    fprintf(*log, "stats_available: %s\n", stats->available ? "yes" : "no");
    if (stats->available)
    {
        fprintf(*log, "max_started_depth: %u\niteration_progress: %u/%u\n"
                "completed_depth: %u\nselected_move_depth: %u\nnodes: %" PRIu64 "\n"
                "stopped: %s\nwinning_move_found: %s\n",
                stats->max_started_depth, stats->iteration_completed_moves,
                stats->iteration_total_moves, stats->completed_depth, stats->selected_move_depth,
                stats->nodes, stats->stopped ? "yes" : "no",
                stats->winning_move_found ? "yes" : "no");
    }
    if (stats->elapsed_available)
        fprintf(*log, "elapsed_ms: %" PRIu64 "\n", stats->elapsed_ms);
    else
        fprintf(*log, "elapsed_ms: unavailable\n");
    fprintf(*log, "cache_available: %s\n", stats->cache_available ? "yes" : "no");
    fprintf(*log, "cache_probes: %" PRIu64 "\ncache_hits: %" PRIu64 "\ncache_hit_rate: %.2f%%\n"
                 "cache_cutoffs: %" PRIu64 "\ncache_stores: %" PRIu64 "\ncache_replacements: %" PRIu64 "\n",
            cache->probes, cache->hits, hit_rate, cache->cutoffs, cache->stores, cache->replacements);
    flush_search_detail(log);
}

/* Keep one thought's observers and diagnostics identical in interactive and batch modes. */
static CliSearchResult search_once(const XqEngineAdapter *engine, XqPosition *pos,
                                   const XqSearchLimits *limits, bool parallel,
                                   XqParallelOptions *parallel_options,
                                   const CliSearchOutput *output, FILE **search_log,
                                   bool collect_stats)
{
    CliSearchResult search = {.status = XQ_PARALLEL_OK};
    unsigned depth = parallel ? parallel_options->depth : limits->max_depth;
    CliSearchProgress *progress = output->sample_search ?
        cli_search_progress_start_sampling_with_limits(
            depth, limits->time_limit_ms, output->show_progress,
            parallel ? parallel_options->thread_count : 1,
            output->sample_interval_ms, output->sample_output_dir, pos,
            parallel ? "parallel" : "builtin", output->sample_timeline,
            output->timeline_byte_limit, output->statistics_byte_limit) :
        output->show_progress ? cli_search_progress_start(depth, limits->time_limit_ms) : NULL;
    XqSearchProgressFn on_progress = progress != NULL ? cli_search_progress_update : NULL;
    XqSearchSampler *sampler = cli_search_progress_sampler(progress);

    if (parallel)
    {
        XqParallelResult result = {0};

        parallel_options->time_limit_ms = limits->time_limit_ms;
        search.status = xq_engine_parallel_search_with_observer(
            engine, pos, parallel_options, &result, on_progress, progress, sampler);
        search.found = search.status == XQ_PARALLEL_OK && result.move_available;
        search.best = result.best_move;
        search.stats = result.stats;
        cli_search_progress_finish(progress,
            search.status != XQ_PARALLEL_OK ? "search failed" :
            !search.found ? "no move" : result.stats.winning_move_found ? "winning move found" :
            result.stats.stopped ? "time limit / clock stop" : "finished", &result.stats);
        if (*search_log != NULL)
            write_parallel_detail(search_log, pos, parallel_options, search.status, &result);
    }
    else
    {
        search.found = xq_engine_find_best_move_with_sampling(
            engine, pos, limits, &search.best,
            collect_stats || *search_log != NULL || progress != NULL ? &search.stats : NULL,
            on_progress, progress, sampler);
        if (progress != NULL)
            cli_search_progress_finish(progress,
                !search.found ? "no move" : search.stats.winning_move_found ? "winning move found" :
                search.stats.stopped ? "time limit / clock stop" : "finished", &search.stats);
        if (*search_log != NULL)
            write_search_detail(search_log, pos, limits, search.found, search.best, &search.stats);
    }

    return search;
}

static void print_batch_position(size_t line_number, XqColor side, const char *status,
                                  const CliSearchResult *search)
{
    char move[8];

    printf("batch position: line=%zu side=%s status=%s best_move=%s ",
           line_number, color_name(side), status,
           search->found ? xq_move_to_string(search->best, move, sizeof(move)) : "none");
    if (search->stats.available)
        printf("completed_depth=%u nodes=%" PRIu64 " ",
               search->stats.completed_depth, search->stats.nodes);
    else
        printf("completed_depth=unavailable nodes=unavailable ");
    if (search->stats.elapsed_available)
        printf("elapsed_ms=%" PRIu64 "\n", search->stats.elapsed_ms);
    else
        printf("elapsed_ms=unavailable\n");
}

static void print_batch_summary(const CliBatchSummary *summary, unsigned failures,
                                 clock_t start)
{
    clock_t end = clock();

    printf("batch summary: processed=%zu no_move=%zu failures=%u total_nodes=%" PRIu64
           " search_elapsed_ms=", summary->processed, summary->no_moves, failures, summary->nodes);
    if (summary->elapsed_unavailable)
        printf("unavailable");
    else
        printf("%" PRIu64, summary->search_elapsed_ms);
    if (start == (clock_t)-1 || end == (clock_t)-1 || end < start)
        printf(" cpu_seconds=unavailable\n");
    else
        printf(" cpu_seconds=%.3f\n", (double)(end - start) / (double)CLOCKS_PER_SEC);
    printf("batch tt summary: probes=%" PRIu64 " hits=%" PRIu64 " cutoffs=%" PRIu64
           " stores=%" PRIu64 " replacements=%" PRIu64 "\n",
           summary->cache.probes, summary->cache.hits, summary->cache.cutoffs,
           summary->cache.stores, summary->cache.replacements);
}

/* A file is a sequence of independent roots, never a game or a source of stdin commands. */
static int run_fen_batch(const char *path, bool parallel, const XqSearchLimits *limits,
                          XqParallelOptions parallel_options, const CliSearchOutput *output,
                          bool search_detail)
{
    clock_t start = clock();
    FILE *input = NULL;
    FILE *search_log = NULL;
    XqTranspositionTable *table = NULL;
    XqEngineAdapter engine = {0};
    CliBatchSummary summary = {0};
    const char *failure = NULL;
    size_t line_number = 0;
    char line[4096];

    parallel_options.transposition_table = NULL;
    input = fopen(path, "rb");
    if (input == NULL)
    {
        failure = "could not open input file";
        goto cleanup;
    }
    if (parallel)
    {
        parallel_options.transposition_table = xq_parallel_transposition_table_create();
        if (parallel_options.transposition_table == NULL)
            fprintf(stderr, "warning: parallel transposition table allocation failed; continuing without cache\n");
    }
    else
    {
        table = xq_transposition_table_create();
        if (table == NULL)
            fprintf(stderr, "warning: transposition table allocation failed; continuing without cache\n");
    }
    engine.transposition_table = table;
    engine.history = NULL;
    if (search_detail)
        search_log = open_search_detail(NULL, XQ_COLOR_NB, path);
    printf("batch: input=%s search=%s depth=%u time_limit_ms=%" PRIu64
           " cache_policy=clear_per_fen engine_side=fen\n", path,
           parallel ? "parallel" : "builtin", parallel ? parallel_options.depth : limits->max_depth,
           limits->time_limit_ms);
    if (parallel)
        printf("batch parallel: threads=%u split_strategy=%s split_percent=%u%s\n",
               parallel_options.thread_count, split_strategy_name(parallel_options.split_strategy),
               parallel_options.split_percent,
               parallel_options.split_strategy == XQ_PARALLEL_SPLIT_INTERLEAVED ? " (ignored)" : "");

    while (fgets(line, sizeof(line), input) != NULL)
    {
        XqPosition pos;
        XqMoveList legal;
        CliSearchResult search;
        char *text = line;
        char *end;

        ++line_number;
        if (ferror(input))
        {
            failure = "failed while reading input file";
            break;
        }
        if (strchr(line, '\n') == NULL && strlen(line) == sizeof(line) - 1u)
        {
            int ch = fgetc(input);
            if (ch != '\n' && ch != EOF)
            {
                failure = "line is too long (maximum 4095 bytes)";
                break;
            }
            if (ferror(input))
            {
                failure = "failed while reading input file";
                break;
            }
        }
        while (isspace((unsigned char)*text))
            ++text;
        end = text + strlen(text);
        while (end > text && isspace((unsigned char)end[-1]))
            --end;
        *end = '\0';
        if (*text == '\0')
            continue;
        if (!xq_position_from_fen(&pos, text))
        {
            failure = "invalid FEN";
            break;
        }

        /* Clearing occurs outside the search/observer lifetime and its elapsed time. */
        if (parallel)
            xq_parallel_transposition_table_clear(parallel_options.transposition_table);
        else
            xq_transposition_table_clear(table);
        xq_generate_legal(&pos, &legal);
        if (search_log != NULL)
        {
            fprintf(search_log, "\n--- batch position ---\ninput_file: %s\nline_number: %zu\n"
                                "side_to_move: %s\n", path, line_number, color_name(pos.side_to_move));
            flush_search_detail(&search_log);
        }
        search = search_once(&engine, &pos, limits, parallel, &parallel_options,
                             output, &search_log, true);
        summary.nodes += search.stats.nodes;
        if (search.stats.elapsed_available)
            summary.search_elapsed_ms += search.stats.elapsed_ms;
        else
            summary.elapsed_unavailable = true;
        summary.cache.probes += search.stats.cache.probes;
        summary.cache.hits += search.stats.cache.hits;
        summary.cache.cutoffs += search.stats.cache.cutoffs;
        summary.cache.stores += search.stats.cache.stores;
        summary.cache.replacements += search.stats.cache.replacements;

        if (search.status != XQ_PARALLEL_OK)
            failure = xq_parallel_status_text(search.status);
        else if (!search.found && legal.count > 0)
            failure = "engine failed to find a move";
        print_batch_position(line_number, pos.side_to_move,
                             failure != NULL ? "error" : search.found ? "ok" : "no_move", &search);
        if (failure != NULL)
            break;
        ++summary.processed;
        if (!search.found)
            ++summary.no_moves;
    }
    if (failure == NULL && ferror(input))
        failure = "failed while reading input file";

cleanup:
    if (input != NULL && fclose(input) != 0 && failure == NULL)
        failure = "failed to close input file";
    if (failure == NULL && summary.processed == 0)
        failure = "input file contains no positions";
    if (failure != NULL)
    {
        if (line_number == 0)
            fprintf(stderr, "error: %s: %s\n", path, failure);
        else
            fprintf(stderr, "error: %s:%zu: %s\n", path, line_number, failure);
    }
    if (search_log != NULL && fclose(search_log) == EOF)
        fprintf(stderr, "warning: cannot close %s\n", search_detail_path);
    xq_transposition_table_destroy(table);
    xq_parallel_transposition_table_destroy(parallel_options.transposition_table);
    print_batch_summary(&summary, failure != NULL ? 1u : 0u, start);
    return failure != NULL ? EXIT_FAILURE : EXIT_SUCCESS;
}

/* Jieqi diagnostics and search glue. Ordinary progress is reused without a sampler. */
static const char *jq_search_reveal_mode_name(CliJqRevealMode mode)
{
    switch (mode)
    {
    case CLI_JQ_REVEAL_MANUAL: return "manual";
    case CLI_JQ_REVEAL_RANDOM: return "random";
    }
    return "unknown";
}

static const char *jq_search_split_strategy_name(XqJqParallelSplitStrategy strategy)
{
    switch (strategy)
    {
    case XQ_JQ_PARALLEL_SPLIT_CONTIGUOUS: return "contiguous";
    case XQ_JQ_PARALLEL_SPLIT_INTERLEAVED: return "interleaved";
    }
    return "unknown";
}

static const char *jq_search_score_kind_name(XqJqSearchScoreKind kind)
{
    switch (kind)
    {
    case XQ_JQ_SEARCH_SCORE_EXACT: return "exact";
    case XQ_JQ_SEARCH_SCORE_UPPER_BOUND: return "upper_bound";
    case XQ_JQ_SEARCH_SCORE_LOWER_BOUND: return "lower_bound";
    }
    return "unknown";
}

static XqParallelStatus jq_search_status_for_cli(XqJqParallelStatus status)
{
    switch (status)
    {
    case XQ_JQ_PARALLEL_OK: return XQ_PARALLEL_OK;
    case XQ_JQ_PARALLEL_INVALID_ARGUMENT: return XQ_PARALLEL_INVALID_ARGUMENT;
    case XQ_JQ_PARALLEL_RESOURCE_ERROR: return XQ_PARALLEL_RESOURCE_ERROR;
    case XQ_JQ_PARALLEL_UNSUPPORTED: return XQ_PARALLEL_UNSUPPORTED;
    }
    return XQ_PARALLEL_INVALID_ARGUMENT;
}

/* The two public statistics structs have distinct types; copy fields rather than
 * relying on their currently matching layouts or casting their pointers. */
static XqSearchStats jq_search_copy_stats(const XqJqSearchStats *stats)
{
    XqSearchStats copied = {0};

    copied.available = stats->available;
    copied.max_started_depth = stats->max_started_depth;
    copied.iteration_completed_moves = stats->iteration_completed_moves;
    copied.iteration_total_moves = stats->iteration_total_moves;
    copied.completed_depth = stats->completed_depth;
    copied.selected_move_depth = stats->selected_move_depth;
    copied.nodes = stats->nodes;
    copied.stopped = stats->stopped;
    copied.winning_move_found = stats->winning_move_found;
    copied.elapsed_available = stats->elapsed_available;
    copied.elapsed_ms = stats->elapsed_ms;
    copied.cache_available = stats->cache_available;
    copied.cache.probes = stats->cache.probes;
    copied.cache.hits = stats->cache.hits;
    copied.cache.cutoffs = stats->cache.cutoffs;
    copied.cache.stores = stats->cache.stores;
    copied.cache.replacements = stats->cache.replacements;
    return copied;
}

static void jq_search_progress_update(const XqJqSearchProgress *update, void *user)
{
    XqSearchProgress copied = {0};

    if (update == NULL)
        return;
    switch (update->event)
    {
    case XQ_JQ_SEARCH_ITERATION_STARTED:
        copied.event = XQ_SEARCH_ITERATION_STARTED;
        break;
    case XQ_JQ_SEARCH_ROOT_MOVE_COMPLETED:
        copied.event = XQ_SEARCH_ROOT_MOVE_COMPLETED;
        break;
    case XQ_JQ_SEARCH_ITERATION_FINISHED:
        copied.event = XQ_SEARCH_ITERATION_FINISHED;
        break;
    default:
        return;
    }
    copied.depth = update->depth;
    copied.completed_moves = update->completed_moves;
    copied.total_moves = update->total_moves;
    copied.sample = NULL;
    cli_search_progress_update(&copied, user);
}

/* These counts are public knowledge, including captured, unrevealed identities.
 * This helper never receives the real-game hidden assignment or its random state. */
static void write_pool_jq(FILE *out, const XqJqPosition *pos)
{
    for (int color = 0; color < XQ_COLOR_NB; ++color)
    {
        fprintf(out, "pool_%s_total: %u\npool_%s:", color_name((XqColor)color),
                (unsigned)pos->pool_total[color], color_name((XqColor)color));
        for (int type = 0; type < XQ_PIECE_TYPE_NB; ++type)
            fprintf(out, " %s=%u", xq_jq_piece_type_name((XqPieceType)type),
                    (unsigned)pos->pool[color][type]);
        fputc('\n', out);
    }
}

static FILE *open_search_detail_jq(const XqJqPosition *pos, XqColor engine_color,
                                   const char *fen_file, CliJqRevealMode reveal_mode)
{
    FILE *log;
    char fen[256];
    char timestamp[64] = "unavailable";
    time_t now;
    struct tm *local;
    int directory_result;

#ifdef _WIN32
    directory_result = _mkdir("build");
#else
    directory_result = mkdir("build", 0777);
#endif
    if (directory_result != 0 && errno != EEXIST)
    {
        fprintf(stderr, "warning: cannot create build directory: %s; search logging disabled\n",
                strerror(errno));
        return NULL;
    }
    log = fopen(search_detail_path, "a");
    if (log == NULL)
    {
        fprintf(stderr, "warning: cannot open %s: %s; search logging disabled\n",
                search_detail_path, strerror(errno));
        return NULL;
    }
    now = time(NULL);
    local = now == (time_t)-1 ? NULL : localtime(&now);
    if (local != NULL && strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S %Z", local) == 0)
        strcpy(timestamp, "unavailable");
    if (fen_file != NULL)
        fprintf(log, "\n=== batch FEN search ===\ntimestamp: %s\ninput_file: %s\n"
                     "engine_color: fen_side_to_move\ncache_policy: clear_per_fen\n"
                     "game_mode: jieqi\nreveal_mode: %s\n",
                timestamp, fen_file, jq_search_reveal_mode_name(reveal_mode));
    else
    {
        if (!xq_jq_position_to_fen(pos, fen, sizeof(fen)))
            strcpy(fen, "unavailable");
        fprintf(log, "\n=== new game ===\ntimestamp: %s\ninitial_fen: %s\nengine_color: %s\n"
                     "game_mode: jieqi\nreveal_mode: %s\n",
                timestamp, fen, color_name(engine_color), jq_search_reveal_mode_name(reveal_mode));
        write_pool_jq(log, pos);
    }
    flush_search_detail(&log);
    return log;
}

/* Called only after the move and its history entry have both been committed. */
static void write_played_move_jq(FILE **log, const XqJqPosition *before,
                                const XqJqPosition *after, XqMove move,
                                int revealed_type, const char *source)
{
    char fen_before[256], fen_after[256], text[8];

    if (log == NULL || *log == NULL)
        return;
    if (!xq_jq_position_to_fen(before, fen_before, sizeof(fen_before)))
        strcpy(fen_before, "unavailable");
    if (!xq_jq_position_to_fen(after, fen_after, sizeof(fen_after)))
        strcpy(fen_after, "unavailable");
    fprintf(*log, "\n--- played move ---\nfullmove_number: %" PRIu64 "\nside_to_move: %s\n"
                  "selected_move: %s\nfen_before: %s\nfen_after: %s\n"
                  "game_mode: jieqi\nmove_source: %s\nrevealed_type: %s\n",
            before->fullmove_number, color_name(before->side_to_move),
            xq_jq_move_to_string(move, text, sizeof(text)), fen_before, fen_after,
            source != NULL ? source : "unknown",
            revealed_type == XQ_JQ_NO_REVEAL ? "none" :
                xq_jq_piece_type_name((XqPieceType)revealed_type));
    fprintf(*log, "public_pool_before:\n");
    write_pool_jq(*log, before);
    fprintf(*log, "public_pool_after:\n");
    write_pool_jq(*log, after);
    flush_search_detail(log);
}

static void write_parallel_detail_jq(FILE **log, const XqJqPosition *pos,
                                     const XqJqParallelOptions *options,
                                     XqJqParallelStatus status,
                                     const XqJqParallelResult *result)
{
    char fen[256], move[8];
    const XqJqSearchStats *stats = &result->stats;
    const XqJqTranspositionStats *cache = &stats->cache;
    double hit_rate = cache->probes == 0 ? 0.0 : 100.0 * (double)cache->hits / (double)cache->probes;

    if (!xq_jq_position_to_fen(pos, fen, sizeof(fen)))
        strcpy(fen, "unavailable");
    fprintf(*log, "\n--- parallel engine search ---\nsearch_mode: parallel\n"
                  "fen_before: %s\nstatus: %s\nselected_move: %s\nscore_available: %s\n",
            fen, xq_jq_parallel_status_text(status),
            result->move_available ? xq_jq_move_to_string(result->best_move, move, sizeof(move)) : "none",
            result->score_available ? "yes" : "no");
    if (result->score_available)
        fprintf(*log, "score: %d\nscore_kind: %s\n", result->score,
                jq_search_score_kind_name(result->score_kind));
    else
        fprintf(*log, "score: unavailable\nscore_kind: unavailable\n");
    fprintf(*log, "depth: %u\ntime_limit_ms: %" PRIu64 "\ntime_mode: monotonic\n"
                  "threads: %u\nsplit_strategy: %s\nsplit_percent: %u\n",
            options->depth, options->time_limit_ms, options->thread_count,
            jq_search_split_strategy_name(options->split_strategy), options->split_percent);
    if (options->split_strategy == XQ_JQ_PARALLEL_SPLIT_INTERLEAVED)
        fprintf(*log, "split_percent_note: ignored for interleaved\n");
    fprintf(*log, "game_mode: jieqi\nscore_estimated: %s\nsearch_seed: %" PRIu64 "\n",
            result->score_estimated ? "yes" : "no", options->random_seed);
    write_pool_jq(*log, pos);
    fprintf(*log, "stats_available: %s\n", stats->available ? "yes" : "no");
    if (stats->available)
    {
        fprintf(*log, "max_started_depth: %u\niteration_progress: %u/%u\n"
                      "completed_depth: %u\nselected_move_depth: %u\nnodes: %" PRIu64 "\n"
                      "stopped: %s\nwinning_move_found: %s\n",
                stats->max_started_depth, stats->iteration_completed_moves,
                stats->iteration_total_moves, stats->completed_depth, stats->selected_move_depth,
                stats->nodes, stats->stopped ? "yes" : "no",
                stats->winning_move_found ? "yes" : "no");
    }
    if (stats->elapsed_available)
        fprintf(*log, "elapsed_ms: %" PRIu64 "\n", stats->elapsed_ms);
    else
        fprintf(*log, "elapsed_ms: unavailable\n");
    fprintf(*log, "cache_available: %s\n", stats->cache_available ? "yes" : "no");
    fprintf(*log, "cache_probes: %" PRIu64 "\ncache_hits: %" PRIu64 "\ncache_hit_rate: %.2f%%\n"
                  "cache_cutoffs: %" PRIu64 "\ncache_stores: %" PRIu64 "\ncache_replacements: %" PRIu64 "\n",
            cache->probes, cache->hits, hit_rate, cache->cutoffs, cache->stores, cache->replacements);
    flush_search_detail(log);
}

/* Both callers use this path; the progress owner is never a sampling controller. */
static XqJqParallelStatus search_once_jq(const XqJqEngineAdapter *engine,
                                        const XqJqPosition *pos,
                                        XqJqParallelOptions *options,
                                        const CliSearchOutput *output, FILE **log,
                                        XqJqParallelResult *result)
{
    CliSearchProgress *progress = output->show_progress ?
        cli_search_progress_start(options->depth, options->time_limit_ms) : NULL;
    XqJqSearchProgressFn on_progress = progress != NULL ? jq_search_progress_update : NULL;
    XqJqParallelStatus status = xq_jq_engine_parallel_search_with_observer(
        engine, pos, options, result, on_progress, progress);
    XqSearchStats copied = jq_search_copy_stats(&result->stats);
    bool found = status == XQ_JQ_PARALLEL_OK && result->move_available;

    cli_search_progress_finish(progress,
        status != XQ_JQ_PARALLEL_OK ? "search failed" :
        !found ? "no move" : result->stats.winning_move_found ? "winning move found" :
        result->stats.stopped ? "time limit / clock stop" : "finished", &copied);
    if (log != NULL && *log != NULL)
        write_parallel_detail_jq(log, pos, options, status, result);
    return status;
}

/* Every line defines a fresh public starting position. No move is played, so
 * neither reveal mode prompts nor a real-game hidden assignment is needed. */
static int run_fen_batch_jq(const char *path, XqJqParallelOptions options,
                           const CliSearchOutput *output, bool search_detail,
                           CliJqRevealMode reveal_mode)
{
    clock_t start = clock();
    FILE *input = NULL;
    FILE *search_log = NULL;
    XqJqEngineAdapter engine = {0};
    CliBatchSummary summary = {0};
    const char *failure = NULL;
    size_t line_number = 0;
    char line[4096];

    options.transposition_table = NULL;
    input = fopen(path, "rb");
    if (input == NULL)
    {
        failure = "could not open input file";
        goto cleanup;
    }
    options.transposition_table = xq_jq_parallel_transposition_table_create();
    if (options.transposition_table == NULL)
        fprintf(stderr, "warning: jieqi parallel transposition table allocation failed; continuing without cache\n");
    engine.history = NULL;
    if (search_detail)
        search_log = open_search_detail_jq(NULL, XQ_COLOR_NB, path, reveal_mode);
    printf("batch: input=%s search=parallel depth=%u time_limit_ms=%" PRIu64
           " cache_policy=clear_per_fen engine_side=fen game_mode=jieqi reveal_mode=%s\n",
           path, options.depth, options.time_limit_ms, jq_search_reveal_mode_name(reveal_mode));
    printf("batch parallel: threads=%u split_strategy=%s split_percent=%u%s\n",
           options.thread_count, jq_search_split_strategy_name(options.split_strategy),
           options.split_percent,
           options.split_strategy == XQ_JQ_PARALLEL_SPLIT_INTERLEAVED ? " (ignored)" : "");

    while (fgets(line, sizeof(line), input) != NULL)
    {
        XqJqPosition pos;
        XqJqMoveList legal;
        XqJqParallelResult result = {0};
        XqJqParallelStatus status;
        CliSearchResult search = {0};
        char *text = line;
        char *end;

        ++line_number;
        if (ferror(input))
        {
            failure = "failed while reading input file";
            break;
        }
        if (strchr(line, '\n') == NULL && strlen(line) == sizeof(line) - 1u)
        {
            int ch = fgetc(input);
            if (ch != '\n' && ch != EOF)
            {
                failure = "line is too long (maximum 4095 bytes)";
                break;
            }
            if (ferror(input))
            {
                failure = "failed while reading input file";
                break;
            }
        }
        while (isspace((unsigned char)*text))
            ++text;
        end = text + strlen(text);
        while (end > text && isspace((unsigned char)end[-1]))
            --end;
        *end = '\0';
        if (*text == '\0')
            continue;
        if (!xq_jq_position_from_fen(&pos, text))
        {
            failure = "invalid jieqi FEN";
            break;
        }

        /* Clearing occurs outside the search/observer lifetime and its elapsed time. */
        xq_jq_parallel_transposition_table_clear(options.transposition_table);
        xq_jq_generate_legal(&pos, &legal);
        if (search_log != NULL)
        {
            fprintf(search_log, "\n--- batch position ---\ninput_file: %s\nline_number: %zu\n"
                                "side_to_move: %s\n", path, line_number, color_name(pos.side_to_move));
            flush_search_detail(&search_log);
        }
        status = search_once_jq(&engine, &pos, &options, output, &search_log, &result);
        search.found = status == XQ_JQ_PARALLEL_OK && result.move_available;
        search.best = result.best_move;
        search.stats = jq_search_copy_stats(&result.stats);
        search.status = jq_search_status_for_cli(status);
        summary.nodes += search.stats.nodes;
        if (search.stats.elapsed_available)
            summary.search_elapsed_ms += search.stats.elapsed_ms;
        else
            summary.elapsed_unavailable = true;
        summary.cache.probes += search.stats.cache.probes;
        summary.cache.hits += search.stats.cache.hits;
        summary.cache.cutoffs += search.stats.cache.cutoffs;
        summary.cache.stores += search.stats.cache.stores;
        summary.cache.replacements += search.stats.cache.replacements;

        if (status != XQ_JQ_PARALLEL_OK)
            failure = xq_jq_parallel_status_text(status);
        else if (!search.found && legal.count > 0)
            failure = "engine failed to find a move";
        print_batch_position(line_number, pos.side_to_move,
                             failure != NULL ? "error" : search.found ? "ok" : "no_move", &search);
        if (failure != NULL)
            break;
        ++summary.processed;
        if (!search.found)
            ++summary.no_moves;
    }
    if (failure == NULL && ferror(input))
        failure = "failed while reading input file";

cleanup:
    if (input != NULL && fclose(input) != 0 && failure == NULL)
        failure = "failed to close input file";
    if (failure == NULL && summary.processed == 0)
        failure = "input file contains no positions";
    if (failure != NULL)
    {
        if (line_number == 0)
            fprintf(stderr, "error: %s: %s\n", path, failure);
        else
            fprintf(stderr, "error: %s:%zu: %s\n", path, line_number, failure);
    }
    if (search_log != NULL && fclose(search_log) == EOF)
        fprintf(stderr, "warning: cannot close %s\n", search_detail_path);
    xq_jq_parallel_transposition_table_destroy(options.transposition_table);
    print_batch_summary(&summary, failure != NULL ? 1u : 0u, start);
    return failure != NULL ? EXIT_FAILURE : EXIT_SUCCESS;
}

typedef struct CliJqMoveInput
{
    XqMove moves[2];
    XqJqPosition positions[2]; /* Position after each validated, revealed move. */
    int revealed_types[2];
    int count;
    uint64_t time_limit_ms;
} CliJqMoveInput;

/* Return 1 for a complete line, 0 for EOF/read failure, and -1 for an overlong
 * line that has been drained. A line exactly filling the buffer is accepted if
 * the next character is its newline or EOF, matching the ordinary CLI. */
static int read_line_jq(char *input, size_t size)
{
    if (fgets(input, (int)size, stdin) == NULL)
        return 0;
    if (strchr(input, '\n') == NULL && !feof(stdin))
    {
        int ch = getchar();
        if (ch != '\n' && ch != EOF)
        {
            while ((ch = getchar()) != '\n' && ch != EOF)
                ;
            printf("input too long (maximum %zu bytes); input ignored.\n", size - 1);
            return -1;
        }
    }
    input[strcspn(input, "\r\n")] = '\0';
    return 1;
}

static char *trim_input_jq(char *text)
{
    char *end;

    while (isspace((unsigned char)*text))
        ++text;
    end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1]))
        --end;
    *end = '\0';
    return text;
}

/* A type letter carries no color: the moving piece keeps its own color. The
 * ordinary character parser also accepts H/h as an alias for N/n. */
static bool parse_reveal_type_jq(const char *text, int *revealed_type)
{
    int piece;
    XqPieceType type;

    if (strlen(text) != 1)
        return false;
    piece = xq_jq_piece_from_char(text[0]);
    if (piece == XQ_EMPTY_PIECE)
        return false;
    type = xq_piece_type(piece);
    if (type == XQ_KING)
        return false;
    *revealed_type = (int)type;
    return true;
}

/* Parse one candidate against the public legal list. Random identities are read
 * only from the separate real-game map, never from search output. Nothing is
 * printed while validating a speculative two-move command. */
static const char *parse_move_token_jq(const char *text, const XqJqPosition *pos,
                                      const XqJqMoveList *legal,
                                      CliJqRevealMode reveal_mode,
                                      const XqJqHiddenState *hidden,
                                      XqMove *move, int *revealed_type)
{
    size_t length = strlen(text);
    int from_file, from_rank, to_file, to_rank;
    XqSquare from, to;
    int i;
    bool suffix;
    bool dark;

    if (length != 4 && (length != 6 || text[4] != '='))
        return "use a four-character move, optionally followed by =TYPE in manual mode";
    suffix = length == 6;
    if (reveal_mode == CLI_JQ_REVEAL_RANDOM && suffix)
        return "random reveal mode accepts coordinate moves without =TYPE";

    from_file = tolower((unsigned char)text[0]) - 'a';
    from_rank = text[1] - '0';
    to_file = tolower((unsigned char)text[2]) - 'a';
    to_rank = text[3] - '0';
    if (!xq_square_is_valid(from_file, from_rank) || !xq_square_is_valid(to_file, to_rank))
        return "move coordinates must use files a-i and ranks 0-9";
    from = xq_square_make(from_file, from_rank);
    to = xq_square_make(to_file, to_rank);
    for (i = 0; i < legal->count; ++i)
        if ((XqSquare)legal->moves[i].from == from && (XqSquare)legal->moves[i].to == to)
            break;
    if (i == legal->count)
        return "move is illegal in the current position";
    *move = legal->moves[i];
    *revealed_type = XQ_JQ_NO_REVEAL;
    dark = xq_jq_position_is_dark(pos, from);
    if (!dark)
        return suffix ? "a revealed piece must be moved without =TYPE" : NULL;

    if (reveal_mode == CLI_JQ_REVEAL_MANUAL)
    {
        if (!suffix)
            return "a dark move needs its revealed type, for example a0a1=R";
        if (!parse_reveal_type_jq(text + 5, revealed_type))
            return "revealed type must be A, B, N (or H), R, C or P; kings are not allowed";
        if (pos->pool[pos->side_to_move][*revealed_type] == 0)
            return "that revealed type is not available in the moving side's public pool";
    }
    else
    {
        XqPieceType type;
        if (hidden == NULL || !xq_jq_hidden_get_type(hidden, from, &type))
            return "cannot obtain the real-game reveal for this dark piece";
        *revealed_type = (int)type;
    }
    return NULL;
}

/* Tokenize and validate on copies. No real board, history, hidden assignment or
 * output is changed until the entire command has been accepted and recorded. */
static const char *parse_move_input_jq(char *text, const XqJqPosition *pos,
                                      const XqJqMoveList *legal,
                                      CliJqRevealMode reveal_mode,
                                      const XqJqHiddenState *hidden,
                                      CliJqMoveInput *parsed)
{
    char *tokens[2];
    int count = 0;
    XqJqMoveList replies;
    const char *error;
    bool timed = false;

    while (*text != '\0')
    {
        while (isspace((unsigned char)*text))
            ++text;
        if (*text == '\0')
            break;
        if (count == 2)
            return "extra input: use one move, a move and time_ms, or exactly two moves";
        tokens[count++] = text;
        while (*text != '\0' && !isspace((unsigned char)*text))
            ++text;
        if (*text != '\0')
            *text++ = '\0';
    }
    if (count == 0)
        return "enter a move";
    if (count == 2 && isdigit((unsigned char)tokens[1][0]))
    {
        if (!parse_time_ms(tokens[1], &parsed->time_limit_ms))
            return "second item is an invalid time limit; use positive integer milliseconds";
        timed = true;
    }

    error = parse_move_token_jq(tokens[0], pos, legal, reveal_mode, hidden,
                                &parsed->moves[0], &parsed->revealed_types[0]);
    if (error != NULL)
        return error;
    parsed->positions[0] = *pos;
    if (!xq_jq_position_make_move(&parsed->positions[0], parsed->moves[0],
                                  parsed->revealed_types[0]))
        return "cannot apply the first move";
    parsed->count = 1;
    if (count == 1 || timed)
        return NULL;

    xq_jq_generate_legal(&parsed->positions[0], &replies);
    if (replies.count == 0)
        return "first move ends the game; enter it alone without an engine reply";
    error = parse_move_token_jq(tokens[1], &parsed->positions[0], &replies, reveal_mode, hidden,
                                &parsed->moves[1], &parsed->revealed_types[1]);
    if (error != NULL)
        return error;
    parsed->positions[1] = parsed->positions[0];
    if (!xq_jq_position_make_move(&parsed->positions[1], parsed->moves[1],
                                  parsed->revealed_types[1]))
        return "cannot apply the second move";
    parsed->count = 2;
    return NULL;
}

static void print_help_jq(CliJqRevealMode reveal_mode)
{
    printf("commands:\n");
    if (reveal_mode == CLI_JQ_REVEAL_MANUAL)
    {
        printf("  a0a1=R [time_ms]  dark move with its revealed type; bright moves use a0a1\n");
        printf("                    types: A B N (or H) R C P, case-insensitive; no kings\n");
        printf("  a0a1=R a9a8=N     play your move and specify the engine reply, without searching\n");
        printf("                    attach =TYPE only to dark movers, on either move\n");
        printf("  engine dark moves prompt for a type after announcing the selected coordinates\n");
    }
    else
    {
        printf("  a0a1 [time_ms]     move; dark types are revealed automatically after commitment\n");
        printf("  a0a1 a9a8         play your move and specify the engine reply, without searching\n");
        printf("                    random reveal mode does not accept =TYPE\n");
    }
    printf("  time_ms overrides only the next searched reply; omit it to use the startup default\n");
    printf("  both specified moves must be legal after their reveals; invalid input commits neither\n");
    printf("  exactly two moves; no third move or extra time argument\n");
    printf("  fen               print the visible board FEN (not a complete midgame save)\n");
    printf("  pool              print the public unknown identity pools, including captured unknowns\n");
    printf("  moves             print legal coordinate moves\n");
    printf("  undo              take back your last move and its engine reply (alias: u)\n");
    if (reveal_mode == CLI_JQ_REVEAL_MANUAL)
        printf("                    undone dark moves may be entered again with a corrected type\n");
    else
        printf("                    real-game dark identities remain fixed when moves are undone\n");
    printf("  flip              rotate the board display; move coordinates stay unchanged\n");
    printf("  help              show these commands (alias: ?)\n");
    printf("  quit              exit (alias: q)\n");
}

static void print_legal_moves_jq(const XqJqMoveList *legal)
{
    char text[8];

    for (int i = 0; i < legal->count; ++i)
    {
        printf("%s", xq_jq_move_to_string(legal->moves[i], text, sizeof(text)));
        if ((i + 1) % 12 == 0 || i == legal->count - 1)
            printf("\n");
        else
            printf(" ");
    }
}

/* Retained reveal values replay the public pools exactly. Random assignments are
 * immutable and manual mode deliberately keeps no permanent identity memory. */
static bool undo_last_turn_jq(XqJqPosition *pos, const XqJqPosition *initial,
                              XqJqHistory *history, size_t plies)
{
    XqJqPosition restored = *initial;
    size_t retained;

    if (history->count <= plies)
        return false;
    retained = history->count - plies;
    for (size_t i = 1; i < retained; ++i)
        if (!xq_jq_position_make_move(&restored, history->entries[i].move,
                                      history->entries[i].revealed_type))
            return false;
    if (!xq_jq_history_truncate(history, retained))
        return false;
    *pos = restored;
    return true;
}

/* Invalid answers retry the same selected move, not the search. EOF/quit leave
 * the pending move uncommitted. Only this moving dark piece is ever revealed. */
static bool read_engine_reveal_jq(const XqJqPosition *pos, XqMove move, int *revealed_type)
{
    char input[128];
    char coordinate[8];

    for (;;)
    {
        printf("Reveal %s engine dark move %s as A/B/N/R/C/P (or quit) > ",
               color_name(pos->side_to_move),
               xq_jq_move_to_string(move, coordinate, sizeof(coordinate)));
        fflush(stdout);
        int result = read_line_jq(input, sizeof(input));
        if (result == 0)
            return false;
        if (result < 0)
            continue;
        char *text = trim_input_jq(input);
        if (strcmp(text, "quit") == 0 || strcmp(text, "q") == 0)
            return false;
        if (!parse_reveal_type_jq(text, revealed_type))
        {
            printf("Enter one type: A, B, N (or H), R, C or P; kings are not allowed.\n");
            continue;
        }
        if (pos->pool[pos->side_to_move][*revealed_type] == 0)
        {
            printf("That type is not available in the moving side's public pool.\n");
            continue;
        }
        return true;
    }
}

static void announce_move_jq(const XqJqPosition *before, XqMove move,
                             int revealed_type, const char *actor)
{
    char text[8];

    printf("%s %s plays: %s", color_name(before->side_to_move), actor,
           xq_jq_move_to_string(move, text, sizeof(text)));
    if (revealed_type != XQ_JQ_NO_REVEAL)
        printf("; reveals %c", xq_jq_piece_to_char(
                   xq_make_piece(before->side_to_move, (XqPieceType)revealed_type)));
    printf("\n");
}

static int run_jieqi(const char *fen, XqColor engine_color, XqJqParallelOptions options,
                     const CliSearchOutput *output, bool search_detail,
                     CliJqRevealMode reveal_mode)
{
    XqJqPosition pos;
    XqJqPosition initial_position;
    XqJqMoveList legal;
    XqJqHistory history;
    XqJqEngineAdapter engine = {0};
    XqJqHiddenState hidden = {0};
    const uint64_t default_time_limit_ms = options.time_limit_ms;
    bool board_flipped = false;
    FILE *search_log = NULL;
    int exit_status = EXIT_SUCCESS;
    char input[4096];

    if (fen != NULL)
    {
        if (!xq_jq_position_from_fen(&pos, fen))
        {
            fprintf(stderr, "error: invalid jieqi FEN: %s\n", fen);
            return EXIT_FAILURE;
        }
    }
    else
        xq_jq_position_startpos(&pos);
    initial_position = pos;
    if (!xq_jq_history_init(&history, &pos))
    {
        fprintf(stderr, "error: jieqi history allocation failed\n");
        return EXIT_FAILURE;
    }
    if (reveal_mode == CLI_JQ_REVEAL_RANDOM)
    {
        /* Real-game assignments have their own randomness. Do not copy this
         * state/map into search options or print it in public diagnostics. */
        struct timespec now;
        uint64_t real_random_state = UINT64_C(0x6a09e667f3bcc909);
        if (timespec_get(&now, TIME_UTC) == TIME_UTC)
        {
            real_random_state ^= (uint64_t)now.tv_sec;
            real_random_state ^= (uint64_t)now.tv_nsec << 32;
        }
        else
        {
            time_t calendar = time(NULL);
            if (calendar != (time_t)-1)
                real_random_state ^= (uint64_t)calendar;
        }
        clock_t process_time = clock();
        if (process_time != (clock_t)-1)
            real_random_state ^= (uint64_t)process_time;
        real_random_state = xq_jq_random_next(&real_random_state);
        if (!xq_jq_hidden_init(&hidden, &pos, &real_random_state))
        {
            fprintf(stderr, "error: cannot initialize real-game jieqi identities\n");
            xq_jq_history_destroy(&history);
            return EXIT_FAILURE;
        }
    }

    options.transposition_table = xq_jq_parallel_transposition_table_create();
    if (options.transposition_table == NULL)
        fprintf(stderr, "warning: jieqi transposition table allocation failed; continuing without cache\n");
    engine.history = &history;
    if (search_detail)
        search_log = open_search_detail_jq(&pos, engine_color, NULL, reveal_mode);

    printf("Jieqi demo: human %s vs parallel %s engine; reveal mode %s\n",
           color_name(xq_color_opponent(engine_color)), color_name(engine_color),
           reveal_mode == CLI_JQ_REVEAL_MANUAL ? "manual" : "random");
    printf("Search: iterative deepening to depth %u, threads %u, split strategy %s, split %u%%%s; ",
           options.depth, options.thread_count,
           options.split_strategy == XQ_JQ_PARALLEL_SPLIT_INTERLEAVED ? "interleaved" : "contiguous",
           options.split_percent,
           options.split_strategy == XQ_JQ_PARALLEL_SPLIT_INTERLEAVED ? " (ignored)" : "");
    if (default_time_limit_ms == 0)
        printf("no time limit by default; ");
    else
        printf("default time limit %" PRIu64 " ms; ", default_time_limit_ms);
    printf("in-memory cache %s.\n", options.transposition_table != NULL ? "enabled" : "disabled");
    printf("Use coordinates a-i and ranks 0-9. Dark pieces are shown as * until revealed.\n");
    printf("Last move: cyan background = from, yellow background = to.\n");
    print_help_jq(reveal_mode);

    for (;;)
    {
        const XqMove *last_move = history.count > 1 ? &history.entries[history.count - 1].move : NULL;
        xq_jq_position_print_with_last_move(&pos, board_flipped, last_move);
        xq_jq_generate_legal(&pos, &legal);
        if (legal.count == 0)
        {
            printf("%s has no legal moves and loses. %s wins.\n",
                   color_name(pos.side_to_move), color_name(xq_color_opponent(pos.side_to_move)));
            printf("Game over. Type undo to take back your last turn, or quit.\n");
        }

        if (legal.count > 0 && pos.side_to_move == engine_color)
        {
            XqJqParallelResult result;
            XqJqParallelStatus status = search_once_jq(
                &engine, &pos, &options, output, &search_log, &result);
            if (status != XQ_JQ_PARALLEL_OK)
            {
                fprintf(stderr, "error: %s\n", xq_jq_parallel_status_text(status));
                exit_status = EXIT_FAILURE;
                break;
            }
            if (!result.move_available)
            {
                fprintf(stderr, "error: jieqi engine failed to select a legal move\n");
                exit_status = EXIT_FAILURE;
                break;
            }
            int revealed_type = XQ_JQ_NO_REVEAL;
            if (xq_jq_position_is_dark(&pos, (XqSquare)result.best_move.from))
            {
                if (reveal_mode == CLI_JQ_REVEAL_MANUAL)
                {
                    char coordinate[8];
                    printf("%s engine selected: %s\n", color_name(engine_color),
                           xq_jq_move_to_string(result.best_move, coordinate, sizeof(coordinate)));
                    if (!read_engine_reveal_jq(&pos, result.best_move, &revealed_type))
                    {
                        printf("Engine move cancelled; no move played.\n");
                        break;
                    }
                }
                else
                {
                    XqPieceType type;
                    if (!xq_jq_hidden_get_type(&hidden, (XqSquare)result.best_move.from, &type))
                    {
                        fprintf(stderr, "error: cannot obtain the engine dark piece's real-game identity\n");
                        exit_status = EXIT_FAILURE;
                        break;
                    }
                    revealed_type = (int)type;
                }
            }
            XqJqPosition after = pos;
            if (!xq_jq_position_make_move(&after, result.best_move, revealed_type))
            {
                fprintf(stderr, "error: cannot apply the selected jieqi move; no move played\n");
                exit_status = EXIT_FAILURE;
                break;
            }
            if (!xq_jq_history_push(&history, result.best_move, revealed_type, &after))
            {
                fprintf(stderr, "error: history allocation failed; no move played; stopping game\n");
                exit_status = EXIT_FAILURE;
                break;
            }
            XqJqPosition before = pos;
            pos = after;
            announce_move_jq(&before, result.best_move, revealed_type, "engine");
            write_played_move_jq(&search_log, &before, &pos, result.best_move, revealed_type, "engine");
            options.time_limit_ms = default_time_limit_ms;
            continue;
        }

        if (legal.count == 0)
            printf("game over > ");
        else
            printf("%s to move > ", color_name(pos.side_to_move));
        fflush(stdout);
        int line_result = read_line_jq(input, sizeof(input));
        if (line_result == 0)
            break;
        if (line_result < 0)
            continue;
        char *command = trim_input_jq(input);
        if (*command == '\0')
            continue;
        if (strcmp(command, "quit") == 0 || strcmp(command, "q") == 0)
            break;
        if (strcmp(command, "help") == 0 || strcmp(command, "?") == 0)
        {
            print_help_jq(reveal_mode);
            continue;
        }
        if (strncmp(command, "save-tt", 7) == 0 &&
            (command[7] == '\0' || isspace((unsigned char)command[7])))
        {
            printf("Jieqi parallel search has no transposition table file support.\n");
            continue;
        }
        if (strcmp(command, "flip") == 0)
        {
            board_flipped = !board_flipped;
            printf("Board view: %s at bottom. Move coordinates are unchanged.\n",
                   board_flipped ? "black" : "red");
            continue;
        }
        if (strcmp(command, "undo") == 0 || strcmp(command, "u") == 0)
        {
            /* A human winning move has no engine reply to retract. */
            size_t plies = pos.side_to_move == engine_color ? 1 : 2;
            if (history.count <= plies)
            {
                printf("No player move to undo.\n");
                continue;
            }
            if (!undo_last_turn_jq(&pos, &initial_position, &history, plies))
            {
                fprintf(stderr, "error: cannot restore jieqi position; undo cancelled\n");
                continue;
            }
            options.time_limit_ms = default_time_limit_ms;
            printf("Undid %zu move(s). Your turn again.\n", plies);
            if (search_log != NULL)
            {
                char restored_fen[256];
                if (!xq_jq_position_to_fen(&pos, restored_fen, sizeof(restored_fen)))
                    strcpy(restored_fen, "unavailable");
                fprintf(search_log, "\n--- jieqi undo ---\nplies_undone: %zu\nfen_after: %s\n",
                        plies, restored_fen);
                write_pool_jq(search_log, &pos);
                flush_search_detail(&search_log);
            }
            continue;
        }
        if (strcmp(command, "moves") == 0)
        {
            print_legal_moves_jq(&legal);
            continue;
        }
        if (strcmp(command, "fen") == 0)
        {
            char visible_fen[256];
            if (xq_jq_position_to_fen(&pos, visible_fen, sizeof(visible_fen)))
                printf("%s\n", visible_fen);
            continue;
        }
        if (strcmp(command, "pool") == 0)
        {
            write_pool_jq(stdout, &pos);
            continue;
        }
        if (legal.count == 0)
        {
            printf("Game over. Type undo or quit.\n");
            continue;
        }

        {
            CliJqMoveInput parsed = {.time_limit_ms = default_time_limit_ms};
            const char *error = parse_move_input_jq(
                command, &pos, &legal, reveal_mode,
                reveal_mode == CLI_JQ_REVEAL_RANDOM ? &hidden : NULL, &parsed);
            size_t original_count = history.count;
            int applied;
            if (error != NULL)
            {
                printf("%s; no moves played.\n", error);
                printf("Type help for input formats or moves for legal first moves.\n");
                continue;
            }
            for (applied = 0; applied < parsed.count; ++applied)
                if (!xq_jq_history_push(&history, parsed.moves[applied],
                                         parsed.revealed_types[applied], &parsed.positions[applied]))
                    break;
            if (applied != parsed.count)
            {
                /* Roll back even when the first append succeeded and the second failed. */
                (void)xq_jq_history_truncate(&history, original_count);
                fprintf(stderr, "error: history allocation failed; no moves played; stopping game\n");
                exit_status = EXIT_FAILURE;
                break;
            }
            XqJqPosition before = pos;
            pos = parsed.positions[parsed.count - 1];
            for (int played = 0; played < parsed.count; ++played)
            {
                const XqJqPosition *step_before = played == 0 ? &before : &parsed.positions[played - 1];
                const char *actor = played == 0 ? "human" : "engine (specified by player)";
                const char *source = played == 0 ? "human" : "specified-engine";
                announce_move_jq(step_before, parsed.moves[played], parsed.revealed_types[played], actor);
                write_played_move_jq(&search_log, step_before, &parsed.positions[played],
                                      parsed.moves[played], parsed.revealed_types[played], source);
            }
            options.time_limit_ms = parsed.count == 1 ? parsed.time_limit_ms : default_time_limit_ms;
        }
    }

    xq_jq_parallel_transposition_table_destroy(options.transposition_table);
    xq_jq_history_destroy(&history);
    if (search_log != NULL && fclose(search_log) == EOF)
        fprintf(stderr, "warning: cannot close %s\n", search_detail_path);
    return exit_status;
}

int main(int argc, char **argv)
{
    XqPosition pos;
    XqPosition initial_position;
    XqMoveList legal;
    XqTranspositionTable *table;
    XqEngineAdapter engine;
    XqHistory history;
    XqSearchLimits default_limits = xq_search_limits_default();
    XqSearchLimits limits;
    bool parallel = false, explicit_depth = false, explicit_time = false;
    bool jieqi = false, explicit_search = false, explicit_reveal = false;
    CliJqRevealMode reveal_mode = CLI_JQ_REVEAL_MANUAL;
    bool parallel_options_set = false;
    XqParallelOptions parallel_options = xq_parallel_options_default();
    bool search_detail = false;
    CliSearchOutput search_output = {
        .sample_interval_ms = 20,
        .sample_output_dir = "build/search_samples",
    };
    bool timeline_limit_set = false;
    bool sample_options_set = false;
    bool board_flipped = false;
    FILE *search_log = NULL;
    int exit_status = EXIT_SUCCESS;
    const char *fen = NULL;
    const char *fen_file = NULL;
    const char *tt_file = NULL;
    XqColor engine_color = XQ_BLACK;
    char input[4096];
    int i;

    for (i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0)
        {
            print_usage(argv[0]);
            return EXIT_SUCCESS;
        }
        if (strcmp(argv[i], "--game") == 0)
        {
            if (++i >= argc || (strcmp(argv[i], "xiangqi") != 0 && strcmp(argv[i], "jieqi") != 0))
            {
                fprintf(stderr, "error: --game requires xiangqi or jieqi\n");
                return EXIT_FAILURE;
            }
            jieqi = strcmp(argv[i], "jieqi") == 0;
            continue;
        }
        if (strcmp(argv[i], "--reveal") == 0)
        {
            if (++i >= argc || (strcmp(argv[i], "manual") != 0 && strcmp(argv[i], "random") != 0))
            {
                fprintf(stderr, "error: --reveal requires manual or random\n");
                return EXIT_FAILURE;
            }
            reveal_mode = strcmp(argv[i], "random") == 0 ? CLI_JQ_REVEAL_RANDOM : CLI_JQ_REVEAL_MANUAL;
            explicit_reveal = true;
            continue;
        }
        if (strcmp(argv[i], "--search") == 0)
        {
            if (++i >= argc || (strcmp(argv[i], "builtin") != 0 && strcmp(argv[i], "parallel") != 0))
            {
                fprintf(stderr, "error: --search requires builtin or parallel\n");
                return EXIT_FAILURE;
            }
            parallel = strcmp(argv[i], "parallel") == 0;
            explicit_search = true;
            continue;
        }
        if (strcmp(argv[i], "--split-strategy") == 0)
        {
            if (++i >= argc ||
                (strcmp(argv[i], "contiguous") != 0 && strcmp(argv[i], "interleaved") != 0))
            {
                fprintf(stderr, "error: --split-strategy requires contiguous or interleaved\n");
                return EXIT_FAILURE;
            }
            parallel_options.split_strategy = strcmp(argv[i], "interleaved") == 0 ?
                XQ_PARALLEL_SPLIT_INTERLEAVED : XQ_PARALLEL_SPLIT_CONTIGUOUS;
            parallel_options_set = true;
            continue;
        }
        if (strcmp(argv[i], "--threads") == 0 || strcmp(argv[i], "--split-percent") == 0)
        {
            bool percentage = strcmp(argv[i], "--split-percent") == 0;
            unsigned value = 0;
            if (++i >= argc ||
                (!(percentage && strcmp(argv[i], "0") == 0) && !parse_depth(argv[i], &value)))
            {
                fprintf(stderr, "error: invalid parallel numeric option\n");
                return EXIT_FAILURE;
            }
            if (percentage && strcmp(argv[i], "0") == 0) value = 0;
            if (percentage && value > 100)
            {
                fprintf(stderr, "error: --split-percent must be 0..100\n");
                return EXIT_FAILURE;
            }
            if (percentage) parallel_options.split_percent = value;
            else parallel_options.thread_count = value;
            parallel_options_set = true;
            continue;
        }
        if (strcmp(argv[i], "--progress") == 0)
        {
            search_output.show_progress = true;
            continue;
        }
        if (strcmp(argv[i], "--sample-search") == 0)
        {
            search_output.sample_search = true;
            continue;
        }
        if (strcmp(argv[i], "--sample-timeline") == 0)
        {
            search_output.sample_timeline = search_output.sample_search = true;
            continue;
        }
        if (strcmp(argv[i], "--sample-timeline-max-mib") == 0)
        {
            uint64_t mib;
            if (++i >= argc || !parse_unsigned_limit(argv[i], &mib) || mib > UINT64_MAX / (1024u * 1024u))
            {
                fprintf(stderr, "error: --sample-timeline-max-mib requires nonnegative integer MiB (0 = unlimited)\n");
                return EXIT_FAILURE;
            }
            search_output.timeline_byte_limit = mib * 1024u * 1024u;
            timeline_limit_set = true;
            continue;
        }
        if (strcmp(argv[i], "--sample-statistics-max-mib") == 0)
        {
            uint64_t mib;
            if (++i >= argc || !parse_unsigned_limit(argv[i], &mib) || mib > SIZE_MAX / (1024u * 1024u))
            {
                fprintf(stderr, "error: --sample-statistics-max-mib requires nonnegative integer MiB (0 = unlimited)\n");
                return EXIT_FAILURE;
            }
            search_output.statistics_byte_limit = (size_t)(mib * 1024u * 1024u);
            sample_options_set = true;
            continue;
        }
        if (strcmp(argv[i], "--sample-interval-ms") == 0)
        {
            if (++i >= argc || !parse_time_ms(argv[i], &search_output.sample_interval_ms))
            {
                fprintf(stderr, "error: --sample-interval-ms requires positive integer milliseconds\n");
                return EXIT_FAILURE;
            }
            sample_options_set = true;
            continue;
        }
        if (strcmp(argv[i], "--sample-output-dir") == 0)
        {
            if (++i >= argc || argv[i][0] == '\0')
            {
                fprintf(stderr, "error: --sample-output-dir requires a nonempty directory\n");
                return EXIT_FAILURE;
            }
            search_output.sample_output_dir = argv[i];
            sample_options_set = true;
            continue;
        }
        if (strcmp(argv[i], "--search-detail") == 0)
        {
            search_detail = true;
            continue;
        }
        if (strcmp(argv[i], "--tt-file") == 0)
        {
            if (++i >= argc || argv[i][0] == '\0')
            {
                fprintf(stderr, "error: --tt-file requires a nonempty file path\n");
                print_usage(argv[0]);
                return EXIT_FAILURE;
            }
            tt_file = argv[i];
            continue;
        }
        if (strcmp(argv[i], "--depth") == 0)
        {
            explicit_depth = true;
            if (++i >= argc)
            {
                fprintf(stderr, "error: --depth requires a positive integer depth argument\n");
                print_usage(argv[0]);
                return EXIT_FAILURE;
            }
            if (!parse_depth(argv[i], &default_limits.max_depth))
            {
                fprintf(stderr, "error: invalid search depth: %s (expected an integer from 1 to %d)\n",
                        argv[i], INT_MAX);
                return EXIT_FAILURE;
            }
            continue;
        }
        if (strcmp(argv[i], "--time-ms") == 0)
        {
            explicit_time = true;
            if (++i >= argc)
            {
                fprintf(stderr, "error: --time-ms requires a positive integer millisecond argument\n");
                print_usage(argv[0]);
                return EXIT_FAILURE;
            }
            if (!parse_time_ms(argv[i], &default_limits.time_limit_ms))
            {
                fprintf(stderr, "error: invalid thinking time: %s (expected positive integer milliseconds)\n", argv[i]);
                return EXIT_FAILURE;
            }
            continue;
        }
        if (strcmp(argv[i], "--fen-file") == 0)
        {
            if (++i >= argc || argv[i][0] == '\0')
            {
                fprintf(stderr, "error: --fen-file requires a nonempty file path\n");
                return EXIT_FAILURE;
            }
            fen_file = argv[i];
            continue;
        }
        if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--fen") == 0)
        {
            if (++i >= argc)
            {
                fprintf(stderr, "error: %s requires a FEN argument\n", argv[i - 1]);
                print_usage(argv[0]);
                return EXIT_FAILURE;
            }
            fen = argv[i];
            continue;
        }
        if (strcmp(argv[i], "-e") == 0 || strcmp(argv[i], "--engine") == 0)
        {
            if (++i >= argc)
            {
                fprintf(stderr, "error: %s requires red or black\n", argv[i - 1]);
                print_usage(argv[0]);
                return EXIT_FAILURE;
            }
            if (!parse_color(argv[i], &engine_color))
            {
                fprintf(stderr, "error: invalid engine color: %s (expected red or black)\n", argv[i]);
                return EXIT_FAILURE;
            }
            continue;
        }

        fprintf(stderr, "error: unknown argument: %s\n", argv[i]);
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (jieqi)
    {
        if (explicit_search && !parallel)
        {
            fprintf(stderr, "error: jieqi supports only --search parallel\n");
            return EXIT_FAILURE;
        }
        if (tt_file != NULL)
        {
            fprintf(stderr, "error: jieqi does not support --tt-file\n");
            return EXIT_FAILURE;
        }
        if (search_output.sample_search || search_output.sample_timeline ||
            timeline_limit_set || sample_options_set)
        {
            fprintf(stderr, "error: jieqi does not support --sample-* options\n");
            return EXIT_FAILURE;
        }
        parallel = true;
    }
    else if (explicit_reveal)
    {
        fprintf(stderr, "error: --reveal requires --game jieqi\n");
        return EXIT_FAILURE;
    }
    if (fen_file != NULL && (fen != NULL || tt_file != NULL))
    {
        fprintf(stderr, "error: --fen-file cannot be combined with --fen or --tt-file\n");
        return EXIT_FAILURE;
    }
    if (timeline_limit_set && !search_output.sample_timeline)
    {
        fprintf(stderr, "error: --sample-timeline-max-mib requires --sample-timeline\n");
        return EXIT_FAILURE;
    }
    if (sample_options_set && !search_output.sample_search)
    {
        fprintf(stderr, "error: sampling options require --sample-search\n");
        return EXIT_FAILURE;
    }
    if ((!parallel && parallel_options_set) || (parallel && tt_file != NULL))
    {
        fprintf(stderr, "error: parallel options require --search parallel; parallel search does not accept TT files\n");
        return EXIT_FAILURE;
    }
#ifdef _WIN32
    if (parallel)
    {
        fprintf(stderr, "error: %s\n", xq_parallel_status_text(XQ_PARALLEL_UNSUPPORTED));
        return EXIT_FAILURE;
    }
#endif
    if (fen_file != NULL && !explicit_time)
        default_limits.time_limit_ms = 0;
    if (parallel)
    {
        if (explicit_depth) parallel_options.depth = default_limits.max_depth;
        if (!explicit_time && fen_file == NULL)
            default_limits.time_limit_ms = parallel_options.time_limit_ms;
        parallel_options.time_limit_ms = default_limits.time_limit_ms;
    }
    if (jieqi)
    {
        XqJqParallelOptions jq_options = xq_jq_parallel_options_default();

        jq_options.depth = parallel_options.depth;
        jq_options.thread_count = parallel_options.thread_count;
        jq_options.split_percent = parallel_options.split_percent;
        jq_options.split_strategy = parallel_options.split_strategy == XQ_PARALLEL_SPLIT_INTERLEAVED
                                        ? XQ_JQ_PARALLEL_SPLIT_INTERLEAVED
                                        : XQ_JQ_PARALLEL_SPLIT_CONTIGUOUS;
        jq_options.time_limit_ms = parallel_options.time_limit_ms;
        if (fen_file != NULL)
            return run_fen_batch_jq(fen_file, jq_options, &search_output, search_detail, reveal_mode);
        return run_jieqi(fen, engine_color, jq_options, &search_output, search_detail, reveal_mode);
    }
    if (fen_file != NULL)
        return run_fen_batch(fen_file, parallel, &default_limits, parallel_options,
                              &search_output, search_detail);
    limits = default_limits;
    if (fen != NULL)
    {
        if (!xq_position_from_fen(&pos, fen))
        {
            fprintf(stderr, "error: invalid FEN: %s\n", fen);
            return EXIT_FAILURE;
        }
    }
    else
        xq_position_startpos(&pos);

    initial_position = pos;
    if (!xq_history_init(&history, &pos))
    {
        fprintf(stderr, "error: history allocation failed\n");
        return EXIT_FAILURE;
    }
    table = parallel ? NULL : xq_transposition_table_create();
    if (tt_file != NULL)
    {
        XqTranspositionIoStatus status = table == NULL ? XQ_TT_IO_NO_MEMORY
                                                      : xq_transposition_table_load(table, tt_file);
        if (status != XQ_TT_IO_OK)
        {
            fprintf(stderr, "error: cannot load transposition table '%s': %s\n",
                    tt_file, tt_io_status_text(status));
            xq_transposition_table_destroy(table);
            xq_history_destroy(&history);
            return EXIT_FAILURE;
        }
        printf("Loaded transposition table from: %s\n", tt_file);
    }
    if (parallel)
    {
        parallel_options.transposition_table = xq_parallel_transposition_table_create();
        if (parallel_options.transposition_table == NULL)
            fprintf(stderr, "warning: parallel transposition table allocation failed; continuing without cache\n");
    }
    engine.static_evaluate = NULL;
    engine.user = NULL;
    engine.score_move = NULL;
    engine.transposition_table = table;
    engine.history = &history;

    if (!parallel && table == NULL)
        fprintf(stderr, "warning: transposition table allocation failed; continuing without cache\n");

    if (search_detail)
        search_log = open_search_detail(&pos, engine_color, NULL);
    if (parallel)
    {
        printf("Search mode: parallel, iterative deepening to max depth %u, threads %u, split strategy %s, split %u%%%s; ",
               parallel_options.depth, parallel_options.thread_count,
               split_strategy_name(parallel_options.split_strategy), parallel_options.split_percent,
               parallel_options.split_strategy == XQ_PARALLEL_SPLIT_INTERLEAVED ?
                   " (ignored for interleaved)" : "");
        if (parallel_options.time_limit_ms == 0)
            printf("no time limit by default; ");
        else
            printf("default time limit %" PRIu64 " ms; ", parallel_options.time_limit_ms);
        printf("in-memory cache %s.\n",
               parallel_options.transposition_table != NULL ? "enabled" : "disabled");
    }

    printf("Xiangqi demo: human %s vs %s %s engine\n",
           color_name(xq_color_opponent(engine_color)), parallel ? "parallel" : "builtin",
           color_name(engine_color));
    printf("Use coordinates a-i and ranks 0-9. Example: b2b9\n");
    printf("Last move: cyan background = from, yellow background = to.\n");
    print_help(parallel);

    for (;;)
    {
        const XqMove *last_move = history.count > 1 ? &history.entries[history.count - 1].move : NULL;
        xq_position_print_with_last_move(&pos, board_flipped, last_move);
        xq_generate_legal(&pos, &legal);

        if (legal.count == 0)
        {
            printf("%s has no legal moves and loses. %s wins.\n",
                   pos.side_to_move == XQ_RED ? "red" : "black",
                   pos.side_to_move == XQ_RED ? "black" : "red");
            printf("Game over. Type undo to take back your last turn, or quit.\n");
        }

        if (legal.count > 0 && pos.side_to_move == engine_color)
        {
            CliSearchResult search = search_once(&engine, &pos, &limits, parallel,
                                                  &parallel_options, &search_output,
                                                  &search_log, false);
            char text[8];

            if (search.status != XQ_PARALLEL_OK)
            {
                fprintf(stderr, "error: %s\n", xq_parallel_status_text(search.status));
                exit_status = EXIT_FAILURE;
            }
            if (!search.found)
            {
                printf("engine failed to move\n");
                break;
            }
            printf("%s engine plays: %s\n", color_name(engine_color),
                   xq_move_to_string(search.best, text, sizeof(text)));
            xq_position_make_move(&pos, search.best);
            if (!xq_history_push(&history, search.best, &pos))
            {
                fprintf(stderr, "error: history allocation failed; stopping game\n");
                exit_status = EXIT_FAILURE;
                break;
            }
            continue;
        }

        if (legal.count == 0)
            printf("game over > ");
        else
            printf("%s to move > ", color_name(pos.side_to_move));
        fflush(stdout);
        if (fgets(input, sizeof(input), stdin) == NULL)
            break;
        if (strchr(input, '\n') == NULL && !feof(stdin))
        {
            int ch = getchar();
            if (ch != '\n' && ch != EOF)
            {
                while ((ch = getchar()) != '\n' && ch != EOF)
                    ;
                printf("input too long (maximum %zu bytes); command ignored.\n", sizeof(input) - 1);
                continue;
            }
        }
        input[strcspn(input, "\r\n")] = '\0';

        if (strcmp(input, "quit") == 0 || strcmp(input, "q") == 0)
            break;
        if (strcmp(input, "help") == 0 || strcmp(input, "?") == 0)
        {
            print_help(parallel);
            continue;
        }
        if (strncmp(input, "save-tt", 7) == 0 &&
            (input[7] == '\0' || isspace((unsigned char)input[7])))
        {
            char *path;
            XqTranspositionIoStatus status;
            if (parallel)
            {
                printf("Parallel search has no transposition table file support.\n");
                continue;
            }
            path = parse_save_path(input + 7);
            if (path == NULL)
            {
                printf("usage: save-tt PATH (nonempty path; optional matching outer quotes)\n");
                continue;
            }
            if (table == NULL)
            {
                fprintf(stderr, "error: no transposition table is available to save\n");
                continue;
            }
            status = xq_transposition_table_save(table, path);
            if (status != XQ_TT_IO_OK)
                fprintf(stderr, "error: cannot save transposition table '%s': %s\n",
                        path, tt_io_status_text(status));
            else
                printf("Saved transposition table to: %s\n", path);
            continue;
        }
        if (strcmp(input, "flip") == 0)
        {
            board_flipped = !board_flipped;
            printf("Board view: %s at bottom. Move coordinates are unchanged.\n",
                   board_flipped ? "black" : "red");
            continue;
        }
        if (strcmp(input, "undo") == 0 || strcmp(input, "u") == 0)
        {
            /* A human winning move has no engine reply to retract. */
            size_t plies = pos.side_to_move == engine_color ? 1 : 2;
            if (history.count <= plies)
            {
                printf("No player move to undo.\n");
                continue;
            }
            if (!undo_last_turn(&pos, &initial_position, &history, plies))
            {
                fprintf(stderr, "error: cannot restore position; undo cancelled\n");
                continue;
            }
            limits = default_limits;
            printf("Undid %zu move(s). Your turn again.\n", plies);
            if (search_log != NULL)
            {
                char restored_fen[128];
                if (!xq_position_to_fen(&pos, restored_fen, sizeof(restored_fen)))
                    strcpy(restored_fen, "unavailable");
                fprintf(search_log, "\n--- undo ---\nplies_undone: %zu\nfen_after: %s\n",
                        plies, restored_fen);
                flush_search_detail(&search_log);
            }
            continue;
        }
        if (strcmp(input, "moves") == 0)
        {
            print_legal_moves(&legal);
            continue;
        }
        if (strcmp(input, "fen") == 0)
        {
            char fen[128];
            if (xq_position_to_fen(&pos, fen, sizeof(fen)))
                printf("%s\n", fen);
            continue;
        }

        if (legal.count == 0)
        {
            printf("Game over. Type undo or quit.\n");
            continue;
        }

        {
            CliMoveInput parsed = {.time_limit_ms = default_limits.time_limit_ms};
            const char *error = parse_move_input(input, &pos, &legal, &parsed);
            size_t original_count = history.count;
            int applied;

            if (error != NULL)
            {
                printf("%s; no moves played.\n", error);
                printf("Use a0a1, a0a1 2000, or a0a1 a9a8; type moves for legal first moves.\n");
                continue;
            }
            for (applied = 0; applied < parsed.count; ++applied)
                if (!xq_history_push(&history, parsed.moves[applied], &parsed.positions[applied]))
                    break;
            if (applied != parsed.count)
            {
                /* Keep the original board and roll back even if only the second append failed. */
                (void)xq_history_truncate(&history, original_count);
                fprintf(stderr, "error: history allocation failed; stopping game\n");
                exit_status = EXIT_FAILURE;
                break;
            }
            pos = parsed.positions[parsed.count - 1];
            if (parsed.count == 2)
            {
                char text[8];
                printf("%s engine plays (specified by player): %s\n", color_name(engine_color),
                       xq_move_to_string(parsed.moves[1], text, sizeof(text)));
                if (search_log != NULL)
                    write_manual_engine_move(&search_log, &parsed.positions[0], &parsed.positions[1],
                                             parsed.moves[1]);
            }
            else
                limits.time_limit_ms = parsed.time_limit_ms;
        }
    }

    xq_transposition_table_destroy(table);
    xq_parallel_transposition_table_destroy(parallel_options.transposition_table);
    xq_history_destroy(&history);
    if (search_log != NULL && fclose(search_log) == EOF)
        fprintf(stderr, "warning: cannot close %s\n", search_detail_path);
    return exit_status;
}
