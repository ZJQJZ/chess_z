#include "movegen_internal.h"

#include <stdio.h>
#include <string.h>

static const int orthogonal[4][2]    = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
static const int advisor_steps[4][2] = {{1, 1}, {1, -1}, {-1, 1}, {-1, -1}};
static const int bishop_steps[4][2]  = {{2, 2}, {2, -2}, {-2, 2}, {-2, -2}};
static const int knight_steps[8][4]  = {{0, 1, 1, 2},    {0, 1, -1, 2},  {0, -1, 1, -2},
                                        {0, -1, -1, -2}, {1, 0, 2, 1},   {1, 0, 2, -1},
                                        {-1, 0, -2, 1},  {-1, 0, -2, -1}};

/**
 * 是否为队友
 */
static bool is_friend(const XqPosition *pos,
                      XqSquare          sq,
                      XqColor           color)
{
    int piece = pos->board[sq];
    return piece != XQ_EMPTY_PIECE && xq_piece_color(piece) == color;
}

/**
 * 是否是敌人
 */
static bool is_enemy(const XqPosition *pos,
                     XqSquare          sq,
                     XqColor           color)
{
    int piece = pos->board[sq];
    return piece != XQ_EMPTY_PIECE && xq_piece_color(piece) != color;
}

/**
 * 是否在九宫 file 列 rank 行
 */
static bool in_palace(XqColor color,
                      int     file,
                      int     rank)
{
    if (file < 3 || file > 5)
        return false;
    return color == XQ_RED ? (rank >= 0 && rank <= 2) : (rank >= 7 && rank <= 9);
}

/**
 * rank 是否在 color 一方的河岸
 */
static bool on_own_side(XqColor color,
                        int     rank)
{
    return color == XQ_RED ? rank <= 4 : rank >= 5;
}

typedef struct MoveWriter
{
    XqMove *next;
    XqMove *end;
    XqMove *first_capture;
    XqMove *king_capture;
} MoveWriter;

/**
 * 将 *pos 的 from->to 走棋追加到 *writer 中调用者须保证 from 和 to 都在棋盘范围内
 */
static void add_move(const XqPosition *pos,
                     MoveWriter       *writer,
                     XqSquare          from,
                     XqSquare          to)
{
    int piece;
    int captured;

    if (writer->next == writer->end)
        return;

    piece    = pos->board[from];
    captured = pos->board[to];
    if (captured != XQ_EMPTY_PIECE)
    {
        if (xq_piece_color(captured) == xq_piece_color(piece))
            return;
        if (writer->first_capture == NULL)
            writer->first_capture = writer->next;
        if ((captured == xq_make_piece(XQ_RED, XQ_KING) ||
             captured == xq_make_piece(XQ_BLACK, XQ_KING)) &&
            writer->king_capture == NULL)
            writer->king_capture = writer->next;
    }

    *writer->next++ = (XqMove){(uint8_t)from, (uint8_t)to, (int8_t)piece, (int8_t)captured};
}

/**
 * 带有棋盘合法位置检测和友伤判断的 add_move
 */
static void add_step_if_valid(const XqPosition *pos,
                              MoveWriter       *writer,
                              XqSquare          from,
                              int               file,
                              int               rank)
{
    XqColor color = xq_piece_color(pos->board[from]);
    if (xq_square_is_valid(file, rank) && !is_friend(pos, xq_square_make(file, rank), color))
        add_move(pos, writer, from, xq_square_make(file, rank));
}

/**
 * 生成车的走法，放到 *writer 中
 */
static void gen_rook(const XqPosition *pos,
                     MoveWriter       *writer,
                     XqSquare          from)
{
    static const int dirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    XqColor          color      = xq_piece_color(pos->board[from]);
    int              from_file  = xq_square_file(from);
    int              from_rank  = xq_square_rank(from);
    int              i;

    for (i = 0; i < 4; ++i)
    {
        int file = from_file + dirs[i][0];
        int rank = from_rank + dirs[i][1];
        while (xq_square_is_valid(file, rank))
        {
            XqSquare to = xq_square_make(file, rank);
            if (pos->board[to] == XQ_EMPTY_PIECE)
                add_move(pos, writer, from, to);
            else
            {
                if (is_enemy(pos, to, color))
                    add_move(pos, writer, from, to);
                break;
            }
            file += dirs[i][0];
            rank += dirs[i][1];
        }
    }
}

/**
 * 生成炮的走法，放到 *writer 中
 */
static void gen_cannon(const XqPosition *pos,
                       MoveWriter       *writer,
                       XqSquare          from)
{
    int row_start = from - xq_square_file(from);
    int row_end   = row_start + XQ_FILES;
    int to;

    /* 炮架前生成空格走法，越过炮架后由 add_move 判断第一颗棋子能否被吃。 */
    /* 向右 */
    for (to = from + 1; to < row_end && pos->board[to] == XQ_EMPTY_PIECE; ++to)
        add_move(pos, writer, from, (XqSquare)to);
    for (++to; to < row_end; ++to)
    {
        if (pos->board[to] == XQ_EMPTY_PIECE)
            continue;
        add_move(pos, writer, from, (XqSquare)to);
        break;
    }

    /* 向左 */
    for (to = from - 1; to >= row_start && pos->board[to] == XQ_EMPTY_PIECE; --to)
        add_move(pos, writer, from, (XqSquare)to);
    for (--to; to >= row_start; --to)
    {
        if (pos->board[to] == XQ_EMPTY_PIECE)
            continue;
        add_move(pos, writer, from, (XqSquare)to);
        break;
    }

    /* 行号递增 */
    for (to = from + XQ_FILES; to < XQ_SQUARES && pos->board[to] == XQ_EMPTY_PIECE; to += XQ_FILES)
        add_move(pos, writer, from, (XqSquare)to);
    for (to += XQ_FILES; to < XQ_SQUARES; to += XQ_FILES)
    {
        if (pos->board[to] == XQ_EMPTY_PIECE)
            continue;
        add_move(pos, writer, from, (XqSquare)to);
        break;
    }

    /* 行号递减 */
    for (to = from - XQ_FILES; to >= 0 && pos->board[to] == XQ_EMPTY_PIECE; to -= XQ_FILES)
        add_move(pos, writer, from, (XqSquare)to);
    for (to -= XQ_FILES; to >= 0; to -= XQ_FILES)
    {
        if (pos->board[to] == XQ_EMPTY_PIECE)
            continue;
        add_move(pos, writer, from, (XqSquare)to);
        break;
    }
}

/**
 * 生成将/帅走法，放到 *writer 中
 */
static void gen_king(const XqPosition *pos,
                     MoveWriter       *writer,
                     XqSquare          from)
{
    XqColor color     = xq_piece_color(pos->board[from]);
    int     file      = xq_square_file(from);
    int     rank      = xq_square_rank(from);
    int     rank_step = color == XQ_RED ? 1 : -1;
    int     i;
    int     scan_rank;

    for (i = 0; i < 4; ++i)
    {
        int to_file = file + orthogonal[i][0];
        int to_rank = rank + orthogonal[i][1];
        if (in_palace(color, to_file, to_rank))
            add_step_if_valid(pos, writer, from, to_file, to_rank);
    }

    scan_rank = rank + rank_step;
    while (scan_rank >= 0 && scan_rank < XQ_RANKS)
    {
        XqSquare to    = xq_square_make(file, scan_rank);
        int      piece = pos->board[to];
        if (piece != XQ_EMPTY_PIECE)
        {
            if (xq_piece_type(piece) == XQ_KING && xq_piece_color(piece) != color)
                add_move(pos, writer, from, to);
            break;
        }
        scan_rank += rank_step;
    }
}

/**
 * 生成士的走法，放到 *writer 中
 */
static void gen_advisor(const XqPosition *pos,
                        MoveWriter       *writer,
                        XqSquare          from)
{
    XqColor color = xq_piece_color(pos->board[from]);
    int     file  = xq_square_file(from);
    int     rank  = xq_square_rank(from);
    int     i;

    for (i = 0; i < 4; ++i)
    {
        int to_file = file + advisor_steps[i][0];
        int to_rank = rank + advisor_steps[i][1];
        if (in_palace(color, to_file, to_rank))
            add_step_if_valid(pos, writer, from, to_file, to_rank);
    }
}

/**
 * 生成象的走法，放到 *writer 中
 */
static void gen_bishop(const XqPosition *pos,
                       MoveWriter       *writer,
                       XqSquare          from)
{
    XqColor color = xq_piece_color(pos->board[from]);
    int     file  = xq_square_file(from);
    int     rank  = xq_square_rank(from);
    int     i;

    for (i = 0; i < 4; ++i)
    {
        int to_file  = file + bishop_steps[i][0];
        int to_rank  = rank + bishop_steps[i][1];
        int eye_file = file + bishop_steps[i][0] / 2;
        int eye_rank = rank + bishop_steps[i][1] / 2;

        if (!xq_square_is_valid(to_file, to_rank) || !on_own_side(color, to_rank))
            continue;
        if (pos->board[xq_square_make(eye_file, eye_rank)] != XQ_EMPTY_PIECE)
            continue;
        add_step_if_valid(pos, writer, from, to_file, to_rank);
    }
}

/**
 * 生成马的走法，放到 *writer 中
 */
static void gen_knight(const XqPosition *pos,
                       MoveWriter       *writer,
                       XqSquare          from)
{
    int file = xq_square_file(from);
    int rank = xq_square_rank(from);
    int i;

    for (i = 0; i < 8; ++i)
    {
        int leg_file = file + knight_steps[i][0];
        int leg_rank = rank + knight_steps[i][1];
        int to_file  = file + knight_steps[i][2];
        int to_rank  = rank + knight_steps[i][3];

        if (!xq_square_is_valid(to_file, to_rank))
            continue;
        if (pos->board[xq_square_make(leg_file, leg_rank)] != XQ_EMPTY_PIECE)
            continue;
        add_step_if_valid(pos, writer, from, to_file, to_rank);
    }
}

/**
 * 生成兵卒的走法，放到 *writer 中
 */
static void gen_pawn(const XqPosition *pos,
                     MoveWriter       *writer,
                     XqSquare          from)
{
    XqColor color   = xq_piece_color(pos->board[from]);
    int     file    = xq_square_file(from);
    int     rank    = xq_square_rank(from);
    int     forward = color == XQ_RED ? 1 : -1;
    bool    crossed = !on_own_side(color, rank);

    add_step_if_valid(pos, writer, from, file, rank + forward);
    if (crossed)
    {
        add_step_if_valid(pos, writer, from, file - 1, rank);
        add_step_if_valid(pos, writer, from, file + 1, rank);
    }
}

static inline void gen_piece(const XqPosition *pos,
                             MoveWriter       *writer,
                             XqSquare          sq,
                             int               piece)
{
    switch (xq_piece_type(piece))
    {
    case XQ_KING:
        gen_king(pos, writer, sq);
        break;
    case XQ_ADVISOR:
        gen_advisor(pos, writer, sq);
        break;
    case XQ_BISHOP:
        gen_bishop(pos, writer, sq);
        break;
    case XQ_KNIGHT:
        gen_knight(pos, writer, sq);
        break;
    case XQ_ROOK:
        gen_rook(pos, writer, sq);
        break;
    case XQ_CANNON:
        gen_cannon(pos, writer, sq);
        break;
    case XQ_PAWN:
        gen_pawn(pos, writer, sq);
        break;
    default:
        break;
    }
}

/**
 * 清空 XqMoveList
 */
void xq_movelist_clear(XqMoveList *list)
{
    list->count = 0;
}

/**
 * 生成伪合法走法生成某一方的所有可行走法之所以称为 "伪"，是因为没考察将军的情况
 */
void xq_generate_pseudo_legal(const XqPosition *pos,
                              XqMoveList       *list)
{
    XqBitboard remaining = pos->occupied[pos->side_to_move];
    MoveWriter writer    = {.next = list->moves, .end = list->moves + XQ_MAX_MOVES};

    while (writer.next != writer.end)
    {
        XqSquare sq = xq_bb_pop_first_square(&remaining);
        if (sq == XQ_NO_SQUARE)
            break;
        gen_piece(pos, &writer, sq, pos->board[sq]);
    }
    list->count = (int)(writer.next - list->moves);
}

static void add_dependency(XqBitboard *dependencies,
                           int         file,
                           int         rank)
{
    if (xq_square_is_valid(file, rank))
        xq_bb_set(dependencies, xq_square_make(file, rank));
}

/* Geometry is independent of occupancy, so blocked rays can retain the same mask. */
static XqBitboard piece_dependencies(XqSquare sq,
                                     int      piece)
{
    XqBitboard dependencies = xq_bb_empty();
    XqColor    color        = xq_piece_color(piece);
    int        file         = xq_square_file(sq);
    int        rank         = xq_square_rank(sq);
    /* A piece can leave and later return to a slot that still holds its previous row. */
    xq_bb_set(&dependencies, sq);
    switch (xq_piece_type(piece))
    {
    case XQ_ROOK:
    case XQ_CANNON:
        for (int f = 0; f < XQ_FILES; ++f)
            xq_bb_set(&dependencies, xq_square_make(f, rank));
        for (int r = 0; r < XQ_RANKS; ++r)
            xq_bb_set(&dependencies, xq_square_make(file, r));
        break;
    case XQ_KING:
    case XQ_ADVISOR: {
        bool king             = xq_piece_type(piece) == XQ_KING;
        const int (*steps)[2] = king ? orthogonal : advisor_steps;
        for (int i = 0; i < 4; ++i)
        {
            int f = file + steps[i][0];
            int r = rank + steps[i][1];
            if (in_palace(color, f, r))
                xq_bb_set(&dependencies, xq_square_make(f, r));
        }
        if (king)
        {
            int direction = color == XQ_RED ? 1 : -1;
            for (int r = rank + direction; r >= 0 && r < XQ_RANKS; r += direction)
                xq_bb_set(&dependencies, xq_square_make(file, r));
        }
        break;
    }
    case XQ_BISHOP:
        for (int i = 0; i < 4; ++i)
        {
            int f = file + bishop_steps[i][0];
            int r = rank + bishop_steps[i][1];
            if (xq_square_is_valid(f, r) && on_own_side(color, r))
            {
                xq_bb_set(&dependencies, xq_square_make(f, r));
                xq_bb_set(&dependencies, xq_square_make((file + f) / 2, (rank + r) / 2));
            }
        }
        break;
    case XQ_KNIGHT:
        for (int i = 0; i < 8; ++i)
        {
            int f = file + knight_steps[i][2];
            int r = rank + knight_steps[i][3];
            if (xq_square_is_valid(f, r))
            {
                xq_bb_set(&dependencies, xq_square_make(f, r));
                xq_bb_set(&dependencies,
                          xq_square_make(file + knight_steps[i][0], rank + knight_steps[i][1]));
            }
        }
        break;
    case XQ_PAWN:
        add_dependency(&dependencies, file, rank + (color == XQ_RED ? 1 : -1));
        if (!on_own_side(color, rank))
        {
            add_dependency(&dependencies, file - 1, rank);
            add_dependency(&dependencies, file + 1, rank);
        }
        break;
    default:
        break;
    }
    return dependencies;
}

/* Copy short rows with fixed-size operations so the compiler can inline them. */
static inline void copy_piece_moves(XqMove       *dst,
                                    const XqMove *src,
                                    int           count)
{
    _Static_assert(XQ_MAX_PIECE_MOVES <= 17, "piece move copy must cover a full row");
    if (count >= 9)
    {
        memcpy(dst, src, 8 * sizeof(*src));
        memcpy(dst + count - 8, src + count - 8, 8 * sizeof(*src));
        if (count == 17)
            dst[8] = src[8];
    }
    else if (count >= 4)
    {
        memcpy(dst, src, 4 * sizeof(*src));
        memcpy(dst + count - 4, src + count - 4, 4 * sizeof(*src));
    }
    else if (count >= 2)
    {
        memcpy(dst, src, 2 * sizeof(*src));
        dst[count - 1] = src[count - 1];
    }
    else if (count == 1)
        dst[0] = src[0];
}

XqMoveCacheSummary xq_move_cache_refresh(XqMoveCache      *cache,
                                         const XqPosition *pos)
{
    XqColor            color     = pos->side_to_move;
    XqBitboard         remaining = pos->occupied[color];
    XqBitboard         changed   = cache->changed[color];
    XqMoveCacheSummary summary   = {0};
    for (;;)
    {
        XqSquare sq = xq_bb_pop_first_square(&remaining);
        if (sq == XQ_NO_SQUARE)
            break;
        XqCachedPieceMoves *row           = &cache->rows[sq];
        XqBitboard          dependencies  = row->dependencies;
        bool                uninitialized = xq_bb_is_empty(dependencies);
        if (uninitialized || ((dependencies.lo & changed.lo) | (dependencies.hi & changed.hi)) != 0)
        {
            int piece = pos->board[sq];
            if (uninitialized || row->piece != piece)
            {
                row->dependencies = piece_dependencies(sq, piece);
                row->piece        = (int8_t)piece;
            }
            MoveWriter writer = {.next = row->moves, .end = row->moves + XQ_MAX_PIECE_MOVES};
            gen_piece(pos, &writer, sq, piece);
            row->count = (uint8_t)(writer.next - row->moves);
            row->first_capture_plus_one =
                writer.first_capture == NULL ? 0 : (uint8_t)(writer.first_capture - row->moves + 1);
            row->king_capture_plus_one =
                writer.king_capture == NULL ? 0 : (uint8_t)(writer.king_capture - row->moves + 1);
        }
        if (row->king_capture_plus_one != 0 &&
            summary.count + row->king_capture_plus_one <= XQ_MAX_MOVES)
            summary.king_capture = true;
        summary.count += row->count;
    }
    cache->changed[color] = xq_bb_empty();
    if (summary.count > XQ_MAX_MOVES)
        summary.count = XQ_MAX_MOVES;
    return summary;
}

void xq_move_cache_collect(const XqMoveCache *cache,
                           const XqPosition  *pos,
                           XqMoveList        *list,
                           bool               captures_only)
{
    XqBitboard remaining = pos->occupied[pos->side_to_move];
    int        available = XQ_MAX_MOVES;
    list->count          = 0;
    while (available > 0)
    {
        XqSquare sq = xq_bb_pop_first_square(&remaining);
        if (sq == XQ_NO_SQUARE)
            break;
        const XqCachedPieceMoves *row   = &cache->rows[sq];
        int                       count = row->count < available ? row->count : available;
        available -= count;
        if (!captures_only)
        {
            copy_piece_moves(list->moves + list->count, row->moves, count);
            list->count += count;
        }
        else if (row->first_capture_plus_one != 0)
        {
            for (int i = row->first_capture_plus_one - 1; i < count; ++i)
            {
                XqMove move = row->moves[i];
                if (move.captured != XQ_EMPTY_PIECE)
                    list->moves[list->count++] = move;
            }
        }
    }
}

/**
 * 判断棋盘上的 sq 棋子是否受到 by_color 方的进攻。 sq 必须放有 by_color 的敌方棋子。
 */
bool xq_square_attacked(const XqPosition *pos,
                        XqSquare          sq,
                        XqColor           by_color)
{
    static const int dirs[4][2]           = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    static const int knight_sources[8][4] = {{-1, -2, -1, -1}, {1, -2, 1, -1},   {-1, 2, -1, 1},
                                             {1, 2, 1, 1},     {-2, -1, -1, -1}, {-2, 1, -1, 1},
                                             {2, -1, 1, -1},   {2, 1, 1, 1}};
    static const int diagonals[4][2]      = {{1, 1}, {1, -1}, {-1, 1}, {-1, -1}};
    int              target_file;
    int              target_rank;
    int              target_piece;
    bool             has_rook;
    bool             has_cannon;
    bool             has_king;
    int              i;

    if (pos == NULL || sq < 0 || sq >= XQ_SQUARES || by_color < 0 || by_color >= XQ_COLOR_NB)
        return false;
    target_piece = pos->board[sq];
    if (target_piece == XQ_EMPTY_PIECE || xq_piece_color(target_piece) == by_color)
        return false;

    target_file = xq_square_file(sq);
    target_rank = xq_square_rank(sq);
    has_rook    = !xq_bb_is_empty(pos->pieces[by_color][XQ_ROOK]);
    has_cannon  = !xq_bb_is_empty(pos->pieces[by_color][XQ_CANNON]);
    has_king    = !xq_bb_is_empty(pos->pieces[by_color][XQ_KING]);

    /*
     * 车、炮、将帅都沿直线攻击。每个方向的第一个棋子可能是车；隔过恰好一个棋子后的第一个棋子可能是
     * 炮。将帅还需分别处理九宫内的一步攻击以及仅针对对方将帅的照面攻击。
     */
    if (has_rook || has_cannon || has_king)
        for (i = 0; i < 4; ++i)
        {
            int  file        = target_file + dirs[i][0];
            int  rank        = target_rank + dirs[i][1];
            bool screen_seen = false;

            while (xq_square_is_valid(file, rank))
            {
                XqSquare from = xq_square_make(file, rank);

                if (pos->board[from] == XQ_EMPTY_PIECE)
                {
                    file += dirs[i][0];
                    rank += dirs[i][1];
                    continue;
                }

                if (!screen_seen)
                {
                    if (has_rook && xq_bb_test(pos->pieces[by_color][XQ_ROOK], from))
                        return true;
                    if (has_king && xq_bb_test(pos->pieces[by_color][XQ_KING], from))
                    {
                        bool adjacent = (file == target_file ? rank == target_rank + dirs[i][1]
                                                             : file == target_file + dirs[i][0]) &&
                                        in_palace(by_color, target_file, target_rank);
                        bool flying = file == target_file && xq_piece_type(target_piece) == XQ_KING;
                        if (adjacent || flying)
                            return true;
                    }
                    if (!has_cannon)
                        break;
                    screen_seen = true;
                }
                else
                {
                    if (xq_bb_test(pos->pieces[by_color][XQ_CANNON], from))
                        return true;
                    break;
                }

                file += dirs[i][0];
                rank += dirs[i][1];
            }
        }

    /* 反向枚举可能跳到目标格的马，并检查相应马腿。 */
    if (!xq_bb_is_empty(pos->pieces[by_color][XQ_KNIGHT]))
        for (i = 0; i < 8; ++i)
        {
            int file     = target_file + knight_sources[i][0];
            int rank     = target_rank + knight_sources[i][1];
            int leg_file = target_file + knight_sources[i][2];
            int leg_rank = target_rank + knight_sources[i][3];

            if (xq_square_is_valid(file, rank) &&
                xq_bb_test(pos->pieces[by_color][XQ_KNIGHT], xq_square_make(file, rank)) &&
                pos->board[xq_square_make(leg_file, leg_rank)] == XQ_EMPTY_PIECE)
                return true;
        }

    if (!xq_bb_is_empty(pos->pieces[by_color][XQ_PAWN]))
    {
        /* 兵卒的前进攻击。 */
        {
            int source_rank = target_rank + (by_color == XQ_RED ? -1 : 1);
            if (xq_square_is_valid(target_file, source_rank) &&
                xq_bb_test(pos->pieces[by_color][XQ_PAWN],
                           xq_square_make(target_file, source_rank)))
                return true;
        }

        /* 过河兵卒的横向攻击。 */
        if (by_color == XQ_RED ? target_rank >= 5 : target_rank <= 4)
            for (i = -1; i <= 1; i += 2)
            {
                int file = target_file + i;
                if (xq_square_is_valid(file, target_rank) &&
                    xq_bb_test(pos->pieces[by_color][XQ_PAWN], xq_square_make(file, target_rank)))
                    return true;
            }
    }

    /* 士从相邻斜格攻击九宫内的目标格。 */
    if (!xq_bb_is_empty(pos->pieces[by_color][XQ_ADVISOR]) &&
        in_palace(by_color, target_file, target_rank))
        for (i = 0; i < 4; ++i)
        {
            int file = target_file + diagonals[i][0];
            int rank = target_rank + diagonals[i][1];
            if (xq_square_is_valid(file, rank) &&
                xq_bb_test(pos->pieces[by_color][XQ_ADVISOR], xq_square_make(file, rank)))
                return true;
        }

    /* 象从两格外攻击，目标必须未过河且象眼为空。 */
    if (!xq_bb_is_empty(pos->pieces[by_color][XQ_BISHOP]) && on_own_side(by_color, target_rank))
        for (i = 0; i < 4; ++i)
        {
            int file     = target_file + diagonals[i][0] * 2;
            int rank     = target_rank + diagonals[i][1] * 2;
            int eye_file = target_file + diagonals[i][0];
            int eye_rank = target_rank + diagonals[i][1];
            if (xq_square_is_valid(file, rank) &&
                xq_bb_test(pos->pieces[by_color][XQ_BISHOP], xq_square_make(file, rank)) &&
                pos->board[xq_square_make(eye_file, eye_rank)] == XQ_EMPTY_PIECE)
                return true;
        }

    return false;
}

/**
 * 查看某一方是否正在被将军，如果此方甚至找不到将帅了（虽然很少见），也算
 */
bool xq_position_in_check(const XqPosition *pos,
                          XqColor           color)
{
    XqSquare king = xq_position_king_square(pos, color);
    if (king == XQ_NO_SQUARE)
        return true;
    return xq_square_attacked(pos, king, xq_color_opponent(color));
}

/**
 * 生成合法走法
 */
void xq_generate_legal(const XqPosition *pos,
                       XqMoveList       *list)
{
    XqMoveList pseudo;
    int        i;

    xq_movelist_clear(list);
    xq_generate_pseudo_legal(pos, &pseudo);
    for (i = 0; i < pseudo.count; ++i)
    {
        XqPosition next  = *pos;
        XqColor    mover = pos->side_to_move;
        if (!xq_position_make_move(&next, pseudo.moves[i]))
            continue;
        if (!xq_position_in_check(&next, mover))
            if (list->count < XQ_MAX_MOVES)
                list->moves[list->count++] = pseudo.moves[i];
    }
}

/**
 * 测试
 */
uint64_t xq_perft(const XqPosition *pos,
                  int               depth)
{
    XqMoveList list;
    uint64_t   nodes = 0;
    int        i;

    if (depth == 0)
        return 1u;

    xq_generate_legal(pos, &list);
    if (depth == 1)
        return (uint64_t)list.count;

    for (i = 0; i < list.count; ++i)
    {
        XqPosition next = *pos;
        xq_position_make_move(&next, list.moves[i]);
        nodes += xq_perft(&next, depth - 1);
    }
    return nodes;
}

/**
 * 将一个 XqMove 转换为字符串
 */
const char *xq_move_to_string(XqMove move,
                              char  *buffer,
                              size_t buffer_size)
{
    if (buffer == NULL || buffer_size < 6)
        return "";

    (void)snprintf(
        buffer, buffer_size, "%c%d%c%d", (char)('a' + xq_square_file((XqSquare)move.from)),
        xq_square_rank((XqSquare)move.from), (char)('a' + xq_square_file((XqSquare)move.to)),
        xq_square_rank((XqSquare)move.to));
    return buffer;
}
