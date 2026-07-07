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

static void print_usage(const char *program)
{
    printf("usage: %s [--depth N] [--fen FEN]\n", program);
}

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

static void lower_text(char *text)
{
    while (*text != '\0')
    {
        *text = (char)tolower((unsigned char)*text);
        ++text;
    }
}

static char *skip_spaces(char *text)
{
    while (*text != '\0' && isspace((unsigned char)*text))
        ++text;
    return text;
}

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

static void format_node_id(const int *path, int ply, int child_index, char *buffer, size_t buffer_size)
{
    size_t used = 0;
    int i;

    if (buffer_size == 0)
        return;
    buffer[0] = '\0';
    if (ply == 0 && child_index == 0)
    {
        (void)snprintf(buffer, buffer_size, "root");
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

static unsigned current_depth(unsigned root_depth, int ply)
{
    if (root_depth > (unsigned)ply)
        return root_depth - (unsigned)ply;
    return 1u;
}

static void print_help(void)
{
    printf("commands:\n");
    printf("  N / open N  enter move N from the current table\n");
    printf("  back        return to parent node\n");
    printf("  root        return to root position\n");
    printf("  board       print only the current board\n");
    printf("  fen         print current FEN\n");
    printf("  eval        print static evaluation\n");
    printf("  list        redraw the current one-ply explanation\n");
    printf("  help        print this help\n");
    printf("  quit        exit\n");
}

static void print_fen(const XqPosition *pos)
{
    char fen[FEN_SIZE];

    if (xq_position_to_fen(pos, fen, sizeof(fen)))
        printf("%s\n", fen);
    else
        printf("could not encode FEN\n");
}

static void print_eval(const XqPosition *pos)
{
    int red = xq_engine_default_static_evaluate(pos, XQ_RED, NULL);
    int black = xq_engine_default_static_evaluate(pos, XQ_BLACK, NULL);
    int current = xq_engine_default_static_evaluate(pos, pos->side_to_move, NULL);

    printf("static eval: red=%d black=%d current(%s)=%d\n",
           red, black, pos->side_to_move == XQ_RED ? "red" : "black", current);
}

static bool explain_current(const XqPosition *pos, unsigned root_depth, const int *path, int ply, XqExplainResult *result)
{
    unsigned depth = current_depth(root_depth, ply);
    char current_id[NODE_ID_SIZE];
    int i;

    format_node_id(path, ply, 0, current_id, sizeof(current_id));
    printf("\nnode=%s side=%s depth=%u\n",
           current_id,
           pos->side_to_move == XQ_RED ? "red" : "black",
           depth);
    xq_position_print(pos);

    if (!xq_engine_explain_one_ply(NULL, pos, depth, result))
    {
        printf("no moves to explain\n");
        return false;
    }

    printf("final_score=%d best=%d\n", result->final_score, result->best_index + 1);
    printf("%-4s %-12s %-5s %-5s %-5s %-9s %-11s %-11s %-8s %-7s %-10s\n",
           "idx", "node", "move", "pc", "cap", "order", "alpha", "beta", "score", "kind", "flags");
    for (i = 0; i < result->count; ++i)
    {
        const XqExplainedMove *explained = &result->moves[i];
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
        XqExplainResult result;
        bool have_result = explain_current(&positions[ply], depth, path, ply, &result);
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
        if (strcmp(command, "back") == 0)
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
            if (!have_result || index > result.count)
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
            if (!xq_position_make_move(&positions[ply + 1], result.moves[index - 1].move))
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
