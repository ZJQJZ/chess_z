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
    /* Pointers into moves, or NULL when there is no candidate of this kind. */
    XqMove *first_capture;
    XqMove *king_capture;
} XqCachedPieceMoves;

/* One cache per position traversal. Zero initialize and keep populated rows at a fixed address. */
typedef struct XqMoveCache
{
    XqCachedPieceMoves rows[XQ_SQUARES];
    XqBitboard changed[XQ_COLOR_NB];
} XqMoveCache;

typedef struct XqMoveCacheSummary
{
    int count;
    bool king_capture;
} XqMoveCacheSummary;

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

/* Refresh affected rows for pos->side_to_move. The summary describes the first XQ_MAX_MOVES. */
XqMoveCacheSummary xq_move_cache_refresh(XqMoveCache *cache, const XqPosition *pos);

/* Collect from a refreshed cache in generator order, applying the list limit before filtering. */
void xq_move_cache_collect(const XqMoveCache *cache, const XqPosition *pos, XqMoveList *list,
                           bool captures_only);

#endif
