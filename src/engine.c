#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "xiangqi/engine.h"

#include "xiangqi/movegen.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#endif

static const int piece_values[XQ_PIECE_TYPE_NB] = {
    [XQ_KING] = 10000, [XQ_ADVISOR] = 120, [XQ_BISHOP] = 120, [XQ_KNIGHT] = 270,
    [XQ_ROOK] = 600,   [XQ_CANNON] = 285,  [XQ_PAWN] = 70,
};

static const int legal_move_values[XQ_PIECE_TYPE_NB] = {
    [XQ_KING] = 10, [XQ_ADVISOR] = 10, [XQ_BISHOP] = 10, [XQ_KNIGHT] = 20,
    [XQ_ROOK] = 20, [XQ_CANNON] = 15,  [XQ_PAWN] = 10,
};

enum
{
    CAPTURE_ORDER_BASE = 100000,
    XQ_MAX_QUIESCENCE_DEPTH = 500,
    XQ_MAX_PV_MOVES = 64,
    XQ_MATE_SCORE = 30000,
    XQ_MATE_THRESHOLD = 29000,
    XQ_TT_BUCKET_SIZE = 4,
    XQ_TT_BUCKET_COUNT = 1 << 18
};

typedef struct ScoredMove
{
    XqMove move;
    int score;
    unsigned completed_depth;
} ScoredMove;

typedef struct SearchContext
{
    uint64_t deadline_ms;
    uint64_t nodes;
    bool time_limited;
    bool stopped;
    XqSearchTimeMode time_mode;
} SearchContext;

typedef struct PrincipalVariation
{
    XqMove moves[XQ_MAX_PV_MOVES];
    int count;
} PrincipalVariation;

typedef struct XqTranspositionEntry
{
    uint64_t key;
    int score;
    XqMove best_move;
    uint16_t depth;
    uint8_t generation;
    uint8_t score_kind;
} XqTranspositionEntry;

typedef struct XqTranspositionBucket
{
    XqTranspositionEntry entries[XQ_TT_BUCKET_SIZE];
} XqTranspositionBucket;

struct XqTranspositionTable
{
    XqTranspositionBucket *buckets; // size: 1 << 18
    uint8_t generation;
    XqTranspositionStats stats;
};

/**
 * @brief  Reads the current monotonic clock and converts it to milliseconds.
 *
 * The monotonic clock is unaffected by changes to the system date, time zone or manual clock
 * adjustments, making it suitable for measuring search duration and deadlines. Windows uses a
 * high-resolution performance counter, while other platforms use `clock_gettime(CLOCK_MONOTONIC)`.
 * Any fractional millisecond is discarded during conversion.
 *
 * @return The number of milliseconds elapsed since a platform-specific fixed reference point, or 0
 *         if the clock cannot be read. The value alone is meaningless; only the difference between
 *         two calls is meaningful.
 */
static uint64_t monotonic_time_ms(void)
{
#ifdef _WIN32
    LARGE_INTEGER counter;
    LARGE_INTEGER frequency;

    /*
     * Both `counter` and `frequency` use counter ticks as the unit for QuadPart, but their QuadPart
     * values have different meanings:
     *     - `counter`'s represents the total number of ticks accumulated so far.
     *     - `frequency`'s represents the number of ticks added per second by the high-resolution
     *       performance counter.
     */
    if (!QueryPerformanceFrequency(&frequency) || !QueryPerformanceCounter(&counter) ||
        frequency.QuadPart <= 0)
        return 0;
    return (uint64_t)((long double)counter.QuadPart * 1000.0L / (long double)frequency.QuadPart);
#else
    struct timespec now;

    /*
     * Similar to the `_WIN32` branch, this retrieves the elapsed time since an unspecified fixed
     * reference point. The result consists of the seconds stored in `now.tv_sec` plus the
     * nanoseconds stored in `now.tv_nsec`. The final return value discards any fractional
     * millisecond.
     */
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;
    return (uint64_t)now.tv_sec * UINT64_C(1000) + (uint64_t)now.tv_nsec / UINT64_C(1000000);
#endif
}

/**
 * @brief  Reads the CPU time consumed by the current process and converts it to milliseconds.
 *
 * CPU time counts only the time during which the process is actively executing on the processor.
 * Time spent waiting or sleeping is generally not included. Fractions of a millisecond are
 * discarded when converting to integer milliseconds.
 *
 * @return The CPU time, in milliseconds, consumed by the current process since it started; returns
 *         0 if the time cannot be obtained.
 */
static uint64_t cpu_time_ms(void)
{
    clock_t now = clock();

    if (now == (clock_t)-1)
        return 0;
    return (uint64_t)((double)now * 1000.0 / (double)CLOCKS_PER_SEC);
}

/**
 * @brief      Reads the current time according to the search timing mode.
 *
 * @param mode Timing mode. `XQ_SEARCH_TIME_CPU` uses process CPU time; all other values use
 *             monotonic wall-clock time.
 * @return     The current time in milliseconds from the selected clock, or 0 if the underlying
 *             clock cannot be read.
 */
static uint64_t search_time_ms(XqSearchTimeMode mode)
{
    return mode == XQ_SEARCH_TIME_CPU ? cpu_time_ms() : monotonic_time_ms();
}

/**
 * @brief         Initializes the context used by a built-in search according to the search limits.
 *
 * The function resets the node count and stop state, normalizes the timing mode, and calculates the
 * absolute deadline when the time limit is nonzero. A zero time limit disables timeout checks.
 *
 * @param context Search context to initialize; must not be null.
 * @param limits  Depth, time, bonus, and timing mode configuration; must not be null.
 */
static void search_context_init(SearchContext *context, const XqSearchLimits *limits)
{
    uint64_t now;

    memset(context, 0, sizeof(*context));
    context->time_mode =
        limits->time_mode == XQ_SEARCH_TIME_CPU ? XQ_SEARCH_TIME_CPU : XQ_SEARCH_TIME_MONOTONIC;
    if (limits->time_limit_ms == 0)
        return;

    now = search_time_ms(context->time_mode);
    context->time_limited = true;
    if (UINT64_MAX - now < limits->time_limit_ms)
        context->deadline_ms = UINT64_MAX;
    else
        context->deadline_ms = now + limits->time_limit_ms;
}

/**
 * 检查搜索是否已经停止或到达时间上限。
 * 非强制检查只在节点计数达到 1024 的整数倍时读取时钟，以降低频繁查询
 * 系统时钟的开销；强制检查则立即读取时钟。检测到超时后会将 stopped
 * 置为 true，后续调用会持续返回停止状态。
 *
 * @param context 当前搜索上下文；为 NULL 时视为未停止且不执行时间检查
 * @param force   为 true 时立即检查时间，为 false 时按照节点间隔检查
 * @return        搜索已经停止或本次检查发现超时时返回 true，否则返回 false
 */
static bool search_check_time(SearchContext *context, bool force)
{
    if (context == NULL || context->stopped || !context->time_limited)
        return context != NULL && context->stopped;
    if (!force && (context->nodes & UINT64_C(1023)) != 0)
        return false;
    if (search_time_ms(context->time_mode) >= context->deadline_ms)
        context->stopped = true;
    return context->stopped;
}

/**
 * 记录搜索进入一个新节点，并按节点间隔检查时间上限。
 * context 非空时先将节点计数加一，再调用非强制时间检查；解释搜索等未使用
 * 搜索上下文的调用可以传入 NULL，此时不计数也不检查时间。
 *
 * @param context 当前搜索上下文，可以为 NULL
 * @return        搜索已经停止或本次节点检查发现超时时返回 true，否则返回 false
 */
static bool search_enter_node(SearchContext *context)
{
    if (context == NULL)
        return false;
    ++context->nodes;
    return search_check_time(context, false);
}

/**
 * 在堆上创建并初始化一张固定容量的置换表
 * 创建失败时返回 NULL；使用完毕后应调用
 * xq_transposition_table_destroy() 释放
 */
XqTranspositionTable *xq_transposition_table_create(void)
{
    XqTranspositionTable *table = (XqTranspositionTable *)malloc(sizeof(*table));

    if (table == NULL)
        return NULL;

    table->buckets = (XqTranspositionBucket *)calloc(XQ_TT_BUCKET_COUNT, sizeof(*table->buckets));
    if (table->buckets == NULL)
    {
        free(table);
        return NULL;
    }

    table->generation = 0;
    memset(&table->stats, 0, sizeof(table->stats));
    return table;
}

/**
 * 清空置换表中的所有条目，并重置轮次和统计信息
 * table 为 NULL 时不执行任何操作
 */
void xq_transposition_table_clear(XqTranspositionTable *table)
{
    if (table == NULL)
        return;

    memset(table->buckets, 0, XQ_TT_BUCKET_COUNT * sizeof(*table->buckets));
    table->generation = 0;
    memset(&table->stats, 0, sizeof(table->stats));
}

/**
 * 销毁置换表，释放桶数组和表对象占用的堆内存
 * table 为 NULL 时不执行任何操作
 */
void xq_transposition_table_destroy(XqTranspositionTable *table)
{
    if (table == NULL)
        return;
    free(table->buckets);
    free(table);
}

/**
 * 将置换表的所有统计计数器重置为零
 * table 为 NULL 时不执行任何操作
 */
void xq_transposition_table_reset_stats(XqTranspositionTable *table)
{
    if (table != NULL)
        memset(&table->stats, 0, sizeof(table->stats));
}

/**
 * 将置换表当前的统计计数复制到 *stats
 * table 为 NULL 时将 *stats 清零；stats 为 NULL 时不执行任何操作
 */
void xq_transposition_table_get_stats(const XqTranspositionTable *table,
                                      XqTranspositionStats *stats)
{
    if (stats == NULL)
        return;
    if (table == NULL)
        memset(stats, 0, sizeof(*stats));
    else
        *stats = table->stats;
}

/**
 * 创建一份内置搜索的默认限制配置。
 * 默认配置将搜索时间限制为 3 秒，使用单调墙钟计时，并为根着法每多完成
 * 一层增加 70 分深度可信奖励。max_depth 为 0 时会规范化为最小深度 1。
 *
 * @param max_depth 迭代加深允许达到的最大搜索深度，0 等同于 1
 * @return          初始化完成的 XqSearchLimits 配置值
 */
XqSearchLimits xq_search_limits_default(unsigned max_depth)
{
    XqSearchLimits limits;

    limits.max_depth = max_depth == 0 ? 1 : max_depth;
    limits.time_limit_ms = 3000;
    limits.depth_bonus = 70;
    limits.time_mode = XQ_SEARCH_TIME_MONOTONIC;
    return limits;
}

/**
 * 开始新一轮迭代加深时递增置换表轮次
 * generation 为 uint8_t，达到上限后按无符号整数规则回绕
 * table 为 NULL 时不执行任何操作
 */
static void tt_new_generation(XqTranspositionTable *table)
{
    if (table != NULL)
        ++table->generation;
}

/**
 * 使用 key 的低位定位四路桶，再遍历桶并比较完整的 64 位 key
 * 不同 key 落入同一桶属于正常的桶索引碰撞，可由完整 key 区分
 * 不同局面仍可能产生完全相同的 64 位 key；与 Java HashMap 不同，
 * 这里没有额外保存并比较完整局面，因为这会显著增加条目大小、
 * 内存带宽和比较开销，当前实现接受这种概率极低的完整哈希碰撞
 * table 为空或桶内没有匹配条目时返回 NULL
 */
static XqTranspositionEntry *tt_find_entry(XqTranspositionTable *table, uint64_t key)
{
    XqTranspositionBucket *bucket;
    int i;

    if (table == NULL)
        return NULL;

    bucket = &table->buckets[key & (XQ_TT_BUCKET_COUNT - 1u)];
    for (i = 0; i < XQ_TT_BUCKET_SIZE; ++i)
        if (bucket->entries[i].depth != 0 && bucket->entries[i].key == key)
            return &bucket->entries[i];
    return NULL;
}

/**
 * 将相对于当前搜索根节点的评分转换为适合写入置换表的评分
 * 将杀分数包含当前 ply，需要加减 ply 消除到达路径的影响，使同一局面
 * 从不同层数命中时仍能正确恢复将杀距离；普通局面评分保持不变
 * 读取条目时由 score_from_tt() 执行反向转换
 */
static int score_to_tt(int score, int ply)
{
    if (score >= XQ_MATE_THRESHOLD)
        return score + ply;
    if (score <= -XQ_MATE_THRESHOLD)
        return score - ply;
    return score;
}

/**
 * score_to_tt() 的逆向转换，将置换表评分恢复为当前 ply 下的搜索评分
 */
static int score_from_tt(int score, int ply)
{
    if (score >= XQ_MATE_THRESHOLD)
        return score - ply;
    if (score <= -XQ_MATE_THRESHOLD)
        return score + ply;
    return score;
}

/**
 * 探测置换表并刷新命中条目的轮次，同时输出 hash move 和评分类型
 * 仅当缓存深度足够且 EXACT 或边界结果可直接截断搜索时，恢复评分并返回 true
 * 返回 false 不一定表示未命中，此时输出的 hash move 仍可用于走法排序
 *
 * @param table         要探测的置换表，为 NULL 时直接返回 false
 * @param key           当前局面包含行棋方的完整 64 位哈希
 * @param depth         当前节点要求的剩余搜索深度
 * @param alpha         当前 alpha-beta 搜索窗口下界
 * @param beta          当前 alpha-beta 搜索窗口上界
 * @param ply           当前节点距离搜索根节点的层数，用于恢复将杀分数
 * @param score         可选输出参数；返回 true 时写入当前 ply 下可复用的评分
 * @param hash_move     可选输出参数；命中条目时写入缓存的最佳着法
 * @param has_hash_move 可选输出参数；写入是否命中了可提供 hash move 的条目
 * @param score_kind    可选输出参数；命中条目时写入缓存评分的边界类型
 * @return              缓存评分可直接复用或截断搜索时返回 true，否则返回 false
 */
static bool tt_probe(XqTranspositionTable *table, uint64_t key, unsigned depth, int alpha, int beta,
                     int ply, int *score, XqMove *hash_move, bool *has_hash_move,
                     XqSearchScoreKind *score_kind)
{
    XqTranspositionEntry *entry;
    int cached_score;

    if (has_hash_move != NULL)
        *has_hash_move = false;
    if (table == NULL)
        return false;

    ++table->stats.probes;
    entry = tt_find_entry(table, key);
    if (entry == NULL)
        return false;

    ++table->stats.hits;
    entry->generation = table->generation;
    if (hash_move != NULL)
        *hash_move = entry->best_move;
    if (has_hash_move != NULL)
        *has_hash_move = true;
    if (score_kind != NULL)
        *score_kind = (XqSearchScoreKind)entry->score_kind;

    if ((unsigned)entry->depth < depth)
        return false;

    cached_score = score_from_tt(entry->score, ply);
    if (entry->score_kind == XQ_SEARCH_SCORE_EXACT ||
        (entry->score_kind == XQ_SEARCH_SCORE_LOWER_BOUND && cached_score >= beta) ||
        (entry->score_kind == XQ_SEARCH_SCORE_UPPER_BOUND && cached_score <= alpha))
    {
        if (score != NULL)
            *score = cached_score;
        ++table->stats.cutoffs;
        return true;
    }

    return false;
}

/**
 * 从置换表获取当前局面的 hash move，并将命中条目刷新为当前轮次
 * 缓存深度不足或评分边界无法截断时，score 不能替代当前搜索，但
 * best_move 仍可作为有价值的排序提示，因此本函数不检查 depth、
 * score_kind 和 alpha-beta 窗口。调用者还会用当前着法列表验证该着法，
 * 所以它只影响搜索顺序，不影响搜索结果的正确性
 *
 * @param table     要查询的置换表，为 NULL 时返回 false
 * @param key       当前局面包含行棋方的完整 64 位哈希
 * @param hash_move 输出缓存的最佳着法，为 NULL 时返回 false
 * @return          找到匹配条目并写入 hash_move 时返回 true，否则返回 false
 */
static bool tt_get_hash_move(XqTranspositionTable *table, uint64_t key, XqMove *hash_move)
{
    XqTranspositionEntry *entry;

    if (table == NULL || hash_move == NULL)
        return false;

    ++table->stats.probes;
    entry = tt_find_entry(table, key);
    if (entry == NULL)
        return false;

    ++table->stats.hits;
    entry->generation = table->generation;
    *hash_move = entry->best_move;
    return true;
}

/**
 * 计算置换表条目的保留分，分数越低越容易在桶冲突时被淘汰
 * 公式为 8 * depth - 4 * age + type_bonus，其中 EXACT 奖励 3 分
 * 例如 depth=5、条目轮次为 7、当前轮次为 10 且类型为 EXACT 时：
 * age=3，保留分为 8 * 5 - 4 * 3 + 3 = 31
 *
 * @param entry              要评估保留价值的置换表条目，必须非 NULL
 * @param current_generation 置换表当前的最新轮次
 * @return                   条目的保留分，越高表示越值得继续保留
 */
static int tt_retention_score(const XqTranspositionEntry *entry, uint8_t current_generation)
{
    unsigned age = (uint8_t)(current_generation - entry->generation);
    int type_bonus = entry->score_kind == XQ_SEARCH_SCORE_EXACT ? 3 : 0;

    return (int)entry->depth * 8 - (int)age * 4 + type_bonus;
}

/**
 * 将搜索结果写入置换表。优先使用空槽或更新相同 key 的条目；桶已满时，
 * 根据深度、年龄和评分类型选择保留分最低的条目，并仅在新条目不更弱时替换
 * depth 为 0 的静态搜索节点不缓存，同时该值被保留为空条目标志
 *
 * @param table      要写入的置换表，为 NULL 时不执行任何操作
 * @param key        当前局面包含行棋方的完整 64 位哈希
 * @param depth      当前节点的剩余搜索深度，为 0 时不写入
 * @param score      当前 ply 下得到的搜索评分，写入前会归一化将杀分数
 * @param ply        当前节点距离搜索根节点的层数
 * @param score_kind 评分类型，可为 EXACT、LOWER_BOUND 或 UPPER_BOUND
 * @param best_move  当前局面搜索得到的最佳着法
 */
static void tt_store(XqTranspositionTable *table, uint64_t key, unsigned depth, int score, int ply,
                     XqSearchScoreKind score_kind, XqMove best_move)
{
    XqTranspositionBucket *bucket;
    XqTranspositionEntry incoming;
    XqTranspositionEntry *target = NULL;
    int weakest_score = INT_MAX;
    int incoming_score;
    int i;

    if (table == NULL || depth == 0)
        return;

    incoming.key = key;
    incoming.score = score_to_tt(score, ply);
    incoming.best_move = best_move;
    incoming.depth = depth > UINT16_MAX ? UINT16_MAX : (uint16_t)depth;
    incoming.generation = table->generation;
    incoming.score_kind = (uint8_t)score_kind;
    incoming_score = tt_retention_score(&incoming, table->generation);

    bucket = &table->buckets[key & (XQ_TT_BUCKET_COUNT - 1u)];
    for (i = 0; i < XQ_TT_BUCKET_SIZE; ++i)
    {
        XqTranspositionEntry *entry = &bucket->entries[i];
        int retention;

        if (entry->depth == 0)
        {
            target = entry;
            break;
        }
        if (entry->key == key)
        {
            retention = tt_retention_score(entry, table->generation);
            entry->generation = table->generation;
            if (incoming_score >= retention)
                target = entry;
            else
                return;
            break;
        }

        retention = tt_retention_score(entry, table->generation);
        if (retention < weakest_score)
        {
            weakest_score = retention;
            target = entry;
        }
    }

    if (target == NULL)
        return;
    if (target->depth != 0 && target->key != key)
    {
        if (incoming_score < weakest_score)
            return;
        ++table->stats.replacements;
    }

    *target = incoming;
    ++table->stats.stores;
}

/**
 * 计算 color 方所有合法着法的机动性价值。
 */
static int evaluate_legal_moves(const XqPosition *pos, XqColor color)
{
    XqPosition evaluation_pos = *pos;
    XqMoveList legal;
    int score = 0;
    int i;

    evaluation_pos.side_to_move = color;
    xq_generate_legal(&evaluation_pos, &legal);
    for (i = 0; i < legal.count; ++i)
        score += legal_move_values[xq_piece_type(legal.moves[i].piece)];
    return score;
}

/**
 * 默认静态评估函数，基于材料和合法着法机动性，返回 perspective 方的局面评分。
 */
int xq_engine_default_static_evaluate(const XqPosition *pos, XqColor perspective, void *user)
{
    int score = 0;
    XqColor opponent = xq_color_opponent(perspective);
    int type;

    (void)user;
    for (type = 0; type < XQ_PIECE_TYPE_NB; ++type)
    {
        int value = piece_values[type];
        score += value * xq_bb_count(pos->pieces[perspective][type]);
        score -= value * xq_bb_count(pos->pieces[opponent][type]);
    }
    // score += evaluate_legal_moves(pos, perspective);
    // score -= evaluate_legal_moves(pos, opponent);
    return score;
}

/**
 * 静态评估函数
 * 根据输入的引擎适配器，计算 perspective 方在 *pos 下的局面评分
 * 如果输入的引擎适配器为空，则直接用 xq_engine_default_static_evaluate 函数评分
 */
static int static_evaluate(const XqEngineAdapter *engine, const XqPosition *pos,
                           XqColor perspective)
{
    if (engine != NULL && engine->static_evaluate != NULL)
        return engine->static_evaluate(pos, perspective, engine->user);
    return xq_engine_default_static_evaluate(pos, perspective, NULL);
}

/**
 * 返回走法的排序分数，分数越高越优先搜索。
 * 引擎未提供自定义评分函数时，使用 MVV-LVA 将吃子着法排在普通着法之前。
 *
 * 默认公式为：
 *   CAPTURE_ORDER_BASE + 被吃棋子的价值 * 16 - 走子棋子的价值
 *
 * CAPTURE_ORDER_BASE 保证吃子着法整体排在普通着法之前；被吃棋子的价值
 * 乘以 16，使吃价值更高棋子的着法通常更靠前；减去走子棋子的价值，则在
 * 吃相同棋子时倾向于优先使用价值更低的棋子。例如兵吃车的排序分数高于
 * 车吃兵，兵吃炮的排序分数也高于车吃炮。
 *
 * 这里的 16 是经验权重，并不保证严格的字典序 MVV-LVA。整个公式也不是
 * 对走后局面的真实评价，只是用于猜测哪些着法更可能较好并尽早触发剪枝；
 * 它只影响搜索效率，不影响完整 Alpha-Beta 搜索的正确结果。
 */
static int move_order_score(const XqEngineAdapter *engine, const XqPosition *pos, XqMove move)
{
    if (engine != NULL && engine->score_move != NULL)
        return engine->score_move(pos, move, engine->user);

    if (move.captured == XQ_EMPTY_PIECE)
        return 0;

    return CAPTURE_ORDER_BASE + piece_values[xq_piece_type(move.captured)] * 16 -
           piece_values[xq_piece_type(move.piece)];
}

/**
 * 预先计算每个走法的排序分数，并通过稳定插入排序按分数降序排列。
 */
static void order_moves(const XqEngineAdapter *engine, const XqPosition *pos, XqMoveList *list)
{
    ScoredMove ordered[XQ_MAX_MOVES];
    int i;

    for (i = 0; i < list->count; ++i)
    {
        ScoredMove current;
        int j = i;

        current.move = list->moves[i];
        current.score = move_order_score(engine, pos, current.move);
        while (j > 0 && ordered[j - 1].score < current.score)
        {
            ordered[j] = ordered[j - 1];
            --j;
        }
        ordered[j] = current;
    }

    for (i = 0; i < list->count; ++i)
        list->moves[i] = ordered[i].move;
}

/**
 * 按搜索分数稳定降序排列根着法。同分着法保留上一轮的相对顺序。
 */
static void order_root_moves(ScoredMove *moves, int count)
{
    int i;

    for (i = 1; i < count; ++i)
    {
        ScoredMove current = moves[i];
        int j = i;

        while (j > 0 && moves[j - 1].score < current.score)
        {
            moves[j] = moves[j - 1];
            --j;
        }
        moves[j] = current;
    }
}

/**
 * 判断两个走法是否相同。
 */
static bool moves_equal(XqMove a, XqMove b)
{
    return a.from == b.from && a.to == b.to;
}

/**
 * 如果 list 中存在 move，则将它稳定移动到首位，并保持其他走法的相对顺序。
 */
static bool prioritize_move(XqMoveList *list, XqMove move)
{
    int i;

    for (i = 0; i < list->count; ++i)
        if (moves_equal(list->moves[i], move))
        {
            XqMove prioritized = list->moves[i];

            while (i > 0)
            {
                list->moves[i] = list->moves[i - 1];
                --i;
            }
            list->moves[0] = prioritized;
            return true;
        }

    return false;
}

/**
 * 用 move 和它的子节点主变化路线构造当前节点的主变化路线。
 */
static void build_principal_variation(PrincipalVariation *pv, XqMove move,
                                      const PrincipalVariation *child_pv)
{
    int child_count = child_pv != NULL ? child_pv->count : 0;
    int i;

    if (child_count > XQ_MAX_PV_MOVES - 1)
        child_count = XQ_MAX_PV_MOVES - 1;

    pv->moves[0] = move;
    for (i = 0; i < child_count; ++i)
        pv->moves[i + 1] = child_pv->moves[i];
    pv->count = child_count + 1;
}

/**
 * 沿足够深的 EXACT 条目重建主变化路线。每一步都从当前局面的伪合法着法
 * 中取出完整 XqMove，避免直接使用陈旧的 piece/captured 字段。重建期间会
 * 临时走子，结束前按相反顺序全部撤销，保证 pos 恢复原状
 *
 * @param table 用于查找后续 EXACT 条目的置换表，为 NULL 时输出空 PV
 * @param pos   主变化起点局面，必须非 NULL，函数返回前会恢复其内容
 * @param depth 最多重建的剩余搜索深度
 * @param pv    输出的主变化路线，必须非 NULL，原有内容会被清空
 */
static void build_pv_from_tt(XqTranspositionTable *table, XqPosition *pos, unsigned depth,
                             PrincipalVariation *pv)
{
    unsigned remaining = depth;
    int made_count = 0;
    int i;

    pv->count = 0;
    while (remaining > 0 && pv->count < XQ_MAX_PV_MOVES)
    {
        XqTranspositionEntry *entry = tt_find_entry(table, xq_position_hash(pos));
        XqMoveList list;

        if (entry == NULL || entry->score_kind != XQ_SEARCH_SCORE_EXACT ||
            (unsigned)entry->depth < remaining)
            break;

        xq_generate_pseudo_legal(pos, &list);
        for (i = 0; i < list.count; ++i)
            if (moves_equal(list.moves[i], entry->best_move))
                break;
        if (i == list.count)
            break;

        entry->generation = table->generation;
        pv->moves[pv->count++] = list.moves[i];
        xq_position_make_move(pos, list.moves[i]);
        ++made_count;
        --remaining;
    }

    for (i = made_count - 1; i >= 0; --i)
        xq_position_unmake_move(pos, pv->moves[i]);
}

/**
 * 逻辑类似 order_moves，只不过多谢带了排序所需要的积分
 */
static void order_moves_for_explain(const XqEngineAdapter *engine, const XqPosition *pos,
                                    XqMoveList *list, int *scores)
{
    ScoredMove ordered[XQ_MAX_MOVES];
    int i;

    for (i = 0; i < list->count; ++i)
    {
        ScoredMove current;
        int j = i;

        current.move = list->moves[i];
        current.score = move_order_score(engine, pos, current.move);
        while (j > 0 && ordered[j - 1].score < current.score)
        {
            ordered[j] = ordered[j - 1];
            --j;
        }
        ordered[j] = current;
    }

    for (i = 0; i < list->count; ++i)
    {
        list->moves[i] = ordered[i].move;
        scores[i] = ordered[i].score;
    }
}

/**
 * 静态搜索函数，为了避免地平线效应，即恰好搜索到局面波动幅度大的策略树部分停止，对 negamax 搜索
 * 的叶子节点采用静态搜索。目前认为吃子、应将是比较明显让局势评分波动的着法，故次静态搜索囊括了这
 * 两类着法。depth 表示静态搜索内部深度，递归时逐层减一，到达
 * XQ_MAX_QUIESCENCE_DEPTH 后返回静态评估；ply 表示当前节点距离整次搜索根节点
 * 的层数，递归时逐层加一，用于计算将杀距离
 */
static int quiescence(const XqEngineAdapter *engine, XqPosition *pos, int depth, int ply, int alpha,
                      int beta, SearchContext *context)
{
    XqMoveList list;
    bool in_check;
    int stand_pat;
    int i;

    if (search_enter_node(context))
        return 0;

    if (depth <= -XQ_MAX_QUIESCENCE_DEPTH)
        return static_evaluate(engine, pos, pos->side_to_move);

    in_check = xq_position_in_check(pos, pos->side_to_move);

    if (!in_check)
    {
        stand_pat = static_evaluate(engine, pos, pos->side_to_move);
        if (stand_pat >= beta)
            return beta;
        if (stand_pat > alpha)
            alpha = stand_pat;
    }

    xq_generate_pseudo_legal(pos, &list);
    if (in_check && list.count == 0)
        return -XQ_MATE_SCORE + ply;
    order_moves(engine, pos, &list);

    for (i = 0; i < list.count; ++i)
    {
        int score;

        if (!in_check && list.moves[i].captured == XQ_EMPTY_PIECE)
            continue;
        if (list.moves[i].captured != XQ_EMPTY_PIECE &&
            xq_piece_type(list.moves[i].captured) == XQ_KING)
            return XQ_MATE_SCORE - (ply + 1);

        xq_position_make_move(pos, list.moves[i]);
        score = -quiescence(engine, pos, depth - 1, ply + 1, -beta, -alpha, context);
        xq_position_unmake_move(pos, list.moves[i]);
        if (context != NULL && context->stopped)
            return 0;
        if (score >= beta)
            return beta;
        if (score > alpha)
            alpha = score;
    }

    return alpha;
}

/**
 * 本象棋引擎的核心：alpha-beta 剪枝后的 Minimax 搜索，并采用了取负最大化的代码简化的写法
 *
 * 首先，Minimax 搜索不再赘述，它就是基本的零和博弈游戏的对某一方最优局面评分的搜索函数，在
 * 己方局面中，搜索最优的走法，而在对方的局面中，搜索最差（对对方最优，即不把对方当傻子）走法
 *
 * 然后，在深入 alpha-beta 剪枝之前，先看这个例子：
 *
 * MAX :       (1:>=x)
 *             /     \
 * MIN :    (2:x)  (3:!(<=x),<=y)
 *                   /           \
 * MAX :          (4:y)      (5:!(<=x),!(>=y))
 *
 * 假设这个局面树中，节点 2 评分为 x，节点 4 评分为 y，那么节点 1 的评分根据节点 2 就可知
 * 大于等于 x，于是如果节点 3 的评分小于等于 x，也不会影响已经计算的结果，记 !(exp) 的含义
 * 为如果节点的分值满足 exp，则不影响目前已计算结果，则为节点 3 标记一个 !(<=x)
 * 再根据节点 4 的评分得到节点 3 的评分 <= y，结合 3 的 !(<=x)，可以知道 5 的评分只要满足
 * <=x 或 >=y 就不会影响目前结果，于是我们就得到了一个计算 5 时的 alpha, beta 的例子，分别
 * 是 x, y
 *
 * 接下来详细说一下计算 MAX 层节点评分时的 alpha-beta 剪枝逻辑，MIN 层同理，只是符号相反：
 * 因为计算 MAX 层节点过程中，或者说计算完某个该 MAX 层节点的子节点后，只能以 >=x 的方式
 * 更新父节点（比如上边那个例子中的 (2) 节点，在计算得到评分 x 后，父节点的范围变成 >= x）
 * 所以，只有计算完某个子节点让父节点评分值 >= beta 时，才跳过该 MAX 层剩余子节点的计算，即剪枝
 * 另外，当计算子节点的评分落入到了 (alpha, beta) 中，在以 >=x 的方式更新父节点后，得到父节点
 * 评分可能出现的范围与 (alpha, beta) 的交集包含于 (alpha, beta)，所以我们可以更新 alpha
 * 值为刚计算的子节点的评分
 *
 * 最后，说下取负最大化的代码简化：通过对对手的局势评分取负，就将 Minimax 搜索转化为 "Maxmax
 * 搜索"，起到简化代码的作用
 *
 * 总结：相比 Minimax，alpha-beta 通过 alpha、beta 两个参数剪枝来提升性能；代价是当局面的
 * 真实评分落在窗口外时，返值可能不再是精确分数，而只是一个足以支持剪枝和决策的上界或下界。
 */
static int negamax(const XqEngineAdapter *engine, XqTranspositionTable *table, XqPosition *pos,
                   unsigned depth, int ply, int alpha, int beta, const XqMove *pv_hint,
                   int pv_hint_count, PrincipalVariation *pv_out, SearchContext *context)
{
    XqMoveList list;
    int best = INT_MIN / 2;
    int alpha_original = alpha;
    uint64_t key;
    XqMove hash_move = {0};
    XqMove node_best_move = {0};
    bool has_hash_move = false;
    bool has_best_move = false;
    bool pv_move_found = false;
    XqSearchScoreKind cached_kind = XQ_SEARCH_SCORE_UPPER_BOUND;
    int cached_score;
    int i;

    if (pv_out != NULL)
        pv_out->count = 0;

    if (search_enter_node(context))
        return 0;

    if (xq_position_king_square(pos, pos->side_to_move) == XQ_NO_SQUARE)
        return -XQ_MATE_SCORE + ply;

    if (depth == 0)
        return quiescence(engine, pos, 0, ply, alpha, beta, context);

    key = xq_position_hash(pos);
    if (tt_probe(table, key, depth, alpha, beta, ply, &cached_score, &hash_move, &has_hash_move,
                 &cached_kind))
    {
        if (pv_out != NULL && cached_kind == XQ_SEARCH_SCORE_EXACT)
            build_pv_from_tt(table, pos, depth, pv_out);
        return cached_score;
    }

    xq_generate_pseudo_legal(pos, &list);
    if (list.count == 0)
        return -XQ_MATE_SCORE + ply;
    order_moves(engine, pos, &list);

    /* 例如当前要求 depth=6、窗口为 [50, 100]，TT 中旧条目是 depth=8、
     * LOWER_BOUND=80、best_move=A。因为 80<beta，缓存不能截断搜索，但仍
     * 提供 hash_move=A；若上一轮完整 PV 的首着是 B，则 pv_hint[0]=B，
     * 此时 hash_move 与 pv_hint[0] 不相等。 */
    if (has_hash_move)
        (void)prioritize_move(&list, hash_move);
    if (pv_hint != NULL && pv_hint_count > 0)
        pv_move_found = prioritize_move(&list, pv_hint[0]);

    for (i = 0; i < list.count; ++i)
    {
        PrincipalVariation child_pv;
        const XqMove *child_hint = NULL;
        int child_hint_count = 0;
        int score;

        if (pv_move_found && moves_equal(list.moves[i], pv_hint[0]))
        {
            child_hint = pv_hint + 1;
            child_hint_count = pv_hint_count - 1;
        }

        xq_position_make_move(pos, list.moves[i]);
        score = -negamax(engine, table, pos, depth - 1, ply + 1, -beta, -alpha, child_hint,
                         child_hint_count, pv_out != NULL ? &child_pv : NULL, context);
        xq_position_unmake_move(pos, list.moves[i]);
        if (context != NULL && context->stopped)
            return 0;
        if (score > best)
        {
            best = score;
            node_best_move = list.moves[i];
            has_best_move = true;
            if (pv_out != NULL)
                build_principal_variation(pv_out, list.moves[i], &child_pv);
        }
        if (score > alpha)
            alpha = score;
        if (alpha >= beta)
            break;
    }

    if (has_best_move)
    {
        XqSearchScoreKind score_kind;

        if (best <= alpha_original)
            score_kind = XQ_SEARCH_SCORE_UPPER_BOUND;
        else if (best >= beta)
            score_kind = XQ_SEARCH_SCORE_LOWER_BOUND;
        else
            score_kind = XQ_SEARCH_SCORE_EXACT;
        tt_store(table, key, depth, best, ply, score_kind, node_best_move);
    }

    return best;
}

/**
 * 在搜索超时后，根据根着法最后完整完成的评分和深度选择返回着法。
 * 每个候选的修正评分为 score + completed_depth * depth_bonus；修正评分
 * 相同时优先选择完成深度更高的着法，评分和深度都相同时保留列表中更靠前
 * 的着法。completed_depth 为 0 的未完成着法不参与比较；如果没有任何着法
 * 完成搜索，则使用列表中的第一步作为保底结果。
 *
 * @param moves       根着法及其最后完整完成的评分和深度，必须非 NULL
 * @param count       根着法数量，必须大于 0
 * @param depth_bonus 每多完成一层加入修正评分的深度可信奖励
 * @return            按修正评分和稳定平分规则选出的根着法
 */
static XqMove select_timed_root_move(const ScoredMove *moves, int count, int depth_bonus)
{
    int best_index = -1;
    int64_t best_adjusted = INT64_MIN;
    int i;

    for (i = 0; i < count; ++i)
    {
        int64_t adjusted;

        if (moves[i].completed_depth == 0)
            continue;
        adjusted = (int64_t)moves[i].score + (int64_t)moves[i].completed_depth * depth_bonus;
        if (best_index < 0 || adjusted > best_adjusted ||
            (adjusted == best_adjusted &&
             moves[i].completed_depth > moves[best_index].completed_depth))
        {
            best_index = i;
            best_adjusted = adjusted;
        }
    }

    return moves[best_index >= 0 ? best_index : 0].move;
}

/**
 * 内部搜索算法。从深度 1 迭代搜索到 depth，并使用上一轮全部根着法的
 * 评分顺序指导下一轮搜索。
 */
static bool builtin_search(const XqEngineAdapter *engine, XqPosition *pos,
                           const XqSearchLimits *limits, XqMove *best_move)
{
    XqMoveList list;
    ScoredMove root_moves[XQ_MAX_MOVES];
    PrincipalVariation previous_pv = {0};
    XqTranspositionTable *table = engine != NULL ? engine->transposition_table : NULL;
    uint64_t root_key;
    XqMove hash_move;
    SearchContext context;
    unsigned depth = limits->max_depth == 0 ? 1 : limits->max_depth;
    unsigned current_depth;
    int i;

    if (pos == NULL || xq_position_king_square(pos, pos->side_to_move) == XQ_NO_SQUARE)
        return false;

    xq_generate_pseudo_legal(pos, &list);
    if (list.count == 0)
        return false;
    order_moves(engine, pos, &list);
    root_key = xq_position_hash(pos);
    tt_new_generation(table);
    if (tt_get_hash_move(table, root_key, &hash_move))
        (void)prioritize_move(&list, hash_move);

    search_context_init(&context, limits);

    for (i = 0; i < list.count; ++i)
    {
        root_moves[i].move = list.moves[i];
        root_moves[i].score = INT_MIN / 2;
        root_moves[i].completed_depth = 0;
    }
    *best_move = root_moves[0].move;

    for (current_depth = 1;; ++current_depth)
    {
        PrincipalVariation current_pv = {0};
        int alpha = INT_MIN / 2;
        int beta = INT_MAX / 2;

        if (current_depth > 1)
            tt_new_generation(table);

        for (i = 0; i < list.count; ++i)
        {
            PrincipalVariation child_pv;
            const XqMove *child_hint = NULL;
            int child_hint_count = 0;
            int score;

            if (search_check_time(&context, true))
                break;

            if (previous_pv.count > 0 && moves_equal(root_moves[i].move, previous_pv.moves[0]))
            {
                child_hint = previous_pv.moves + 1;
                child_hint_count = previous_pv.count - 1;
            }

            xq_position_make_move(pos, root_moves[i].move);
            score = -negamax(engine, table, pos, current_depth - 1, 1, -beta, -alpha, child_hint,
                             child_hint_count, &child_pv, &context);
            xq_position_unmake_move(pos, root_moves[i].move);

            if (context.stopped)
                break;

            root_moves[i].score = score;
            root_moves[i].completed_depth = current_depth;
            if (score > alpha)
            {
                alpha = score;
                build_principal_variation(&current_pv, root_moves[i].move, &child_pv);
            }

            if (search_check_time(&context, true))
                break;
        }

        if (context.stopped)
        {
            *best_move = select_timed_root_move(root_moves, list.count, limits->depth_bonus);
            break;
        }

        order_root_moves(root_moves, list.count);
        *best_move = root_moves[0].move;
        previous_pv = current_pv;
        tt_store(table, root_key, current_depth, root_moves[0].score, 0, XQ_SEARCH_SCORE_EXACT,
                 root_moves[0].move);

        if (current_depth == depth)
            break;
    }

    return true;
}

/**
 * 引擎搜索算法，引擎为空或引擎搜索函数的话调用 builtin_search
 */
bool xq_engine_find_best_move(const XqEngineAdapter *engine, XqPosition *pos, unsigned depth,
                              XqMove *best_move)
{
    XqSearchLimits limits = xq_search_limits_default(depth);

    if (best_move == NULL)
        return false;
    if (engine != NULL && engine->search != NULL)
        return engine->search(pos, depth, best_move, engine->user);
    return builtin_search(engine, pos, &limits, best_move);
}

/**
 * 按指定深度、时间和深度奖励限制为当前局面寻找最佳着法。
 * 内置搜索会执行迭代加深并在超时时使用 limits 中的深度奖励选择结果；
 * max_depth 为 0 时按 1 处理。若 engine 提供自定义 search 回调，则只把规范化
 * 后的最大深度传给该回调，时间限制和深度奖励由自定义搜索自行管理。
 *
 * @param engine    引擎适配器，可以为 NULL；未提供 search 时使用内置搜索
 * @param pos       要搜索的当前局面，内置搜索要求非 NULL，搜索结束后保持局面不变
 * @param limits    搜索深度、时间上限、深度奖励和计时模式配置，必须非 NULL
 * @param best_move 输出选出的最佳着法，必须非 NULL
 * @return          成功找到并写入最佳着法时返回 true；参数无效、无可搜索着法或自定义搜索失败时返回
 *                  false
 */
bool xq_engine_find_best_move_with_limits(const XqEngineAdapter *engine, XqPosition *pos,
                                          const XqSearchLimits *limits, XqMove *best_move)
{
    XqSearchLimits effective_limits;

    if (best_move == NULL || limits == NULL)
        return false;
    effective_limits = *limits;
    if (effective_limits.max_depth == 0)
        effective_limits.max_depth = 1;
    if (engine != NULL && engine->search != NULL)
        return engine->search(pos, effective_limits.max_depth, best_move, engine->user);
    return builtin_search(engine, pos, &effective_limits, best_move);
}

/**
 * 带有解释信息的引擎搜索算法，引擎为空或引擎搜索函数为空的话调用 builtin_search
 */
bool xq_engine_explain_search_one_ply(const XqEngineAdapter *engine, XqPosition *pos,
                                      unsigned depth, XqExplainResult *result)
{
    XqMoveList list;
    int order_scores[XQ_MAX_MOVES];
    int alpha = INT_MIN / 2;
    int beta = INT_MAX / 2;
    int i;

    if (result == NULL)
        return false;

    result->count = 0;
    result->best_index = -1;
    result->final_score = INT_MIN / 2;

    if (pos == NULL)
        return false;
    if (depth == 0)
        depth = 1;
    if (xq_position_king_square(pos, pos->side_to_move) == XQ_NO_SQUARE)
        return false;

    xq_generate_pseudo_legal(pos, &list);
    if (list.count == 0)
        return false;
    order_moves_for_explain(engine, pos, &list, order_scores);

    for (i = 0; i < list.count; ++i)
    {
        XqExplainedMove *explained = &result->moves[result->count];
        int alpha_before = alpha;
        int score;

        xq_position_make_move(pos, list.moves[i]);
        score = -negamax(engine, NULL, pos, depth - 1, 1, -beta, -alpha, NULL, 0, NULL, NULL);
        xq_position_unmake_move(pos, list.moves[i]);

        explained->move = list.moves[i];
        explained->depth = depth;
        explained->alpha_before = alpha_before;
        explained->beta = beta;
        explained->score = score;
        explained->order_score = order_scores[i];
        explained->score_kind = XQ_SEARCH_SCORE_UPPER_BOUND;
        explained->is_best = false;
        explained->caused_cutoff = false;

        if (score > alpha)
        {
            alpha = score;
            result->best_index = result->count;
            explained->score_kind = XQ_SEARCH_SCORE_EXACT;
        }
        if (alpha >= beta)
        {
            explained->score_kind = XQ_SEARCH_SCORE_LOWER_BOUND;
            explained->caused_cutoff = true;
            ++result->count;
            break;
        }

        ++result->count;
    }

    result->final_score = alpha;
    if (result->best_index >= 0 && result->best_index < result->count)
        result->moves[result->best_index].is_best = true;
    return result->count > 0;
}

/**
 * 解释当前局面的一层静态搜索：先展示 stand pat，再展示静态搜索实际会继续看的吃子/应将着法。
 * 每个着法的 score 仍由递归 quiescence 计算，避免解释结果和真实搜索结果不一致。
 */
bool xq_engine_explain_quiescence_one_ply(const XqEngineAdapter *engine, XqPosition *pos,
                                          XqQuiescenceExplainResult *result)
{
    XqMoveList list;
    int order_scores[XQ_MAX_MOVES];
    int alpha = INT_MIN / 2;
    int beta = INT_MAX / 2;
    int i;

    if (result == NULL)
        return false;

    result->count = 0;
    result->best_index = -1;
    result->alpha_before = alpha;
    result->alpha_after_stand_pat = alpha;
    result->beta = beta;
    result->stand_pat = 0;
    result->final_score = alpha;
    result->in_check = false;
    result->stand_pat_used = false;
    result->stand_pat_cutoff = false;

    if (pos == NULL)
        return false;
    if (xq_position_king_square(pos, pos->side_to_move) == XQ_NO_SQUARE)
        return false;

    result->in_check = xq_position_in_check(pos, pos->side_to_move);
    if (!result->in_check)
    {
        result->stand_pat_used = true;
        result->stand_pat = static_evaluate(engine, pos, pos->side_to_move);
        if (result->stand_pat >= beta)
        {
            result->stand_pat_cutoff = true;
            result->final_score = beta;
            return true;
        }
        if (result->stand_pat > alpha)
            alpha = result->stand_pat;
        result->alpha_after_stand_pat = alpha;
    }

    xq_generate_pseudo_legal(pos, &list);
    if (result->in_check && list.count == 0)
    {
        result->final_score = -30000;
        return true;
    }
    order_moves_for_explain(engine, pos, &list, order_scores);

    for (i = 0; i < list.count; ++i)
    {
        XqExplainedMove *explained;
        int alpha_before;
        int score;

        if (!result->in_check && list.moves[i].captured == XQ_EMPTY_PIECE)
            continue;

        explained = &result->moves[result->count];
        alpha_before = alpha;

        if (list.moves[i].captured != XQ_EMPTY_PIECE &&
            xq_piece_type(list.moves[i].captured) == XQ_KING)
            score = XQ_MATE_SCORE - 1;
        else
        {
            xq_position_make_move(pos, list.moves[i]);
            score = -quiescence(engine, pos, -1, 1, -beta, -alpha, NULL);
            xq_position_unmake_move(pos, list.moves[i]);
        }

        explained->move = list.moves[i];
        explained->depth = 0;
        explained->alpha_before = alpha_before;
        explained->beta = beta;
        explained->score = score;
        explained->order_score = order_scores[i];
        explained->score_kind = XQ_SEARCH_SCORE_UPPER_BOUND;
        explained->is_best = false;
        explained->caused_cutoff = false;

        if (score >= beta)
        {
            result->best_index = result->count;
            explained->score_kind = XQ_SEARCH_SCORE_LOWER_BOUND;
            explained->caused_cutoff = true;
            ++result->count;
            result->final_score = beta;
            result->moves[result->best_index].is_best = true;
            return true;
        }
        if (score > alpha)
        {
            alpha = score;
            result->best_index = result->count;
            explained->score_kind = XQ_SEARCH_SCORE_EXACT;
        }

        ++result->count;
    }

    result->final_score = alpha;
    if (result->best_index >= 0 && result->best_index < result->count)
        result->moves[result->best_index].is_best = true;
    return true;
}
