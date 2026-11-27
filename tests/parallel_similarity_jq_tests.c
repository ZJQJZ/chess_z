#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "xiangqi/jieqi/movegen_jq.h"
#include "movegen_internal_jq.h"

static _Thread_local unsigned pseudo_legal_generations;
static _Thread_local unsigned full_collections;
static _Thread_local unsigned capture_collections;
static _Thread_local unsigned dark_move_applications;
static _Thread_local unsigned identity_samples;

static void count_pseudo_legal_generation(const XqJqPosition *position, XqJqMoveList *moves)
{
    ++pseudo_legal_generations;
    xq_jq_generate_pseudo_legal(position, moves);
}

static XqJqMoveCacheSummary count_cache_refresh(XqJqMoveCache *cache, const XqJqPosition *position)
{
    ++pseudo_legal_generations;
    return xq_jq_move_cache_refresh(cache, position);
}

static void count_cache_collection(const XqJqMoveCache *cache, const XqJqPosition *position,
                                    XqJqMoveList *moves, bool captures_only)
{
    if (captures_only)
        ++capture_collections;
    else
        ++full_collections;
    xq_jq_move_cache_collect(cache, position, moves, captures_only);
}

static bool count_move_application(XqJqPosition *position, XqMove move, int revealed_type)
{
    if (xq_jq_position_is_dark(position, (XqSquare)move.from))
        ++dark_move_applications;
    return xq_jq_position_make_move(position, move, revealed_type);
}

static bool count_identity_sample(const XqJqPosition *position, XqColor color,
                                   uint64_t *state, XqPieceType *type)
{
    ++identity_samples;
    return xq_jq_position_sample_type(position, color, state, type);
}

/* CMake copies the implementation with small, independent test-only enum values. */
#define xq_jq_generate_pseudo_legal count_pseudo_legal_generation
#define xq_jq_move_cache_refresh count_cache_refresh
#define xq_jq_move_cache_collect count_cache_collection
#define xq_jq_position_make_move count_move_application
#define xq_jq_position_sample_type count_identity_sample
#include "parallel_search_under_test.c"
#undef xq_jq_position_sample_type
#undef xq_jq_position_make_move
#undef xq_jq_move_cache_collect
#undef xq_jq_move_cache_refresh
#undef xq_jq_generate_pseudo_legal

_Static_assert(PARALLEL_MAX_SIMILAR_POSITION_QUERIES == XQ_TEST_SIMILARITY_QUERY_LIMIT,
               "the test driver and generated implementation must use the same query limit");

#include <stdio.h>

#define CHECK(value) do { if (!(value)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #value); abort(); \
} } while (0)

typedef struct Fixture
{
    Search search;
    Node parent;
    Node node;
} Fixture;

static XqMove board_move(const XqJqPosition *position, int file, int rank,
                         int to_file, int to_rank)
{
    XqSquare from = xq_square_make(file, rank);
    XqSquare to = xq_square_make(to_file, to_rank);
    return (XqMove){(uint8_t)from, (uint8_t)to, position->board[from], position->board[to]};
}

static void check_hash(const XqJqPosition *position, XqMove move,
                       int revealed_type, bool expected)
{
    unsigned char before[sizeof(*position)];
    memcpy(before, position, sizeof(before));
    XqJqPosition applied = *position;
    CHECK(xq_jq_position_make_move(&applied, move, revealed_type) == expected);
    uint64_t sentinel = UINT64_C(0x123456789abcdef0);
    uint64_t key = sentinel;
    CHECK(xq_jq_position_hash_after_move(position, move, revealed_type, &key) == expected);
    CHECK(key == (expected ? xq_jq_position_hash(&applied) : sentinel));
    CHECK(memcmp(before, position, sizeof(before)) == 0);
}

static void incremental_hashes(void)
{
    XqJqPosition position;
    CHECK(xq_jq_position_from_fen(&position,
        "r*3k3r/9/1c5c*1/p*1p1p*1p1p*/9/9/P*1P1P*1P1P*/1C5C*1/9/R*3K3R r - - 0 1"));
    for (int color = XQ_RED; color < XQ_COLOR_NB; ++color)
    {
        position.side_to_move = (XqColor)color;
        XqJqMoveList moves;
        xq_jq_generate_pseudo_legal(&position, &moves);
        for (int i = 0; i < moves.count; ++i)
        {
            XqMove move = moves.moves[i];
            if (!xq_jq_position_is_dark(&position, (XqSquare)move.from))
                check_hash(&position, move, XQ_JQ_NO_REVEAL, true);
            else
                for (int type = XQ_ADVISOR; type < XQ_PIECE_TYPE_NB; ++type)
                    check_hash(&position, move, type, position.pool[color][type] != 0);
        }

        /* Direct moves exercise bright and dark attackers against a dark target. */
        int own_rank = color == XQ_RED ? 2 : 7;
        int opposing_rank = color == XQ_RED ? 7 : 2;
        XqMove dark_capture = board_move(&position, 7, own_rank, 7, opposing_rank);
        XqMove bright_capture = board_move(&position, 1, own_rank, 7, opposing_rank);
        check_hash(&position, dark_capture, XQ_ROOK, true);
        check_hash(&position, bright_capture, XQ_JQ_NO_REVEAL, true);
        check_hash(&position, dark_capture, XQ_JQ_NO_REVEAL, false);
        check_hash(&position, dark_capture, XQ_KING, false);
        check_hash(&position, bright_capture, XQ_ROOK, false);
        check_hash(&position, (XqMove){.from = XQ_SQUARES, .to = 0}, XQ_JQ_NO_REVEAL, false);
        bright_capture.to = XQ_SQUARES;
        check_hash(&position, bright_capture, XQ_JQ_NO_REVEAL, false);
        check_hash(&position, board_move(&position, 0, 1, 0, 2), XQ_JQ_NO_REVEAL, false);
        check_hash(&position, board_move(&position, 1, own_rank, 1, own_rank), XQ_JQ_NO_REVEAL, false);
        dark_capture.captured = XQ_EMPTY_PIECE;
        check_hash(&position, dark_capture, XQ_ROOK, false);
    }
    position.fullmove_number = UINT64_MAX;
    check_hash(&position, board_move(&position, 1, 7, 1, 6), XQ_JQ_NO_REVEAL, false);
}

static void node_init(Node *node, const XqJqPosition *position, Node *parent, int move_index)
{
    memset(node, 0, sizeof(*node));
    node->position = *position;
    node->parent = parent;
    node->parent_move_index = move_index;
    node->depth = 4;
    node->maximizing = position->side_to_move == XQ_RED;
    node->owner_pv.best_index = -1;
    atomic_init(&node->value.lower, -WIN);
    atomic_init(&node->value.upper, WIN);
    atomic_init(&node->submitted_count, 0);
    atomic_init(&node->submitted_bound, node->maximizing ? -WIN : WIN);
    atomic_init(&node->best_index, -1);
    atomic_init(&node->alpha, -INFINITY_SCORE);
    atomic_init(&node->beta, INFINITY_SCORE);
    atomic_init(&node->state, NODE_RUNNING);
    atomic_init(&node->repetition_seen, 0);
    atomic_init(&node->estimated, 0);
}

static XqJqPosition sibling_position(const Fixture *fixture, int index)
{
    XqJqPosition result = fixture->parent.position;
    CHECK(xq_jq_position_make_move(&result, fixture->parent.moves.moves[index], XQ_JQ_NO_REVEAL));
    return result;
}

static uint64_t sibling_key(const Fixture *fixture, int index)
{
    XqJqPosition result = sibling_position(fixture, index);
    return xq_jq_position_hash(&result);
}

static void fixture_init(Fixture *fixture)
{
    memset(fixture, 0, sizeof(*fixture));
    fixture->search.options = xq_jq_parallel_options_default();
    fixture->search.options.time_limit_ms = 0;
    fixture->search.options.thread_count = 1;
    fixture->search.options.transposition_table = xq_jq_parallel_transposition_table_create();
    CHECK(fixture->search.options.transposition_table != NULL);
    CHECK((uintptr_t)fixture->search.options.transposition_table->buckets %
              PARALLEL_CACHE_LINE_ALIGNMENT == 0);
    fixture->search.root_color = XQ_RED;
    atomic_init(&fixture->search.time_state, TIME_ACTIVE);

    XqJqPosition start;
    CHECK(xq_jq_position_from_fen(&start, "4k4/9/9/p1p3p1p/9/9/P1P*1P1P1P/9/9/4K4 r - - 0 1"));
    node_init(&fixture->parent, &start, NULL, -1);
    fixture->parent.moves.count = 5;
    for (int i = 0; i < 5; ++i)
        fixture->parent.moves.moves[i] = board_move(&start, i * 2, 3, i * 2, 4);
    XqJqPosition current = sibling_position(fixture, 0);
    node_init(&fixture->node, &current, &fixture->parent, 0);
    fixture->node.moves.count = 3;
    fixture->node.moves.moves[0] = board_move(&current, 0, 6, 0, 5);
    fixture->node.moves.moves[1] = board_move(&current, 2, 6, 2, 5);
    fixture->node.moves.moves[2] = board_move(&current, 6, 6, 6, 5);
    (void)xq_jq_move_cache_refresh(&fixture->search.stats.move_cache, &current);
}

static void fixture_destroy(Fixture *fixture)
{
    xq_jq_parallel_transposition_table_destroy(fixture->search.options.transposition_table);
}

static void check_next(SimilarPositionIterator *iterator, const Fixture *fixture, int index)
{
    uint64_t key;
    CHECK(parallel_similar_position_next(iterator, &key));
    CHECK(key == sibling_key(fixture, index));
}

static void sibling_iterator(void)
{
    Fixture fixture;
    fixture_init(&fixture);
    atomic_store(&fixture.parent.best_index, 3);
    SimilarPositionIterator iterator = parallel_similar_positions(&fixture.node);
    atomic_store(&fixture.parent.best_index, 4);
    check_next(&iterator, &fixture, 3);
    check_next(&iterator, &fixture, 2);
    check_next(&iterator, &fixture, 4);
    uint64_t key = UINT64_MAX;
    CHECK(!parallel_similar_position_next(&iterator, &key) && key == UINT64_MAX);
    iterator = parallel_similar_positions(&fixture.parent);
    CHECK(!parallel_similar_position_next(&iterator, &key) && key == UINT64_MAX);

    atomic_store(&fixture.parent.best_index, 1); /* Preferred move has a dark source. */
    fixture.parent.moves.moves[2].from = XQ_SQUARES;
    fixture.parent.moves.moves[3].from = (uint8_t)xq_square_make(0, 1);
    iterator = parallel_similar_positions(&fixture.node);
    check_next(&iterator, &fixture, 4);
    CHECK(!parallel_similar_position_next(&iterator, &key) && key == UINT64_MAX);
    fixture_destroy(&fixture);
}

static void store_hint(Fixture *fixture, int sibling, int move, bool estimated)
{
    XqJqTranspositionStats ignored = {0};
    parallel_tt_store(fixture->search.options.transposition_table, sibling_key(fixture, sibling),
                      1, WIN, XQ_JQ_SEARCH_SCORE_EXACT, fixture->node.moves.moves[move],
                      estimated, &ignored);
}

static void cached_hint_move_lookup(void)
{
    XqJqPosition position;
    XqJqMoveCache move_cache = {0};
    Node node;

    xq_jq_position_startpos(&position);
    XqMove opposing_move = board_move(&position, 0, 3, 0, 4);
    (void)xq_jq_move_cache_refresh(&move_cache, &position);
    position.side_to_move = XQ_BLACK;
    node_init(&node, &position, NULL, -1);
    (void)xq_jq_move_cache_refresh(&move_cache, &node.position);
    xq_jq_move_cache_collect(&move_cache, &node.position, &node.moves, false);

    CHECK(move_cache.rows[opposing_move.from].count != 0);
    CHECK(parallel_find_similar_hint_move(&node, &move_cache, opposing_move) == NULL);

    XqMove capture = board_move(&position, 1, 7, 1, 0);
    XqMove hint = capture;
    hint.captured = XQ_EMPTY_PIECE;
    const XqMove *matched = parallel_find_similar_hint_move(&node, &move_cache, hint);
    CHECK(capture.captured != XQ_EMPTY_PIECE);
    CHECK(matched != NULL && matched->captured == capture.captured &&
          matched->from == capture.from && matched->to == capture.to && matched->piece == capture.piece);

    CHECK(xq_jq_position_from_fen(&position, "kR3C1Rn/R1N1N1N1R/1N1N1N1N1/2N1N1N2/1N1N1N1N1/"
                                          "2N1N1N2/1N1N1N1N1/2N1N1N2/1C1N1N1N1/C2K4C r - - 0 1"));
    CHECK(xq_jq_position_remove_piece(&position, xq_square_make(3, 0)));
    node_init(&node, &position, NULL, -1);
    xq_jq_move_cache_reset(&move_cache);
    (void)xq_jq_move_cache_refresh(&move_cache, &position);
    xq_jq_move_cache_collect(&move_cache, &position, &node.moves, false);
    CHECK(node.moves.count > XQ_MAX_MOVES);
    XqMove last_move = node.moves.moves[node.moves.count - 1];
    const XqJqCachedPieceMoves *last_row = &move_cache.rows[last_move.from];
    CHECK(parallel_find_similar_hint_move(&node, &move_cache, last_move) ==
          &last_row->moves[last_row->count - 1]);
}

static void repeated_keys_consume_budget(void)
{
    Fixture fixture;
    fixture_init(&fixture);
    const XqJqMoveCache *move_cache = &fixture.search.stats.move_cache;
    XqJqMoveList *siblings = &fixture.parent.moves;
    siblings->moves[6] = siblings->moves[4];
    siblings->moves[5] = siblings->moves[3];
    siblings->moves[3] = siblings->moves[2];
    siblings->moves[4] = siblings->moves[2];
    siblings->moves[2] = siblings->moves[0];
    siblings->count = 7;
    /* The current position's duplicate is skipped. The other duplicate spends a query. */
    store_hint(&fixture, 2, 2, false);
    store_hint(&fixture, 3, 0, true);
    store_hint(&fixture, 5, 1, false);
    store_hint(&fixture, 6, 2, false);

    unsigned char original[sizeof(fixture.node.moves)];
    memcpy(original, &fixture.node.moves, sizeof(original));
    SimilarMoveHints hints = {.quiet_count = 99, .winning_count = 99};
    CHECK(parallel_collect_similar_hints(&fixture.search, &fixture.node, move_cache, &hints) ==
          PARALLEL_MAX_SIMILAR_POSITION_QUERIES);
    if (PARALLEL_MAX_SIMILAR_POSITION_QUERIES == 0)
        CHECK(hints.quiet_count == 0 && hints.winning_count == 0);
    else
    {
        CHECK(hints.quiet_count == 1 && hints.quiet_moves[0].from == fixture.node.moves.moves[0].from);
        CHECK(hints.winning_count == 0);
    }
    CHECK(memcmp(original, &fixture.node.moves, sizeof(original)) == 0);

    xq_jq_parallel_transposition_table_clear(fixture.search.options.transposition_table);
    siblings->moves[4] = siblings->moves[5];
    store_hint(&fixture, 3, 0, true);
    store_hint(&fixture, 4, 1, false);
    CHECK(parallel_collect_similar_hints(&fixture.search, &fixture.node, move_cache, &hints) ==
          PARALLEL_MAX_SIMILAR_POSITION_QUERIES);
    if (PARALLEL_MAX_SIMILAR_POSITION_QUERIES != 0)
    {
        CHECK(hints.quiet_count == 1 && hints.quiet_moves[0].from == fixture.node.moves.moves[0].from);
        CHECK(hints.winning_count == 1 && hints.winning_moves[0].from == fixture.node.moves.moves[1].from);
    }

    xq_jq_parallel_transposition_table_clear(fixture.search.options.transposition_table);
    store_hint(&fixture, 6, 2, false);
    CHECK(parallel_collect_similar_hints(&fixture.search, &fixture.node, move_cache, &hints) ==
          PARALLEL_MAX_SIMILAR_POSITION_QUERIES);
    CHECK(hints.quiet_count == 0 && hints.winning_count == 0);
    fixture_destroy(&fixture);
}

static void pseudo_legal_keys(void)
{
    Fixture fixture;
    fixture_init(&fixture);
    XqJqPosition start;
    CHECK(xq_jq_position_from_fen(&start, "3k5/3R5/9/9/4r4/9/9/4R4/9/4K4 r - - 0 1"));
    node_init(&fixture.parent, &start, NULL, -1);
    fixture.parent.moves.count = 3;
    fixture.parent.moves.moves[0] = board_move(&start, 4, 0, 5, 0);
    fixture.parent.moves.moves[1] = board_move(&start, 4, 2, 3, 2);
    fixture.parent.moves.moves[2] = board_move(&start, 3, 8, 3, 9);
    XqJqPosition current = sibling_position(&fixture, 0);
    node_init(&fixture.node, &current, &fixture.parent, 0);
    XqJqPosition self_check = sibling_position(&fixture, 1);
    XqJqPosition missing_king = sibling_position(&fixture, 2);
    CHECK(xq_jq_position_in_check(&self_check, XQ_RED));
    CHECK(xq_jq_position_king_square(&missing_king, XQ_BLACK) == XQ_NO_SQUARE);

    SimilarPositionIterator iterator = parallel_similar_positions(&fixture.node);
    check_next(&iterator, &fixture, 1);
    check_next(&iterator, &fixture, 2);
    SimilarMoveHints hints = {0};
    XqJqMoveCache *move_cache = &fixture.search.stats.move_cache;
    xq_jq_move_cache_reset(move_cache);
    (void)xq_jq_move_cache_refresh(move_cache, &current);
    CHECK(parallel_collect_similar_hints(&fixture.search, &fixture.node, move_cache, &hints) ==
          PARALLEL_MAX_SIMILAR_POSITION_QUERIES);
    CHECK(hints.quiet_count == 0 && hints.winning_count == 0);
    fixture_destroy(&fixture);
}

static void cancellation_after_collection(void)
{
    for (int timed_stop = 0; timed_stop < 2; ++timed_stop)
    {
        Fixture fixture;
        fixture_init(&fixture);
        if (timed_stop)
            atomic_store(&fixture.search.time_state, TIME_STOP);
        else
            atomic_store(&fixture.parent.state, NODE_CANCELLED);
        store_hint(&fixture, 2, 0, false);

        SimilarMoveHints hints = {0};
        CHECK(parallel_collect_similar_hints(&fixture.search, &fixture.node,
                                            &fixture.search.stats.move_cache, &hints) ==
              PARALLEL_MAX_SIMILAR_POSITION_QUERIES);
        CHECK(hints.winning_count == (PARALLEL_MAX_SIMILAR_POSITION_QUERIES != 0 ? 1u : 0u));
        CHECK(atomic_load(&fixture.node.state) == NODE_RUNNING);
        CHECK(atomic_load(&fixture.parent.state) == (timed_stop ? NODE_RUNNING : NODE_CANCELLED));
        CHECK(atomic_load(&fixture.search.time_state) == (timed_stop ? TIME_STOP : TIME_ACTIVE));
        CHECK(check_node(&fixture.search, &fixture.node));
        CHECK(atomic_load(&fixture.node.state) == NODE_CANCELLED);

        SearchThreadStats stats = {0};
        ParallelPv pv = {0};
        Interval stopped = search_node(&fixture.search, &fixture.node.position, &fixture.parent,
                                       0, 1, false, XQ_JQ_NO_REVEAL, NULL, NULL, 0, &pv, &stats);
        CHECK(stopped.cancelled && stats.nodes == 1 && pv.count == 0);
        fixture_destroy(&fixture);
    }
}

static unsigned count_reveal_samples(unsigned depth, unsigned order, void *user)
{
    (void)depth;
    (void)order;
    atomic_uint *calls = user;
    atomic_fetch_add_explicit(calls, 1, memory_order_relaxed);
    return 2;
}

static void count_root_events(const XqJqSearchProgress *progress, void *user)
{
    (void)progress;
    atomic_uint *calls = user;
    atomic_fetch_add_explicit(calls, 1, memory_order_relaxed);
}

static void check_immediate_king_capture(const XqJqParallelResult *result, unsigned legal_moves)
{
    CHECK(result->move_available && result->best_move.from == xq_square_make(4, 5));
    CHECK(result->best_move.to == xq_square_make(4, 9));
    CHECK(result->best_move.captured == xq_make_piece(XQ_BLACK, XQ_KING));
    CHECK(result->score_available && result->score_kind == XQ_JQ_SEARCH_SCORE_EXACT);
    CHECK(result->score == WIN && !result->score_estimated && result->stats.winning_move_found);
    CHECK(result->stats.selected_move_depth == 1 && result->stats.nodes == 0);
    CHECK(result->stats.max_started_depth == 0 && result->stats.completed_depth == 0);
    CHECK(result->stats.iteration_completed_moves == 0 && !result->stats.stopped);
    CHECK(result->stats.iteration_total_moves == legal_moves);
}

static void root_king_capture_terminals(void)
{
    atomic_uint reveal_calls = 0;
    atomic_uint root_events = 0;
    XqJqParallelOptions options = xq_jq_parallel_options_default();
    options.depth = 5;
    options.thread_count = 4;
    options.time_limit_ms = 0;
    options.sample_count = count_reveal_samples;
    options.sample_user = &reveal_calls;
    options.transposition_table = xq_jq_parallel_transposition_table_create();
    CHECK(options.transposition_table != NULL);

    const char *const fens[] = {
        "4k4/9/9/9/4R4/9/9/9/9/3K5 r - - 0 1",
        "4k4/9/9/9/4R*4/9/9/9/9/3K5 r - - 0 1",
    };
    XqJqPosition position;
    XqJqParallelResult result;
    for (unsigned i = 0; i < sizeof(fens) / sizeof(fens[0]); ++i)
    {
        CHECK(xq_jq_position_from_fen(&position, fens[i]));
        unsigned char original[sizeof(position)];
        memcpy(original, &position, sizeof(original));

        /* A quiet cache hint can precede the capture in the legal list. */
        XqMove quiet = board_move(&position, 4, 5, 0, 5);
        XqJqMoveList legal;
        xq_jq_generate_legal(&position, &legal);
        CHECK(quiet.captured == XQ_EMPTY_PIECE && parallel_prioritize_move(&legal, quiet));
        uint64_t root_key = xq_jq_position_hash(&position);
        XqJqTranspositionStats ignored = {0};
        parallel_tt_store(options.transposition_table, root_key, 1, 12345,
                          XQ_JQ_SEARCH_SCORE_EXACT, quiet, false, &ignored);

        CHECK(xq_jq_engine_parallel_search_with_observer(NULL, &position, &options, &result,
              count_root_events, &root_events) == XQ_JQ_PARALLEL_OK);
        check_immediate_king_capture(&result, (unsigned)legal.count);
        CHECK(result.stats.cache.hits == 1 && result.stats.cache.cutoffs == 0);
        CHECK(result.stats.cache.stores == 1);
        ParallelTtEntry cached;
        CHECK(parallel_tt_peek(options.transposition_table, root_key, &cached));
        CHECK(cached.depth == 1 && cached.score == WIN && !cached.estimated);
        CHECK(cached.score_kind == XQ_JQ_SEARCH_SCORE_EXACT);
        CHECK(cached.best_move.from == result.best_move.from &&
              cached.best_move.to == result.best_move.to);

        CHECK(xq_jq_engine_parallel_search_with_observer(NULL, &position, &options, &result,
              count_root_events, &root_events) == XQ_JQ_PARALLEL_OK);
        check_immediate_king_capture(&result, (unsigned)legal.count);
        CHECK(result.stats.cache.cutoffs == 1 && result.stats.cache.stores == 0);
        CHECK(atomic_load(&reveal_calls) == 0 && atomic_load(&root_events) == 0);
        CHECK(memcmp(original, &position, sizeof(original)) == 0);
    }

    for (int color = XQ_RED; color < XQ_COLOR_NB; ++color)
    {
        XqJqPosition terminal = position;
        XqSquare king = xq_jq_position_king_square(&terminal, (XqColor)color);
        CHECK(xq_jq_position_remove_piece(&terminal, king));
        CHECK(xq_jq_engine_parallel_search(NULL, &terminal, &options, &result) == XQ_JQ_PARALLEL_OK);
        CHECK(!result.move_available && result.score_available && !result.score_estimated);
        CHECK(result.score == (color == XQ_RED ? -WIN : WIN) && result.stats.nodes == 0);
    }
    xq_jq_parallel_transposition_table_destroy(options.transposition_table);
}

static void internal_king_capture_terminals(void)
{
    Fixture fixture;
    fixture_init(&fixture);
    XqJqPosition position;
    CHECK(xq_jq_position_from_fen(&position, "4k4/9/9/9/4R4/9/9/9/9/3K5 r - - 0 1"));
    Node ancestor;
    node_init(&ancestor, &position, NULL, -1);
    fixture.parent.parent = &ancestor;
    fixture.parent.in_check = true;
    SearchThreadStats stats = {0};
    ParallelPv pv = {0};
    Interval value = search_node(&fixture.search, &position, &fixture.parent,
                                 0, 0, false, XQ_JQ_NO_REVEAL, NULL, NULL, 0, &pv, &stats);
    CHECK(!value.cancelled && !value.estimated && value.lower == WIN && value.upper == WIN);
    CHECK(stats.nodes == 1 && stats.cache.probes == 0 && pv.count == 0);
    CHECK(!atomic_load(&fixture.parent.repetition_seen));

    /* Facing generals allow a capture even while the side to move is also in check. */
    CHECK(xq_jq_position_from_fen(&position, "4k4/9/9/9/9/9/9/9/9/4K4 r - - 0 1"));
    CHECK(xq_jq_position_in_check(&position, XQ_RED) && xq_jq_position_in_check(&position, XQ_BLACK));
    XqMove capture = board_move(&position, 4, 0, 4, 9);
    XqJqTranspositionStats ignored = {0};
    parallel_tt_store(fixture.search.options.transposition_table, xq_jq_position_hash(&position),
                      1, 12345, XQ_JQ_SEARCH_SCORE_EXACT, capture, false, &ignored);
    fixture.parent.parent = NULL;
    fixture.search.root_color = XQ_BLACK;
    stats = (SearchThreadStats){0};
    value = search_node(&fixture.search, &position, &fixture.parent,
                        0, 1, false, XQ_JQ_NO_REVEAL, NULL, NULL, 0, &pv, &stats);
    CHECK(!value.cancelled && !value.estimated && value.lower == -WIN && value.upper == -WIN);
    CHECK(stats.nodes == 1 && stats.cache.probes == 0 && pv.count == 0);

    /* The parser accepts reversed generals. Facing alone generates no capture here. */
    CHECK(xq_jq_position_from_fen(&position, "4K4/9/9/9/9/9/9/9/9/4k4 r - - 0 1"));
    CHECK(xq_jq_position_in_check(&position, XQ_RED) && xq_jq_position_in_check(&position, XQ_BLACK));
    XqJqMoveList moves;
    xq_jq_generate_pseudo_legal(&position, &moves);
    CHECK(moves.count == 0);
    stats = (SearchThreadStats){0};
    value = search_node(&fixture.search, &position, &fixture.parent,
                        0, 1, false, XQ_JQ_NO_REVEAL, NULL, NULL, 0, &pv, &stats);
    CHECK(!value.cancelled && !value.estimated && value.lower == WIN && value.upper == WIN);
    CHECK(stats.nodes == 1 && pv.count == 0);
    fixture_destroy(&fixture);
}

static void move_collection_after_cutoffs(void)
{
    Fixture fixture;
    fixture_init(&fixture);
    fixture.search.evaluate = xq_jq_engine_default_static_evaluate;
    XqJqPosition position;
    CHECK(xq_jq_position_from_fen(&position, "4k4/9/9/9/9/9/9/9/9/3K5 r - - 0 1"));
    int score = fixture.search.evaluate(&position, XQ_RED, NULL);
    SearchThreadStats stats = {0};
    ParallelPv pv = {0};
    pseudo_legal_generations = 0;
    full_collections = 0;
    capture_collections = 0;
    Interval value = search_node(&fixture.search, &position, &fixture.parent,
                                 0, 0, false, XQ_JQ_NO_REVEAL, NULL, NULL, 0, &pv, &stats);
    CHECK(!value.cancelled && !value.estimated && value.lower == score && value.upper == score);
    CHECK(stats.nodes == 2 && pv.count == 0 && pseudo_legal_generations == 1);
    CHECK(capture_collections == 1 && full_collections == 0);

    /* Stand pat can resolve a quiescence leaf without assembling the capture list. */
    XqJqMoveCacheSummary candidates = xq_jq_move_cache_refresh(&stats.move_cache, &position);
    node_init(&fixture.node, &position, NULL, -1);
    capture_collections = 0;
    QuiescenceResult quiet = parallel_quiescence_start(&fixture.search, &fixture.node, &position,
                                                       candidates.count, -INFINITY_SCORE, score,
                                                       &stats);
    CHECK(!quiet.stopped && quiet.score == score);
    CHECK(capture_collections == 0 && full_collections == 0);

    /* An exact table hit resolves the ordinary node before collecting its moves. */
    XqMove quiet_move = board_move(&position, 3, 0, 3, 1);
    parallel_tt_store(fixture.search.options.transposition_table, xq_jq_position_hash(&position),
                      1, score, XQ_JQ_SEARCH_SCORE_EXACT, quiet_move, false, &stats.cache);
    value = search_node(&fixture.search, &position, &fixture.parent,
                        0, 1, false, XQ_JQ_NO_REVEAL, NULL, NULL, 0, &pv, &stats);
    CHECK(!value.cancelled && value.lower == score && value.upper == score);
    CHECK(stats.cache.cutoffs == 1 && capture_collections == 0 && full_collections == 0);
    fixture_destroy(&fixture);
}

static void quiescence_reveal_failure(void)
{
    Fixture fixture;
    fixture_init(&fixture);
    fixture.search.evaluate = xq_jq_engine_default_static_evaluate;
    atomic_init(&fixture.search.error_status, XQ_JQ_PARALLEL_OK);

    XqJqPosition position;
    CHECK(xq_jq_position_from_fen(&position, "4k4/9/9/9/9/p8/9/R*8/9/3K5 r - - 0 1"));
    /* An empty reveal pool forces the dark rook's capture to fail during sampling. */
    CHECK(position.pool_total[XQ_RED] > 0);
    position.pool_total[XQ_RED] = 0;

    SearchThreadStats stats = {0};
    ParallelPv pv = {0};
    Interval value = search_node(&fixture.search, &position, &fixture.parent,
                                 0, 0, false, XQ_JQ_NO_REVEAL, NULL, NULL, 0, &pv, &stats);
    CHECK(value.cancelled && stats.nodes == 2 && pv.count == 0);
    CHECK(atomic_load(&fixture.search.error_status) == XQ_JQ_PARALLEL_INVALID_ARGUMENT);
    fixture_destroy(&fixture);
}

static void child_move_failure(void)
{
    Fixture fixture;
    fixture_init(&fixture);
    atomic_init(&fixture.search.error_status, XQ_JQ_PARALLEL_OK);

    /* The dark move requires a sampled type before it can be applied. */
    CHECK(xq_jq_position_is_dark(&fixture.parent.position, fixture.parent.moves.moves[1].from));
    ParallelPv pv = {.count = 1};
    SearchThreadStats stats = {0};
    Interval value = search_node(&fixture.search, NULL, &fixture.parent, 1, 0, true,
                                 XQ_JQ_NO_REVEAL, NULL, NULL, 0, &pv, &stats);
    CHECK(value.cancelled && stats.nodes == 0 && pv.count == 0);
    CHECK(atomic_load(&fixture.search.error_status) == XQ_JQ_PARALLEL_INVALID_ARGUMENT);
    CHECK(atomic_load(&fixture.search.time_state) == TIME_STOP);
    fixture_destroy(&fixture);
}

static void check_dark_pv(const XqJqPosition *position, XqMove move, bool legal)
{
    unsigned char original[sizeof(*position)];
    memcpy(original, position, sizeof(original));
    ParallelTtEntry first = {
        .key = xq_jq_position_hash(position), .depth = 5,
        .score_kind = XQ_JQ_SEARCH_SCORE_EXACT, .best_move = move,
    };
    unsigned char original_entry[sizeof(first)];
    memcpy(original_entry, &first, sizeof(original_entry));
    ParallelPv pv = {.count = 3};
    dark_move_applications = 0;
    identity_samples = 0;
    pseudo_legal_generations = 0;
    parallel_pv_from_tt(NULL, position, 5, &first, &pv);
    CHECK(pv.count == (legal ? 1 : 0));
    if (legal)
        CHECK(memcmp(&pv.moves[0], &move, sizeof(move)) == 0);
    CHECK(pseudo_legal_generations == 1);
    CHECK(dark_move_applications == 0 && identity_samples == 0);
    CHECK(memcmp(original, position, sizeof(original)) == 0);
    CHECK(memcmp(original_entry, &first, sizeof(first)) == 0);
}

static void dark_pv_validation(void)
{
    const struct
    {
        const char *fen;
        int from_file, from_rank, to_file, to_rank;
        bool in_check, legal, capture;
    } cases[] = {
        {"4k4/9/9/9/9/4P4/9/R*8/9/4K4 r - - 0 1", 0, 2, 0, 3, false, true, false},
        /* Moving the blocker exposes the king; an unrelated move does not answer a check. */
        {"3k5/4r4/9/9/9/9/9/4R*4/9/4K4 r - - 0 1", 4, 2, 5, 2, false, false, false},
        {"3k5/4r4/9/9/9/9/9/R*8/9/4K4 r - - 0 1", 0, 2, 0, 3, true, false, false},
        /* A dark move can answer check by capturing the attacker or interposing. */
        {"3k5/9/9/9/9/9/9/9/R*3r4/4K4 r - - 0 1", 0, 1, 4, 1, true, true, true},
        {"3k5/4r4/9/9/9/9/9/9/R*8/4K4 r - - 0 1", 0, 1, 4, 1, true, true, false},
        /* After capturing the rook, the destination remains occupied as the cannon's screen. */
        {"3k5/4c4/9/9/9/9/9/9/R*3r4/4K4 r - - 0 1", 0, 1, 4, 1, true, false, true},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
    {
        XqJqPosition position;
        CHECK(xq_jq_position_from_fen(&position, cases[i].fen));
        CHECK(xq_jq_position_in_check(&position, position.side_to_move) == cases[i].in_check);
        XqMove move = board_move(&position, cases[i].from_file, cases[i].from_rank,
                                 cases[i].to_file, cases[i].to_rank);
        CHECK(xq_jq_position_is_dark(&position, (XqSquare)move.from));
        CHECK((move.captured != XQ_EMPTY_PIECE) == cases[i].capture);
        check_dark_pv(&position, move, cases[i].legal);
    }

    XqJqPosition black;
    CHECK(xq_jq_position_from_fen(&black,
        "4k4/9/r*8/9/9/4P4/9/9/9/4K4 b - - 0 1"));
    XqMove move = board_move(&black, 0, 7, 0, 6);
    black.fullmove_number = UINT64_MAX - 1;
    check_dark_pv(&black, move, true);
    black.fullmove_number = UINT64_MAX;
    check_dark_pv(&black, move, false);
}

static void bright_prefix_stops_at_dark_move(void)
{
    XqJqPosition position;
    CHECK(xq_jq_position_from_fen(&position,
        "4k4/9/9/9/9/4P4/9/R*8/9/4K4 b - - 0 1"));
    unsigned char original[sizeof(position)];
    memcpy(original, &position, sizeof(original));
    XqMove bright = board_move(&position, 4, 9, 3, 9);
    XqJqPosition after_bright = position;
    CHECK(xq_jq_position_make_move(&after_bright, bright, XQ_JQ_NO_REVEAL));
    XqMove dark = board_move(&after_bright, 0, 2, 0, 3);
    XqJqPosition after_reveal = after_bright;
    CHECK(xq_jq_position_make_move(&after_reveal, dark, XQ_ROOK));
    XqMove suffix = board_move(&after_reveal, 3, 9, 3, 8);
    XqJqParallelTranspositionTable *table = xq_jq_parallel_transposition_table_create();
    CHECK(table != NULL);
    XqJqTranspositionStats ignored = {0};
    parallel_tt_store(table, xq_jq_position_hash(&after_bright), 4, 0,
                      XQ_JQ_SEARCH_SCORE_EXACT, dark, false, &ignored);
    parallel_tt_store(table, xq_jq_position_hash(&after_reveal), 4, 0,
                      XQ_JQ_SEARCH_SCORE_EXACT, suffix, false, &ignored);
    ParallelTtEntry first = {
        .key = xq_jq_position_hash(&position), .depth = 5,
        .score_kind = XQ_JQ_SEARCH_SCORE_EXACT, .best_move = bright,
    };
    ParallelPv pv = {0};
    dark_move_applications = 0;
    identity_samples = 0;
    pseudo_legal_generations = 0;
    parallel_pv_from_tt(table, &position, 5, &first, &pv);
    CHECK(pv.count == 2);
    CHECK(memcmp(&pv.moves[0], &bright, sizeof(bright)) == 0);
    CHECK(memcmp(&pv.moves[1], &dark, sizeof(dark)) == 0);
    CHECK(pseudo_legal_generations == 2);
    CHECK(dark_move_applications == 0 && identity_samples == 0);
    CHECK(memcmp(original, &position, sizeof(original)) == 0);
    xq_jq_parallel_transposition_table_destroy(table);
}

static void mark_split_range(SearchRange range, int count, unsigned *visits)
{
    CHECK(range.begin >= 0 && range.begin < range.end && range.end <= count);
    CHECK(range.step > 0);
    for (int i = range.begin; i < range.end; i += range.step)
        CHECK(++visits[i] == 1);
}

static void check_split_assignment(XqJqParallelSplitStrategy strategy,
                                    unsigned depth, unsigned consumed_depth, int move_count,
                                    unsigned idle_workers, unsigned split_percent,
                                    SearchRange expected_owner, unsigned expected_helpers)
{
    Search search = {0};
    Node node = {0};
    Worker workers[3] = {0};
    search.options = xq_jq_parallel_options_default();
    search.options.split_strategy = strategy;
    search.options.split_percent = split_percent;
    search.root_iteration_depth = depth + consumed_depth;
    search.workers = workers;
    search.worker_count = 3;
    node.depth = depth;
    node.moves.count = move_count;
    for (unsigned i = 0; i < search.worker_count; ++i)
    {
        workers[i].search = &search;
        atomic_init(&workers[i].state,
                    (idle_workers & (1u << i)) ? WORKER_IDLE : WORKER_RESERVED);
        CHECK(search_semaphore_init(&workers[i].task_available));
    }

    TaskCompletion *pending = NULL;
    SearchRange owner = split_node(&search, &node, &pending);
    CHECK(owner.begin == expected_owner.begin && owner.end == expected_owner.end);
    CHECK(owner.step == expected_owner.step);
    unsigned visits[XQ_JQ_MAX_MOVES] = {0};
    mark_split_range(owner, move_count, visits);
    unsigned helpers = 0;
    unsigned assigned_workers = 0;
    for (TaskCompletion *completion = pending; completion != NULL; completion = completion->next)
    {
        Worker *worker = completion->worker;
        unsigned index = (unsigned)(worker - workers);
        CHECK(index < search.worker_count && (idle_workers & (1u << index)));
        CHECK(!(assigned_workers & (1u << index)));
        assigned_workers |= 1u << index;
        ++helpers;
        CHECK(atomic_load(&worker->state) == WORKER_READY);
        CHECK(worker->node == &node && worker->completion == completion);
        CHECK(worker->range.begin == (strategy == XQ_JQ_PARALLEL_SPLIT_INTERLEAVED
                                      ? (int)helpers : expected_owner.end));
        CHECK(worker->range.end == move_count && worker->range.step == expected_owner.step);
        mark_split_range(worker->range, move_count, visits);
    }
    CHECK(helpers == expected_helpers);
    for (int i = 0; i < move_count; ++i)
        CHECK(visits[i] == 1);
    for (unsigned i = 0; i < search.worker_count; ++i)
        if (!(assigned_workers & (1u << i)))
            CHECK(atomic_load(&workers[i].state) ==
                  ((idle_workers & (1u << i)) ? WORKER_IDLE : WORKER_RESERVED));

    /* No physical workers were started: consume their dispatches and complete each task here. */
    while (pending != NULL)
    {
        TaskCompletion *next = pending->next;
        Worker *worker = pending->worker;
        search_semaphore_wait(&worker->task_available);
        worker->node = NULL;
        worker->completion = NULL;
        atomic_store(&pending->finished, 1);
        atomic_store(&worker->state, WORKER_IDLE);
        search_semaphore_post(&pending->notification);
        task_completion_release(pending); /* Worker reference. */
        search_semaphore_wait(&pending->notification);
        CHECK(atomic_load(&pending->finished));
        task_completion_release(pending); /* Owner reference. */
        pending = next;
    }
    for (unsigned i = 0; i < search.worker_count; ++i)
        search_semaphore_destroy(&workers[i].task_available);
}

static void split_assignments(void)
{
    /* Three participants, including the owner, partition all ordered moves at root or below it. */
    check_split_assignment(XQ_JQ_PARALLEL_SPLIT_INTERLEAVED, 5, 0, 8, 7, 50,
                           (SearchRange){0, 8, 3}, 2);
    check_split_assignment(XQ_JQ_PARALLEL_SPLIT_INTERLEAVED, 5, 2, 8, 7, 0,
                           (SearchRange){0, 8, 3}, 2);
    /* Busy helpers and short move lists reduce the actual participant count. */
    check_split_assignment(XQ_JQ_PARALLEL_SPLIT_INTERLEAVED, 5, 0, 8, 2, 50,
                           (SearchRange){0, 8, 2}, 1);
    check_split_assignment(XQ_JQ_PARALLEL_SPLIT_INTERLEAVED, 5, 0, 2, 7, 50,
                           (SearchRange){0, 2, 2}, 1);
    check_split_assignment(XQ_JQ_PARALLEL_SPLIT_INTERLEAVED, 5, 0, 1, 7, 50,
                           (SearchRange){0, 1, 1}, 0);
    check_split_assignment(XQ_JQ_PARALLEL_SPLIT_INTERLEAVED, 5, 0, 8, 0, 50,
                           (SearchRange){0, 8, 1}, 0);
    check_split_assignment(XQ_JQ_PARALLEL_SPLIT_INTERLEAVED, 4, 2, 8, 7, 50,
                           (SearchRange){0, 8, 1}, 0);
    /* Contiguous splitting retains its one-helper tail batch and depth/percentage gates. */
    check_split_assignment(XQ_JQ_PARALLEL_SPLIT_CONTIGUOUS, 5, 0, 8, 7, 50,
                           (SearchRange){0, 4, 1}, 1);
    check_split_assignment(XQ_JQ_PARALLEL_SPLIT_CONTIGUOUS, 4, 2, 8, 7, 50,
                           (SearchRange){0, 8, 1}, 0);
    check_split_assignment(XQ_JQ_PARALLEL_SPLIT_CONTIGUOUS, 5, 0, 8, 7, 0,
                           (SearchRange){0, 8, 1}, 0);
}

static void searches_with_splits(void)
{
    XqJqPosition position;
    CHECK(xq_jq_position_from_fen(&position,
        "4k4/9/9/9/4p4/4P4/9/9/9/4K4 r - - 0 1"));
    unsigned char original[sizeof(position)];
    memcpy(original, &position, sizeof(original));
    int serial_score = 0;
    for (unsigned mode = 0; mode < 3; ++mode)
    {
        XqJqParallelOptions options = xq_jq_parallel_options_default();
        options.depth = 7;
        options.thread_count = mode == 0 ? 1 : 4;
        options.time_limit_ms = 0;
        options.split_strategy = mode == 2 ? XQ_JQ_PARALLEL_SPLIT_CONTIGUOUS :
                                             XQ_JQ_PARALLEL_SPLIT_INTERLEAVED;
        XqJqParallelResult result;
        CHECK(xq_jq_engine_parallel_search(NULL, &position, &options, &result) == XQ_JQ_PARALLEL_OK);
        CHECK(result.move_available && result.score_available && !result.score_estimated);
        CHECK(!result.stats.stopped && !result.stats.winning_move_found);
        CHECK(result.stats.completed_depth == options.depth);
        CHECK(result.stats.selected_move_depth == options.depth);
        CHECK(memcmp(original, &position, sizeof(original)) == 0);
        if (mode == 0)
            serial_score = result.score;
        else
            CHECK(result.score == serial_score);
    }
}

int main(void)
{
    incremental_hashes();
    sibling_iterator();
    cached_hint_move_lookup();
    repeated_keys_consume_budget();
    pseudo_legal_keys();
    cancellation_after_collection();
    root_king_capture_terminals();
    internal_king_capture_terminals();
    move_collection_after_cutoffs();
    quiescence_reveal_failure();
    child_move_failure();
    dark_pv_validation();
    bright_prefix_stops_at_dark_move();
    split_assignments();
    searches_with_splits();
    puts("Jieqi parallel similarity checks passed");
    return 0;
}
