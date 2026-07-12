#include "xiangqi/engine.h"
#include "xiangqi/position.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SEARCH_DEPTH 6u
#define LINE_BUFFER_SIZE 256u

static const char *default_input_path = "stats/random_fen/random_positions.fen";

/**
 * 返回从 start 到当前时刻消耗的 CPU 时间（秒）；计时失败时返回 0.0。
 */
static double elapsed_seconds(clock_t start)
{
    clock_t end = clock();

    if (start == (clock_t)-1 || end == (clock_t)-1)
        return 0.0;
    return (double)(end - start) / (double)CLOCKS_PER_SEC;
}

/**
 * 类似 generate_positions.c 中的 checksum_text
 */
static uint64_t checksum_move(uint64_t checksum, XqMove move)
{
    checksum ^= (uint64_t)move.from;
    checksum *= UINT64_C(1099511628211);
    checksum ^= (uint64_t)move.to;
    checksum *= UINT64_C(1099511628211);
    return checksum;
}

/**
 * 打印信息
 */
static void print_summary(size_t searched, unsigned failures, clock_t start,
                          uint64_t checksum, const XqTranspositionTable *table)
{
    XqTranspositionStats stats;

    printf("search summary: completed=%zu failures=%u cpu_seconds=%.3f checksum=%" PRIu64 "\n",
           searched, failures, elapsed_seconds(start), checksum);
    xq_transposition_table_get_stats(table, &stats);
    printf("tt summary: probes=%" PRIu64 " hits=%" PRIu64
           " cutoffs=%" PRIu64 " stores=%" PRIu64
           " replacements=%" PRIu64 "\n",
           stats.probes, stats.hits, stats.cutoffs,
           stats.stores, stats.replacements);
}

/**
 * 失败回调
 */
static int fail_at_line(FILE *input, const char *path, size_t line_number,
                        const char *reason, const char *line, size_t searched,
                        clock_t start, uint64_t checksum,
                        XqTranspositionTable *table)
{
    if (input != NULL)
        (void)fclose(input);
    if (line_number == 0)
        fprintf(stderr, "error: %s: %s\n", path, reason);
    else if (line != NULL && line[0] != '\0')
        fprintf(stderr, "error: %s:%zu: %s: %s\n", path, line_number, reason, line);
    else
        fprintf(stderr, "error: %s:%zu: %s\n", path, line_number, reason);
    print_summary(searched, 1u, start, checksum, table);
    xq_transposition_table_destroy(table);
    return EXIT_FAILURE;
}

int main(int argc, char **argv)
{
    const char *input_path;
    FILE *input;
    char line[LINE_BUFFER_SIZE];
    size_t line_number = 0;
    size_t searched = 0;
    uint64_t checksum = UINT64_C(14695981039346656037);
    clock_t start = clock();
    XqTranspositionTable *table;
    XqEngineAdapter engine;

    if (argc > 2)
    {
        fprintf(stderr, "usage: %s [fen_file]\n", argv[0]);
        return EXIT_FAILURE;
    }

    input_path = argc == 2 ? argv[1] : default_input_path;
    printf("search: input=%s depth=%u\n", input_path, SEARCH_DEPTH);

    table = xq_transposition_table_create();
    if (table == NULL)
        fprintf(stderr, "warning: transposition table allocation failed; continuing without cache\n");
    engine.static_evaluate = NULL;
    engine.search = NULL;
    engine.user = NULL;
    engine.score_move = NULL;
    engine.transposition_table = table;

    input = fopen(input_path, "rb");
    if (input == NULL)
        return fail_at_line(NULL, input_path, 0, "could not open input file", NULL,
                            searched, start, checksum, table);

    while (fgets(line, sizeof(line), input) != NULL)
    {
        XqPosition pos;
        XqMove best;
        char *newline;
        size_t length;

        ++line_number;
        length = strlen(line);
        newline = strchr(line, '\n');
        if (newline == NULL && length == sizeof(line) - 1u)
        {
            int ch;

            while ((ch = fgetc(input)) != '\n' && ch != EOF)
                ;
            return fail_at_line(input, input_path, line_number, "line is too long", NULL,
                                searched, start, checksum, table);
        }
        if (newline != NULL)
            *newline = '\0';

        length = strlen(line);
        if (length > 0 && line[length - 1u] == '\r')
            line[length - 1u] = '\0';
        if (line[0] == '\0')
            continue;

        if (!xq_position_from_fen(&pos, line))
            return fail_at_line(input, input_path, line_number, "invalid FEN", line,
                                searched, start, checksum, table);

        if (!xq_engine_find_best_move(&engine, &pos, SEARCH_DEPTH, &best))
            return fail_at_line(input, input_path, line_number, "engine failed to find a move", line,
                                searched, start, checksum, table);

        if (!xq_position_make_move(&pos, best) || !xq_position_validate(&pos))
            return fail_at_line(input, input_path, line_number, "engine move produced an invalid position", line,
                                searched, start, checksum, table);

        checksum = checksum_move(checksum, best);
        ++searched;
    }

    if (ferror(input))
        return fail_at_line(input, input_path, line_number, "failed while reading input file", NULL,
                            searched, start, checksum, table);
    if (fclose(input) != 0)
        return fail_at_line(NULL, input_path, line_number, "failed to close input file", NULL,
                            searched, start, checksum, table);
    if (searched == 0)
        return fail_at_line(NULL, input_path, 0, "input file contains no positions", NULL,
                            searched, start, checksum, table);

    print_summary(searched, 0u, start, checksum, table);
    xq_transposition_table_destroy(table);
    return EXIT_SUCCESS;
}
