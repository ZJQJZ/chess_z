#include "xiangqi/jieqi/position_jq.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/**
 * 判断某个棋盘编号是否合法
 */
static bool valid_square(XqSquare sq)
{
    return sq >= 0 && sq < XQ_SQUARES;
}

/**
 * 通过固定的 SplitMix64 混合函数为棋子位置生成可复现的 Zobrist 键。
 * 不使用运行时随机表，避免初始化顺序和并发问题。
 */
static uint64_t zobrist_mix(uint64_t value)
{
    value += UINT64_C(0x9e3779b97f4a7c15);
    value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
}

static uint64_t zobrist_piece_key(int piece, XqSquare sq)
{
    uint64_t index = (uint64_t)piece * XQ_SQUARES + (uint64_t)sq;
    return zobrist_mix(UINT64_C(0x243f6a8885a308d3) + index);
}

static uint64_t zobrist_side_key(void)
{
    return zobrist_mix(UINT64_C(0x13198a2e03707344));
}

static uint64_t zobrist_dark_key(XqSquare sq)
{
    return zobrist_mix(UINT64_C(0xa4093822299f31d0) + (uint64_t)sq);
}

static uint64_t zobrist_pool_key(XqColor color, XqPieceType type, uint16_t count)
{
    uint64_t index = ((uint64_t)color * XQ_PIECE_TYPE_NB + (uint64_t)type) << 16;
    return count == 0 ? UINT64_C(0) :
           zobrist_mix(UINT64_C(0x082efa98ec4e6c89) + index + count);
}

/* These helpers update only public information. Hidden real-game assignments never
 * contribute to a position or its hash. */
static void set_pool_count(XqJqPosition *pos, XqColor color, XqPieceType type,
                           uint16_t count)
{
    uint16_t previous = pos->pool[color][type];
    pos->knowledge_hash ^= zobrist_pool_key(color, type, previous) ^
                           zobrist_pool_key(color, type, count);
    pos->pool_total[color] = (uint16_t)(pos->pool_total[color] - previous + count);
    pos->pool[color][type] = count;
}

/**
 * 根据棋子类型枚举获取棋子字符串名称
 */
const char *xq_jq_piece_type_name(XqPieceType type)
{
    static const char *names[] = {
        "king", "advisor", "bishop", "knight", "rook", "cannon", "pawn"};
    if (type < 0 || type >= XQ_PIECE_TYPE_NB)
        return "unknown";
    return names[type];
}

/**
 * 根据 piece（程序内部的棋盘单位信息标识）获取终端显示的更易读的棋盘单位 char
 */
char xq_jq_piece_to_char(int piece)
{
    static const char red_chars[] = {'K', 'A', 'B', 'N', 'R', 'C', 'P'};
    static const char black_chars[] = {'k', 'a', 'b', 'n', 'r', 'c', 'p'};
    XqPieceType type;

    if (piece == XQ_EMPTY_PIECE)
        return '.';

    type = xq_piece_type(piece);
    if (type < 0 || type >= XQ_PIECE_TYPE_NB)
        return '?';
    return xq_piece_color(piece) == XQ_RED ? red_chars[type] : black_chars[type];
}

/**
 * xq_jq_piece_to_char(int) 的逆操作
 */
int xq_jq_piece_from_char(char ch)
{
    XqColor color = isupper((unsigned char)ch) ? XQ_RED : XQ_BLACK;
    switch ((char)tolower((unsigned char)ch))
    {
    case 'k':
        return xq_make_piece(color, XQ_KING);
    case 'a':
        return xq_make_piece(color, XQ_ADVISOR);
    case 'b':
        return xq_make_piece(color, XQ_BISHOP);
    case 'n':
    case 'h':
        return xq_make_piece(color, XQ_KNIGHT);
    case 'r':
        return xq_make_piece(color, XQ_ROOK);
    case 'c':
        return xq_make_piece(color, XQ_CANNON);
    case 'p':
        return xq_make_piece(color, XQ_PAWN);
    default:
        return XQ_EMPTY_PIECE;
    }
}

/**
 * 清空、重置 XqJqPosition
 */
void xq_jq_position_clear(XqJqPosition *pos)
{
    int i;

    if (pos == NULL)
        return;
    memset(pos, 0, sizeof(*pos));
    for (i = 0; i < XQ_SQUARES; ++i)
        pos->board[i] = XQ_EMPTY_PIECE;
    pos->side_to_move = XQ_RED;
    pos->fullmove_number = 1;
}

/**
 * 在 *pos 的 sq 位置安置 color 方的 type 类型的棋子
 */
bool xq_jq_position_set_piece(XqJqPosition *pos, XqSquare sq, XqColor color, XqPieceType type)
{
    int piece;

    if (pos == NULL || !valid_square(sq) || color < 0 || color >= XQ_COLOR_NB ||
        type < 0 || type >= XQ_PIECE_TYPE_NB)
        return false;
    if (pos->board[sq] != XQ_EMPTY_PIECE)
        xq_jq_position_remove_piece(pos, sq);

    piece = xq_make_piece(color, type);
    pos->board[sq] = (int8_t)piece;
    pos->piece_hash ^= zobrist_piece_key(piece, sq);
    xq_bb_set(&pos->pieces[color][type], sq);
    xq_bb_set(&pos->occupied[color], sq);
    xq_bb_set(&pos->all_occupied, sq);
    return true;
}

/**
 * 删除 *pos 中 sq 处的棋子
 * 如果 sq 位置的棋子为空，则返回 false
 */
bool xq_jq_position_remove_piece(XqJqPosition *pos, XqSquare sq)
{
    int piece;
    XqColor color;
    XqPieceType type;

    if (pos == NULL || !valid_square(sq))
        return false;

    piece = pos->board[sq];
    if (piece == XQ_EMPTY_PIECE)
        return false;

    color = xq_piece_color(piece);
    type = xq_piece_type(piece);
    (void)xq_jq_position_set_dark(pos, sq, false);
    pos->piece_hash ^= zobrist_piece_key(piece, sq);
    pos->board[sq] = XQ_EMPTY_PIECE;
    xq_bb_clear(&pos->pieces[color][type], sq);
    xq_bb_clear(&pos->occupied[color], sq);
    xq_bb_clear(&pos->all_occupied, sq);
    return true;
}

/**
 * 标记暗子时保留 board 中公开的初始类型，不改变身份库。
 */
bool xq_jq_position_set_dark(XqJqPosition *pos, XqSquare sq, bool dark)
{
    if (pos == NULL || !valid_square(sq) || pos->board[sq] == XQ_EMPTY_PIECE ||
        (dark && xq_piece_type(pos->board[sq]) == XQ_KING))
        return false;
    if (xq_bb_test(pos->dark, sq) != dark)
    {
        pos->knowledge_hash ^= zobrist_dark_key(sq);
        if (dark)
            xq_bb_set(&pos->dark, sq);
        else
            xq_bb_clear(&pos->dark, sq);
    }
    return true;
}

bool xq_jq_position_is_dark(const XqJqPosition *pos, XqSquare sq)
{
    return pos != NULL && valid_square(sq) && xq_bb_test(pos->dark, sq);
}

/**
 * 根据新起点的所有暗子初始类型重建身份库，不用于恢复中盘局面。
 */
bool xq_jq_position_rebuild_pool(XqJqPosition *pos)
{
    uint16_t counts[XQ_COLOR_NB][XQ_PIECE_TYPE_NB] = {{0}};
    XqBitboard remaining;

    if (pos == NULL || (pos->dark.hi >> (XQ_SQUARES - 64)) != 0)
        return false;
    remaining = pos->dark;
    while (!xq_bb_is_empty(remaining))
    {
        XqSquare sq = xq_bb_first_square(remaining);
        int piece = pos->board[sq];
        if (piece < 0 || piece >= XQ_COLOR_NB * XQ_PIECE_TYPE_NB ||
            xq_piece_type(piece) == XQ_KING)
            return false;
        ++counts[xq_piece_color(piece)][xq_piece_type(piece)];
        xq_bb_clear(&remaining, sq);
    }
    for (int color = 0; color < XQ_COLOR_NB; ++color)
        for (int type = 0; type < XQ_PIECE_TYPE_NB; ++type)
            set_pool_count(pos, (XqColor)color, (XqPieceType)type, counts[color][type]);
    return true;
}

/**
 * 检查走子和揭示参数，不检查移动规则或自身将帅是否安全。
 */
static bool valid_move(const XqJqPosition *pos, XqMove move, int revealed_type)
{
    XqSquare from = (XqSquare)move.from;
    XqSquare to = (XqSquare)move.to;
    int piece;
    int captured;
    XqColor color;
    bool moved_dark;

    if (pos == NULL || !valid_square(from) || !valid_square(to) || from == to)
        return false;

    piece = pos->board[from];
    captured = pos->board[to];
    if (piece < 0 || piece >= XQ_COLOR_NB * XQ_PIECE_TYPE_NB ||
        captured < XQ_EMPTY_PIECE || captured >= XQ_COLOR_NB * XQ_PIECE_TYPE_NB ||
        move.piece != piece || move.captured != captured)
        return false;
    color = xq_piece_color(piece);
    moved_dark = xq_jq_position_is_dark(pos, from);
    if (color != pos->side_to_move ||
        (captured != XQ_EMPTY_PIECE && xq_piece_color(captured) == color) ||
        (color == XQ_BLACK && pos->fullmove_number == UINT64_MAX))
        return false;
    if (moved_dark)
    {
        if (xq_piece_type(piece) == XQ_KING || revealed_type <= XQ_KING ||
            revealed_type >= XQ_PIECE_TYPE_NB ||
            pos->pool[color][revealed_type] == 0 || pos->pool_total[color] == 0)
            return false;
    }
    else if (revealed_type != XQ_JQ_NO_REVEAL)
        return false;

    return true;
}

/**
 * 根据输入的一步走棋 XqMove move，改变 *pos，不保存撤销记录。
 */
bool xq_jq_position_make_move(XqJqPosition *pos, XqMove move, int revealed_type)
{
    XqSquare from = (XqSquare)move.from;
    XqSquare to = (XqSquare)move.to;
    int piece;
    int captured;
    XqColor color;
    bool moved_dark;

    if (!valid_move(pos, move, revealed_type))
        return false;
    piece = pos->board[from];
    captured = pos->board[to];
    color = xq_piece_color(piece);
    moved_dark = xq_jq_position_is_dark(pos, from);

    if (captured != XQ_EMPTY_PIECE)
        xq_jq_position_remove_piece(pos, to);
    xq_jq_position_remove_piece(pos, from);
    xq_jq_position_set_piece(pos, to, color,
                             moved_dark ? (XqPieceType)revealed_type : xq_piece_type(piece));
    if (moved_dark)
        set_pool_count(pos, color, (XqPieceType)revealed_type,
                       (uint16_t)(pos->pool[color][revealed_type] - 1));

    pos->side_to_move = xq_color_opponent(pos->side_to_move);
    if (pos->side_to_move == XQ_RED)
        ++pos->fullmove_number;
    return true;
}

/**
 * 由需要回退的调用方在走子前保存记录；本函数不修改局面。
 */
bool xq_jq_position_save_undo(const XqJqPosition *pos, XqMove move,
                              int revealed_type, XqJqUndo *undo)
{
    XqJqUndo saved;
    XqColor color;
    bool moved_dark;

    if (undo == NULL || !valid_move(pos, move, revealed_type))
        return false;
    color = xq_piece_color(move.piece);
    moved_dark = xq_jq_position_is_dark(pos, (XqSquare)move.from);

    memset(&saved, 0, sizeof(saved));
    saved.move = move;
    saved.moved_dark = moved_dark;
    saved.captured_dark = xq_jq_position_is_dark(pos, (XqSquare)move.to);
    saved.revealed_type = (int8_t)revealed_type;
    saved.side_to_move = pos->side_to_move;
    saved.halfmove_clock = pos->halfmove_clock;
    saved.fullmove_number = pos->fullmove_number;
    saved.previous_pool_count = moved_dark ? pos->pool[color][revealed_type] : 0;
    saved.piece_hash = pos->piece_hash;
    saved.knowledge_hash = pos->knowledge_hash;
    *undo = saved;
    return true;
}

/**
 * 撤回 xq_jq_position_make_move 已经执行过的一步走棋。
 * undo 必须是当前最后一步成功走棋产生的记录；随机状态不随撤销回退。
 */
bool xq_jq_position_unmake_move(XqJqPosition *pos, const XqJqUndo *undo)
{
    XqSquare from;
    XqSquare to;
    int piece;
    int captured;
    XqColor color;

    if (pos == NULL || undo == NULL)
        return false;
    from = (XqSquare)undo->move.from;
    to = (XqSquare)undo->move.to;
    piece = undo->move.piece;
    captured = undo->move.captured;
    if (!valid_square(from) || !valid_square(to) || from == to ||
        piece < 0 || piece >= XQ_COLOR_NB * XQ_PIECE_TYPE_NB ||
        captured < XQ_EMPTY_PIECE || captured >= XQ_COLOR_NB * XQ_PIECE_TYPE_NB)
        return false;
    color = xq_piece_color(piece);
    if (undo->side_to_move != color || pos->side_to_move != xq_color_opponent(color) ||
        pos->board[from] != XQ_EMPTY_PIECE ||
        xq_jq_position_is_dark(pos, from) || xq_jq_position_is_dark(pos, to) ||
        (captured != XQ_EMPTY_PIECE && xq_piece_color(captured) == color) ||
        (undo->captured_dark &&
         (captured == XQ_EMPTY_PIECE || xq_piece_type(captured) == XQ_KING)) ||
        pos->halfmove_clock != undo->halfmove_clock ||
        (color == XQ_BLACK && undo->fullmove_number == UINT64_MAX) ||
        pos->fullmove_number != undo->fullmove_number + (color == XQ_BLACK ? 1u : 0u))
        return false;
    if (undo->moved_dark)
    {
        if (xq_piece_type(piece) == XQ_KING || undo->revealed_type <= XQ_KING ||
            undo->revealed_type >= XQ_PIECE_TYPE_NB ||
            undo->previous_pool_count == 0 ||
            pos->pool[color][undo->revealed_type] != undo->previous_pool_count - 1 ||
            pos->board[to] != xq_make_piece(color, (XqPieceType)undo->revealed_type))
            return false;
    }
    else if (undo->revealed_type != XQ_JQ_NO_REVEAL || pos->board[to] != piece)
        return false;

    xq_jq_position_remove_piece(pos, to);
    xq_jq_position_set_piece(pos, from, xq_piece_color(piece), xq_piece_type(piece));
    if (undo->moved_dark)
    {
        (void)xq_jq_position_set_dark(pos, from, true);
        set_pool_count(pos, color, (XqPieceType)undo->revealed_type, undo->previous_pool_count);
    }
    if (captured != XQ_EMPTY_PIECE)
    {
        xq_jq_position_set_piece(pos, to, xq_piece_color(captured), xq_piece_type(captured));
        if (undo->captured_dark)
            (void)xq_jq_position_set_dark(pos, to, true);
    }
    pos->side_to_move = undo->side_to_move;
    pos->halfmove_clock = undo->halfmove_clock;
    pos->fullmove_number = undo->fullmove_number;
    pos->piece_hash = undo->piece_hash;
    pos->knowledge_hash = undo->knowledge_hash;

    return true;
}

/**
 * 根据字符串初始化棋盘
 */
void xq_jq_position_startpos(XqJqPosition *pos)
{
    static const char *start_fen =
        "r*n*b*a*ka*b*n*r*/9/1c*5c*1/p*1p*1p*1p*1p*/9/9/"
        "P*1P*1P*1P*1P*/1C*5C*1/9/R*N*B*A*KA*B*N*R* r - - 0 1";
    (void)xq_jq_position_from_fen(pos, start_fen);
}

/**
 * 根据 *fen 初始化 *pos
 * 初始化规则看 xq_jq_position_startpos 的 *start_fen 就能明白
 */
bool xq_jq_position_from_fen(XqJqPosition *pos, const char *fen)
{
    int fen_rank = 0;
    int file = 0;
    const char *p = fen;
    XqJqPosition parsed;

    if (pos == NULL || fen == NULL)
        return false;

    xq_jq_position_clear(&parsed);

    while (*p != '\0' && *p != ' ')
    {
        int rank = XQ_RANKS - 1 - fen_rank;

        if (*p == '/')
        {
            if (file != XQ_FILES || fen_rank >= XQ_RANKS - 1)
                return false;
            ++fen_rank;
            file = 0;
            ++p;
            continue;
        }

        if (isdigit((unsigned char)*p))
        {
            if (*p < '1' || *p > '9')
                return false;
            file += *p - '0';
            if (file > XQ_FILES)
                return false;
            ++p;
            continue;
        }

        if (file >= XQ_FILES || fen_rank >= XQ_RANKS)
            return false;

        {
            int piece = xq_jq_piece_from_char(*p);
            XqSquare sq = xq_square_make(file, rank);
            if (piece == XQ_EMPTY_PIECE)
                return false;
            xq_jq_position_set_piece(&parsed, sq, xq_piece_color(piece), xq_piece_type(piece));
            ++p;
            if (*p == '*')
            {
                if (!xq_jq_position_set_dark(&parsed, sq, true))
                    return false;
                ++p;
            }
        }
        ++file;
    }

    if (fen_rank != XQ_RANKS - 1 || file != XQ_FILES)
        return false;

    while (*p == ' ')
        ++p;
    if (*p == 'b')
        parsed.side_to_move = XQ_BLACK;
    else if (*p == 'r' || *p == 'w')
        parsed.side_to_move = XQ_RED;
    else
        return false;
    ++p;
    if (*p != '\0' && *p != ' ')
        return false;

    if (!xq_jq_position_rebuild_pool(&parsed) || !xq_jq_position_validate(&parsed))
        return false;
    *pos = parsed;
    return true;
}

/**
 * 导出当前公开棋面及暗子标记；只有新起点的身份库可由此重新构造。
 */
bool xq_jq_position_to_fen(const XqJqPosition *pos, char *buffer, size_t buffer_size)
{
    size_t used = 0;
    int fen_rank;

    if (pos == NULL || buffer == NULL || buffer_size == 0)
        return false;

    for (fen_rank = 0; fen_rank < XQ_RANKS; ++fen_rank)
    {
        int rank = XQ_RANKS - 1 - fen_rank;
        int empty = 0;
        int file;

        for (file = 0; file < XQ_FILES; ++file)
        {
            XqSquare sq = xq_square_make(file, rank);
            int piece = pos->board[sq];
            char ch;

            if (piece == XQ_EMPTY_PIECE)
            {
                ++empty;
                continue;
            }

            if (empty > 0)
            {
                if (used + 1 >= buffer_size)
                    return false;
                buffer[used++] = (char)('0' + empty);
                empty = 0;
            }

            ch = xq_jq_piece_to_char(piece);
            if (used + 1 >= buffer_size)
                return false;
            buffer[used++] = ch;
            if (xq_jq_position_is_dark(pos, sq))
            {
                if (used + 1 >= buffer_size)
                    return false;
                buffer[used++] = '*';
            }
        }

        if (empty > 0)
        {
            if (used + 1 >= buffer_size)
                return false;
            buffer[used++] = (char)('0' + empty);
        }

        if (fen_rank != XQ_RANKS - 1)
        {
            if (used + 1 >= buffer_size)
                return false;
            buffer[used++] = '/';
        }
    }

    if (used + 6 >= buffer_size)
        return false;
    buffer[used++] = ' ';
    buffer[used++] = pos->side_to_move == XQ_RED ? 'r' : 'b';
    buffer[used++] = ' ';
    buffer[used++] = '-';
    buffer[used++] = ' ';
    buffer[used++] = '-';
    buffer[used] = '\0';
    return true;
}

/**
 * 返回将帅所在的棋盘编号
 * 如果没找到的话，返回 XQ_NO_SQUARE
 */
XqSquare xq_jq_position_king_square(const XqJqPosition *pos, XqColor color)
{
    if (pos == NULL || color < 0 || color >= XQ_COLOR_NB)
        return XQ_NO_SQUARE;
    return xq_bb_first_square(pos->pieces[color][XQ_KING]);
}

/**
 * 查看 *pos 中某种类型棋子的数量
 */
int xq_jq_position_piece_count(const XqJqPosition *pos, XqColor color, XqPieceType type)
{
    if (pos == NULL || color < 0 || color >= XQ_COLOR_NB ||
        type < 0 || type >= XQ_PIECE_TYPE_NB)
        return 0;
    return xq_bb_count(pos->pieces[color][type]);
}

/**
 * 返回包含棋子布局、暗子标记、公开身份库和行棋方的局面哈希。
 * 回合计数和本地秘密身份不参与哈希。
 */
uint64_t xq_jq_position_hash(const XqJqPosition *pos)
{
    return pos->piece_hash ^ pos->knowledge_hash ^
           (pos->side_to_move == XQ_BLACK ? zobrist_side_key() : UINT64_C(0));
}

/**
 * 用于检查某个 *pos 是否数据一致且双方存在将帅
 * 根据 *pos 的 board 信息复制一份相匹配的 XqJqPosition 中的 pieces、occupied、all 信息
 * 然后和 *pos 中的进行比较查看是否一致，最后看是否存在双方将帅
 */
bool xq_jq_position_validate(const XqJqPosition *pos)
{
    XqBitboard pieces[XQ_COLOR_NB][XQ_PIECE_TYPE_NB];
    XqBitboard occupied[XQ_COLOR_NB] = {xq_bb_empty(), xq_bb_empty()};
    XqBitboard all = xq_bb_empty();
    uint64_t piece_hash = UINT64_C(0);
    uint64_t knowledge_hash = UINT64_C(0);
    uint16_t dark_count[XQ_COLOR_NB] = {0};
    int sq;
    int color;
    int type;

    if (pos == NULL || pos->side_to_move < 0 || pos->side_to_move >= XQ_COLOR_NB ||
        pos->fullmove_number == 0 || (pos->dark.hi >> (XQ_SQUARES - 64)) != 0)
        return false;

    for (color = 0; color < XQ_COLOR_NB; ++color)
        for (type = 0; type < XQ_PIECE_TYPE_NB; ++type)
            pieces[color][type] = xq_bb_empty();

    for (sq = 0; sq < XQ_SQUARES; ++sq)
    {
        int piece = pos->board[sq];
        if (piece == XQ_EMPTY_PIECE)
        {
            if (xq_bb_test(pos->dark, (XqSquare)sq))
                return false;
            continue;
        }

        if (piece < 0 || piece >= XQ_COLOR_NB * XQ_PIECE_TYPE_NB)
            return false;

        xq_bb_set(&occupied[xq_piece_color(piece)], (XqSquare)sq);
        xq_bb_set(&all, (XqSquare)sq);
        xq_bb_set(&pieces[xq_piece_color(piece)][xq_piece_type(piece)], (XqSquare)sq);
        piece_hash ^= zobrist_piece_key(piece, (XqSquare)sq);
        if (xq_bb_test(pos->dark, (XqSquare)sq))
        {
            if (xq_piece_type(piece) == XQ_KING)
                return false;
            ++dark_count[xq_piece_color(piece)];
            knowledge_hash ^= zobrist_dark_key((XqSquare)sq);
        }
    }

    for (color = 0; color < XQ_COLOR_NB; ++color)
        for (type = 0; type < XQ_PIECE_TYPE_NB; ++type)
            if (pieces[color][type].lo != pos->pieces[color][type].lo ||
                pieces[color][type].hi != pos->pieces[color][type].hi)
                return false;

    if (occupied[XQ_RED].lo != pos->occupied[XQ_RED].lo || occupied[XQ_RED].hi != pos->occupied[XQ_RED].hi)
        return false;
    if (occupied[XQ_BLACK].lo != pos->occupied[XQ_BLACK].lo || occupied[XQ_BLACK].hi != pos->occupied[XQ_BLACK].hi)
        return false;
    if (all.lo != pos->all_occupied.lo || all.hi != pos->all_occupied.hi)
        return false;
    if (piece_hash != pos->piece_hash)
        return false;
    for (color = 0; color < XQ_COLOR_NB; ++color)
    {
        unsigned total = 0;
        if (pos->pool[color][XQ_KING] != 0)
            return false;
        for (type = 0; type < XQ_PIECE_TYPE_NB; ++type)
        {
            total += pos->pool[color][type];
            knowledge_hash ^= zobrist_pool_key((XqColor)color, (XqPieceType)type,
                                               pos->pool[color][type]);
        }
        if (total != pos->pool_total[color] || total > XQ_SQUARES || total < dark_count[color])
            return false;
    }
    if (knowledge_hash != pos->knowledge_hash)
        return false;

    return xq_jq_position_piece_count(pos, XQ_RED, XQ_KING) == 1 &&
           xq_jq_position_piece_count(pos, XQ_BLACK, XQ_KING) == 1;
}

/**
 * 调用方持有的 SplitMix64 状态，不使用全局随机数或运行时共享随机表。
 */
uint64_t xq_jq_random_next(uint64_t *state)
{
    uint64_t value;

    if (state == NULL)
        return UINT64_C(0);
    *state += UINT64_C(0x9e3779b97f4a7c15);
    value = *state;
    value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
}

/* Rejection removes modulo bias; callers always pass a positive bound. */
static uint64_t random_bounded(uint64_t *state, uint64_t bound)
{
    uint64_t threshold = (UINT64_C(0) - bound) % bound;
    uint64_t value;

    do
    {
        value = xq_jq_random_next(state);
    } while (value < threshold);
    return value % bound;
}

/**
 * 从有效局面的公开身份库按数量加权抽样。直接使用已维护的总数，
 * 每次最多扫描六类棋子，不分配内存、不扣库。
 */
bool xq_jq_position_sample_type(const XqJqPosition *pos, XqColor color,
                                uint64_t *state, XqPieceType *type)
{
    unsigned total;
    uint64_t index;

    if (pos == NULL || state == NULL || type == NULL || color < 0 || color >= XQ_COLOR_NB ||
        pos->pool[color][XQ_KING] != 0)
        return false;
    total = pos->pool_total[color];
    if (total == 0 || total > XQ_SQUARES)
        return false;
    index = random_bounded(state, total);
    for (int candidate = XQ_ADVISOR; candidate < XQ_PIECE_TYPE_NB; ++candidate)
    {
        uint16_t count = pos->pool[color][candidate];
        if (index < count)
        {
            *type = (XqPieceType)candidate;
            return true;
        }
        index -= count;
    }
    return false;
}

/**
 * 仅在新起点为本地对局预分配真实身份。映射不进入公开局面，走棋和撤销
 * 都不改映射；暗子在首次移动前仍位于原格，所以无需额外的棋子编号。
 */
bool xq_jq_hidden_init(XqJqHiddenState *hidden, const XqJqPosition *pos, uint64_t *state)
{
    XqJqHiddenState assigned;
    uint64_t next_state;
    unsigned live_count[XQ_COLOR_NB] = {0};

    if (hidden == NULL || pos == NULL || state == NULL || !xq_jq_position_validate(pos))
        return false;
    for (int sq = 0; sq < XQ_SQUARES; ++sq)
    {
        assigned.type[sq] = XQ_JQ_NO_REVEAL;
        if (xq_jq_position_is_dark(pos, (XqSquare)sq))
            ++live_count[xq_piece_color(pos->board[sq])];
    }
    for (int color = 0; color < XQ_COLOR_NB; ++color)
        if (live_count[color] != pos->pool_total[color])
            return false;
    next_state = *state;
    for (int color = 0; color < XQ_COLOR_NB; ++color)
    {
        int8_t identities[XQ_SQUARES];
        unsigned count = 0;
        unsigned next = 0;

        for (int type = XQ_ADVISOR; type < XQ_PIECE_TYPE_NB; ++type)
            for (unsigned i = 0; i < pos->pool[color][type]; ++i)
                identities[count++] = (int8_t)type;
        for (unsigned remaining = count; remaining > 1; --remaining)
        {
            unsigned other = (unsigned)random_bounded(&next_state, remaining);
            int8_t temporary = identities[remaining - 1];
            identities[remaining - 1] = identities[other];
            identities[other] = temporary;
        }
        for (int sq = 0; sq < XQ_SQUARES; ++sq)
            if (xq_jq_position_is_dark(pos, (XqSquare)sq) &&
                xq_piece_color(pos->board[sq]) == (XqColor)color)
                assigned.type[sq] = identities[next++];
    }
    *hidden = assigned;
    *state = next_state;
    return true;
}

bool xq_jq_hidden_get_type(const XqJqHiddenState *hidden, XqSquare original_square,
                          XqPieceType *type)
{
    int identity;

    if (hidden == NULL || type == NULL || !valid_square(original_square))
        return false;
    identity = hidden->type[original_square];
    if (identity <= XQ_KING || identity >= XQ_PIECE_TYPE_NB)
        return false;
    *type = (XqPieceType)identity;
    return true;
}

/**
 * 将内部的棋子表达 piece 转换为更易懂的中文字符
 */
const char *xq_jq_piece_to_text(int piece)
{
    static const char *red_text[] = {"帥", "仕", "相", "馬", "車", "炮", "兵"};
    static const char *black_text[] = {"将", "士", "象", "馬", "車", "炮", "卒"};
    XqPieceType type;

    if (piece == XQ_EMPTY_PIECE)
        return ".";

    type = xq_piece_type(piece);
    if (type < 0 || type >= XQ_PIECE_TYPE_NB)
        return "?";

    return xq_piece_color(piece) == XQ_RED ? red_text[type] : black_text[type];
}

/**
 * 打印 *pos 的状况
 */
void xq_jq_position_print(const XqJqPosition *pos)
{
    xq_jq_position_print_oriented(pos, false);
}

/* Only the traversal order changes; square labels retain their original meaning. */
void xq_jq_position_print_oriented(const XqJqPosition *pos, bool flipped)
{
    xq_jq_position_print_with_last_move(pos, flipped, NULL);
}

void xq_jq_position_print_with_last_move(const XqJqPosition *pos, bool flipped, const XqMove *last_move)
{
    int row;
    int column;

    for (row = 0; row < XQ_RANKS; ++row)
    {
        int rank = flipped ? row : XQ_RANKS - 1 - row;
        printf("%d ", rank);
        for (column = 0; column < XQ_FILES; ++column)
        {
            int file = flipped ? XQ_FILES - 1 - column : column;
            XqSquare sq = xq_square_make(file, rank);
            int piece = pos->board[sq];
            const char *text = xq_jq_piece_to_text(piece);
            const char *marker = xq_jq_position_is_dark(pos, sq) ? "*" : " ";
            if (last_move != NULL && (sq == last_move->from || sq == last_move->to))
            {
                int foreground = piece != XQ_EMPTY_PIECE && xq_piece_color(piece) == XQ_RED ? 31 : 30;
                int background = sq == last_move->from ? 106 : 103;
                /* All cells reserve one column for the public dark marker. */
                printf("\x1b[%d;%dm%s%s%s\x1b[0m ", foreground, background, text,
                       piece == XQ_EMPTY_PIECE ? " " : "", marker);
            }
            else if (piece != XQ_EMPTY_PIECE && xq_piece_color(piece) == XQ_RED)
                printf("\x1b[31m%s%s\x1b[0m ", text, marker);
            else if (piece != XQ_EMPTY_PIECE)
                printf("%s%s ", text, marker);
            else
                printf("%s   ", text);
        }
        printf("\n");
    }
    printf("  ");
    for (column = 0; column < XQ_FILES; ++column)
    {
        int file = flipped ? XQ_FILES - 1 - column : column;
        printf("%c%s", 'a' + file, column == XQ_FILES - 1 ? "\n" : "   ");
    }
    printf("side: %s\n", pos->side_to_move == XQ_RED ? "red" : "black");
}
