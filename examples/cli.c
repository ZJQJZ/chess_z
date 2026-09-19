#include "xiangqi/engine.h"
#include "xiangqi/movegen.h"
#include "xiangqi/position.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    printf("usage: %s [--fen FEN] [--engine red|black]\n", program);
    printf("\n");
    printf("options:\n");
    printf("  -f, --fen FEN           initialize the position from FEN\n");
    printf("  -e, --engine COLOR      choose the engine side: red or black\n");
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

/**
 * help
 */
static void print_help(void)
{
    printf("commands:\n");
    printf("  a0a1  move from file/rank to file/rank\n");
    printf("  fen   print current FEN\n");
    printf("  moves print legal moves\n");
    printf("  quit  exit\n");
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

int main(int argc, char **argv)
{
    XqPosition pos;
    XqMoveList legal;
    XqTranspositionTable *table;
    XqEngineAdapter engine;
    const char *fen = NULL;
    XqColor engine_color = XQ_BLACK;
    char input[64];
    int i;

    for (i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0)
        {
            print_usage(argv[0]);
            return EXIT_SUCCESS;
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

    table = xq_transposition_table_create();
    engine.static_evaluate = NULL;
    engine.search = NULL;
    engine.user = NULL;
    engine.score_move = NULL;
    engine.transposition_table = table;

    if (table == NULL)
        fprintf(stderr, "warning: transposition table allocation failed; continuing without cache\n");

    printf("Xiangqi demo: human %s vs builtin %s engine\n",
           color_name(xq_color_opponent(engine_color)), color_name(engine_color));
    printf("Use coordinates a-i and ranks 0-9. Example: b2b9\n");
    print_help();

    for (;;)
    {
        xq_position_print(&pos);
        xq_generate_legal(&pos, &legal);

        if (legal.count == 0)
        {
            printf("%s has no legal moves and loses. %s wins.\n",
                   pos.side_to_move == XQ_RED ? "red" : "black",
                   pos.side_to_move == XQ_RED ? "black" : "red");
            break;
        }

        if (pos.side_to_move == engine_color)
        {
            XqMove best;
            char text[8];
            if (!xq_engine_find_best_move(&engine, &pos, NULL, &best))
            {
                printf("engine failed to move\n");
                break;
            }
            printf("%s engine plays: %s\n", color_name(engine_color),
                   xq_move_to_string(best, text, sizeof(text)));
            xq_position_make_move(&pos, best);
            continue;
        }

        printf("%s to move > ", color_name(pos.side_to_move));
        if (fgets(input, sizeof(input), stdin) == NULL)
            break;
        input[strcspn(input, "\r\n")] = '\0';

        if (strcmp(input, "quit") == 0 || strcmp(input, "q") == 0)
            break;
        if (strcmp(input, "help") == 0 || strcmp(input, "?") == 0)
        {
            print_help();
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

        {
            XqMove move;
            if (!parse_move_text(input, &legal, &move))
            {
                printf("illegal move or bad format. Try moves like b2b9, or type moves.\n");
                continue;
            }
            xq_position_make_move(&pos, move);
        }
    }

    xq_transposition_table_destroy(table);
    return 0;
}
