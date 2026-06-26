#ifndef XIANGQI_BITBOARD_H
#define XIANGQI_BITBOARD_H

#include <stdbool.h>
#include <stdint.h>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

#include "xiangqi/types.h"

typedef struct XqBitboard {
    uint64_t lo;
    uint64_t hi;
} XqBitboard;

/**
 * 返回一个崭新的空棋盘
 */
static inline XqBitboard xq_bb_empty(void) {
    XqBitboard bb = {0u, 0u};
    return bb;
}

static inline bool xq_bb_is_empty(XqBitboard bb) {
    return bb.lo == 0u && bb.hi == 0u;
}

static inline XqBitboard xq_bb_from_square(XqSquare sq) {
    XqBitboard bb = {0u, 0u};
    if (sq < 64) {
        bb.lo = UINT64_C(1) << sq;
    } else {
        bb.hi = UINT64_C(1) << (sq - 64);
    }
    return bb;
}

/**
 * 设置 *bb 上 sq 位置的棋子标记
 */
static inline void xq_bb_set(XqBitboard *bb, XqSquare sq) {
    if (sq < 64) {
        bb->lo |= UINT64_C(1) << sq;
    } else {
        bb->hi |= UINT64_C(1) << (sq - 64);
    }
}

/**
 * 清除 *bb 上 sq 位置的棋子标记
 */
static inline void xq_bb_clear(XqBitboard *bb, XqSquare sq) {
    if (sq < 64) {
        bb->lo &= ~(UINT64_C(1) << sq);
    } else {
        bb->hi &= ~(UINT64_C(1) << (sq - 64));
    }
}

/**
 * 判断棋盘 bb 的 sq 位置是否有棋子
 */
static inline bool xq_bb_test(XqBitboard bb, XqSquare sq) {
    if (sq < 64) {
        return (bb.lo & (UINT64_C(1) << sq)) != 0u;
    }
    return (bb.hi & (UINT64_C(1) << (sq - 64))) != 0u;
}

/**
 * 棋盘编号从小到大第一个棋子出现的位置
 */
static inline XqSquare xq_bb_first_square(XqBitboard bb) {
    if (bb.lo != 0u) {
#if defined(_MSC_VER)
        unsigned long index;
        _BitScanForward64(&index, bb.lo);
        return (XqSquare)index;
#elif defined(__GNUC__) || defined(__clang__)
        return (XqSquare)__builtin_ctzll(bb.lo);
#else
        XqSquare sq = 0;
        while ((bb.lo & UINT64_C(1)) == 0u) {
            bb.lo >>= 1;
            ++sq;
        }
        return sq;
#endif
    }

    if (bb.hi != 0u) {
#if defined(_MSC_VER)
        unsigned long index;
        _BitScanForward64(&index, bb.hi);
        return (XqSquare)(64 + index);
#elif defined(__GNUC__) || defined(__clang__)
        return (XqSquare)(64 + __builtin_ctzll(bb.hi));
#else
        XqSquare sq = 64;
        while ((bb.hi & UINT64_C(1)) == 0u) {
            bb.hi >>= 1;
            ++sq;
        }
        return sq;
#endif
    }

    return XQ_NO_SQUARE;
}

static inline XqBitboard xq_bb_or(XqBitboard a, XqBitboard b) {
    XqBitboard bb = {a.lo | b.lo, a.hi | b.hi};
    return bb;
}

static inline XqBitboard xq_bb_and(XqBitboard a, XqBitboard b) {
    XqBitboard bb = {a.lo & b.lo, a.hi & b.hi};
    return bb;
}

static inline XqBitboard xq_bb_not(XqBitboard a) {
    XqBitboard bb = {~a.lo, ~a.hi & ((UINT64_C(1) << 26) - 1u)};
    return bb;
}

/**
 * 返回一个棋盘中有多少棋子
 */
static inline int xq_bb_count(XqBitboard bb) {
#if defined(_MSC_VER)
    return (int)(__popcnt64(bb.lo) + __popcnt64(bb.hi));
#elif defined(__GNUC__) || defined(__clang__)
    return __builtin_popcountll(bb.lo) + __builtin_popcountll(bb.hi);
#else
    int count = 0;
    while (bb.lo != 0u) {
        bb.lo &= bb.lo - 1u;
        ++count;
    }
    while (bb.hi != 0u) {
        bb.hi &= bb.hi - 1u;
        ++count;
    }
    return count;
#endif
}

#endif
