#ifndef XIANGQI_JIEQI_MOVEGEN_INTERNAL_JQ_H
#define XIANGQI_JIEQI_MOVEGEN_INTERNAL_JQ_H

#include "xiangqi/jieqi/movegen_jq.h"

/* A rook or cannon can reach at most all other squares on its rank and file. */
enum { XQ_JQ_MAX_PIECE_MOVES = XQ_FILES + XQ_RANKS - 2 };

typedef struct XqJqCachedPieceMoves
{
    XqBitboard dependencies;
    uint8_t count;
    int8_t piece;
    /* One-based indices. Zero means the row has no candidate of this kind. */
    uint8_t first_capture_plus_one;
    uint8_t king_capture_plus_one;
    XqMove moves[XQ_JQ_MAX_PIECE_MOVES];
} XqJqCachedPieceMoves;

/* One cache per position traversal. Zero initialization starts a new one. */
typedef struct XqJqMoveCache
{
    XqJqCachedPieceMoves rows[XQ_SQUARES];
    XqBitboard changed[XQ_COLOR_NB];
} XqJqMoveCache;

typedef struct XqJqMoveCacheSummary
{
    int count;
    bool king_capture;
} XqJqMoveCacheSummary;

/* Mark all rows for refresh before reusing the cache on an unrelated traversal. */
static inline void xq_jq_move_cache_reset(XqJqMoveCache *cache)
{
    XqBitboard all = {UINT64_MAX, (UINT64_C(1) << (XQ_SQUARES - 64)) - 1};
    for (int color = 0; color < XQ_COLOR_NB; ++color)
        cache->changed[color] = all;
}

/* Report both squares on make and unmake, including destinations that stay occupied. */
static inline void xq_jq_move_cache_invalidate(XqJqMoveCache *cache, XqSquare from, XqSquare to)
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

/* Refresh affected rows for the side to move and summarize the full candidate list. */
XqJqMoveCacheSummary xq_jq_move_cache_refresh(XqJqMoveCache *cache, const XqJqPosition *pos);

/* Collect all candidates or just captures from a cache refreshed for this position. */
void xq_jq_move_cache_collect(const XqJqMoveCache *cache, const XqJqPosition *pos,
                               XqJqMoveList *list, bool captures_only);

#endif
