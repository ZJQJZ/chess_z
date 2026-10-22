#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

/* CMake copies the implementation with small, independent test-only enum values. */
#include "parallel_search_under_test.c"

_Static_assert(PARALLEL_MAX_SIMILAR_POSITION_QUERIES == XQ_TEST_SIMILARITY_QUERY_LIMIT,
               "the test driver and generated implementation must use the same query limit");

#include <stdio.h>

/* Unlike assert(), these checks remain enabled in Release builds. */
#define CHECK(value) do { if (!(value)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #value); abort(); \
} } while (0)

typedef struct Fixture
{
    Search search;
    Node parent;
    Node node;
    XqMove quiet[5];
    XqMove captures[2];
} Fixture;

static bool equal_move(XqMove a, XqMove b)
{
    return a.from == b.from && a.to == b.to && a.piece == b.piece &&
           a.captured == b.captured;
}

static XqMove find_move(const XqPosition *position, int file, int rank,
                        int to_file, int to_rank)
{
    XqMoveList moves;
    xq_generate_pseudo_legal(position, &moves);
    for (int i = 0; i < moves.count; ++i)
        if (moves.moves[i].from == xq_square_make(file, rank) &&
            moves.moves[i].to == xq_square_make(to_file, to_rank))
            return moves.moves[i];
    CHECK(false);
    return (XqMove){0};
}

static void node_init(Node *node, const XqPosition *position, Node *parent, int index)
{
    memset(node, 0, sizeof(*node));
    node->position = *position;
    node->parent = parent;
    node->parent_move_index = index;
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
}

static XqPosition sibling_position(const Fixture *fixture, int index)
{
    XqPosition sibling = fixture->parent.position;
    CHECK(index >= 0 && index < fixture->parent.moves.count);
    CHECK(xq_position_make_move(&sibling, fixture->parent.moves.moves[index]));
    return sibling;
}

static uint64_t sibling_key(const Fixture *fixture, int index)
{
    XqPosition sibling = sibling_position(fixture, index);
    return xq_position_hash(&sibling);
}

static void fixture_init(Fixture *fixture)
{
    memset(fixture, 0, sizeof(*fixture));
    fixture->search.options = xq_parallel_options_default();
    fixture->search.options.thread_count = 1;
    fixture->search.options.transposition_table = xq_parallel_transposition_table_create();
    CHECK(fixture->search.options.transposition_table != NULL);
    fixture->search.root_color = XQ_RED;
    fixture->search.evaluate = xq_engine_default_static_evaluate;
    atomic_init(&fixture->search.time_state, TIME_ACTIVE);
    atomic_init(&fixture->search.root_started, 0);
    atomic_init(&fixture->search.root_completed, 0);
    atomic_init(&fixture->search.shutdown, 0);
    XqPosition start;
    xq_position_startpos(&start);
    node_init(&fixture->parent, &start, NULL, -1);
    fixture->parent.moves.count = 5;
    for (int i = 0; i < 5; ++i)
        fixture->parent.moves.moves[i] = find_move(&start, i * 2, 3, i * 2, 4);
    XqPosition current = sibling_position(fixture, 0);
    node_init(&fixture->node, &current, &fixture->parent, 0);
    for (int i = 0; i < 5; ++i)
        fixture->quiet[i] = find_move(&current, i * 2, 6, i * 2, 5);
    fixture->captures[0] = find_move(&current, 1, 7, 1, 0);
    fixture->captures[1] = find_move(&current, 7, 7, 7, 0);
    fixture->node.moves.count = 7;
    fixture->node.moves.moves[0] = fixture->captures[0];
    fixture->node.moves.moves[1] = fixture->captures[1];
    for (int i = 0; i < 5; ++i)
        fixture->node.moves.moves[i + 2] = fixture->quiet[i];
    parallel_order_moves(&fixture->node.position, &fixture->node.moves);
}

static void fixture_destroy(Fixture *fixture)
{
    xq_parallel_transposition_table_destroy(fixture->search.options.transposition_table);
}

static ParallelTtEntry *entry_for(XqParallelTranspositionTable *table, uint64_t key)
{
    ParallelTtBucket *bucket = &table->buckets[key & (PARALLEL_TT_BUCKET_COUNT - 1u)];
    for (int i = 0; i < PARALLEL_TT_BUCKET_SIZE; ++i)
        if (bucket->entries[i].depth != 0 && bucket->entries[i].key == key)
            return &bucket->entries[i];
    CHECK(false);
    return NULL;
}

static void store_scored_hint(Fixture *fixture, int index, int score,
                              XqSearchScoreKind kind, XqMove move)
{
    XqTranspositionStats ignored = {0};
    parallel_tt_store(fixture->search.options.transposition_table, sibling_key(fixture, index),
                      1, score, kind, move, &ignored);
}

static void store_hint(Fixture *fixture, int index, XqSearchScoreKind kind, XqMove move)
{
    store_scored_hint(fixture, index, 12345, kind, move);
}

static void check_moves(const XqMoveList *actual, const XqMoveList *expected)
{
    CHECK(actual->count == expected->count);
    for (int i = 0; i < actual->count; ++i)
        CHECK(equal_move(actual->moves[i], expected->moves[i]));
}

/* Retain end-to-end hint tests while checking that collection never changes move order. */
static unsigned collect_and_apply(Search *search, Node *node, unsigned *winning_count)
{
    XqMoveList original = node->moves;
    SimilarMoveHints hints = {.quiet_count = 123, .winning_count = 123};
    unsigned queries = parallel_collect_similar_hints(search, node, &hints);
    check_moves(&node->moves, &original);
    CHECK(hints.quiet_count <= queries && hints.winning_count <= queries);
    if (winning_count != NULL)
        *winning_count = hints.winning_count;
    parallel_apply_move_hints(node, &hints, NULL, NULL, 0);
    return queries;
}

static void iterator_order(void)
{
    Fixture fixture;
    fixture_init(&fixture);
    atomic_store(&fixture.parent.best_index, 3);
    SimilarPositionIterator iterator = parallel_similar_positions(&fixture.node);
    /* A concurrent new best branch must not change an existing enumeration. */
    atomic_store(&fixture.parent.best_index, 4);
    const int expected[] = {3, 1, 2, 4};
    for (unsigned i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i)
    {
        XqPosition candidate;
        CHECK(parallel_similar_position_next(&iterator, &candidate));
        CHECK(xq_position_hash(&candidate) == sibling_key(&fixture, expected[i]));
    }
    XqPosition candidate;
    CHECK(!parallel_similar_position_next(&iterator, &candidate));
    atomic_store(&fixture.parent.best_index, 0);
    iterator = parallel_similar_positions(&fixture.node);
    for (int i = 1; i < 5; ++i)
    {
        CHECK(parallel_similar_position_next(&iterator, &candidate));
        CHECK(xq_position_hash(&candidate) == sibling_key(&fixture, i));
    }
    CHECK(!parallel_similar_position_next(&iterator, &candidate));
    iterator = parallel_similar_positions(&fixture.parent);
    CHECK(!parallel_similar_position_next(&iterator, &candidate));
    /* Failed construction is skipped without yielding a position. */
    fixture.parent.moves.moves[1].from = XQ_SQUARES;
    atomic_store(&fixture.parent.best_index, 1);
    iterator = parallel_similar_positions(&fixture.node);
    CHECK(parallel_similar_position_next(&iterator, &candidate));
    CHECK(xq_position_hash(&candidate) == sibling_key(&fixture, 2));
    fixture_destroy(&fixture);
}

static void readonly_peek(void)
{
    Fixture fixture;
    fixture_init(&fixture);
    XqParallelTranspositionTable *table = fixture.search.options.transposition_table;
    table->generation = 3;
    store_hint(&fixture, 1, XQ_SEARCH_SCORE_EXACT, fixture.quiet[3]);
    table->generation = 11;
    table->stats = (XqTranspositionStats){7, 5, 3, 2, 1};
    XqTranspositionStats before = table->stats;
    ParallelTtEntry entry;
    uint64_t key = sibling_key(&fixture, 1);
    CHECK(parallel_tt_peek(table, key, &entry));
    CHECK(entry.generation == 3 && entry_for(table, key)->generation == 3);
    CHECK(entry.key == key && equal_move(entry.best_move, fixture.quiet[3]));
    CHECK(memcmp(&before, &table->stats, sizeof(before)) == 0);
    ParallelTtEntry unchanged = entry;
    CHECK(!parallel_tt_peek(table, key ^ UINT64_C(0x100000000), &entry));
    CHECK(memcmp(&entry, &unchanged, sizeof(entry)) == 0);
    ParallelTtBucket *bucket = &table->buckets[key & (PARALLEL_TT_BUCKET_COUNT - 1u)];
    atomic_store(&bucket->occupied, 1);
    CHECK(!parallel_tt_peek(table, key, &entry));
    CHECK(atomic_load(&bucket->occupied) == 1);
    atomic_store(&bucket->occupied, 0);
    CHECK(!parallel_tt_peek(NULL, key, &entry));
    CHECK(parallel_tt_snapshot(table, key, &entry));
    CHECK(entry.generation == 11 && entry_for(table, key)->generation == 11);
    CHECK(memcmp(&before, &table->stats, sizeof(before)) == 0);
    fixture_destroy(&fixture);
}

static void disabled_or_inapplicable(void)
{
    Fixture fixture;
    fixture_init(&fixture);
    store_hint(&fixture, 1, XQ_SEARCH_SCORE_EXACT, fixture.quiet[4]);
    XqMoveList original = fixture.node.moves;
    unsigned winning = 123;
#if XQ_TEST_SIMILARITY_QUERY_LIMIT == 0
    uint64_t first_key = sibling_key(&fixture, 1);
    ParallelTtBucket *busy = &fixture.search.options.transposition_table->buckets[
        first_key & (PARALLEL_TT_BUCKET_COUNT - 1u)];
    atomic_store(&busy->occupied, 1);
    CHECK(collect_and_apply(&fixture.search, &fixture.node, &winning) == 0);
    CHECK(winning == 0);
    check_moves(&fixture.node.moves, &original);
    CHECK(atomic_load(&busy->occupied) == 1);
    atomic_store(&busy->occupied, 0);
#endif
    fixture.node.depth = 0;
    winning = 123;
    CHECK(collect_and_apply(&fixture.search, &fixture.node, &winning) == 0);
    CHECK(winning == 0);
    fixture.node.depth = 4;
    XqParallelTranspositionTable *table = fixture.search.options.transposition_table;
    fixture.search.options.transposition_table = NULL;
    winning = 123;
    CHECK(collect_and_apply(&fixture.search, &fixture.node, &winning) == 0);
    CHECK(winning == 0);
    fixture.search.options.transposition_table = table;
    winning = 123;
    CHECK(collect_and_apply(&fixture.search, &fixture.parent, &winning) == 0);
    CHECK(winning == 0);
    atomic_store(&fixture.parent.state, NODE_CANCELLED);
    winning = 123;
    CHECK(collect_and_apply(&fixture.search, &fixture.node, &winning) == 0);
    CHECK(winning == 0);
    check_moves(&fixture.node.moves, &original);
    fixture_destroy(&fixture);
}

#if XQ_TEST_SIMILARITY_QUERY_LIMIT > 0
static void collection_preserves_moves(void)
{
    Fixture fixture;
    fixture_init(&fixture);
    XqMoveList original = fixture.node.moves;
    XqMove stale_quiet = fixture.quiet[3];
    stale_quiet.captured = xq_make_piece(XQ_RED, XQ_ROOK);
    XqMove stale_winner = fixture.captures[1];
    stale_winner.captured = XQ_EMPTY_PIECE;
    store_hint(&fixture, 1, XQ_SEARCH_SCORE_EXACT, stale_quiet);
    store_scored_hint(&fixture, 2, WIN, XQ_SEARCH_SCORE_LOWER_BOUND, stale_winner);
    SimilarMoveHints hints = {.quiet_count = 123, .winning_count = 123};
    CHECK(parallel_collect_similar_hints(&fixture.search, &fixture.node, &hints) == 2);
    check_moves(&fixture.node.moves, &original);
    CHECK(hints.quiet_count == 1 && hints.winning_count == 1);
    CHECK(equal_move(hints.quiet_moves[0], fixture.quiet[3]));
    CHECK(equal_move(hints.winning_moves[0], fixture.captures[1]));
    CHECK(fixture.node.pv_hint == NULL && fixture.node.pv_hint_count == 0);
    fixture_destroy(&fixture);
}

static void unified_hint_precedence(void)
{
    Fixture fixture;
    fixture_init(&fixture);
    XqMoveList original = fixture.node.moves;
    SimilarMoveHints hints = {
        .quiet_moves = {fixture.quiet[3]},
        .quiet_count = 1,
        .winning_moves = {fixture.quiet[4], fixture.captures[1]},
        .winning_count = 2,
    };
    XqMove hash_move = fixture.quiet[1];
    XqMove pv_hint[] = {fixture.quiet[2], fixture.quiet[0]};
    parallel_apply_move_hints(&fixture.node, &hints, &hash_move, pv_hint, 2);
    XqMoveList expected = {
        .moves = {fixture.quiet[2], fixture.quiet[4], fixture.captures[1],
                  fixture.quiet[1], fixture.captures[0], fixture.quiet[3], fixture.quiet[0]},
        .count = 7,
    };
    check_moves(&fixture.node.moves, &expected);
    CHECK(fixture.node.pv_hint == pv_hint && fixture.node.pv_hint_count == 2);

    /* Absent, empty or invalid PVs clear old suffixes and leave the complete WIN prefix first. */
    for (int mode = 0; mode < 3; ++mode)
    {
        fixture.node.moves = original;
        fixture.node.pv_hint = pv_hint;
        fixture.node.pv_hint_count = 2;
        XqMove invalid = {.from = XQ_SQUARES};
        parallel_apply_move_hints(&fixture.node, &hints, &hash_move,
                                  mode == 0 ? NULL : mode == 1 ? pv_hint : &invalid,
                                  mode == 1 ? 0 : 1);
        CHECK(fixture.node.pv_hint == NULL && fixture.node.pv_hint_count == 0);
        CHECK(equal_move(fixture.node.moves.moves[0], hints.winning_moves[0]));
        CHECK(equal_move(fixture.node.moves.moves[1], hints.winning_moves[1]));
        CHECK(equal_move(fixture.node.moves.moves[2], hash_move));
    }

    /* A WIN/PV overlap is promoted only once in the final order and retains its PV suffix. */
    fixture.node.moves = original;
    pv_hint[0] = hints.winning_moves[1];
    parallel_apply_move_hints(&fixture.node, &hints, &hash_move, pv_hint, 2);
    CHECK(equal_move(fixture.node.moves.moves[0], pv_hint[0]));
    CHECK(equal_move(fixture.node.moves.moves[1], hints.winning_moves[0]));
    CHECK(equal_move(fixture.node.moves.moves[2], hash_move));
    CHECK(fixture.node.pv_hint == pv_hint && fixture.node.pv_hint_count == 2);
    fixture_destroy(&fixture);
}

static void ordering_and_hints(void)
{
    Fixture fixture;
    fixture_init(&fixture);
    XqMoveList original = fixture.node.moves;
    atomic_store(&fixture.parent.best_index, 3);
    XqMove stale = fixture.quiet[3];
    stale.captured = xq_make_piece(XQ_RED, XQ_ROOK);
    store_hint(&fixture, 3, XQ_SEARCH_SCORE_EXACT, stale);
    store_hint(&fixture, 1, XQ_SEARCH_SCORE_LOWER_BOUND, fixture.quiet[1]);
    XqParallelTranspositionTable *table = fixture.search.options.transposition_table;
    table->generation = 12;
    XqTranspositionStats before = table->stats;
    XqTranspositionStats thread_before = fixture.search.stats.cache;
    CHECK(collect_and_apply(&fixture.search, &fixture.node, NULL) == 2);
    XqMoveList expected = original;
    const int quiet_order[] = {3, 1, 0, 2, 4};
    for (int i = 0; i < 5; ++i)
        expected.moves[i + 2] = fixture.quiet[quiet_order[i]];
    check_moves(&fixture.node.moves, &expected);
    CHECK(entry_for(table, sibling_key(&fixture, 3))->generation == 0);
    CHECK(entry_for(table, sibling_key(&fixture, 1))->generation == 0);
    CHECK(memcmp(&before, &table->stats, sizeof(before)) == 0);
    CHECK(memcmp(&thread_before, &fixture.search.stats.cache, sizeof(thread_before)) == 0);
    CHECK(atomic_load(&fixture.node.state) == NODE_RUNNING);
    CHECK(atomic_load(&fixture.node.value.lower) == -WIN);
    CHECK(atomic_load(&fixture.node.value.upper) == WIN);
    CHECK(fixture.node.owner_pv.child_pv.count == 0);
    /* The unified stage applies cache and PV hints together with collected hints. */
    fixture.node.moves = original;
    SimilarMoveHints hints;
    CHECK(parallel_collect_similar_hints(&fixture.search, &fixture.node, &hints) == 2);
    XqMove hash_move = fixture.quiet[4];
    XqMove pv_hint[] = {fixture.quiet[2]};
    parallel_apply_move_hints(&fixture.node, &hints, &hash_move, pv_hint, 1);
    CHECK(fixture.node.pv_hint == pv_hint && fixture.node.pv_hint_count == 1);
    expected.moves[0] = fixture.quiet[2];
    expected.moves[1] = fixture.quiet[4];
    expected.moves[2] = original.moves[0];
    expected.moves[3] = original.moves[1];
    expected.moves[4] = fixture.quiet[3];
    expected.moves[5] = fixture.quiet[1];
    expected.moves[6] = fixture.quiet[0];
    check_moves(&fixture.node.moves, &expected);
    fixture_destroy(&fixture);
}

static void query_budget(void)
{
    /* Rejected outcomes consume a query, so a third useful entry is never reached. */
    for (int scenario = 0; scenario < 6; ++scenario)
    {
        Fixture fixture;
        fixture_init(&fixture);
        XqMoveList original = fixture.node.moves;
        XqMove rejected = fixture.quiet[4];
        XqSearchScoreKind kind = XQ_SEARCH_SCORE_EXACT;
        if (scenario == 2)
            kind = XQ_SEARCH_SCORE_UPPER_BOUND;
        else if (scenario == 3)
            rejected.from = XQ_SQUARES;
        else if (scenario == 4)
            rejected.piece = xq_make_piece(XQ_BLACK, XQ_ROOK);
        else if (scenario == 5)
            rejected = fixture.captures[0];
        if (scenario >= 1)
            store_hint(&fixture, 1, kind, rejected);
        store_hint(&fixture, 3, XQ_SEARCH_SCORE_EXACT, fixture.quiet[4]);
        ParallelTtBucket *busy = NULL;
        if (scenario == 1)
        {
            uint64_t key = sibling_key(&fixture, 1);
            busy = &fixture.search.options.transposition_table->buckets[
                key & (PARALLEL_TT_BUCKET_COUNT - 1u)];
            atomic_store(&busy->occupied, 1);
        }
        CHECK(collect_and_apply(&fixture.search, &fixture.node, NULL) == 2);
        check_moves(&fixture.node.moves, &original);
        if (busy != NULL)
        {
            CHECK(atomic_load(&busy->occupied) == 1);
            atomic_store(&busy->occupied, 0);
        }
        fixture_destroy(&fixture);
    }
}

static void winning_hints(void)
{
    /* Winning scores are side-to-move scores, including for root-side MIN nodes. */
    for (int scenario = 0; scenario < 3; ++scenario)
    {
        Fixture fixture;
        fixture_init(&fixture);
        CHECK(!fixture.node.maximizing);
        XqMoveList expected = fixture.node.moves;
        XqMove winner = scenario == 2 ? fixture.captures[1] : fixture.quiet[4];
        XqMove stale = winner;
        stale.captured = scenario == 2 ? XQ_EMPTY_PIECE : xq_make_piece(XQ_RED, XQ_ROOK);
        store_scored_hint(&fixture, 1, WIN,
            scenario == 1 ? XQ_SEARCH_SCORE_LOWER_BOUND : XQ_SEARCH_SCORE_EXACT, stale);
        /* Every distinct winning hint is retained, in source discovery order. */
        XqMove later_winner = fixture.quiet[3];
        store_scored_hint(&fixture, 2, WIN, XQ_SEARCH_SCORE_EXACT, later_winner);
        unsigned winning = 123;
        CHECK(collect_and_apply(&fixture.search, &fixture.node, &winning) == 2);
        CHECK(winning == 2 && equal_move(fixture.node.moves.moves[0], winner));
        CHECK(equal_move(fixture.node.moves.moves[1], later_winner));
        CHECK(parallel_prioritize_move(&expected, later_winner));
        CHECK(parallel_prioritize_move(&expected, winner));
        check_moves(&fixture.node.moves, &expected);
        CHECK(atomic_load(&fixture.node.state) == NODE_RUNNING);
        CHECK(atomic_load(&fixture.node.value.lower) == -WIN);
        CHECK(atomic_load(&fixture.node.value.upper) == WIN);
        CHECK(atomic_load(&fixture.node.submitted_count) == 0);
        CHECK(fixture.node.owner_pv.child_pv.count == 0);
        fixture_destroy(&fixture);
    }

    /* Negative wins remain ordinary hints; an UPPER_BOUND positive win is not accepted. */
    for (int upper = 0; upper < 2; ++upper)
    {
        Fixture fixture;
        fixture_init(&fixture);
        XqMoveList original = fixture.node.moves;
        store_scored_hint(&fixture, 1, upper ? WIN : -WIN,
            upper ? XQ_SEARCH_SCORE_UPPER_BOUND : XQ_SEARCH_SCORE_EXACT, fixture.quiet[4]);
        unsigned winning = 123;
        CHECK(collect_and_apply(&fixture.search, &fixture.node, &winning) == 2);
        CHECK(winning == 0);
        CHECK(equal_move(fixture.node.moves.moves[0], original.moves[0]));
        if (upper)
            check_moves(&fixture.node.moves, &original);
        else
            CHECK(equal_move(fixture.node.moves.moves[2], fixture.quiet[4]));
        fixture_destroy(&fixture);
    }

    /* A terminal score with no matching current move does not end enumeration. */
    for (int wrong_piece = 0; wrong_piece < 2; ++wrong_piece)
    {
        Fixture fixture;
        fixture_init(&fixture);
        XqMove rejected = fixture.quiet[4];
        if (wrong_piece)
            rejected.piece = xq_make_piece(XQ_BLACK, XQ_ROOK);
        else
            rejected.to = XQ_SQUARES;
        store_scored_hint(&fixture, 1, WIN, XQ_SEARCH_SCORE_EXACT, rejected);
        store_scored_hint(&fixture, 2, WIN, XQ_SEARCH_SCORE_LOWER_BOUND, fixture.quiet[2]);
        unsigned winning = 123;
        CHECK(collect_and_apply(&fixture.search, &fixture.node, &winning) == 2);
        CHECK(winning == 1 && equal_move(fixture.node.moves.moves[0], fixture.quiet[2]));
        fixture_destroy(&fixture);
    }

    /* Preserve earlier ordinary hints and current metadata, even when a winner duplicates one. */
    for (int same_move = 0; same_move < 2; ++same_move)
    {
        Fixture fixture;
        fixture_init(&fixture);
        XqMoveList expected = fixture.node.moves;
        XqMove ordinary = fixture.quiet[3];
        XqMove winner = same_move ? ordinary : fixture.quiet[4];
        XqMove stale = winner;
        stale.captured = xq_make_piece(XQ_RED, XQ_ROOK);
        store_hint(&fixture, 1, XQ_SEARCH_SCORE_EXACT, ordinary);
        store_scored_hint(&fixture, 2, WIN, XQ_SEARCH_SCORE_EXACT, stale);
        /* Apply the same stable, quiet-region promotion expected before the winning promotion. */
        memmove(expected.moves + 3, expected.moves + 2, 3 * sizeof(XqMove));
        expected.moves[2] = ordinary;
        CHECK(parallel_prioritize_move(&expected, winner));
        unsigned winning = 123;
        CHECK(collect_and_apply(&fixture.search, &fixture.node, &winning) == 2);
        CHECK(winning == 1);
        check_moves(&fixture.node.moves, &expected);
        fixture_destroy(&fixture);
    }

    /* A win never stops enumeration: later hints and unusable outcomes still use the budget. */
    for (int scenario = 0; scenario < 6; ++scenario)
    {
        Fixture fixture;
        fixture_init(&fixture);
        XqMoveList expected = fixture.node.moves;
        XqMove winner = fixture.quiet[4];
        store_scored_hint(&fixture, 1, WIN, XQ_SEARCH_SCORE_EXACT, winner);
        /* A third winner must remain unseen once the two-query budget is exhausted. */
        store_scored_hint(&fixture, 3, WIN, XQ_SEARCH_SCORE_EXACT, fixture.quiet[2]);
        if (scenario == 0 || scenario == 2)
            store_hint(&fixture, 2, XQ_SEARCH_SCORE_LOWER_BOUND, fixture.quiet[3]);
        else if (scenario == 3)
            store_scored_hint(&fixture, 2, WIN, XQ_SEARCH_SCORE_UPPER_BOUND, fixture.quiet[3]);
        else if (scenario == 4)
            store_scored_hint(&fixture, 2, WIN, XQ_SEARCH_SCORE_LOWER_BOUND, winner);
        else if (scenario == 5)
            fixture.parent.moves.count = 2;
        ParallelTtBucket *busy = NULL;
        if (scenario == 2)
        {
            uint64_t key = sibling_key(&fixture, 2);
            busy = &fixture.search.options.transposition_table->buckets[
                key & (PARALLEL_TT_BUCKET_COUNT - 1u)];
            atomic_store(&busy->occupied, 1);
        }
        if (scenario == 0)
        {
            memmove(expected.moves + 3, expected.moves + 2, 3 * sizeof(XqMove));
            expected.moves[2] = fixture.quiet[3];
        }
        CHECK(parallel_prioritize_move(&expected, winner));
        unsigned winning = 123;
        CHECK(collect_and_apply(&fixture.search, &fixture.node, &winning) ==
              (scenario == 5 ? 1u : 2u));
        CHECK(winning == 1);
        check_moves(&fixture.node.moves, &expected);
        if (busy != NULL)
        {
            CHECK(atomic_load(&busy->occupied) == 1);
            atomic_store(&busy->occupied, 0);
        }
        fixture_destroy(&fixture);
    }
}

static void duplicate_candidates_and_hints(void)
{
    Fixture fixture;
    fixture_init(&fixture);
    /* Current key and duplicate sibling keys consume no lookup budget. */
    XqMove sibling = fixture.parent.moves.moves[1];
    XqMove second = fixture.parent.moves.moves[2];
    fixture.parent.moves.count = 6;
    fixture.parent.moves.moves[1] = fixture.parent.moves.moves[0];
    fixture.parent.moves.moves[2] = sibling;
    fixture.parent.moves.moves[3] = sibling;
    fixture.parent.moves.moves[4] = second;
    fixture.parent.moves.moves[5] = fixture.parent.moves.moves[0];
    store_hint(&fixture, 2, XQ_SEARCH_SCORE_EXACT, fixture.quiet[4]);
    store_hint(&fixture, 4, XQ_SEARCH_SCORE_LOWER_BOUND, fixture.quiet[2]);
    CHECK(collect_and_apply(&fixture.search, &fixture.node, NULL) == 2);
    CHECK(equal_move(fixture.node.moves.moves[2], fixture.quiet[4]));
    CHECK(equal_move(fixture.node.moves.moves[3], fixture.quiet[2]));
    xq_parallel_transposition_table_clear(fixture.search.options.transposition_table);
    store_hint(&fixture, 2, XQ_SEARCH_SCORE_EXACT, fixture.quiet[4]);
    store_hint(&fixture, 4, XQ_SEARCH_SCORE_LOWER_BOUND, fixture.quiet[4]);
    fixture.node.moves.count = 5;
    for (int i = 0; i < 5; ++i)
        fixture.node.moves.moves[i] = fixture.quiet[i];
    CHECK(collect_and_apply(&fixture.search, &fixture.node, NULL) == 2);
    const int expected[] = {4, 0, 1, 2, 3};
    for (int i = 0; i < 5; ++i)
        CHECK(equal_move(fixture.node.moves.moves[i], fixture.quiet[expected[i]]));
    fixture_destroy(&fixture);
}

static void invalid_candidates(void)
{
    Fixture fixture;
    fixture_init(&fixture);
    /* Failed construction followed by a missing king does not consume queries. */
    fixture.parent.moves.moves[1].from = XQ_SQUARES;
    fixture.parent.moves.moves[2].to = (uint8_t)xq_position_king_square(
        &fixture.parent.position, XQ_BLACK);
    store_hint(&fixture, 3, XQ_SEARCH_SCORE_EXACT, fixture.quiet[3]);
    store_hint(&fixture, 4, XQ_SEARCH_SCORE_EXACT, fixture.quiet[4]);
    CHECK(collect_and_apply(&fixture.search, &fixture.node, NULL) == 2);
    CHECK(equal_move(fixture.node.moves.moves[2], fixture.quiet[3]));
    CHECK(equal_move(fixture.node.moves.moves[3], fixture.quiet[4]));
    fixture_destroy(&fixture);

    /* A pinned rook's pseudo-legal move exposes the mover's king. */
    fixture_init(&fixture);
    CHECK(xq_position_from_fen(&fixture.parent.position,
        "3k5/9/9/9/4r4/9/9/4R4/9/4K4 r - -"));
    fixture.parent.moves.count = 3;
    fixture.parent.moves.moves[0] = find_move(&fixture.parent.position, 4, 2, 4, 3);
    fixture.parent.moves.moves[1] = find_move(&fixture.parent.position, 4, 2, 3, 2);
    fixture.parent.moves.moves[2] = find_move(&fixture.parent.position, 4, 2, 4, 4);
    fixture.node.position = sibling_position(&fixture, 0);
    xq_generate_pseudo_legal(&fixture.node.position, &fixture.node.moves);
    parallel_order_moves(&fixture.node.position, &fixture.node.moves);
    CHECK(collect_and_apply(&fixture.search, &fixture.node, NULL) == 1);
    /* A source whose resulting side differs from this node is ignored. */
    fixture.node.position.side_to_move = XQ_RED;
    CHECK(collect_and_apply(&fixture.search, &fixture.node, NULL) == 0);
    fixture_destroy(&fixture);
}

static void timing_and_cutoff_isolation(void)
{
    Fixture fixture;
    fixture_init(&fixture);
    fixture.search.options.time_limit_ms = 1;
    CHECK(parallel_clock_ms(&fixture.search.start_ms));
    fixture.search.deadline_ms = fixture.search.start_ms;
    CHECK(collect_and_apply(&fixture.search, &fixture.node, NULL) == 0);
    CHECK(atomic_load(&fixture.search.time_state) == TIME_STOP);
    fixture_destroy(&fixture);

    fixture_init(&fixture);
    /* An exact, very deep sibling score must never return as this node's score. */
    XqTranspositionStats ignored = {0};
    parallel_tt_store(fixture.search.options.transposition_table, sibling_key(&fixture, 1),
        100, 12345, XQ_SEARCH_SCORE_EXACT, fixture.quiet[4], &ignored);
    SearchThreadStats stats = {0};
    ParallelPv pv = {0};
    Interval value = search_node(&fixture.search, &fixture.node.position, &fixture.parent,
        0, 1, NULL, NULL, 0, &pv, &stats);
    CHECK(!value.cancelled);
    CHECK(stats.nodes > 1 && stats.cache.cutoffs == 0);
    CHECK(value.lower != -12345 && value.upper != -12345);
    fixture_destroy(&fixture);
}

typedef struct OrderingTrace
{
    XqSearchSampler *sampler;
    XqMove first[2];
    bool recorded[2];
    XqMove branches[4];
    unsigned branch_count;
} OrderingTrace;

static void trace_ordering(const XqSearchProgress *event, void *user)
{
    OrderingTrace *trace = user;
    if (event->event == XQ_SEARCH_PATH_SAMPLE)
    {
        const XqSearchSample *sample = event->sample;
        if (sample->normal_ply > 0 && trace->branch_count < 4 &&
            (trace->branch_count == 0 ||
             !equal_move(trace->branches[trace->branch_count - 1], sample->path[0])))
            trace->branches[trace->branch_count++] = sample->path[0];
        for (unsigned i = 0; i < 2; ++i)
            if (!trace->recorded[i] && sample->normal_ply > i)
            {
                CHECK(sample->path_length > i);
                trace->first[i] = sample->path[i];
                trace->recorded[i] = true;
            }
    }
    xq_search_sampler_request(trace->sampler);
}

static void winning_search_order_and_pv(void)
{
    XqMove baseline_reply = {0};
    for (int mode = 0; mode < 4; ++mode)
    {
        Fixture fixture;
        fixture_init(&fixture);
        XqMove winner = fixture.quiet[4];
        XqMove later_winner = fixture.quiet[3];
        store_scored_hint(&fixture, 1, WIN, XQ_SEARCH_SCORE_EXACT, winner);
        store_scored_hint(&fixture, 2, WIN, XQ_SEARCH_SCORE_LOWER_BOUND, later_winner);
        XqTranspositionStats ignored = {0};
        /* Same-position TT disagrees; PV is absent, conflicting, matching, or invalid. */
        parallel_tt_store(fixture.search.options.transposition_table,
            xq_position_hash(&fixture.node.position), 1, 12345, XQ_SEARCH_SCORE_EXACT,
            fixture.quiet[1], &ignored);
        XqPosition child = fixture.node.position;
        CHECK(xq_position_make_move(&child, winner));
        XqMove pv_hint[2] = {
            mode == 2 ? winner : mode == 3 ? (XqMove){0} : fixture.quiet[2],
            find_move(&child, 0, 4, 0, 5)
        };
        OrderingTrace trace = {0};
        trace.sampler = xq_search_sampler_create(1);
        CHECK(trace.sampler != NULL);
        SearchThreadStats stats = {0};
        stats.sampling = xq_sampling_thread(trace.sampler, 0);
        xq_sampling_begin(stats.sampling, 2, trace_ordering, &trace);
        xq_search_sampler_request(trace.sampler);
        ParallelPv pv = {0};
        Interval value = search_node(&fixture.search, &fixture.node.position, &fixture.parent,
            0, 2, NULL, mode == 0 ? NULL : pv_hint, mode == 0 ? 0 : 2, &pv, &stats);
        CHECK(!value.cancelled);
        CHECK(stats.nodes > 2 && stats.cache.cutoffs == 0);
        CHECK(value.lower > -WIN && value.upper < WIN);
        CHECK(trace.recorded[0] && trace.recorded[1]);
        CHECK(equal_move(trace.first[0], mode == 1 ? pv_hint[0] : winner));
        /* PV may lead, but the complete winning prefix must stay ahead of the TT move. */
        unsigned offset = mode == 1 ? 1 : 0;
        CHECK(trace.branch_count >= offset + 3);
        CHECK(equal_move(trace.branches[offset], winner));
        CHECK(equal_move(trace.branches[offset + 1], later_winner));
        CHECK(equal_move(trace.branches[offset + 2], fixture.quiet[1]));
        if (mode == 0)
        {
            baseline_reply = trace.first[1];
            CHECK(!equal_move(baseline_reply, pv_hint[1]));
        }
        else if (mode == 3)
            CHECK(equal_move(trace.first[1], baseline_reply));
        else
            CHECK(equal_move(trace.first[1], pv_hint[1]));
        xq_search_sampler_destroy(trace.sampler);
        fixture_destroy(&fixture);
    }
}
#endif

typedef struct SampleCheck
{
    XqSearchSampler *sampler;
    XqPosition root;
    pthread_mutex_t lock;
    unsigned normal, quiet, helpers, idle, iterations;
} SampleCheck;

static void sample_progress(const XqSearchProgress *event, void *user)
{
    SampleCheck *check = user;
    CHECK(pthread_mutex_lock(&check->lock) == 0);
    if (event->event == XQ_SEARCH_PATH_SAMPLE)
    {
        CHECK(event->sample != NULL && !event->sample->truncated);
        const XqSearchSample *sample = event->sample;
        check->normal += sample->state == XQ_SEARCH_THREAD_SEARCH;
        check->quiet += sample->state == XQ_SEARCH_THREAD_QUIESCENCE;
        check->helpers += sample->thread_id != 0;
        CHECK(sample->path_length == sample->normal_ply + sample->quiescence_ply);
        XqPosition position = check->root;
        for (size_t i = 0; i < sample->path_length; ++i)
        {
            CHECK(position.board[sample->path[i].from] == sample->path[i].piece);
            CHECK(position.board[sample->path[i].to] == sample->path[i].captured);
            CHECK(xq_position_make_move(&position, sample->path[i]));
        }
    }
    else if (event->event == XQ_SEARCH_THREAD_STATE)
        check->idle += event->sample->state == XQ_SEARCH_THREAD_IDLE;
    else if (event->event == XQ_SEARCH_ITERATION_FINISHED)
        ++check->iterations;
    xq_search_sampler_request(check->sampler);
    CHECK(pthread_mutex_unlock(&check->lock) == 0);
}

static int slow_evaluate(const XqPosition *position, XqColor perspective, void *user)
{
    (void)position;
    (void)perspective;
    (void)user;
    struct timespec delay = {0, 2000000};
    nanosleep(&delay, NULL);
    return 0;
}

static void searches_and_sampling(void)
{
    const char *fen = "4k4/9/9/9/4p4/4P4/9/9/9/4K4 r - -";
    int serial_score = 0;
    for (unsigned mode = 0; mode < 3; ++mode)
    {
        SampleCheck check = {0};
        CHECK(xq_position_from_fen(&check.root, fen));
        CHECK(pthread_mutex_init(&check.lock, NULL) == 0);
        unsigned threads = mode == 0 ? 1 : 2;
        check.sampler = xq_search_sampler_create(threads);
        CHECK(check.sampler != NULL);
        xq_search_sampler_request(check.sampler);
        XqParallelOptions options = xq_parallel_options_default();
        options.depth = 6;
        options.thread_count = threads;
        options.split_strategy = mode == 2 ? XQ_PARALLEL_SPLIT_INTERLEAVED :
                                             XQ_PARALLEL_SPLIT_CONTIGUOUS;
        options.transposition_table = xq_parallel_transposition_table_create();
        CHECK(options.transposition_table != NULL);
        XqPosition before = check.root;
        XqParallelResult result;
        CHECK(xq_engine_parallel_search_with_observer(NULL, &check.root, &options, &result,
            sample_progress, &check, check.sampler) == XQ_PARALLEL_OK);
        CHECK(result.move_available && result.score_available && !result.stats.stopped);
        CHECK(result.stats.completed_depth == options.depth);
        CHECK(result.stats.cache_available && result.stats.cache.probes > 0);
        CHECK(check.normal && check.quiet && check.idle && check.iterations == options.depth);
        CHECK(memcmp(&before, &check.root, sizeof(before)) == 0);
        if (mode == 0)
            serial_score = result.score;
        else
            CHECK(result.score == serial_score);
        xq_parallel_transposition_table_destroy(options.transposition_table);
        xq_search_sampler_destroy(check.sampler);
        CHECK(pthread_mutex_destroy(&check.lock) == 0);
    }

    XqPosition start;
    xq_position_startpos(&start);
    XqEngineAdapter adapter = {0};
    adapter.static_evaluate = slow_evaluate;
    for (unsigned strategy = 0; strategy < 2; ++strategy)
    {
        XqParallelOptions options = xq_parallel_options_default();
        options.depth = 8;
        options.thread_count = 2;
        options.split_strategy = (XqParallelSplitStrategy)strategy;
        options.time_limit_ms = 1;
        options.transposition_table = xq_parallel_transposition_table_create();
        CHECK(options.transposition_table != NULL);
        XqParallelResult result;
        CHECK(xq_engine_parallel_search(&adapter, &start, &options, &result) == XQ_PARALLEL_OK);
        CHECK(result.move_available && result.stats.stopped);
        xq_parallel_transposition_table_destroy(options.transposition_table);
    }
}

int main(void)
{
    iterator_order();
    readonly_peek();
    disabled_or_inapplicable();
#if XQ_TEST_SIMILARITY_QUERY_LIMIT > 0
    collection_preserves_moves();
    unified_hint_precedence();
    ordering_and_hints();
    query_budget();
    winning_hints();
    duplicate_candidates_and_hints();
    invalid_candidates();
    timing_and_cutoff_isolation();
    winning_search_order_and_pv();
#endif
    searches_and_sampling();
    puts("parallel similarity checks passed");
    return 0;
}
