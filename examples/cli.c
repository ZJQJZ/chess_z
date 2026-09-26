#include "xiangqi/engine.h"
#include "xiangqi/movegen.h"
#include "xiangqi/position.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
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
    printf("usage: %s [--fen FEN] [--engine red|black] [--time-ms MS] [--tt-file PATH] [--search-detail]\n", program);
    printf("\n");
    printf("options:\n");
    printf("  -f, --fen FEN           initialize the position from FEN\n");
    printf("  -e, --engine COLOR      choose the engine side: red or black\n");
    printf("      --time-ms MS        default thinking time per engine move (positive integer milliseconds)\n");
    printf("      --tt-file PATH      load a saved transposition table before the first search\n");
    printf("      --search-detail     append each engine search to build/search_detail.txt\n");
    printf("  -h, --help              show this help\n");
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
static bool parse_move_text(const char *text, XqMoveList *legal, XqMove *move)
{
    int from_file;
    int from_rank;
    int to_file;
    int to_rank;
    XqSquare from;
    XqSquare to;
    int i;

    if (strlen(text) < 4)
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

/* Parse an optional positive millisecond limit for the next engine reply only. */
static bool parse_move_input(const char *text, XqMoveList *legal, XqMove *move,
                             uint64_t *time_limit_ms)
{
    while (isspace((unsigned char)*text))
        ++text;
    if (strlen(text) < 4 || !parse_move_text(text, legal, move))
        return false;
    text += 4;
    if (*text != '\0' && !isspace((unsigned char)*text))
        return false;
    while (isspace((unsigned char)*text))
        ++text;
    return *text == '\0' || parse_time_ms(text, time_limit_ms);
}

/**
 * help
 */
static void print_help(void)
{
    printf("commands:\n");
    printf("  a0a1 [time_ms]  move; optional positive integer time limit for the next engine reply\n");
    printf("                  e.g. a0a1 2000 (2 seconds); omit time to use the default\n");
    printf("  fen   print current FEN\n");
    printf("  moves print legal moves\n");
    printf("  undo  take back your last move and the engine reply (alias: u)\n");
    printf("  flip  rotate the board display 180 degrees; move coordinates stay unchanged\n");
    printf("  save-tt PATH  save the current transposition table (paths may contain spaces)\n");
    printf("  quit  exit\n");
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
    fprintf(out, "depth_limit: %u\ntime_limit_ms: %" PRIu64 "\ntime_mode: %s\ndepth_bonus: %d\n",
            limits->max_depth, limits->time_limit_ms,
            limits->time_mode == XQ_SEARCH_TIME_CPU ? "cpu" : "monotonic", limits->depth_bonus);
    fprintf(out, "stats_available: %s\n", stats->available ? "yes" : "no");
    if (stats->available)
        fprintf(out, "max_started_depth: %u\ncompleted_depth: %u\nselected_move_depth: %u\n"
                     "nodes: %" PRIu64 "\nstopped: %s\n",
                stats->max_started_depth, stats->completed_depth, stats->selected_move_depth,
                stats->nodes, stats->stopped ? "yes" : "no");
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
        if (strcmp(argv[i], "--time-ms") == 0)
        {
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
    table = xq_transposition_table_create();
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

    if (table == NULL)
        fprintf(stderr, "warning: transposition table allocation failed; continuing without cache\n");

    if (search_detail)
        search_log = open_search_detail(&pos, engine_color);

    printf("Xiangqi demo: human %s vs builtin %s engine\n",
           color_name(xq_color_opponent(engine_color)), color_name(engine_color));
    printf("Use coordinates a-i and ranks 0-9. Example: b2b9\n");
    print_help();

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
            found = xq_engine_find_best_move_with_stats(&engine, &pos, &limits, &best,
                                                       search_log != NULL ? &stats : NULL);
            if (search_log != NULL)
                write_search_detail(&search_log, &pos, &limits, found, best, &stats);
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
            print_help();
            continue;
        }
        if (strncmp(input, "save-tt", 7) == 0 &&
            (input[7] == '\0' || isspace((unsigned char)input[7])))
        {
            char *path = parse_save_path(input + 7);
            XqTranspositionIoStatus status;
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
            XqMove move;
            uint64_t time_limit_ms = default_limits.time_limit_ms;
            if (!parse_move_input(input, &legal, &move, &time_limit_ms))
            {
                printf("illegal move or bad format. Use b2b9 [time_ms] (positive integer milliseconds), or type moves.\n");
                continue;
            }
            limits.time_limit_ms = time_limit_ms;
            xq_position_make_move(&pos, move);
            if (!xq_history_push(&history, move, &pos))
            {
                fprintf(stderr, "error: history allocation failed; stopping game\n");
                exit_status = EXIT_FAILURE;
                break;
            }
        }
    }

    xq_transposition_table_destroy(table);
    xq_history_destroy(&history);
    if (search_log != NULL && fclose(search_log) == EOF)
        fprintf(stderr, "warning: cannot close %s\n", search_detail_path);
    return exit_status;
}
