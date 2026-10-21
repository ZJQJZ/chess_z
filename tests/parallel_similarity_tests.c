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

static void store_hint(Fixture *fixture, int index, XqSearchScoreKind kind, XqMove move)
{
    XqTranspositionStats ignored = {0};
    parallel_tt_store(fixture->search.options.transposition_table, sibling_key(fixture, index),
                      1, 12345, kind, move, &ignored);
}

static void check_moves(const XqMoveList *actual, const XqMoveList *expected)
{
    CHECK(actual->count == expected->count);
    for (int i = 0; i < actual->count; ++i)
        CHECK(equal_move(actual->moves[i], expected->moves[i]));
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
#if XQ_TEST_SIMILARITY_QUERY_LIMIT == 0
    uint64_t first_key = sibling_key(&fixture, 1);
    ParallelTtBucket *busy = &fixture.search.options.transposition_table->buckets[
        first_key & (PARALLEL_TT_BUCKET_COUNT - 1u)];
    atomic_store(&busy->occupied, 1);
    CHECK(parallel_order_similar_moves(&fixture.search, &fixture.node) == 0);
    check_moves(&fixture.node.moves, &original);
    CHECK(atomic_load(&busy->occupied) == 1);
    atomic_store(&busy->occupied, 0);
#endif
    fixture.node.depth = 0;
    CHECK(parallel_order_similar_moves(&fixture.search, &fixture.node) == 0);
    fixture.node.depth = 4;
    XqParallelTranspositionTable *table = fixture.search.options.transposition_table;
    fixture.search.options.transposition_table = NULL;
    CHECK(parallel_order_similar_moves(&fixture.search, &fixture.node) == 0);
    fixture.search.options.transposition_table = table;
    CHECK(parallel_order_similar_moves(&fixture.search, &fixture.parent) == 0);
    atomic_store(&fixture.parent.state, NODE_CANCELLED);
    CHECK(parallel_order_similar_moves(&fixture.search, &fixture.node) == 0);
    check_moves(&fixture.node.moves, &original);
    fixture_destroy(&fixture);
}

#if XQ_TEST_SIMILARITY_QUERY_LIMIT > 0
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
    CHECK(parallel_order_similar_moves(&fixture.search, &fixture.node) == 2);
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
    /* The existing promotions run after the similar-position ordering. */
    CHECK(parallel_prioritize_move(&fixture.node.moves, fixture.quiet[4]));
    CHECK(parallel_prioritize_move(&fixture.node.moves, fixture.quiet[2]));
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
        CHECK(parallel_order_similar_moves(&fixture.search, &fixture.node) == 2);
        check_moves(&fixture.node.moves, &original);
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
    CHECK(parallel_order_similar_moves(&fixture.search, &fixture.node) == 2);
    CHECK(equal_move(fixture.node.moves.moves[2], fixture.quiet[4]));
    CHECK(equal_move(fixture.node.moves.moves[3], fixture.quiet[2]));
    xq_parallel_transposition_table_clear(fixture.search.options.transposition_table);
    store_hint(&fixture, 2, XQ_SEARCH_SCORE_EXACT, fixture.quiet[4]);
    store_hint(&fixture, 4, XQ_SEARCH_SCORE_LOWER_BOUND, fixture.quiet[4]);
    fixture.node.moves.count = 5;
    for (int i = 0; i < 5; ++i)
        fixture.node.moves.moves[i] = fixture.quiet[i];
    CHECK(parallel_order_similar_moves(&fixture.search, &fixture.node) == 2);
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
    CHECK(parallel_order_similar_moves(&fixture.search, &fixture.node) == 2);
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
    CHECK(parallel_order_similar_moves(&fixture.search, &fixture.node) == 1);
    /* A source whose resulting side differs from this node is ignored. */
    fixture.node.position.side_to_move = XQ_RED;
    CHECK(parallel_order_similar_moves(&fixture.search, &fixture.node) == 0);
    fixture_destroy(&fixture);
}

static void timing_and_cutoff_isolation(void)
{
    Fixture fixture;
    fixture_init(&fixture);
    fixture.search.options.time_limit_ms = 1;
    CHECK(parallel_clock_ms(&fixture.search.start_ms));
    fixture.search.deadline_ms = fixture.search.start_ms;
    CHECK(parallel_order_similar_moves(&fixture.search, &fixture.node) == 0);
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
    ordering_and_hints();
    query_budget();
    duplicate_candidates_and_hints();
    invalid_candidates();
    timing_and_cutoff_isolation();
#endif
    searches_and_sampling();
    puts("parallel similarity checks passed");
    return 0;
}
