#ifndef XIANGQI_BITBOARD_H
#define XIANGQI_BITBOARD_H

#include <stdbool.h>
#include <stdint.h>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

#include "xiangqi/types.h"

/**
 * 用两个 64 位无符号整数表示中国象棋棋盘的 90 个位置
 * lo 的第 0..63 位对应棋盘位置 0..63；hi 的第 0..25 位对应位置 64..89，
 * hi 剩余的高 38 位不属于棋盘，应始终保持为 0
 */
typedef struct XqBitboard
{
    uint64_t lo;
    uint64_t hi;
} XqBitboard;

/**
 * @brief 创建一个不包含任何棋盘位置的空位棋盘
 *
 * @return lo 和 hi 均为 0 的 XqBitboard
 */
static inline XqBitboard xq_bb_empty(void)
{
    XqBitboard bb = {0u, 0u};
    return bb;
}

/**
 * @brief 看棋盘上是否没有棋子
 *
 * @param bb 要检查的位棋盘
 * @return bb 中没有任何位置被设置时返回 true，否则返回 false
 */
static inline bool xq_bb_is_empty(XqBitboard bb)
{
    return (bb.lo | bb.hi) == 0u;
}

/**
 * @brief 设置 bb 上 sq 位置的棋子标记
 *
 * @param bb 要修改的位棋盘，不能为 NULL
 * @param sq 要设置的棋盘位置，取值范围应为 0..89
 */
static inline void xq_bb_set(XqBitboard *bb,
                             XqSquare    sq)
{
    if (sq < 64)
        bb->lo |= UINT64_C(1) << sq;
    else
        bb->hi |= UINT64_C(1) << (sq - 64);
}

/**
 * @brief 清除 bb 上 sq 位置的棋子标记
 *
 * @param bb 要修改的位棋盘，不能为 NULL
 * @param sq 要清除的棋盘位置，取值范围应为 0..89
 */
static inline void xq_bb_clear(XqBitboard *bb,
                               XqSquare    sq)
{
    if (sq < 64)
        bb->lo &= ~(UINT64_C(1) << sq);
    else
        bb->hi &= ~(UINT64_C(1) << (sq - 64));
}

/**
 * @brief 判断棋盘 bb 的 sq 位置是否有棋子
 *
 * @param bb 要检查的位棋盘
 * @param sq 要检查的棋盘位置，取值范围应为 0..89
 * @return sq 对应位已被设置时返回 true，否则返回 false
 */
static inline bool xq_bb_test(XqBitboard bb,
                              XqSquare   sq)
{
    if (sq < 64)
        return (bb.lo & (UINT64_C(1) << sq)) != 0u;
    return (bb.hi & (UINT64_C(1) << (sq - 64))) != 0u;
}

/**
 * @brief 返回非零 64 位整数中最低置位的索引
 *
 * MSVC、GCC 和 Clang 使用内建位扫描，其他编译器逐位右移
 *
 * @param bits 要扫描的非零整数
 * @return 最低置位的索引，范围为 0..63
 */
static inline unsigned xq_bb_first_bit_index(uint64_t bits)
{
#if defined(_MSC_VER)
    unsigned long index;
    _BitScanForward64(&index, bits);
    return (unsigned)index;
#elif defined(__GNUC__) || defined(__clang__)
    return (unsigned)__builtin_ctzll(bits);
#else
    unsigned index = 0;
    while ((bits & UINT64_C(1)) == 0u)
    {
        bits >>= 1;
        ++index;
    }
    return index;
#endif
}

/**
 * @brief 返回位棋盘中编号最小的已置位格子
 *
 * @param bb 要查找的位棋盘
 * @return 第一个被设置的位置编号，bb 为空时返回 XQ_NO_SQUARE
 */
static inline XqSquare xq_bb_first_square(XqBitboard bb)
{
    if (bb.lo != 0u)
        return (XqSquare)xq_bb_first_bit_index(bb.lo);
    if (bb.hi != 0u)
        return (XqSquare)(64 + xq_bb_first_bit_index(bb.hi));
    return XQ_NO_SQUARE;
}

/**
 * @brief 取出并清除位棋盘中编号最小的已置位格子
 *
 * 先处理 lo，再处理 hi。对非零的那一段执行 bits & (bits - 1) 可清除最低置位
 *
 * @param bb 要修改的位棋盘，不能为 NULL
 * @return 被清除的位置编号，bb 为空时返回 XQ_NO_SQUARE
 */
static inline XqSquare xq_bb_pop_first_square(XqBitboard *bb)
{
    uint64_t bits = bb->lo;
    if (bits != 0u)
    {
        bb->lo = bits & (bits - 1u);
        return (XqSquare)xq_bb_first_bit_index(bits);
    }
    bits = bb->hi;
    if (bits != 0u)
    {
        bb->hi = bits & (bits - 1u);
        return (XqSquare)(64 + xq_bb_first_bit_index(bits));
    }
    return XQ_NO_SQUARE;
}

/**
 * @brief 返回一个棋盘中有多少棋子
 *
 * 根据编译器预定义宏选择对应的位计数实现
 * MSVC 使用 __popcnt64，GCC 和 Clang 使用 __builtin_popcountll
 * 其他编译器使用逐次清除最低有效位的通用实现
 *
 * @param bb 要统计已设置位数量的位棋盘
 * @return 位棋盘中已设置为 1 的位数
 */
static inline int xq_bb_count(XqBitboard bb)
{
#if defined(_MSC_VER)
    /* MSVC 使用 __popcnt64 分别统计 lo 和 hi 中已设置的位 */
    return (int)(__popcnt64(bb.lo) + __popcnt64(bb.hi));
#elif defined(__GNUC__) || defined(__clang__)
    /* GCC 和 Clang 使用 __builtin_popcountll 分别统计 lo 和 hi 中已设置的位 */
    return __builtin_popcountll(bb.lo) + __builtin_popcountll(bb.hi);
#else
    /* 其他编译器使用逐次清除最低有效位的通用实现 */
    int count = 0;
    while (bb.lo != 0u)
    {
        bb.lo &= bb.lo - 1u;
        ++count;
    }
    while (bb.hi != 0u)
    {
        bb.hi &= bb.hi - 1u;
        ++count;
    }
    return count;
#endif
}

#endif
