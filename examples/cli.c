#include "xiangqi/engine.h"
#include "xiangqi/parallel_search.h"
#include "xiangqi/movegen.h"
#include "xiangqi/position.h"

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

/**
 * 打印命令行程序的用法、可用选项以及 FEN 参数的输入提示。
 *
 * @param program 命令行程序名称，通常传入 argv[0]。
 */
static void print_usage(const char *program)
{
    printf("usage: %s [--fen FEN] [--engine red|black] [--depth N] [--time-ms MS] [--tt-file PATH] [--search-detail]\n", program);
    printf("\n");
    printf("options:\n");
    printf("  --search builtin|parallel  search implementation (default builtin)\n");
    printf("  --threads N                parallel threads including caller (default 2)\n");
    printf("  --split-percent P          parallel batch percentage, 0..100 (default 50)\n");
    printf("  --parallel-trace PATH      append parallel boundary events\n");
    printf("  parallel: fixed depth (default 3), no time limit or transposition table\n");
    printf("  %-21s  %s\n", "-f, --fen FEN", "initialize the position from FEN");
    printf("  %-21s  %s\n", "-e, --engine COLOR", "choose the engine side: red or black");
    printf("  %-21s  %s\n", "    --depth N", "search depth in plies (builtin default 10; parallel fixed default 3)");
    printf("  %-21s  %s\n", "    --time-ms MS", "default thinking time per engine move (positive integer milliseconds)");
    printf("  %-21s  %s\n", "", "a nearly complete iteration may finish after this budget");
    printf("  %-21s  %s\n", "    --tt-file PATH", "load a saved transposition table before the first search");
    printf("  %-21s  %s\n", "    --search-detail", "append each engine search to build/search_detail.txt");
    printf("  %-21s  %s\n", "-h, --help", "show this help");
    printf("\n");
    printf("FEN strings containing spaces must be quoted.\n");
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

/* Parse a positive millisecond limit, rejecting overflow and extra arguments. */
static bool parse_time_ms(const char *text, uint64_t *time_limit_ms)
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
    if (*text != '\0' || value == 0)
        return false;
    *time_limit_ms = value;
    return true;
}

typedef struct CliMoveInput
{
    XqMove moves[2];
    XqPosition positions[2]; /* Position after each validated move. */
    int count;
    uint64_t time_limit_ms;
    bool explicit_time;
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
        parsed->explicit_time = true;
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
    if (parallel)
        printf("  a0a1            move; parallel search uses fixed depth, without time_ms\n");
    else
    {
        printf("  a0a1 [time_ms]  move; optional positive integer time limit for the next engine reply\n");
        printf("                  e.g. a0a1 2000 (2 seconds); omit time to use the default\n");
    }
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

static FILE *open_search_detail(const XqPosition *pos, XqColor engine_color)
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
    if (!xq_position_to_fen(pos, fen, sizeof(fen)))
        strcpy(fen, "unavailable");
    fprintf(log, "\n=== new game ===\ntimestamp: %s\ninitial_fen: %s\nengine_color: %s\n",
            timestamp, fen, color_name(engine_color));
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
    fprintf(out, "\n--- engine search ---\nfullmove_number: %u\nside_to_move: %s\n"
                 "fen_before: %s\nsuccess: %s\nselected_move: %s\n",
            (unsigned)pos->fullmove_number, color_name(pos->side_to_move), fen,
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
    fprintf(*log, "\n--- manual engine move ---\nfullmove_number: %u\nengine_color: %s\n"
                  "selected_move: %s\nfen_before: %s\nfen_after: %s\n",
            (unsigned)before->fullmove_number, color_name(before->side_to_move),
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

/* Event callbacks are serialized by the parallel search mutex. */
typedef struct ParallelLog
{
    FILE *file;
    uint64_t search_id;
} ParallelLog;

static void write_parallel_event(const XqParallelEvent *event, void *user)
{
    ParallelLog *log = user;
    char move[8];
    if (log->file == NULL)
        return;
    fprintf(log->file,
            "search=%" PRIu64 " event=%" PRIu64 " kind=%s node=%" PRIu64
            " parent=%" PRIu64 " worker=%u depth=%u type=%s move=%s"
            " lower=%d upper=%d alpha=%d beta=%d alpha_source=%" PRIu64
            " beta_source=%" PRIu64 "\n",
            log->search_id, event->sequence, event->kind, event->node_id,
            event->parent_id, event->worker_id, event->depth,
            event->maximizing ? "MAX" : "MIN",
            event->move_available ? xq_move_to_string(event->move, move, sizeof(move)) : "none",
            event->lower, event->upper, event->alpha, event->beta,
            event->alpha_source, event->beta_source);
    if (ferror(log->file))
    {
        fprintf(stderr, "warning: parallel trace write failed; tracing disabled\n");
        fclose(log->file);
        log->file = NULL;
    }
}

static void write_parallel_detail(FILE **log, const XqPosition *pos,
                                  const XqParallelOptions *options,
                                  XqParallelStatus status, const XqParallelResult *result)
{
    char fen[128], move[8];
    if (!xq_position_to_fen(pos, fen, sizeof(fen)))
        strcpy(fen, "unavailable");
    fprintf(*log, "\n--- parallel engine search ---\nsearch_mode: parallel\n"
            "fen_before: %s\nstatus: %s\nselected_move: %s\nscore: %d\n"
            "depth: %u\nthreads: %u\nsplit_percent: %u\nnodes: %" PRIu64
            "\nsplits: %" PRIu64 "\nbound_updates: %" PRIu64
            "\ncancelled_tasks: %" PRIu64 "\n",
            fen, xq_parallel_status_text(status),
            result->move_available ? xq_move_to_string(result->best_move, move, sizeof(move)) : "none",
            result->score, options->depth, options->thread_count, options->split_percent,
            result->nodes, result->splits, result->bound_updates, result->cancelled_tasks);
    if (result->elapsed_available)
        fprintf(*log, "elapsed_ms: %" PRIu64 "\n", result->elapsed_ms);
    else
        fprintf(*log, "elapsed_ms: unavailable\n");
    flush_search_detail(log);
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
    bool parallel_options_set = false;
    XqParallelOptions parallel_options = xq_parallel_options_default();
    ParallelLog parallel_log = {0};
    const char *parallel_trace_path = NULL;
    bool search_detail = false;
    bool board_flipped = false;
    FILE *search_log = NULL;
    int exit_status = EXIT_SUCCESS;
    const char *fen = NULL;
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
        if (strcmp(argv[i], "--search") == 0)
        {
            if (++i >= argc || (strcmp(argv[i], "builtin") != 0 && strcmp(argv[i], "parallel") != 0))
            {
                fprintf(stderr, "error: --search requires builtin or parallel\n");
                return EXIT_FAILURE;
            }
            parallel = strcmp(argv[i], "parallel") == 0;
            continue;
        }
        if (strcmp(argv[i], "--threads") == 0 || strcmp(argv[i], "--split-percent") == 0)
        {
            bool percentage = strcmp(argv[i], "--split-percent") == 0;
            unsigned value;
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
        if (strcmp(argv[i], "--parallel-trace") == 0)
        {
            if (++i >= argc || argv[i][0] == '\0')
            {
                fprintf(stderr, "error: --parallel-trace requires a path\n");
                return EXIT_FAILURE;
            }
            parallel_trace_path = argv[i];
            parallel_options_set = true;
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

    if ((!parallel && parallel_options_set) || (parallel && (explicit_time || tt_file != NULL)))
    {
        fprintf(stderr, "error: parallel options require --search parallel; parallel search has no time limit or TT\n");
        return EXIT_FAILURE;
    }
#ifdef _WIN32
    if (parallel)
    {
        fprintf(stderr, "error: %s\n", xq_parallel_status_text(XQ_PARALLEL_UNSUPPORTED));
        return EXIT_FAILURE;
    }
#endif
    if (parallel && explicit_depth) parallel_options.depth = default_limits.max_depth;
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
    engine.static_evaluate = NULL;
    engine.search = NULL;
    engine.user = NULL;
    engine.score_move = NULL;
    engine.transposition_table = table;
    engine.history = &history;

    if (!parallel && table == NULL)
        fprintf(stderr, "warning: transposition table allocation failed; continuing without cache\n");

    if (search_detail)
        search_log = open_search_detail(&pos, engine_color);
    if (parallel_trace_path != NULL)
    {
        parallel_log.file = fopen(parallel_trace_path, "a");
        if (parallel_log.file == NULL)
            fprintf(stderr, "warning: cannot open parallel trace: %s; tracing disabled\n", strerror(errno));
        else
        {
            fprintf(parallel_log.file, "\n=== parallel trace session ===\n");
            parallel_options.trace = write_parallel_event;
            parallel_options.trace_user = &parallel_log;
        }
    }
    if (parallel)
        printf("Search mode: parallel, fixed depth %u, threads %u, split %u%%; no time limit or cache.\n",
               parallel_options.depth, parallel_options.thread_count, parallel_options.split_percent);

    printf("Xiangqi demo: human %s vs %s %s engine\n",
           color_name(xq_color_opponent(engine_color)), parallel ? "parallel" : "builtin",
           color_name(engine_color));
    printf("Use coordinates a-i and ranks 0-9. Example: b2b9\n");
    print_help(parallel);

    for (;;)
    {
        xq_position_print_oriented(&pos, board_flipped);
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
            XqMove best = {0};
            XqSearchStats stats;
            bool found;
            char text[8];
            if (parallel)
            {
                XqParallelResult result;
                XqParallelStatus status;
                ++parallel_log.search_id;
                status = xq_engine_parallel_search(&pos, &parallel_options, NULL, NULL, &result);
                found = status == XQ_PARALLEL_OK && result.move_available;
                best = result.best_move;
                if (status != XQ_PARALLEL_OK)
                {
                    fprintf(stderr, "error: %s\n", xq_parallel_status_text(status));
                    exit_status = EXIT_FAILURE;
                }
                if (search_log != NULL)
                    write_parallel_detail(&search_log, &pos, &parallel_options, status, &result);
                if (parallel_log.file != NULL && fflush(parallel_log.file) != 0)
                {
                    fprintf(stderr, "warning: parallel trace flush failed; tracing disabled\n");
                    fclose(parallel_log.file);
                    parallel_log.file = NULL;
                }
            }
            else
            {
                found = xq_engine_find_best_move_with_stats(&engine, &pos, &limits, &best,
                                                           search_log != NULL ? &stats : NULL);
                if (search_log != NULL)
                    write_search_detail(&search_log, &pos, &limits, found, best, &stats);
            }
            if (!found)
            {
                printf("engine failed to move\n");
                break;
            }
            printf("%s engine plays: %s\n", color_name(engine_color),
                   xq_move_to_string(best, text, sizeof(text)));
            xq_position_make_move(&pos, best);
            if (!xq_history_push(&history, best, &pos))
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
                printf("Parallel search has no transposition table to save.\n");
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
            if (error == NULL && parallel && parsed.explicit_time)
                error = "parallel search has no time limit; enter a move without time_ms";
            int applied;

            if (error != NULL)
            {
                printf("%s; no moves played.\n", error);
                if (parallel)
                    printf("Use a0a1 or a0a1 a9a8; type moves for legal first moves.\n");
                else
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
    xq_history_destroy(&history);
    if (parallel_log.file != NULL && fclose(parallel_log.file) != 0)
        fprintf(stderr, "warning: cannot close parallel trace\n");
    if (search_log != NULL && fclose(search_log) == EOF)
        fprintf(stderr, "warning: cannot close %s\n", search_detail_path);
    return exit_status;
}
