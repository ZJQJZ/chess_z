#ifndef XIANGQI_MOVEGEN_INTERNAL_H
#define XIANGQI_MOVEGEN_INTERNAL_H

#include "xiangqi/movegen.h"

/* A rook or cannon can reach at most all other squares on its rank and file. */
enum { XQ_MAX_PIECE_MOVES = XQ_FILES + XQ_RANKS - 2 };

typedef struct XqCachedPieceMoves
{
    /* Keep the refresh metadata together, ahead of the move storage. */
    XqBitboard dependencies;
    uint8_t count;
    int8_t piece;
    bool king_capture;
    _Alignas(uint32_t) XqMove moves[XQ_MAX_PIECE_MOVES];
    /* Points into moves, or is NULL when there is no capture. */
    XqMove *first_capture;
} XqCachedPieceMoves;

typedef struct XqCachedKingCheck
{
    XqBitboard dependencies;
    XqSquare king;
    bool in_check;
    bool valid;
} XqCachedKingCheck;

/* One cache per position traversal. Zero initialize and keep populated rows at a fixed address. */
typedef struct XqMoveCache
{
    XqCachedPieceMoves rows[XQ_SQUARES];
    XqBitboard changed[XQ_COLOR_NB];
    XqCachedKingCheck king_check[XQ_COLOR_NB];
} XqMoveCache;

typedef struct XqMoveCacheSummary
{
    int count;
    bool king_capture;
    bool in_check; /* Available when king_capture is false. */
} XqMoveCacheSummary;

/* Reuse the storage on an unrelated traversal by invalidating rows and check results. */
static inline void xq_move_cache_reset(XqMoveCache *cache)
{
    XqBitboard all = {UINT64_MAX, (UINT64_C(1) << (XQ_SQUARES - 64)) - 1};
    for (int color = 0; color < XQ_COLOR_NB; ++color)
    {
        cache->changed[color] = all;
        cache->king_check[color].valid = false;
    }
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

/* Refresh affected rows and check status for pos->side_to_move. The count is capped at
 * XQ_MAX_MOVES. */
XqMoveCacheSummary xq_move_cache_refresh(XqMoveCache *cache, const XqPosition *pos);

/* Collect from a refreshed cache in generator order, applying the list limit before filtering. */
void xq_move_cache_collect(const XqMoveCache *cache, const XqPosition *pos, XqMoveList *list,
                           bool captures_only);

#endif
