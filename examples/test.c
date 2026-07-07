#include "xiangqi/engine.h"
#include "xiangqi/movegen.h"
#include "xiangqi/position.h"

int main()
{
    char *a = "r1ba1abnr/4k4/2n3c2/2p3p1p/p3C4/7C1/P1P1P1P1P/N8/7R1/R1BAKAB2 b - - 0 1";
    XqPosition pos;
    xq_position_from_fen(&pos, a);
    xq_position_print(&pos);
    return 0;
}