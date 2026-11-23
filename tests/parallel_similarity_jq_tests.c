#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "xiangqi/jieqi/movegen_jq.h"
#include "movegen_internal_jq.h"

static _Thread_local unsigned pseudo_legal_generations;

static void count_pseudo_legal_generation(const XqJqPosition *position, XqJqMoveList *moves)
{
    ++pseudo_legal_generations;
    xq_jq_generate_pseudo_legal(position, moves);
}

static void count_cached_generation(XqJqMoveCache *cache, const XqJqPosition *position,
                                    XqJqMoveList *moves)
{
    ++pseudo_legal_generations;
    xq_jq_move_cache_generate(cache, position, moves);
}

/* CMake copies the implementation with small, independent test-only enum values. */
#define xq_jq_generate_pseudo_legal count_pseudo_legal_generation
#define xq_jq_move_cache_generate count_cached_generation
#include "parallel_search_under_test.c"
#undef xq_jq_move_cache_generate
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

static void query_budget_and_deduplication(void)
{
    Fixture fixture;
    fixture_init(&fixture);
    XqJqMoveList *siblings = &fixture.parent.moves;
    siblings->moves[6] = siblings->moves[4];
    siblings->moves[5] = siblings->moves[3];
    siblings->moves[3] = siblings->moves[2];
    siblings->moves[4] = siblings->moves[2];
    siblings->moves[2] = siblings->moves[0];
    siblings->count = 7;
    store_hint(&fixture, 3, 0, true);
    store_hint(&fixture, 5, 1, false);
    store_hint(&fixture, 6, 2, false);

    unsigned char original[sizeof(fixture.node.moves)];
    memcpy(original, &fixture.node.moves, sizeof(original));
    SimilarMoveHints hints = {.quiet_count = 99, .winning_count = 99};
    CHECK(parallel_collect_similar_hints(&fixture.search, &fixture.node, &hints) ==
          PARALLEL_MAX_SIMILAR_POSITION_QUERIES);
    if (PARALLEL_MAX_SIMILAR_POSITION_QUERIES == 0)
        CHECK(hints.quiet_count == 0 && hints.winning_count == 0);
    else
    {
        CHECK(hints.quiet_count == 1 && hints.quiet_moves[0].from == fixture.node.moves.moves[0].from);
        CHECK(hints.winning_count == 1 && hints.winning_moves[0].from == fixture.node.moves.moves[1].from);
    }
    CHECK(memcmp(original, &fixture.node.moves, sizeof(original)) == 0);

    xq_jq_parallel_transposition_table_clear(fixture.search.options.transposition_table);
    store_hint(&fixture, 6, 2, false);
    CHECK(parallel_collect_similar_hints(&fixture.search, &fixture.node, &hints) ==
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
    CHECK(parallel_collect_similar_hints(&fixture.search, &fixture.node, &hints) ==
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
        CHECK(parallel_collect_similar_hints(&fixture.search, &fixture.node, &hints) ==
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
                                       0, 1, false, NULL, NULL, 0, &pv, &stats);
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
                                 0, 0, false, NULL, NULL, 0, &pv, &stats);
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
                        0, 1, false, NULL, NULL, 0, &pv, &stats);
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
                        0, 1, false, NULL, NULL, 0, &pv, &stats);
    CHECK(!value.cancelled && !value.estimated && value.lower == WIN && value.upper == WIN);
    CHECK(stats.nodes == 1 && pv.count == 0);
    fixture_destroy(&fixture);
}

static void quiescence_reuses_leaf_moves(void)
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
    Interval value = search_node(&fixture.search, &position, &fixture.parent,
                                 0, 0, false, NULL, NULL, 0, &pv, &stats);
    CHECK(!value.cancelled && !value.estimated && value.lower == score && value.upper == score);
    CHECK(stats.nodes == 2 && pv.count == 0 && pseudo_legal_generations == 1);
    fixture_destroy(&fixture);
}

int main(void)
{
    incremental_hashes();
    sibling_iterator();
    query_budget_and_deduplication();
    pseudo_legal_keys();
    cancellation_after_collection();
    root_king_capture_terminals();
    internal_king_capture_terminals();
    quiescence_reuses_leaf_moves();
    puts("Jieqi parallel similarity checks passed");
    return 0;
}
