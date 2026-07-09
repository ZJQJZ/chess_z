#include "xiangqi/engine.h"

#include "xiangqi/movegen.h"

#include <limits.h>

static const int piece_values[XQ_PIECE_TYPE_NB] = {
    [XQ_KING] = 10000,
    [XQ_ADVISOR] = 120,
    [XQ_BISHOP] = 120,
    [XQ_KNIGHT] = 270,
    [XQ_ROOK] = 600,
    [XQ_CANNON] = 285,
    [XQ_PAWN] = 70,
};

static const int legal_move_values[XQ_PIECE_TYPE_NB] = {
    [XQ_KING] = 10,
    [XQ_ADVISOR] = 10,
    [XQ_BISHOP] = 10,
    [XQ_KNIGHT] = 20,
    [XQ_ROOK] = 20,
    [XQ_CANNON] = 15,
    [XQ_PAWN] = 10,
};

enum
{
    CAPTURE_ORDER_BASE = 100000
};

typedef struct ScoredMove
{
    XqMove move;
    int score;
} ScoredMove;

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
static int static_evaluate(const XqEngineAdapter *engine, const XqPosition *pos, XqColor perspective)
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

    return CAPTURE_ORDER_BASE +
           piece_values[xq_piece_type(move.captured)] * 16 -
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
 * 逻辑类似 order_moves，只不过多谢带了排序所需要的积分
 */
static void order_moves_for_explain(const XqEngineAdapter *engine, const XqPosition *pos, XqMoveList *list, int *scores)
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
 * 两类着法
 */
static int quiescence(const XqEngineAdapter *engine, XqPosition *pos, int depth, int alpha, int beta)
{
    XqMoveList list;
    bool in_check = xq_position_in_check(pos, pos->side_to_move);
    int stand_pat;
    int i;

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
        return -30000 - depth;
    order_moves(engine, pos, &list);

    for (i = 0; i < list.count; ++i)
    {
        int score;

        if (!in_check && list.moves[i].captured == XQ_EMPTY_PIECE)
            continue;
        if (list.moves[i].captured != XQ_EMPTY_PIECE &&
            xq_piece_type(list.moves[i].captured) == XQ_KING)
            return 30000 + depth;

        if (!xq_position_make_move(pos, list.moves[i]))
            continue;
        score = -quiescence(engine, pos, depth - 1, -beta, -alpha);
        xq_position_unmake_move(pos, list.moves[i]);
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
static int negamax(const XqEngineAdapter *engine, XqPosition *pos, unsigned depth, int alpha, int beta)
{
    XqMoveList list;
    int best = INT_MIN / 2;
    int i;

    if (xq_position_king_square(pos, pos->side_to_move) == XQ_NO_SQUARE)
        return -30000 - (int)depth;

    if (depth == 0)
        return static_evaluate(engine, pos, pos->side_to_move);

    xq_generate_pseudo_legal(pos, &list);
    if (list.count == 0)
        return -30000 - (int)depth;
    order_moves(engine, pos, &list);

    for (i = 0; i < list.count; ++i)
    {
        int score;

        if (!xq_position_make_move(pos, list.moves[i]))
            continue;
        score = -negamax(engine, pos, depth - 1, -beta, -alpha);
        xq_position_unmake_move(pos, list.moves[i]);
        if (score > best)
            best = score;
        if (score > alpha)
            alpha = score;
        if (alpha >= beta)
            break;
    }

    return best;
}

/**
 * 内部搜索算法，通过 negamax 函数计算出 *pos 盘面下，depth 深度的最佳走法
 */
static bool builtin_search(const XqEngineAdapter *engine, XqPosition *pos, unsigned depth, XqMove *best_move)
{
    XqMoveList list;
    int alpha = INT_MIN / 2;
    int beta = INT_MAX / 2;
    int i;

    if (depth == 0)
        depth = 1;

    if (xq_position_king_square(pos, pos->side_to_move) == XQ_NO_SQUARE)
        return false;

    xq_generate_pseudo_legal(pos, &list);
    if (list.count == 0)
        return false;
    order_moves(engine, pos, &list);

    for (i = 0; i < list.count; ++i)
    {
        int score;

        if (!xq_position_make_move(pos, list.moves[i]))
            continue;
        score = -negamax(engine, pos, depth - 1, -beta, -alpha);
        xq_position_unmake_move(pos, list.moves[i]);
        if (score > alpha)
        {
            alpha = score;
            *best_move = list.moves[i];
        }
    }

    return true;
}

/**
 * 带有解释信息的引擎搜索算法，引擎为空或引擎搜索函数为空的话调用 builtin_search
 */
bool xq_engine_explain_one_ply(const XqEngineAdapter *engine, XqPosition *pos, unsigned depth, XqExplainResult *result)
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

        if (!xq_position_make_move(pos, list.moves[i]))
            continue;
        score = -negamax(engine, pos, depth - 1, -beta, -alpha);
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
 * 引擎搜索算法，引擎为空或引擎搜索函数的话调用 builtin_search
 */
bool xq_engine_find_best_move(const XqEngineAdapter *engine, XqPosition *pos, unsigned depth, XqMove *best_move)
{
    if (best_move == NULL)
        return false;
    if (engine != NULL && engine->search != NULL)
        return engine->search(pos, depth, best_move, engine->user);
    return builtin_search(engine, pos, depth, best_move);
}
