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
    // const char *chinese_fen = "4C4/3j2M2/7P1/9/2xp2x2/9/c8/9/3z1z3/4J1C2 r - -";
    // const char *chinese_fen = "P1xsp1CM1/c2Csj3/7m1/1cB6/2M1B1z2/9/9/2m6/5z3/4Jp3 r - -";
    
    // 24 过
    // const char *chinese_fen = "5sx2/4j4/4xs1m1/9/4M4/4P1C2/9/3z5/2c1z4/3J5 r - -";

    // 25 过
    // const char *chinese_fen = "4js3/1M2s4/4p4/9/9/3C3C1/4p4/4X4/4S1cm1/3SJ1X2 r - -";

    // 122 过
    // const char *chinese_fen = "2c2j1M1/9/7C1/2zM2z2/1mx6/9/9/2mpp4/2z2z3/3J1z1c1 r - -";

    //123
    // const char *chinese_fen = "1BB1js3/C3sm3/9/9/p5xM1/c8/P8/9/4z1z2/5J3 r - -";

    // const char *chinese_fen = "cmxsj1x2/1C2s4/7cm/z7z/9/6P2/B3z3B/X6P1/4S4/3S1J1C1 r - -";
    // const char *chinese_fen = "cmxsjsx1c/9/8p/2C5z/6z2/4P1p2/z3B3m/X1M1P3M/6C2/3SJSX2 r - -";
    // const char *chinese_fen = "cmxsjsx1c/8p/9/4C3z/6z2/4P1p2/z3B3m/X1M1P3M/6C2/3SJSX2 r - -";
    // const char *chinese_fen = "2xj1s3/4s4/m7x/z3z3z/2B6/3mp1B2/B2c4B/4P3X/4M2C1/2X1JP1M1 r - -";
    // const char *chinese_fen = "2xsjs2c/3C1C3/c7x/z3zmz1z/2z6/4p4/B5p2/X1M1P1P1X/4S4/4JS3 b - -";


    // 途游 残局
    // 1
    // const char *chinese_fen = "4jsx2/1M2s4/4x4/3M5/9/9/6m1c/4X4/4S4/2XS1J3 r - -";
    // 2
    const char *chinese_fen = "3sj4/4s1B2/9/9/9/9/9/5c3/9/4J3C r - -";


    /*
    3
    3ak4/4c4/4bP3/1R3N3/2b6/3nC1p2/9/9/2r1r4/3K5 r - -

    4
    7P1/2N4P1/5k3/9/9/4N4/5n3/3p5/4p4/3K5 r - -

    5
    c4kC2/1P2P4/9/9/9/9/9/6nC1/9/3K1A1r1 r - -

    6
    1r1k1P3/2PCa4/C4a3/9/9/9/4p4/9/1p7/4K4 r - -

    7
    9/4k1r1n/4b1r1n/9/9/9/9/5p3/4p1p1c/R1R2K2c r - -

    8
    2ba1k2C/1c2a1P2/1c2b1P1n/9/9/9/9/4B4/4AC3/2BAK4 r - -

    9
    2bak4/4aR3/4b4/4C2r1/9/9/9/2R6/3p1p3/2p1K1p2 r - -

    10
    2bk2b2/4P1N2/2c4R1/9/9/9/9/4B2C1/3r1p3/4K4 r - -
    
    11
    4k3C/3Pa1P2/5a3/3Np4/9/9/9/9/1r1rp4/5K3 r - -

    12
    2bak1P2/1N2aP3/4b4/9/2n1N4/9/9/9/1r2p4/5K3 r - -

    13
    3k5/4a4/9/9/9/3N5/9/9/3p1pr2/1R2K4 r - -
    
    14
    2ba5/4k3C/3ab4/9/9/9/9/9/1rp1p1CR1/3K5 r - -

    15
    2bak4/4aP3/4b1n2/9/9/6C1R/4c4/4B4/4Ap1n1/2BAK4 r - -

    16
    2b1ka3/4a2N1/4b4/1R2p4/9/r8/4c2n1/4B4/4A3r/2BAKR3 r - -

    17
    3ak4/4a1R2/4b4/6N2/1r2N4/9/9/9/5p3/4K4 r - -

    18
    5kc2/3r3R1/5a3/4p4/8C/7C1/9/9/3p1pp2/4K4 r - -

    19
    R4a3/2rc5/5k3/3P5/1R7/9/9/5p3/4p4/5K3 r - -

    20
    2bak2r1/4aP2R/1N2b4/4C3c/9/9/9/1n2p4/9/3K5 r - -

    21
    3k5/4a4/3a5/N8/9/7R1/9/2n6/3r1p3/4K4 r - -

    22
    3aka3/5P3/4b4/r3N4/9/8R/9/2n6/9/4KA3 r - -

    23
    1C2k4/9/3a5/9/9/9/2R1P4/6n2/C2p1r3/4K3c r - -

    24
    5ab2/4k4/4ba1n1/9/4N4/4C1R2/9/3p5/2r1p4/3K5 r - -

    25
    4ka3/1N2a4/4c4/9/9/3R3R1/4c4/4B4/4A1rn1/3AK1B2 r - -

    26
    c1ba5/4k1N2/3abR3/9/9/9/9/5C3/2p1p4/1p1K5 r - -

    27
    8R/5k3/2N4r1/9/1n3n3/4C4/9/5p3/2r1p1p2/5Kc2 r - -

    28
    3ak1b2/4a4/4b4/9/6N2/9/6r2/4C3C/4p4/2B2K3 r - -

    29
    3a2b1N/4ak1P1/9/9/9/9/9/8C/3r1p3/4K4 r - -

    30
    4k4/4a4/2P5n/5N3/9/5R3/9/9/2p2p2r/C3K4 r - -

    31
    3a1k3/1N2a4/9/6N2/4R4/9/9/9/3p1r3/4K4 r - -

    32
    3a1a1C1/2PcPnC2/2nkb3R/9/2b6/9/9/9/5p3/2rAK1p2 r - -
    
    33
    3P1kb1R/2C1a4/2R1b4/2C6/9/9/9/3r1p3/4K4/3p1r3 r - -

    34
    3k2b2/4PP3/2P1b4/p8/9/4R4/P3R3P/2p1BA1p1/3r1p3/1p2K1B2 r - -

    35
    2b1k1PP1/4a1R2/4baN2/9/8r/2R6/9/3p5/2nr5/3AK4 r - -

    36
    2bak4/C2Ra4/N3b3R/1C5n1/9/2Bn5/9/B8/c1rp1p3/4Kc3 r - -

    37
    2bak4/C3a2NR/4b4/9/9/1C2c4/9/2n1B4/1r2A1c2/3RKAr2 r - -

    38
    r4k3/4a1P2/1n3a3/4N4/9/9/9/4C4/4p4/2BK5 r - -

    39
    9/5k2c/c5P1b/2r3N2/9/6n2/9/8R/1r1p5/4K1p1C r - 

    40
    9/1RNk2nr1/1Nc4RC/7n1/9/9/9/9/r3p4/4CK3 r - -

    41
    4ka3/3P1P3/4b4/9/9/9/9/9/3p2p2/4K4 r - -

    42
    5k3/9/9/9/9/7r1/4R4/4B4/9/4K1B2 r - -

    43
    2rak1b2/3R5/4b4/9/1n7/5R3/9/4B4/9/4K4 r - -

    44
    2n1k4/2P1a4/1C1r1a3/9/9/9/9/9/9/4K4 r - -

    45
    1R1ak1b1R/9/1CNc3r1/4p4/6p1p/9/P8/4B1n2/2nCA2r1/2BAK3c r - -

    46
    4kab2/4a4/2Ncb2C1/6p2/9/1R3RP2/P1r4r1/4B4/2n1A4/2BA1K3 r - -

    47
    3C1a2c/9/C2kbN3/4P4/9/9/9/9/r4p3/4K4 r - -

    48
    3k1a3/4a4/9/2R6/N8/9/6n2/5K3/4r4/9 r - -

    49
    4ka1R1/2PP5/3an3b/4p4/6b2/2C6/9/4C4/1rp1p4/3K5 r - -

    50
    3k1n3/7rc/9/4N1N2/3C5/3R5/9/2n6/5rp2/4K2c1 r - -

    51
    4k1b2/4aP3/4c4/CCR6/2b6/9/5r1n1/4B4/4A2r1/2B1K3c r - -

    52
    2ba1a2R/3k5/9/3N5/9/8R/9/3pB4/4rp3/3K5 r - -

    53
    2b2a3/3ka4/4b2C1/9/4N4/9/9/4C4/3r1p3/2B1KAB2 r - -

    54
    1N7/4ak3/2Ra5/9/4N4/9/9/5rn2/3p5/3AK1B2 r - -

    55
    4k4/4aRP2/r2Rba3/9/2b6/r3C4/9/2C6/4p4/5K1n1 r - -

    56
    3ack3/1NR1a1P2/3n2R2/9/2C6/9/9/9/1r2p2r1/3K5 r - -

    57
    4rc3/4ak3/9/9/9/9/9/2R6/3pA4/cr1AKC3 r - -

    58
    2b1ka3/3Ra4/1c2b3c/N8/R8/2B6/4C1r2/C4n3/2r1Ap1n1/3AK1B2 r - -

    59
    2ba1kb2/1P2a1R2/3cnrN2/C8/9/9/9/9/4p4/3K5 r - -

    60
    5k2C/6N2/4b2n1/4p4/8N/9/8R/9/1c1p1p3/2p1K4 r - -

    61
    3a1k3/1Nc1a1R2/5rN2/9/9/9/9/9/3p5/4K1Rrc r - -

    62
    1C1NR4/1C2c3R/5k3/6n1N/8r/9/9/1r2n4/4pc3/3K5 r - -

    63
    3kcr3/4N4/4N4/9/9/R3p4/C8/3Ap4/2pR1rn2/c3K4 r - -

    64
    R1b2k3/4a1P2/b2a5/2p6/5P3/8r/9/2n6/C2p3r1/RCc1K4 r - -

    65
    5a2C/3NakCR1/6Nc1/3r2P1p/7n1/9/9/9/3p1p3/4K2cr r - -

    66
    3akarr1/2C2P2C/2N6/9/R2P5/9/9/4B1p2/3pp4/2B2K3 r - -

    67
    3a5/3r5/3kc2PC/6P2/5Np2/9/9/3r1p3/4p4/5K3 r - -

    68
    2N6/2r6/bC3k3/9/4p4/9/6r2/5n3/9/2RK2c2 r - -

    69
    2n4R1/7N1/4k2n1/5P3/9/9/4p4/3p2C2/4p1p2/5K3 r - -

    70
    6b2/2Rn5/3P1k3/4N4/9/9/7r1/5p3/4p4/5K3 r - -

    71
    3ak4/4a4/4b3N/c4P3/2b1C4/9/9/5A3/1n1KA4/9 r - -

    72
    1nbak4/4a2Rc/4b4/9/9/9/2C3C2/6n2/2r2p3/3AKA3 r - -

    73
    3a5/4ak3/9/3R5/5P3/r8/9/7C1/r2p5/4K4 r - -

    74
    3a5/4ac3/2C1k4/9/9/1N7/9/4C4/3pA4/4KAnc1 r - -

    75
    2b1k4/9/n4a3/1N1R5/6p2/9/9/3C5/4p1r2/5K3 r - -

    76
    4k3C/5N1rC/5c3/4p4/4N4/9/9/6n2/2r2p3/3RK4 r - -

    77
    3ak4/9/4N2r1/1n2p1N2/9/8R/9/4B4/3p5/4K4 r - -

    78
    4kab2/4a4/N2cb4/1N7/9/2R6/9/9/5p3/4KA1r1 r - -

    79
    1c2k1C2/c4P3/5a3/9/9/9/9/7R1/3rp4/5K1C1 r - -

    80
    3k1aP2/8C/5a3/4R1N2/9/9/9/6n2/3r1p3/4K4 r - -

    81
    2bk5/1R5N1/9/9/9/9/9/5Cr2/r2p1p3/4K2R1 r - -

    82
    2b1ka3/4a3c/4b4/6n2/7RN/9/9/3CCR3/2rp1p3/4KAr1c r - -

    83
    4ka3/2R1aR3/3cb4/9/3N5/9/9/C3C4/3p1pnr1/4K4 r - -

    84
    2bakN3/4aP3/4b4/9/9/1C7/C3c4/4B1c2/4Ar3/3RKA1r1 r - -

    85
    3a1k3/2R1a4/3cb1n2/1C2N4/2b1C3R/9/9/9/r1r2p3/4K4 r - -

    86
    5k1n1/c3a4/5aR1b/6N2/2r5C/9/9/9/3p1p3/4K1R2 r - -

    87
    2b1ka3/4a3c/4b4/6n2/7RN/9/9/3CCR3/2rp1p3/4KAr1c r - -

    88
    2bak4/1C2a1n2/4b2Rr/4p4/2P3N2/9/5n3/2C1B4/4A4/4KAB1c r - -

    89
    1rb4C1/5k3/2Ncb4/9/9/3r5/9/5CR2/4p4/5K1p1 r - -

    90
    3akarr1/1NC2P2C/9/9/R8/9/6p2/4B4/2n1p4/5K3 r - -

    91
    2Rakc3/4aR3/2P1b1n2/4C4/6b2/2B6/4c4/2n1B3C/1r2A1p2/4KA3 r - -

    92
    5k3/2PR5/5a3/1c7/4PN3/9/9/9/4p1r2/3c1K3 r - -

    93
    6b2/3ka4/4ba3/9/9/6R2/6p2/5Rn2/9/3AKA1rc r - -

    94
    2bak3C/4a4/4c3b/6R2/9/4P2R1/r8/2n1B4/3r5/4KAB2 r - -

    95
    5a3/3k3N1/1R1cba3/6N2/5r3/9/9/9/4p4/3K2B2 r - -

    96
    3k1ab2/1R1Na4/4b4/7n1/8C/9/9/r8/3p5/4KA3 r - -

    97
    2b6/3ka2R1/5a3/N8/6N2/9/9/4Bp3/3r5/c4K3 r - -

    98
    4kab2/9/3abR3/1N1N5/9/9/9/7r1/3p5/4K1p2 r - -

    99
    2b6/c1P1ak3/1NP4Rc/9/4RPb2/9/6n2/4B4/2p3p1r/3rCK3 r - -

    100
    2nnPab2/3k5/3ab4/C3p4/4Rcp2/1N3r3/9/4Bp1R1/4r4/5K3 r - -

    101
    2baka3/3P3N1/bN7/7nc/9/4C1P2/P5n1P/B3R3B/4Apr2/2RAK3c r - -

    102
    1nb2kr2/3Pa2R1/3nba3/N3C2C1/6r2/9/9/9/9/5K3 r - -

    103
    4ka3/4a4/b3c4/nr7/2p3b2/2PR5/4C4/Bp2C4/4A4/c3KAB2 r - -

    104
    3akab2/9/9/1n5N1/2p5C/7cR/9/4B4/4p4/1n1K2B2 r - -

    105
    5a3/9/3kb4/2N6/9/6C2/7R1/1n1p5/4p4/5K3 r - -

    106
    2b2k3/1R2a1P2/3a5/4p1N2/6b2/9/3nP4/4C1C2/3rAp3/c1nAK4 r - -

    107
    2b2k3/1R2a1P2/3a5/4p1N2/6b2/9/3nP4/4C1C2/3rAp3/c1nAK4 r - -

    108
    C3k4/4a1r2/5a3/1N7/1Cp6/1R7/9/3p5/4p3r/3K5 r - -

    109
    2ba1k3/4a4/9/1CR1C4/9/2B6/2p6/3KBA3/r3r3c/6R2 r - -

    110
    2bak4/3Ra2R1/2n2c2C/5rpNn/2b6/9/4PC3/2r1BA3/4Ap3/c3K4 r - -

    111
    9/1RcPck3/b2aPNP2/1R7/9/C1N6/C8/3n4p/3nr2r1/5K1p1 r - -

    112
    1n2ka3/R3a1P2/1P2b1n1C/5R3/2b1P4/1N7/9/5p3/4r4/3p1K3 r - -
    
    113
    3a4R/1C2k4/3N5/9/9/r8/9/3p5/4p4/3K5 r - -

    114
    c4a3/2N1knR2/9/4P4/7C1/6n2/9/1r1p5/4p4/3K5 r - -
    
    115
    4kabC1/4a4/4b4/1R7/4P1N2/2N6/5n3/5p3/4p2r1/5K3 r - -

    116
    9/4a4/3k1a3/1NP1p4/9/8C/4P4/cr1AKA3/5r3/2n6 r - -

    117
    4kaPP1/c2Pa2nR/4b1P1r/6n2/6bN1/9/4C1R2/9/1r1p1p3/4K4 r - -

    118
    2rk2N2/5P3/1n7/C5p1p/9/9/R8/9/3p1p1c1/4K1p2 r - -

    119
    C1bac1RN1/r2Rak3/7n1/1rP6/2N1P1p2/9/9/2n6/5p3/4Kc3 r - -

    120
    4P3C/R8/2N1k1CNb/4p1rr1/9/9/9/3n5/4p4/3K5 r - -
    */


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
