#include "xiangqi/engine.h"
#include "xiangqi/movegen.h"
#include "xiangqi/position.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/**
 * 将使用中文棋子名称首拼的 FEN 转换为标准英文棋子字母 FEN。
 * 只转换第一个空白字符之前的棋盘字段，行棋方等后续字段保持不变。
 * 红方大写、黑方小写的规则在转换后保持不变。
 *
 * 中文首拼映射为：j -> k、s -> a、x -> b、m -> n、c -> r、
 * p -> c、b/z -> p。
 *
 * @param chinese_fen 待转换的中文首拼 FEN 字符串。
 * @param english_fen 用于保存标准英文 FEN 的输出缓冲区。
 * @param buffer_size english_fen 缓冲区的容量，包括结尾的 '\0'。
 * @return 转换成功时返回 true；参数无效、缓冲区不足或棋盘字段中包含
 *         无法识别的字母时返回 false。
 */
static bool chinese_fen_to_english(const char *chinese_fen,
                                   char *english_fen,
                                   size_t buffer_size)
{
    bool in_board = true;
    size_t length;
    size_t i;

    if (chinese_fen == NULL || english_fen == NULL)
        return false;

    length = strlen(chinese_fen);
    if (buffer_size <= length)
        return false;

    for (i = 0; i < length; ++i)
    {
        unsigned char input = (unsigned char)chinese_fen[i];
        char output = (char)input;

        if (in_board && isspace(input))
            in_board = false;
        else if (in_board && isalpha(input))
        {
            bool uppercase = isupper(input) != 0;

            switch (tolower(input))
            {
            case 'j':
                output = 'k';
                break;
            case 's':
                output = 'a';
                break;
            case 'x':
                output = 'b';
                break;
            case 'm':
                output = 'n';
                break;
            case 'c':
                output = 'r';
                break;
            case 'p':
                output = 'c';
                break;
            case 'b':
            case 'z':
                output = 'p';
                break;
            default:
                english_fen[0] = '\0';
                return false;
            }

            if (uppercase)
                output = (char)toupper((unsigned char)output);
        }

        english_fen[i] = output;
    }

    english_fen[length] = '\0';
    return true;
}

int main()
{
    // const char *chinese_fen = "1mxsjsxmc/9/cp7/4P1z1z/z4p3/4B2P1/B2z2B1B/4X4/M3S3C/3CJSXM1 b - -";
    // const char *chinese_fen = "4B3P/C8/2M1j1PMx/4z1cc1/9/9/9/3m5/4z4/3J5 r - -";
    // const char *chinese_fen = "4jsC2/4s4/4C4/9/9/9/9/9/c8/5J3 b - -";
    // const char *chinese_fen = "cmxsj1xmc/3p5/3s5/b1bP2b2/9/9/B1B3B1B/XPM3p2/3JC4/C2S1SX2 b - -";
    // const char *chinese_fen = "2xsj2c1/4sB2C/1M2x4/4P3p/9/9/9/1m2z4/9/3J5 r - -";
    // const char *chinese_fen = "4B3P/C8/2M1j1PMx/4z1cc1/9/9/9/3m5/4z4/3J5 r - -";
    const char *chinese_fen = "4C4/3j2M2/7P1/9/2xp2x2/9/c8/9/3z1z3/4J1C2 r - -";
    char english_fen[128];
    XqPosition pos;

    if (!chinese_fen_to_english(chinese_fen, english_fen, sizeof(english_fen)))
    {
        fprintf(stderr, "failed to convert Chinese FEN\n");
        return 1;
    }

    printf("%s\n", english_fen);
    if (!xq_position_from_fen(&pos, english_fen))
    {
        fprintf(stderr, "converted FEN is invalid\n");
        return 1;
    }
    xq_position_print(&pos);
    return 0;
}
