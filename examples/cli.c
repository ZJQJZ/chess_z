#include "xiangqi/engine.h"
#include "xiangqi/movegen.h"
#include "xiangqi/position.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

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

int main(void)
{
    XqPosition pos;
    XqMoveList legal;
    char input[64];

    xq_position_startpos(&pos);
    printf("Xiangqi demo: human red vs builtin black engine\n");
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

        if (pos.side_to_move == XQ_BLACK)
        {
            XqMove best;
            char text[8];
            if (!xq_engine_find_best_move(NULL, &pos, 5, &best))
            {
                printf("engine failed to move\n");
                break;
            }
            printf("black engine plays: %s\n", xq_move_to_string(best, text, sizeof(text)));
            xq_position_make_move(&pos, best);
            continue;
        }

        printf("red to move > ");
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

    return 0;
}
