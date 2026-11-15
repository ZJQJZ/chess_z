#ifndef XIANGQI_JIEQI_POSITION_JQ_H
#define XIANGQI_JIEQI_POSITION_JQ_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "xiangqi/bitboard.h"
#include "xiangqi/types.h"

enum
{
    XQ_JQ_NO_REVEAL = -1
};

typedef struct XqJqPosition
{
    XqBitboard pieces[XQ_COLOR_NB][XQ_PIECE_TYPE_NB];
    XqBitboard occupied[XQ_COLOR_NB];
    XqBitboard all_occupied;
    uint64_t piece_hash;
    int8_t board[XQ_SQUARES];
    XqColor side_to_move;
    uint16_t halfmove_clock;
    uint64_t fullmove_number; /* Starts at 1; incremented after Black moves. */
    XqBitboard dark;
    /* Unknown identities include captured pieces whose identities were never revealed. */
    uint16_t pool[XQ_COLOR_NB][XQ_PIECE_TYPE_NB];
    uint16_t pool_total[XQ_COLOR_NB];
    uint64_t knowledge_hash; /* Incremental hash of dark markers and identity counts. */
} XqJqPosition;

typedef struct XqJqUndo
{
    XqMove move;
    bool moved_dark;
    bool captured_dark;
    int8_t revealed_type;
    XqColor side_to_move;
    uint16_t halfmove_clock;
    uint64_t fullmove_number;
    uint16_t previous_pool_count;
    uint64_t piece_hash;
    uint64_t knowledge_hash;
} XqJqUndo;

/* Keep this outside positions borrowed by search. Entries are indexed by the original
 * square of each dark piece. They remain unchanged after moves, captures and undo. */
typedef struct XqJqHiddenState
{
    int8_t type[XQ_SQUARES];
} XqJqHiddenState;

const char *xq_jq_piece_type_name(XqPieceType type);
char xq_jq_piece_to_char(int piece);
int xq_jq_piece_from_char(char ch);
const char *xq_jq_piece_to_text(int piece);

void xq_jq_position_clear(XqJqPosition *pos);
void xq_jq_position_startpos(XqJqPosition *pos);
/* FEN is a new starting position: a trailing '*' marks a dark non-king piece,
 * and all starred types form the initial identity pools. Failure leaves pos unchanged. */
bool xq_jq_position_from_fen(XqJqPosition *pos, const char *fen);
/* Exports the visible board. Midgame output does not preserve the identity pools. */
bool xq_jq_position_to_fen(const XqJqPosition *pos, char *buffer, size_t buffer_size);

/* Low-level board editing does not alter identity pools. set_piece places a bright
 * piece; remove_piece also removes its dark marker. Rebuild pools only for a new start. */
bool xq_jq_position_set_piece(XqJqPosition *pos, XqSquare sq, XqColor color, XqPieceType type);
bool xq_jq_position_remove_piece(XqJqPosition *pos, XqSquare sq);
bool xq_jq_position_set_dark(XqJqPosition *pos, XqSquare sq, bool dark);
bool xq_jq_position_is_dark(const XqJqPosition *pos, XqSquare sq);
bool xq_jq_position_rebuild_pool(XqJqPosition *pos);

/* Applies a generated move. Dark movers require an explicit, available non-king
 * type; bright movers require XQ_JQ_NO_REVEAL. Does not check movement geometry or
 * self-check and does not prepare an undo record. Failure leaves pos unchanged.
 * Neither making nor unmaking a move changes any RNG/hidden state. */
bool xq_jq_position_make_move(XqJqPosition *pos, XqMove move, int revealed_type);
/* Optional for callers that need in-place search or game undo. Save before making
 * the move, and use the record only if make_move succeeds. Does not change pos;
 * failure leaves undo unchanged. */
bool xq_jq_position_save_undo(const XqJqPosition *pos, XqMove move,
                              int revealed_type, XqJqUndo *undo);
/* The record must describe the last successful move; restore all deeper search
 * moves first and do not otherwise edit the position before unmaking it. */
bool xq_jq_position_unmake_move(XqJqPosition *pos, const XqJqUndo *undo);

XqSquare xq_jq_position_king_square(const XqJqPosition *pos, XqColor color);
int xq_jq_position_piece_count(const XqJqPosition *pos, XqColor color, XqPieceType type);
uint64_t xq_jq_position_hash(const XqJqPosition *pos);
bool xq_jq_position_validate(const XqJqPosition *pos);

/* SplitMix64 state is owned by the caller. */
uint64_t xq_jq_random_next(uint64_t *state);
/* Sampling reads only the public pool, does not consume an identity and must use
 * a separate state from the real game. pos must be valid: pool_total must match
 * the type counts, as checked by position_validate and maintained by pool updates.
 * Sampling does not repeat that consistency check. */
bool xq_jq_position_sample_type(const XqJqPosition *pos, XqColor color,
                                uint64_t *state, XqPieceType *type);
/* Preassigns identities for a new starting position, using caller-owned randomness.
 * Fails without changing hidden or state unless pool totals equal live dark counts. */
bool xq_jq_hidden_init(XqJqHiddenState *hidden, const XqJqPosition *pos, uint64_t *state);
bool xq_jq_hidden_get_type(const XqJqHiddenState *hidden, XqSquare original_square,
                          XqPieceType *type);

/* Unrevealed pieces are displayed as '*' without their initial piece names. */
void xq_jq_position_print(const XqJqPosition *pos);
void xq_jq_position_print_oriented(const XqJqPosition *pos, bool flipped);
void xq_jq_position_print_with_last_move(const XqJqPosition *pos, bool flipped,
                                       const XqMove *last_move);

#endif
