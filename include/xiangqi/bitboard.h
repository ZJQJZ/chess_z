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
 * 创建一个不包含任何棋盘位置的空位棋盘
 *
 * @return lo 和 hi 均为 0 的 XqBitboard
 */
static inline XqBitboard xq_bb_empty(void)
{
    XqBitboard bb = {0u, 0u};
    return bb;
}

/**
 * 看棋盘上是否没有棋子
 *
 * @param bb 要检查的位棋盘
 * @return bb 中没有任何位置被设置时返回 true，否则返回 false
 */
static inline bool xq_bb_is_empty(XqBitboard bb)
{
    return bb.lo == 0u && bb.hi == 0u;
}

/**
 * 设置 *bb 上 sq 位置的棋子标记
 *
 * @param bb 要修改的位棋盘，不能为 NULL
 * @param sq 要设置的棋盘位置，取值范围应为 0..89
 */
static inline void xq_bb_set(XqBitboard *bb, XqSquare sq)
{
    if (sq < 64)
        bb->lo |= UINT64_C(1) << sq;
    else
        bb->hi |= UINT64_C(1) << (sq - 64);
}

/**
 * 清除 bb 上 sq 位置的棋子标记
 *
 * @param bb 要修改的位棋盘，不能为 NULL
 * @param sq 要清除的棋盘位置，取值范围应为 0..89
 */
static inline void xq_bb_clear(XqBitboard *bb, XqSquare sq)
{
    if (sq < 64)
        bb->lo &= ~(UINT64_C(1) << sq);
    else
        bb->hi &= ~(UINT64_C(1) << (sq - 64));
}

/**
 * 判断棋盘 bb 的 sq 位置是否有棋子
 *
 * @param bb 要检查的位棋盘
 * @param sq 要检查的棋盘位置，取值范围应为 0..89
 * @return sq 对应位已被设置时返回 true，否则返回 false
 */
static inline bool xq_bb_test(XqBitboard bb, XqSquare sq)
{
    if (sq < 64)
        return (bb.lo & (UINT64_C(1) << sq)) != 0u;
    return (bb.hi & (UINT64_C(1) << (sq - 64))) != 0u;
}

/**
 * 棋盘编号从小到大第一个棋子出现的位置
 * 从编号 0 开始查找位棋盘中第一个被设置的位置，优先检查 lo，
 * 再检查 hi；位棋盘为空时返回 XQ_NO_SQUARE
 * 实现会根据编译器预定义宏选择最高效的位扫描方式：MSVC 使用
 * _BitScanForward64，GCC 和 Clang 使用 __builtin_ctzll；其他编译器
 * 则使用逐位右移的可移植实现作为兜底
 *
 * @param bb 要查找的位棋盘
 * @return 第一个被设置的位置编号；bb 为空时返回 XQ_NO_SQUARE
 */
static inline XqSquare xq_bb_first_square(XqBitboard bb)
{
    if (bb.lo != 0u)
    {
#if defined(_MSC_VER)
        /* MSVC：使用编译器内建指令获取最低有效位的索引 */
        unsigned long index;
        _BitScanForward64(&index, bb.lo);
        return (XqSquare)index;
#elif defined(__GNUC__) || defined(__clang__)
        /* GCC/Clang：末尾零的数量就是最低有效位的索引 */
        return (XqSquare)__builtin_ctzll(bb.lo);
#else
        /* 其他编译器：逐位右移，直到最低位为 1 */
        XqSquare sq = 0;
        while ((bb.lo & UINT64_C(1)) == 0u)
        {
            bb.lo >>= 1;
            ++sq;
        }
        return sq;
#endif
    }

    if (bb.hi != 0u)
    {
#if defined(_MSC_VER)
        /* MSVC：扫描 hi 后加 64，转换为完整棋盘位置编号 */
        unsigned long index;
        _BitScanForward64(&index, bb.hi);
        return (XqSquare)(64 + index);
#elif defined(__GNUC__) || defined(__clang__)
        /* GCC/Clang：扫描 hi，并加 64 转换为完整棋盘位置编号 */
        return (XqSquare)(64 + __builtin_ctzll(bb.hi));
#else
        /* 其他编译器：从棋盘位置 64 开始逐位查找 */
        XqSquare sq = 64;
        while ((bb.hi & UINT64_C(1)) == 0u)
        {
            bb.hi >>= 1;
            ++sq;
        }
        return sq;
#endif
    }

    return XQ_NO_SQUARE;
}

/**
 * 返回一个棋盘中有多少棋子
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
