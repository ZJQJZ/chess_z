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

static inline void xq_bb_set(XqBitboard *bb, XqSquare sq) {
    if (sq < 64) {
        bb->lo |= UINT64_C(1) << sq;
    } else {
        bb->hi |= UINT64_C(1) << (sq - 64);
    }
}

static inline void xq_bb_clear(XqBitboard *bb, XqSquare sq) {
    if (sq < 64) {
        bb->lo &= ~(UINT64_C(1) << sq);
    } else {
        bb->hi &= ~(UINT64_C(1) << (sq - 64));
    }
}

static inline bool xq_bb_test(XqBitboard bb, XqSquare sq) {
    if (sq < 64) {
        return (bb.lo & (UINT64_C(1) << sq)) != 0u;
    }
    return (bb.hi & (UINT64_C(1) << (sq - 64))) != 0u;
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
