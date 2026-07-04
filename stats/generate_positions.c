#include "xiangqi/movegen.h"
#include "xiangqi/position.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <windows.h>
#endif

#define POSITION_COUNT 100u
#define MAX_RANDOM_PLIES 100u
#define MAX_GENERATION_ATTEMPTS 256u
#define RANDOM_SEED UINT32_C(0x00C0FFEE)

static const char *default_output_path = "stats/random_fen/random_positions.fen";

typedef struct RandomState
{
    uint32_t state;
} RandomState;

/**
 * 伪随机数生成器，用于生成下一个 uint32_t 随机数
 */
static uint32_t random_next(RandomState *random)
{
    uint32_t value = random->state;

    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    random->state = value;
    return value;
}

/**
 * 带有范围的 random_next
 */
static unsigned random_below(RandomState *random, unsigned upper_bound)
{
    return (unsigned)(random_next(random) % upper_bound);
}

/**
 * 基于 FNV-1a 算法伪 *text 生成指纹
 */
static uint64_t checksum_text(uint64_t checksum, const char *text)
{
    const unsigned char *p = (const unsigned char *)text;

    while (*p != '\0')
    {
        checksum ^= (uint64_t)*p++;
        checksum *= UINT64_C(1099511628211);
    }
    checksum ^= (uint64_t)'\n';
    checksum *= UINT64_C(1099511628211);
    return checksum;
}

/**
 * 对 *pos 随机走 0 - MAX_RANDOM_PLIES 步，可能会随机失败，会有重试，重试次数记在 *retry_count
 * 成功则返回 true
 */
static bool generate_position(RandomState *random, XqPosition *pos, unsigned *retry_count)
{
    unsigned target_plies = random_below(random, MAX_RANDOM_PLIES + 1u);
    unsigned attempt;

    for (attempt = 0; attempt < MAX_GENERATION_ATTEMPTS; ++attempt)
    {
        unsigned ply;
        bool complete = true;

        xq_position_startpos(pos);
        for (ply = 0; ply < target_plies; ++ply)
        {
            XqMoveList legal;
            unsigned move_index;

            xq_generate_legal(pos, &legal);
            if (legal.count == 0)
            {
                complete = false;
                break;
            }

            move_index = random_below(random, (unsigned)legal.count);
            if (!xq_position_make_move(pos, legal.moves[move_index]))
            {
                complete = false;
                break;
            }
        }

        if (complete && xq_position_validate(pos))
        {
            XqMoveList legal;

            xq_generate_legal(pos, &legal);
            if (legal.count > 0)
                return true;
        }

        ++*retry_count;
    }

    return false;
}

/**
 * 在堆内存中分配 "xxx.tmp" 字符串返回给调用者
 */
static char *make_temp_path(const char *output_path)
{
    size_t length = strlen(output_path);
    char *temp_path = (char *)malloc(length + 5u);

    if (temp_path == NULL)
        return NULL;
    memcpy(temp_path, output_path, length);
    memcpy(temp_path + length, ".tmp", 5u);
    return temp_path;
}

static bool replace_file(const char *temp_path, const char *output_path)
{
#if defined(_WIN32)
    return MoveFileExA(temp_path, output_path,
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return rename(temp_path, output_path) == 0;
#endif
}

/**
 * 用已经写完的临时文件替换正式输出文件
 */
static double elapsed_seconds(clock_t start)
{
    clock_t end = clock();

    if (start == (clock_t)-1 || end == (clock_t)-1)
        return 0.0;
    return (double)(end - start) / (double)CLOCKS_PER_SEC;
}

int main(int argc, char **argv)
{
    const char *output_path;
    char *temp_path;
    FILE *output;
    RandomState random = {RANDOM_SEED};
    uint64_t checksum = UINT64_C(14695981039346656037);
    unsigned generated = 0;
    unsigned retries = 0;
    bool success = true;
    clock_t start = clock();

    if (argc > 2)
    {
        fprintf(stderr, "usage: %s [fen_file]\n", argv[0]);
        return EXIT_FAILURE;
    }

    output_path = argc == 2 ? argv[1] : default_output_path;
    temp_path = make_temp_path(output_path);
    if (temp_path == NULL)
    {
        fprintf(stderr, "error: could not allocate temporary path\n");
        return EXIT_FAILURE;
    }

    printf("generate: output=%s positions=%u plies=0..%u seed=0x%08" PRIx32 "\n",
           output_path, POSITION_COUNT, MAX_RANDOM_PLIES, RANDOM_SEED);

    output = fopen(temp_path, "wb");
    if (output == NULL)
    {
        fprintf(stderr, "error: could not open temporary file %s\n", temp_path);
        free(temp_path);
        return EXIT_FAILURE;
    }

    while (generated < POSITION_COUNT)
    {
        XqPosition pos;
        char fen[128];

        if (!generate_position(&random, &pos, &retries))
        {
            fprintf(stderr, "error: failed to generate position %u after %u attempts\n",
                    generated + 1u, MAX_GENERATION_ATTEMPTS);
            success = false;
            break;
        }
        if (!xq_position_to_fen(&pos, fen, sizeof(fen)))
        {
            fprintf(stderr, "error: failed to encode position %u as FEN\n", generated + 1u);
            success = false;
            break;
        }
        if (fprintf(output, "%s\n", fen) < 0)
        {
            fprintf(stderr, "error: failed to write position %u to %s\n",
                    generated + 1u, temp_path);
            success = false;
            break;
        }

        checksum = checksum_text(checksum, fen);
        ++generated;
    }

    if (success && fflush(output) != 0)
    {
        fprintf(stderr, "error: failed to flush %s\n", temp_path);
        success = false;
    }
    if (fclose(output) != 0)
    {
        fprintf(stderr, "error: failed to close %s\n", temp_path);
        success = false;
    }

    if (success && !replace_file(temp_path, output_path))
    {
        fprintf(stderr, "error: could not replace %s with completed output\n", output_path);
        success = false;
    }
    if (!success)
        (void)remove(temp_path);

    printf("generate summary: completed=%u retries=%u failures=%u cpu_seconds=%.3f checksum=%" PRIu64 "\n",
           generated, retries, success ? 0u : 1u, elapsed_seconds(start), checksum);

    free(temp_path);
    return success ? EXIT_SUCCESS : EXIT_FAILURE;
}
