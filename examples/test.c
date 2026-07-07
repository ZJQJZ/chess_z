#include "xiangqi/engine.h"
#include "xiangqi/movegen.h"
#include "xiangqi/position.h"

int main()
{
    char *a = "1CCa2b1r/4a4/5kn2/p1N1c3p/6R2/9/4P4/9/4A4/2B1KcB2 b - -";
    XqPosition pos;
    xq_position_from_fen(&pos, a);
    xq_position_print(&pos);
    return 0;
}