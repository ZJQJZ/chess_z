#include "xiangqi/engine.h"
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

enum
{
    MAX_HISTORY = 256,
    INPUT_SIZE = 256,
    FEN_SIZE = 256,
    NODE_ID_SIZE = 64
};

/**
 * 目前 program 都是 main 函数的 argv[0]，此函数意义在于用户这个程序应该怎么运行、支持哪些命令行参数
 */
static void print_usage(const char *program)
{
    printf("usage: %s [--depth N] [--fen FEN] [--time-ms MS] [--tt-file PATH] [--moves MOVE ...]\n", program);
    printf("  --time-ms MS  total root-search budget (positive milliseconds); iterative deepening.\n");
    printf("  --moves ...   observe this path in the root search; accepts quoted sequences too.\n");
    printf("  --tt-file PATH load a transposition table; updates stay in memory, the file is unchanged.\n");
    printf("  Any of these options prints one target node and exits. Unvisited paths are not searched separately.\n");
    printf("  Single-shot search follows CLI: iterative deepening, default depth 10 and time 3000 ms.\n");
    printf("  An empty transposition table is used unless --tt-file loads one. Root cache is ordering-only.\n");
    printf("  Final root selection is separate from the retained target visit; timeout may finish a nearly complete iteration.\n");
    printf("  Otherwise an interrupted iteration falls back to the last complete iteration, if available.\n");
    printf("  Depth is measured from the root; interactive mode keeps its default depth of 4.\n");
    printf("  Path targets require remaining depth >= 1; single-shot mode rejects --depth 0.\n");
    printf("  N=0 is supported only by the original interactive quiescence explanation.\n");
}

/**
 * 将搜索节点类型转换为描述字符串 "exact"、"upper"、"lower"
 */
static const char *score_kind_text(XqSearchScoreKind kind)
{
    switch (kind)
    {
    case XQ_SEARCH_SCORE_EXACT:
        return "exact";
    case XQ_SEARCH_SCORE_UPPER_BOUND:
        return "upper";
    case XQ_SEARCH_SCORE_LOWER_BOUND:
        return "lower";
    default:
        return "?";
    }
}

static const char *tt_status_text(XqExplainTtStatus status)
{
    switch (status)
    {
    case XQ_EXPLAIN_TT_DISABLED: return "disabled";
    case XQ_EXPLAIN_TT_NOT_PROBED: return "not-probed";
    case XQ_EXPLAIN_TT_MISS: return "miss";
    case XQ_EXPLAIN_TT_HIT: return "hit";
    case XQ_EXPLAIN_TT_CUTOFF: return "direct";
    }
    return "?";
}

static bool tt_has_entry(const XqExplainTtInfo *info)
{
    return info->status == XQ_EXPLAIN_TT_HIT || info->status == XQ_EXPLAIN_TT_CUTOFF;
}

/* Scores printed here are from the queried node's own side-to-move perspective. */
static void print_tt_info(const char *label, const XqExplainTtInfo *info, unsigned required_depth)
{
    printf("%s status=%s", label, tt_status_text(info->status));
    if (tt_has_entry(info))
    {
        char move[8];
        if (!info->ordering_only)
            printf(" required_depth=%u", required_depth);
        printf(" cached_depth=%u cached_score=%d cached_kind=%s cached_best=%s hash_move_used=%s",
               info->depth, info->score, score_kind_text(info->score_kind),
               xq_move_to_string(info->best_move, move, sizeof(move)),
               info->hash_move_used ? "yes" : "no");
        if (info->status == XQ_EXPLAIN_TT_HIT)
            printf(" reason=%s", info->ordering_only ? "ordering-only" :
                   info->depth < required_depth ? "insufficient-depth" : "bound-outside-cutoff");
    }
    printf("\n");
}

/**
 * 把字符串中的英文字母转化为小写
 */
static void lower_text(char *text)
{
    while (*text != '\0')
    {
        *text = (char)tolower((unsigned char)*text);
        ++text;
    }
}

/**
 * 跳过字符串开头的所有空白字符，返回第一个非空白字符的位置
 */
static char *skip_spaces(char *text)
{
    while (*text != '\0' && isspace((unsigned char)*text))
        ++text;
    return text;
}

/**
 * 带完整合法性检查的正整数转换函数
 */
static bool parse_positive_int(const char *text, int *value)
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
    *value = (int)parsed;
    return true;
}

/**
 * 带完整合法性检查的非负整数转换函数。
 */
static bool parse_nonnegative_int(const char *text, int *value)
{
    char *end;
    long parsed;

    errno = 0;
    parsed = strtol(text, &end, 10);
    if (errno == ERANGE || text == end || parsed < 0 || parsed > INT_MAX)
        return false;
    while (*end != '\0')
    {
        if (!isspace((unsigned char)*end))
            return false;
        ++end;
    }
    *value = (int)parsed;
    return true;
}

/**
 * 根据从根节点到当前节点的选择路径，生成便于阅读的节点编号。
 *
 * path 保存从根节点走到当前节点时依次选择的着法编号；ply 既表示当前
 * 节点所在的层数，也表示 path 中有效元素的数量。因此，ply 为 3 时，
 * 函数会依次读取 path[0]、path[1] 和 path[2]。
 *
 * child_index 用于决定是否在当前路径末尾追加一个子节点编号：
 *   - child_index == 0：生成当前节点的编号；
 *   - child_index > 0：生成当前节点的第 child_index 个子节点的编号。
 *
 * 生成的字符串写入 buffer，buffer_size 是 buffer 的总容量，用于避免
 * 写越界。如果容量不足，buffer 中可能只保留一个被截断的节点编号。
 * 调用者应保证 buffer 不为 NULL、buffer_size 与 buffer 的实际容量一致，
 * 并且 path 至少包含 ply 个有效元素。
 *
 * 根节点是一个特殊情况：当 ply == 0 且 child_index == 0 时，结果为
 * "root"。
 *
 * 示例（假设缓冲区足够大）：
 *   path = {1, 4, 2}, ply = 3, child_index = 0  -> "1.4.2"
 *   path = {1, 4, 2}, ply = 3, child_index = 2  -> "1.4.2.2"
 *   path = {3},       ply = 1, child_index = 7  -> "3.7"
 *   ply = 0, child_index = 0                    -> "root"
 */
static void format_node_id(const int *path, int ply, int child_index, char *buffer, size_t buffer_size)
{
    size_t used = 0;
    int i;

    if (buffer_size == 0)
        return;
    buffer[0] = '\0';
    if (ply == 0 && child_index == 0)
    {
        snprintf(buffer, buffer_size, "root");
        return;
    }

    for (i = 0; i < ply; ++i)
    {
        int written = snprintf(buffer + used, buffer_size - used, "%s%d", used == 0 ? "" : ".", path[i]);
        if (written < 0 || (size_t)written >= buffer_size - used)
            return;
        used += (size_t)written;
    }

    if (child_index > 0 && used < buffer_size)
        (void)snprintf(buffer + used, buffer_size - used, "%s%d", used == 0 ? "" : ".", child_index);
}

/**
 * 根据已经向下浏览的层数 ply，计算当前节点还应该搜索多深
 * ply: 已经向下浏览的层数
 * root_depth: 输入的搜索层数
 */
static unsigned current_depth(unsigned root_depth, int ply)
{
    if (root_depth > (unsigned)ply)
        return root_depth - (unsigned)ply;
    return 0u;
}

/**
 * 打印帮助信息
 */
static void print_help(void)
{
    printf("commands:\n");
    printf("  N / open N  enter move N from the current table\n");
    printf("  back / b    return to parent node\n");
    printf("  root        return to root position\n");
    printf("  board       print only the current board\n");
    printf("  fen         print current FEN\n");
    printf("  eval        print static evaluation\n");
    printf("  list        redraw the current one-ply explanation\n");
    printf("  help / ?    print this help\n");
    printf("  quit / q    exit\n");
}

/**
 * 根据局面信息打印 FEN 字符串
 */
static void print_fen(const XqPosition *pos)
{
    char fen[FEN_SIZE];

    if (xq_position_to_fen(pos, fen, sizeof(fen)))
        printf("%s\n", fen);
    else
        printf("could not encode FEN\n");
}

/**
 * 打印当前局面评分：对红方、黑方、行棋方
 */
static void print_eval(const XqPosition *pos)
{
    int red = xq_engine_default_static_evaluate(pos, XQ_RED, NULL);
    int black = xq_engine_default_static_evaluate(pos, XQ_BLACK, NULL);
    int current = xq_engine_default_static_evaluate(pos, pos->side_to_move, NULL);

    printf("static eval: red=%d black=%d current(%s)=%d\n",
           red, black, pos->side_to_move == XQ_RED ? "red" : "black", current);
}

/**
 * 在 navigation 尾部追加 move
 */
static void add_navigation_move(XqMoveList *navigation, XqMove move)
{
    if (navigation->count < XQ_MAX_MOVES)
        navigation->moves[navigation->count++] = move;
}

/**
 * 把一组“已经解释过的候选走法”打印成表格
 */
static void print_explained_moves(const XqExplainedMove *moves, int count, const int *path, int ply,
                                   const XqExplainTtInfo *cache)
{
    int i;

    printf("%-4s %-12s %-5s %-5s %-5s %-9s %-11s %-11s %-8s %-7s %-10s",
           "idx", "node", "move", "pc", "cap", "order", "alpha", "beta", "score", "kind", "flags");
    if (cache != NULL)
        printf(" %-10s %-8s %-10s %-8s %-8s %-9s", "child_tt", "tt_depth", "tt_score",
               "tt_kind", "tt_best", "hash_used");
    printf("\n");
    for (i = 0; i < count; ++i)
    {
        const XqExplainedMove *explained = &moves[i];
        char move_text[8];
        char node_id[NODE_ID_SIZE];
        char flags[16] = "";

        format_node_id(path, ply, i + 1, node_id, sizeof(node_id));
        if (explained->is_best)
            (void)strcat(flags, "best");
        if (explained->caused_cutoff)
            (void)strcat(flags, flags[0] == '\0' ? "cutoff" : ",cutoff");

        printf("%-4d %-12s %-5s %-5c %-5c %-9d %-11d %-11d %-8d %-7s %-10s",
               i + 1,
               node_id,
               xq_move_to_string(explained->move, move_text, sizeof(move_text)),
               xq_piece_to_char(explained->move.piece),
               xq_piece_to_char(explained->move.captured),
               explained->order_score,
               explained->alpha_before,
               explained->beta,
               explained->score,
               score_kind_text(explained->score_kind),
               flags);
        if (cache != NULL)
        {
            const XqExplainTtInfo *info = &cache[i];
            printf(" %-10s", tt_status_text(info->status));
            if (tt_has_entry(info))
            {
                char cached_move[8];
                printf(" %-8u %-10d %-8s %-8s %-9s", info->depth, info->score,
                       score_kind_text(info->score_kind),
                       xq_move_to_string(info->best_move, cached_move, sizeof(cached_move)),
                       info->hash_move_used ? "yes" : "no");
            }
            else
                printf(" %-8s %-10s %-8s %-8s %-9s", "-", "-", "-", "-", "-");
        }
        printf("\n");
    }
}

/**
 * 类似 explain_current，只不过是针对静态搜索
 */
static bool explain_quiescence_current(XqPosition *pos, const int *path, int ply, XqMoveList *navigation)
{
    XqQuiescenceExplainResult result;
    int i;

    if (!xq_engine_explain_quiescence_one_ply(NULL, pos, &result))
    {
        printf("no quiescence result\n");
        return false;
    }

    printf("qsearch in_check=%s final_score=%d",
           result.in_check ? "yes" : "no",
           result.final_score);
    if (result.stand_pat_used)
        printf(" stand_pat=%d alpha_after_stand_pat=%d",
               result.stand_pat,
               result.alpha_after_stand_pat);
    if (result.stand_pat_cutoff)
        printf(" stand_pat_cutoff=yes");
    printf("\n");

    if (result.count == 0)
        printf("no tactical continuations\n");
    else
        print_explained_moves(result.moves, result.count, path, ply, NULL);

    for (i = 0; i < result.count; ++i)
        add_navigation_move(navigation, result.moves[i].move);
    return true;
}

/**
 * 解释当前节点：普通搜索深度展示一层 alpha-beta；叶子深度展示静态搜索。
 */
static bool explain_current(XqPosition *pos, unsigned root_depth, const int *path, int ply, XqMoveList *navigation)
{
    unsigned depth = current_depth(root_depth, ply);
    char current_id[NODE_ID_SIZE];
    XqExplainResult result;
    int i;

    xq_movelist_clear(navigation);
    format_node_id(path, ply, 0, current_id, sizeof(current_id));
    printf("\nnode=%s side=%s depth=%u\n",
           current_id,
           pos->side_to_move == XQ_RED ? "red" : "black",
           depth);
    xq_position_print(pos);

    if (depth == 0)
        return explain_quiescence_current(pos, path, ply, navigation);

    if (!xq_engine_explain_search_one_ply(NULL, pos, depth, &result))
    {
        printf("no moves to explain\n");
        return false;
    }

    printf("final_score=%d best=%d\n", result.final_score, result.best_index + 1);
    print_explained_moves(result.moves, result.count, path, ply, NULL);
    for (i = 0; i < result.count; ++i)
        add_navigation_move(navigation, result.moves[i].move);
    return true;
}

typedef struct ExplainOptions
{
    unsigned depth;
    bool depth_set;
    bool single;
    uint64_t time_ms;
    const char *fen;
    const char *tt_file;
    XqMove *moves;
    const char **move_texts;
    size_t count;
    size_t capacity;
} ExplainOptions;

/* Same positive, overflow-checked millisecond syntax as xiangqi_cli. */
static bool parse_time_ms(const char *text, uint64_t *time_ms)
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
    *time_ms = value;
    return true;
}

/* Tokenize without modifying argv, retaining only coordinates until FEN is known. */
static bool append_moves(ExplainOptions *options, const char *text)
{
    size_t before = options->count;
    while (*text != '\0')
    {
        const char *begin;
        size_t length;
        int ff, fr, tf, tr;
        while (isspace((unsigned char)*text))
            ++text;
        if (*text == '\0')
            break;
        begin = text;
        while (*text != '\0' && !isspace((unsigned char)*text))
            ++text;
        length = (size_t)(text - begin);
        if (length != 4)
        {
            fprintf(stderr, "invalid move %zu: %.*s (expected four characters)\n",
                    options->count + 1, (int)length, begin);
            return false;
        }
        ff = tolower((unsigned char)begin[0]) - 'a';
        fr = begin[1] - '0';
        tf = tolower((unsigned char)begin[2]) - 'a';
        tr = begin[3] - '0';
        if (!xq_square_is_valid(ff, fr) || !xq_square_is_valid(tf, tr))
        {
            fprintf(stderr, "invalid move %zu: %.4s\n", options->count + 1, begin);
            return false;
        }
        if (options->count == options->capacity)
        {
            size_t capacity = options->capacity == 0 ? 16 : options->capacity * 2;
            XqMove *moves;
            const char **texts;
            if (capacity > (size_t)INT_MAX || capacity > SIZE_MAX / sizeof(*moves) ||
                capacity > SIZE_MAX / sizeof(*texts))
                return false;
            moves = realloc(options->moves, capacity * sizeof(*moves));
            if (moves == NULL)
            {
                fprintf(stderr, "could not allocate move path\n");
                return false;
            }
            options->moves = moves;
            texts = realloc(options->move_texts, capacity * sizeof(*texts));
            if (texts == NULL)
            {
                fprintf(stderr, "could not allocate move path\n");
                return false;
            }
            options->move_texts = texts;
            options->capacity = capacity;
        }
        options->move_texts[options->count] = begin;
        options->moves[options->count++] = (XqMove){
            .from = (uint8_t)xq_square_make(ff, fr),
            .to = (uint8_t)xq_square_make(tf, tr),
        };
    }
    if (options->count == before)
    {
        fprintf(stderr, "--moves requires a nonempty move sequence\n");
        return false;
    }
    return true;
}

/* 0: invalid input, 1: run, 2: help. */
static int parse_options(int argc, char **argv, ExplainOptions *options)
{
    int arg;
    for (arg = 1; arg < argc; ++arg)
    {
        if (strcmp(argv[arg], "--depth") == 0)
        {
            int parsed;
            if (++arg == argc || !parse_nonnegative_int(argv[arg], &parsed))
                return 0;
            options->depth = (unsigned)parsed;
            options->depth_set = true;
        }
        else if (strcmp(argv[arg], "--fen") == 0)
        {
            if (++arg == argc)
                return 0;
            options->fen = argv[arg];
        }
        else if (strcmp(argv[arg], "--tt-file") == 0)
        {
            if (++arg == argc || argv[arg][0] == '\0')
            {
                fprintf(stderr, "--tt-file requires a nonempty file path\n");
                return 0;
            }
            options->tt_file = argv[arg];
            options->single = true;
        }
        else if (strcmp(argv[arg], "--time-ms") == 0)
        {
            if (++arg == argc || !parse_time_ms(argv[arg], &options->time_ms))
            {
                fprintf(stderr, "--time-ms requires positive integer milliseconds\n");
                return 0;
            }
            options->single = true;
        }
        else if (strcmp(argv[arg], "--moves") == 0)
        {
            size_t before = options->count;
            options->single = true;
            while (arg + 1 < argc && strncmp(argv[arg + 1], "--", 2) != 0)
                if (!append_moves(options, argv[++arg]))
                    return 0;
            if (options->count == before)
            {
                fprintf(stderr, "--moves requires a nonempty move sequence\n");
                return 0;
            }
        }
        else if (strcmp(argv[arg], "--help") == 0 || strcmp(argv[arg], "-h") == 0)
            return 2;
        else
            return 0;
    }
    if (options->single)
    {
        XqSearchLimits defaults = xq_search_limits_default();
        if (!options->depth_set)
            options->depth = defaults.max_depth;
        if (options->time_ms == 0)
            options->time_ms = defaults.time_limit_ms;
    }
    return 1;
}

static bool validate_path(const XqPosition *root, ExplainOptions *options, XqPosition *target)
{
    size_t step;
    *target = *root;
    for (step = 0; step < options->count; ++step)
    {
        XqMoveList legal;
        int i;
        xq_generate_legal(target, &legal);
        for (i = 0; i < legal.count; ++i)
            if (legal.moves[i].from == options->moves[step].from &&
                legal.moves[i].to == options->moves[step].to)
                break;
        if (i == legal.count)
        {
            fprintf(stderr, "illegal move %zu: %.4s\n", step + 1, options->move_texts[step]);
            return false;
        }
        options->moves[step] = legal.moves[i];
        if (!xq_position_make_move(target, legal.moves[i]))
            return false;
    }
    return true;
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

/* This event has its own iteration and is not inferred from the retained target visit. */
static void print_tt_block(const XqExplainTtBlock *block, const ExplainOptions *options)
{
    size_t i;
    if (!block->available)
        return;
    printf("latest_ancestor_cache_block root_depth=%u ply=%zu remaining_depth=%u alpha=%d beta=%d path=",
           block->root_depth, block->ply, block->remaining_depth, block->alpha, block->beta);
    if (block->ply == 0)
        printf("root");
    for (i = 0; i < block->ply; ++i)
    {
        char move[8];
        printf("%s%s", i == 0 ? "" : " ",
               xq_move_to_string(options->moves[i], move, sizeof(move)));
    }
    printf("\n");
    print_tt_info("ancestor_tt", &block->tt, block->remaining_depth);
    printf("The ancestor returned directly from the table; the target was not reached in that iteration.\n");
}

/* Show the candidate snapshot that actually supplied the final root decision. */
static void print_root_selection(const XqPathExplainResult *result)
{
    const XqRootSearchExplain *root = &result->root;
    const XqSearchStats *stats = &result->stats;
    const char *policy;
    char move[8];
    int i;
    if (!root->move_available)
    {
        printf("root_selection: no legal root move\n");
        return;
    }
    if (stats->selected_move_depth == 0)
        policy = "fallback";
    else if (stats->completed_depth == 0)
        policy = "partial-first-iteration";
    else if (stats->stopped && stats->max_started_depth > stats->completed_depth)
        policy = "timeout-completed-iteration";
    else
        policy = "completed-iteration";
    printf("root_selection selected_move=%s selected_move_depth=%u policy=%s\n",
           xq_move_to_string(root->selected_move, move, sizeof(move)),
           stats->selected_move_depth, policy);
    if (!stats->stopped || root->count == 0)
        return;
    printf("Root candidates below belong to the iteration used for the final selection.\n"
           "Scores use the root side's perspective and may be bounds; unsearched rows have no score.\n");
    printf("%-6s %-9s %-16s %s\n", "move", "score", "completed_depth", "selected");
    for (i = 0; i < root->count; ++i)
    {
        const XqRootMoveExplain *row = &root->moves[i];
        printf("%-6s ", xq_move_to_string(row->move, move, sizeof(move)));
        if (row->completed_depth != 0)
            printf("%-9d %-16u", row->score, row->completed_depth);
        else
            printf("%-9s %-16u", "-", 0u);
        printf(" %s\n", i == root->selected_index ? "yes" : "");
    }
}

static int explain_path_once(XqPosition *root, ExplainOptions *options)
{
    XqPosition target;
    XqPathExplainResult result;
    XqSearchLimits limits = xq_search_limits_default();
    XqEngineAdapter engine = {0};
    bool found;
    size_t i;
    if (options->depth == 0)
    {
        fprintf(stderr, "path explanation requires --depth >= 1\n");
        return EXIT_FAILURE;
    }
    if (!validate_path(root, options, &target))
        return EXIT_FAILURE;
    engine.transposition_table = xq_transposition_table_create();
    if (options->tt_file != NULL)
    {
        XqTranspositionIoStatus status;
        status = engine.transposition_table == NULL ? XQ_TT_IO_NO_MEMORY
                 : xq_transposition_table_load(engine.transposition_table, options->tt_file);
        if (status != XQ_TT_IO_OK)
        {
            fprintf(stderr, "cannot load transposition table '%s': %s\n",
                    options->tt_file, tt_io_status_text(status));
            xq_transposition_table_destroy(engine.transposition_table);
            return EXIT_FAILURE;
        }
        printf("Loaded transposition table from: %s\n", options->tt_file);
    }
    else if (engine.transposition_table == NULL)
        fprintf(stderr, "warning: transposition table allocation failed; continuing without cache\n");
    limits.max_depth = options->depth;
    limits.time_limit_ms = options->time_ms;
    found = xq_engine_explain_path(&engine, root, &limits, options->moves, options->count, &result);
    xq_transposition_table_destroy(engine.transposition_table);
    if (!found)
        return EXIT_FAILURE;
    printf("path=");
    if (options->count == 0)
        printf("root");
    for (i = 0; i < options->count; ++i)
    {
        char text[8];
        printf("%s%s", i == 0 ? "" : " ",
               xq_move_to_string(options->moves[i], text, sizeof(text)));
    }
    printf("\nside=%s ply=%zu depth_limit=%u time_limit_ms=%" PRIu64 "\n",
           target.side_to_move == XQ_RED ? "red" : "black", options->count,
           options->depth, options->time_ms);
    xq_position_print(&target);
    printf("fen: ");
    print_fen(&target);
    printf("search stopped=%s completed_root_depth=%u max_started_depth=%u nodes=%" PRIu64,
           result.stats.stopped ? "yes" : "no", result.stats.completed_depth,
           result.stats.max_started_depth, result.stats.nodes);
    if (result.stats.elapsed_available)
        printf(" elapsed_ms=%" PRIu64, result.stats.elapsed_ms);
    printf("\n");
    print_root_selection(&result);
    if (result.stats.cache_available)
    {
        print_tt_info("root_ordering_tt", &result.root.ordering_tt, 1);
        printf("Root cache policy: ordering lookup before iteration 1; no new root probes in later iterations.\n");
        printf("cache probes=%" PRIu64 " hits=%" PRIu64 " cutoffs=%" PRIu64
               " stores=%" PRIu64 " replacements=%" PRIu64 "\n",
               result.stats.cache.probes, result.stats.cache.hits, result.stats.cache.cutoffs,
               result.stats.cache.stores, result.stats.cache.replacements);
        print_tt_block(&result.blocked_tt, options);
    }
    if (!result.visited)
    {
        printf("path not visited with remaining depth >= 1 in this search\n");
        return EXIT_SUCCESS;
    }
    printf("Target visit below is independent of the final root selection; best indexes this visit's candidates.\n");
    printf("node root_depth=%u remaining_depth=%u complete=%s alpha=%d beta=%d\n",
           result.root_depth, result.remaining_depth, result.complete ? "yes" : "no",
           result.alpha_before, result.beta);
    if (result.score_available)
        printf("%s=%d kind=%s best=%d\n", result.complete ? "final_score" : "partial_score",
               result.node.final_score, result.complete ? score_kind_text(result.score_kind) : "partial",
               result.node.best_index + 1);
    else
        printf("no completed candidate score\n");
    if (result.stats.cache_available)
        print_tt_info("target_tt", &result.tt, result.remaining_depth);
    if (result.tt.status == XQ_EXPLAIN_TT_CUTOFF)
        printf("returned directly from transposition table; no candidates expanded\n");
    if (result.node.count != 0)
    {
        if (result.stats.cache_available)
            printf("child_tt is the direct child's entry probe only; tt_score/tt_kind use the child\n"
                   "side's perspective, while score/kind use the target side's perspective.\n"
                   "direct=cache return; hit=entry found but depth/bound cannot return;\n"
                   "miss=no entry; not-probed=leaf/terminal without a lookup.\n");
        print_explained_moves(result.node.moves, result.node.count, NULL, 0,
                              result.stats.cache_available ? result.move_tt : NULL);
    }
    else if (result.tt.status != XQ_EXPLAIN_TT_CUTOFF)
        printf("no searched continuations\n");
    return EXIT_SUCCESS;
}

int main(int argc, char **argv)
{
    XqPosition positions[MAX_HISTORY + 1];
    int path[MAX_HISTORY];
    int ply = 0;
    ExplainOptions options = {.depth = 4u};
    unsigned depth;
    int parsed = parse_options(argc, argv, &options);
    char input[INPUT_SIZE];

    if (parsed != 1)
    {
        print_usage(argv[0]);
        free(options.moves);
        free(options.move_texts);
        return parsed == 2 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    depth = options.depth;
    if (options.fen != NULL)
    {
        if (!xq_position_from_fen(&positions[0], options.fen))
        {
            fprintf(stderr, "invalid FEN: %s\n", options.fen);
            free(options.moves);
            free(options.move_texts);
            return EXIT_FAILURE;
        }
    }
    else
        xq_position_startpos(&positions[0]);
    if (options.single)
    {
        int status = explain_path_once(&positions[0], &options);
        free(options.moves);
        free(options.move_texts);
        return status;
    }
    free(options.moves);
    free(options.move_texts);

    print_help();
    for (;;)
    {
        XqMoveList navigation;
        bool have_result = explain_current(&positions[ply], depth, path, ply, &navigation);
        char *command;

        printf("explain> ");
        if (fgets(input, sizeof(input), stdin) == NULL)
            break;
        input[strcspn(input, "\r\n")] = '\0';
        command = skip_spaces(input);
        lower_text(command);

        if (command[0] == '\0' || strcmp(command, "list") == 0)
            continue;
        if (strcmp(command, "quit") == 0 || strcmp(command, "q") == 0)
            break;
        if (strcmp(command, "help") == 0 || strcmp(command, "?") == 0)
        {
            print_help();
            continue;
        }
        if (strcmp(command, "board") == 0)
        {
            xq_position_print(&positions[ply]);
            continue;
        }
        if (strcmp(command, "fen") == 0)
        {
            print_fen(&positions[ply]);
            continue;
        }
        if (strcmp(command, "eval") == 0)
        {
            print_eval(&positions[ply]);
            continue;
        }
        if (strcmp(command, "back") == 0 || strcmp(command, "b") == 0)
        {
            if (ply > 0)
                --ply;
            else
                printf("already at root\n");
            continue;
        }
        if (strcmp(command, "root") == 0)
        {
            ply = 0;
            continue;
        }

        {
            int index;
            char *number_text = command;

            if (strncmp(command, "open", 4) == 0 && isspace((unsigned char)command[4]))
                number_text = skip_spaces(command + 4);
            if (!parse_positive_int(number_text, &index))
            {
                printf("unknown command: %s\n", command);
                continue;
            }
            if (!have_result || index > navigation.count)
            {
                printf("move index out of range\n");
                continue;
            }
            if (ply >= MAX_HISTORY)
            {
                printf("history limit reached\n");
                continue;
            }

            positions[ply + 1] = positions[ply];
            if (!xq_position_make_move(&positions[ply + 1], navigation.moves[index - 1]))
            {
                printf("could not make selected move\n");
                continue;
            }
            path[ply] = index;
            ++ply;
        }
    }

    return EXIT_SUCCESS;
}
