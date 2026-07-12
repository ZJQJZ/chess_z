#include "xiangqi/engine.h"
#include "xiangqi/movegen.h"
#include "xiangqi/position.h"

#include <stdio.h>

typedef struct ScoredMove
{
    XqMove move;
    int score;
} ScoredMove;

typedef struct PrincipalVariation
{
    XqMove moves[64];
    int count;
} PrincipalVariation;

typedef struct XqTranspositionEntry
{
    uint64_t key;
    int score;
    XqMove best_move;
    uint16_t depth;
    uint8_t generation;
    uint8_t score_kind;
} XqTranspositionEntry;

typedef struct XqTranspositionBucket
{
    XqTranspositionEntry entries[4];
} XqTranspositionBucket;

struct XqTranspositionTable
{
    XqTranspositionBucket *buckets; // size: 1 << 18
    uint8_t generation;
    XqTranspositionStats stats;
};

int main()
{
    // char *a = "r1ba1abnr/4k4/2n3c2/2p3p1p/p3C4/7C1/P1P1P1P1P/N8/7R1/R1BAKAB2 b - - 0 1";
    // char *a = "1nbakabnr/9/1c1R5/1r2C1pCp/p7c/4P4/P5P1P/4B4/N3AR3/4KABN1 b - -";
    char *a = "1nbakabnr/9/rc7/4C1p1p/p4c3/4P2C1/P2p2P1P/4B4/N3A3R/3RKABN1 b - -";
    XqPosition pos;
    xq_position_from_fen(&pos, a);
    xq_position_print(&pos);
    printf("%zu\n", sizeof(XqTranspositionEntry));
    printf("%zu\n", sizeof(XqTranspositionBucket));
    return 0;
}