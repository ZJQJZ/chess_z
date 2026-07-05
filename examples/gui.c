#include "xiangqi/engine.h"
#include "xiangqi/movegen.h"
#include "xiangqi/position.h"

#include <raylib.h>

#include <stdbool.h>
#include <stdio.h>

enum
{
    WINDOW_WIDTH = 620,
    WINDOW_HEIGHT = 680,
    BOARD_LEFT = 70,
    BOARD_TOP = 55,
    CELL_SIZE = 60,
    ENGINE_DEPTH = 6
};

static Vector2 square_center(XqSquare sq)
{
    Vector2 center = {
        (float)(BOARD_LEFT + xq_square_file(sq) * CELL_SIZE),
        (float)(BOARD_TOP + (9 - xq_square_rank(sq)) * CELL_SIZE)};
    return center;
}

static XqSquare square_at_mouse(Vector2 mouse)
{
    int file;
    int screen_rank;

    if (mouse.x < BOARD_LEFT - CELL_SIZE / 2 ||
        mouse.x > BOARD_LEFT + 8 * CELL_SIZE + CELL_SIZE / 2 ||
        mouse.y < BOARD_TOP - CELL_SIZE / 2 ||
        mouse.y > BOARD_TOP + 9 * CELL_SIZE + CELL_SIZE / 2)
        return XQ_NO_SQUARE;

    file = (int)((mouse.x - BOARD_LEFT + CELL_SIZE / 2) / CELL_SIZE);
    screen_rank = (int)((mouse.y - BOARD_TOP + CELL_SIZE / 2) / CELL_SIZE);
    if (!xq_square_is_valid(file, 9 - screen_rank))
        return XQ_NO_SQUARE;
    return xq_square_make(file, 9 - screen_rank);
}

static void draw_board_lines(void)
{
    Color line = (Color){90, 55, 25, 255};
    int i;

    for (i = 0; i < 10; ++i)
        DrawLine(BOARD_LEFT, BOARD_TOP + i * CELL_SIZE,
                 BOARD_LEFT + 8 * CELL_SIZE, BOARD_TOP + i * CELL_SIZE, line);

    DrawLine(BOARD_LEFT, BOARD_TOP, BOARD_LEFT, BOARD_TOP + 9 * CELL_SIZE, line);
    DrawLine(BOARD_LEFT + 8 * CELL_SIZE, BOARD_TOP,
             BOARD_LEFT + 8 * CELL_SIZE, BOARD_TOP + 9 * CELL_SIZE, line);
    for (i = 1; i < 8; ++i)
    {
        int x = BOARD_LEFT + i * CELL_SIZE;
        DrawLine(x, BOARD_TOP, x, BOARD_TOP + 4 * CELL_SIZE, line);
        DrawLine(x, BOARD_TOP + 5 * CELL_SIZE, x, BOARD_TOP + 9 * CELL_SIZE, line);
    }

    DrawLine(BOARD_LEFT + 3 * CELL_SIZE, BOARD_TOP,
             BOARD_LEFT + 5 * CELL_SIZE, BOARD_TOP + 2 * CELL_SIZE, line);
    DrawLine(BOARD_LEFT + 5 * CELL_SIZE, BOARD_TOP,
             BOARD_LEFT + 3 * CELL_SIZE, BOARD_TOP + 2 * CELL_SIZE, line);
    DrawLine(BOARD_LEFT + 3 * CELL_SIZE, BOARD_TOP + 7 * CELL_SIZE,
             BOARD_LEFT + 5 * CELL_SIZE, BOARD_TOP + 9 * CELL_SIZE, line);
    DrawLine(BOARD_LEFT + 5 * CELL_SIZE, BOARD_TOP + 7 * CELL_SIZE,
             BOARD_LEFT + 3 * CELL_SIZE, BOARD_TOP + 9 * CELL_SIZE, line);

    DrawText("CHU HE", BOARD_LEFT + 75, BOARD_TOP + 4 * CELL_SIZE + 20, 20, line);
    DrawText("HAN JIE", BOARD_LEFT + 310, BOARD_TOP + 4 * CELL_SIZE + 20, 20, line);
}

static void draw_position(const XqPosition *pos, XqSquare selected, const char *status,
                          Font piece_font)
{
    XqMoveList legal;
    int sq;

    draw_board_lines();
    if (selected != XQ_NO_SQUARE)
    {
        int i;
        xq_generate_legal(pos, &legal);
        DrawCircleV(square_center(selected), 29.0f, (Color){255, 205, 60, 180});
        for (i = 0; i < legal.count; ++i)
            if ((XqSquare)legal.moves[i].from == selected)
                DrawCircleV(square_center((XqSquare)legal.moves[i].to), 7.0f,
                            (Color){40, 150, 70, 210});
    }

    for (sq = 0; sq < XQ_SQUARES; ++sq)
    {
        int piece = pos->board[sq];
        Vector2 center;
        Color color;
        const char *text;
        Vector2 text_size;

        if (piece == XQ_EMPTY_PIECE)
            continue;
        center = square_center((XqSquare)sq);
        color = xq_piece_color(piece) == XQ_RED ? (Color){190, 45, 35, 255}
                                                : (Color){35, 35, 35, 255};
        DrawCircleV(center, 24.0f, (Color){244, 213, 151, 255});
        DrawCircleLines((int)center.x, (int)center.y, 24.0f, color);
        text = xq_piece_to_text(piece);
        text_size = MeasureTextEx(piece_font, text, 28.0f, 0.0f);
        DrawTextEx(piece_font, text,
                   (Vector2){center.x - text_size.x / 2, center.y - text_size.y / 2},
                   28.0f, 0.0f, color);
    }

    DrawText(status, 20, WINDOW_HEIGHT - 35, 20, (Color){55, 40, 25, 255});
}

static bool make_selected_move(XqPosition *pos, XqSquare from, XqSquare to)
{
    XqMoveList legal;
    int i;

    xq_generate_legal(pos, &legal);
    for (i = 0; i < legal.count; ++i)
        if ((XqSquare)legal.moves[i].from == from &&
            (XqSquare)legal.moves[i].to == to)
            return xq_position_make_move(pos, legal.moves[i]);
    return false;
}

int main(void)
{
    XqPosition pos;
    XqSquare selected = XQ_NO_SQUARE;
    bool game_over = false;
    char status[80] = "Red: select a piece";
    int codepoint_count;
    int *codepoints;
    Font piece_font;

    xq_position_startpos(&pos);
    InitWindow(WINDOW_WIDTH, WINDOW_HEIGHT, "chess_z");
    codepoints = LoadCodepoints("帥仕相馬車炮兵将士象卒", &codepoint_count);
    piece_font = LoadFontEx("/Library/Fonts/Arial Unicode.ttf", 48,
                            codepoints, codepoint_count);
    UnloadCodepoints(codepoints);
    SetTargetFPS(60);

    while (!WindowShouldClose())
    {
        if (!game_over && pos.side_to_move == XQ_RED &&
            IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
        {
            XqSquare clicked = square_at_mouse(GetMousePosition());
            if (clicked != XQ_NO_SQUARE)
            {
                int piece = pos.board[clicked];
                if (selected == XQ_NO_SQUARE)
                {
                    if (piece != XQ_EMPTY_PIECE && xq_piece_color(piece) == XQ_RED)
                        selected = clicked;
                }
                else if (piece != XQ_EMPTY_PIECE && xq_piece_color(piece) == XQ_RED)
                    selected = clicked;
                else
                {
                    if (make_selected_move(&pos, selected, clicked))
                    {
                        XqMove best;
                        snprintf(status, sizeof(status), "Black is thinking...");
                        if (xq_engine_find_best_move(NULL, &pos, ENGINE_DEPTH, &best))
                        {
                            XqMoveList replies;
                            char move_text[8];
                            xq_position_make_move(&pos, best);
                            snprintf(status, sizeof(status), "Black played %s",
                                     xq_move_to_string(best, move_text, sizeof(move_text)));
                            xq_generate_legal(&pos, &replies);
                            if (replies.count == 0)
                            {
                                snprintf(status, sizeof(status), "Black wins");
                                game_over = true;
                            }
                        }
                        else
                        {
                            snprintf(status, sizeof(status), "Red wins");
                            game_over = true;
                        }
                    }
                    selected = XQ_NO_SQUARE;
                }
            }
        }

        BeginDrawing();
        ClearBackground((Color){238, 202, 135, 255});
        draw_position(&pos, selected, status, piece_font);
        EndDrawing();
    }

    UnloadFont(piece_font);
    CloseWindow();
    return 0;
}
