#include "xiangqi/engine.h"
#include "xiangqi/movegen.h"
#include "xiangqi/position.h"

int main()
{
    // char *a = "r1ba1abnr/4k4/2n3c2/2p3p1p/p3C4/7C1/P1P1P1P1P/N8/7R1/R1BAKAB2 b - - 0 1";
    // char *a = "1nbakabnr/9/1c1R5/1r2C1pCp/p7c/4P4/P5P1P/4B4/N3AR3/4KABN1 b - -";
    char *a = "1nbakabnr/9/rc7/4C1p1p/p4c3/4P2C1/P2p2P1P/4B4/N3A3R/3RKABN1 b - -";
    XqPosition pos;
    xq_position_from_fen(&pos, a);
    xq_position_print(&pos);
    return 0;
}