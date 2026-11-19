#ifndef XIANGQI_MOVEGEN_INTERNAL_H
#define XIANGQI_MOVEGEN_INTERNAL_H

#include "xiangqi/movegen.h"

/* A rook or cannon can reach at most all other squares on its rank and file. */
enum { XQ_MAX_PIECE_MOVES = XQ_FILES + XQ_RANKS - 2 };

typedef struct XqCachedPieceMoves
{
    XqBitboard dependencies;
    XqMove moves[XQ_MAX_PIECE_MOVES];
    uint8_t count;
    int8_t piece;
} XqCachedPieceMoves;

/* One cache per position traversal. Zero initialization starts a new one. */
typedef struct XqMoveCache
{
    XqCachedPieceMoves rows[XQ_SQUARES];
    XqBitboard changed[XQ_COLOR_NB];
} XqMoveCache;

/* Reuse the storage on an unrelated traversal by marking every square changed. */
static inline void xq_move_cache_reset(XqMoveCache *cache)
{
    XqBitboard all = {UINT64_MAX, (UINT64_C(1) << (XQ_SQUARES - 64)) - 1};
    for (int color = 0; color < XQ_COLOR_NB; ++color)
        cache->changed[color] = all;
}

/* Report each changed square before collecting again; for a single square pass it twice.
 * This includes an occupied destination whose piece changed after a capture. */
static inline void xq_move_cache_invalidate(XqMoveCache *cache, XqSquare from, XqSquare to)
{
    XqBitboard changed = xq_bb_empty();
    xq_bb_set(&changed, from);
    xq_bb_set(&changed, to);
    for (int color = 0; color < XQ_COLOR_NB; ++color)
    {
        cache->changed[color].lo |= changed.lo;
        cache->changed[color].hi |= changed.hi;
    }
}

/* Refresh affected rows for pos->side_to_move, then emit in the full generator's order. */
void xq_move_cache_generate(XqMoveCache *cache, const XqPosition *pos, XqMoveList *list);

#endif
