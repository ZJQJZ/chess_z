#include "xiangqi/engine.h"
#include "xiangqi/movegen.h"
#include "xiangqi/position.h"

#include <ctype.h>
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
    printf("usage: %s [--depth N] [--fen FEN]\n", program);
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

    parsed = strtol(text, &end, 10);
    if (text == end || parsed <= 0 || parsed > INT_MAX)
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
    printf("  depth 0     automatically shows quiescence search\n");
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
static void print_explained_moves(const XqExplainedMove *moves, int count, const int *path, int ply)
{
    int i;

    printf("%-4s %-12s %-5s %-5s %-5s %-9s %-11s %-11s %-8s %-7s %-10s\n",
           "idx", "node", "move", "pc", "cap", "order", "alpha", "beta", "score", "kind", "flags");
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

        printf("%-4d %-12s %-5s %-5c %-5c %-9d %-11d %-11d %-8d %-7s %-10s\n",
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
        print_explained_moves(result.moves, result.count, path, ply);

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
    print_explained_moves(result.moves, result.count, path, ply);
    for (i = 0; i < result.count; ++i)
        add_navigation_move(navigation, result.moves[i].move);
    return true;
}

int main(int argc, char **argv)
{
    XqPosition positions[MAX_HISTORY + 1];
    int path[MAX_HISTORY];
    int ply = 0;
    unsigned depth = 4u;
    const char *fen = NULL;
    int arg;
    char input[INPUT_SIZE];

    for (arg = 1; arg < argc; ++arg)
    {
        if (strcmp(argv[arg], "--depth") == 0)
        {
            int parsed;
            if (arg + 1 >= argc || !parse_positive_int(argv[arg + 1], &parsed))
            {
                print_usage(argv[0]);
                return EXIT_FAILURE;
            }
            depth = (unsigned)parsed;
            ++arg;
        }
        else if (strcmp(argv[arg], "--fen") == 0)
        {
            if (arg + 1 >= argc)
            {
                print_usage(argv[0]);
                return EXIT_FAILURE;
            }
            fen = argv[++arg];
        }
        else if (strcmp(argv[arg], "--help") == 0 || strcmp(argv[arg], "-h") == 0)
        {
            print_usage(argv[0]);
            return EXIT_SUCCESS;
        }
        else
        {
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    if (fen != NULL)
    {
        if (!xq_position_from_fen(&positions[0], fen))
        {
            fprintf(stderr, "invalid FEN: %s\n", fen);
            return EXIT_FAILURE;
        }
    }
    else
        xq_position_startpos(&positions[0]);

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
