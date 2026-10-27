/* Small synthetic regressions only; never opens reports or starts a search.
 * Run with: node tests/search_sample_compact_model_tests.js */
"use strict";

const assert = require("node:assert/strict");
const model = require("../tools/search_sample_viewer/model.js");
const timeline = require("../tools/search_sample_viewer/timeline.js");
let checks = 0;

const FEN = "r3k4/9/9/9/9/9/9/9/9/R3K3R r - -";
const SESSION = {
    format_version: 1, search_id: 7, initial_fen: FEN,
    thread_capacity: 16, sample_interval_ms: 1, max_depth: 2048,
    path_limit: 1024, finished: true
};
const FIELDS = ["id", "parent", "kind", "move", "inclusive", "self",
    "truncated_self", "unrecorded_self", "pathDepth", "normalPly", "quiescencePly"];

async function test(name, body) {
    await body(); ++checks; console.log("ok compact-model " + checks + " - " + name);
}
function node(id, parent, kind, move, inclusive = 1, self = 0, extra = {}) {
    return {id, parent, kind, move, inclusive, self, ...extra};
}
function report(nodes, extra = {}) {
    return {
        format_version: 1, search_id: 7, iteration_depth: 8,
        valid_samples: nodes.find(item => item.parent === null).inclusive,
        truncated_samples: 0, unrecorded_path_samples: 0,
        max_normal_ply: 8, max_quiescence_ply: 8,
        statistics_bytes: 4096, finished: true, incomplete: false,
        thread_statistics_available: false, threads: [], nodes, ...extra
    };
}
function fixture() {
    return report([
        node(0, null, "root", null, 6),
        node(1, 0, "normal_move", "a0a9", 4, 1),
        node(2, 1, "quiescence", null, 3),
        node(3, 2, "quiescence_move", "e9e8", 3, 3, {truncated_self: 1}),
        node(4, 0, "normal_move", "i0i1", 2, 2, {unrecorded_self: 2})
    ], {truncated_samples: 1, unrecorded_path_samples: 2, incomplete: true});
}
function metadata(raw) {
    const {nodes, ...rest} = raw; return rest;
}
function builder(raw, options = {}) {
    const result = model.createIterationBuilder(model.parseSession(SESSION),
        {compact: true, pageSize: 8, ...options});
    raw.nodes.forEach(result.addNode);
    return result;
}
function compact(raw, options) { return builder(raw, options).finish(metadata(raw)); }
function numericOrder(a, b) { return a.id < b.id ? -1 : a.id > b.id ? 1 : 0; }
function snapshot(iteration) {
    return Array.from(iteration.nodes, item => ({
        ...Object.fromEntries(FIELDS.map(key => [key, item[key]])),
        children: Array.from(item.children, child => child.id)
    })).sort(numericOrder);
}
function compareOrdinary(raw, actual = compact(raw)) {
    const expected = model.parseIteration(raw, model.parseSession(SESSION));
    assert.deepEqual(snapshot(actual), snapshot(expected));
    for (const key of ["format_version", "search_id", "iteration_depth", "depth",
        "valid_samples", "validSamples", "truncated_samples", "unrecorded_path_samples",
        "max_normal_ply", "max_quiescence_ply", "statistics_bytes", "finished", "incomplete",
        "thread_statistics_available", "threads"]) {
        assert.deepEqual(actual[key], expected[key], key);
    }
    return actual;
}
function move(index) {
    // Distinct coordinate strings, intentionally independent of board legality.
    const from = index % 90, to = Math.floor(index / 90) % 90;
    return String.fromCharCode(97 + from % 9) + Math.floor(from / 9) +
        String.fromCharCode(97 + to % 9) + Math.floor(to / 9);
}
function pagedFixture() {
    const rows = [node(0, null, "root", null, 30)];
    for (let group = 0; group < 3; ++group) {
        const parent = group * 11 + 1;
        rows.push(node(parent, 0, "normal_move", move(group), 10));
        for (let leaf = 1; leaf <= 10; ++leaf)
            rows.push(node(parent + leaf, parent, "normal_move", move(leaf), 1, 1));
    }
    return report(rows);
}

async function main() {
    await test("compact views preserve every normalized field and input data", () => {
        const raw = fixture(), before = JSON.stringify(raw), actual = compareOrdinary(raw);
        assert.equal(JSON.stringify(raw), before);
        assert.notEqual(actual.byId.get(3), raw.nodes[3]);
        assert.equal(actual.root.id, 0);
        assert.equal(actual.depth, 8);
        assert.equal(actual.validSamples, 6);
    });
    await test("node and lookup facades retain the operations used by the viewer", () => {
        const actual = compact(fixture()), visited = [];
        assert.equal(actual.nodes.length, 5);
        assert.equal(actual.byId.size, 5);
        assert.equal(actual.children.size, 5);
        actual.nodes.forEach(item => visited.push(item.id));
        assert.deepEqual(visited.slice().sort((a, b) => a - b), [0, 1, 2, 3, 4]);
        assert.deepEqual(Array.from(actual.nodes, item => item.id).sort((a, b) => a - b), visited.sort((a, b) => a - b));
        for (const id of visited) {
            assert.equal(actual.byId.has(id), true);
            assert.equal(actual.children.has(id), true);
            assert.equal(actual.byId.get(id).id, id);
        }
        assert.equal(actual.byId.has(404), false);
        assert.equal(actual.byId.get(404), undefined);
        assert.equal(actual.children.has(404), false);
        assert.equal(actual.children.get(404), undefined);
        assert.equal(actual.children.get(3).length, 0);
    });
    await test("page boundaries and child-before-parent input preserve the complete tree", () => {
        for (const order of [rows => rows, rows => rows.reverse(),
            rows => rows.filter((_, i) => i % 2).concat(rows.filter((_, i) => !(i % 2)))]) {
            const raw = pagedFixture(); raw.nodes = order(raw.nodes);
            const actual = compareOrdinary(raw);
            assert.equal(actual.nodes.length, 34);
            for (const id of [7, 8, 15, 16, 23, 24, 31, 32, 33]) {
                assert.equal(actual.byId.get(id).id, id);
                assert.ok(model.pathTo(actual, id).length >= 2);
            }
        }
    });
    await test("sparse safe-integer IDs never alias page offsets or reserved sentinels", () => {
        const raw = fixture();
        const ids = [Number.MAX_SAFE_INTEGER, 2 ** 24 - 1, 2 ** 24,
            2 ** 32 - 1, 2 ** 32];
        for (const item of raw.nodes) {
            item.id = ids[item.id];
            if (item.parent !== null) item.parent = ids[item.parent];
        }
        raw.nodes.reverse();
        const actual = compareOrdinary(raw);
        assert.equal(actual.root.id, Number.MAX_SAFE_INTEGER);
        assert.deepEqual(model.pathTo(actual, ids[3]).map(item => item.id), ids.slice(0, 4));
        assert.equal(actual.byId.has(2 ** 24 + 1), false);
        assert.equal(actual.byId.has(Number.MAX_SAFE_INTEGER - 1), false);
        assert.equal(actual.byId.get(ids[4]).unrecorded_self, 2);
    });
    await test("high-ID promotion preserves older parent, first-child and sibling links in the same page", () => {
        const high = Number.MAX_SAFE_INTEGER;
        // Creation order makes validation visit page 0, the high-ID page, then
        // page 1. Each promoted link column already contains a low-ID link.
        const raw = report([
            node(0, null, "root", null, 5),
            node(1, 0, "normal_move", "a0a1", 3),
            node(2, 1, "normal_move", "e9e8", 2),
            node(3, 2, "normal_move", "a1a2", 1, 1),
            node(4, high, "normal_move", "e9e8", 1, 1),
            node(high - 1, 1, "normal_move", "a1a3", 1, 1),
            node(high, 0, "normal_move", "i0i1"),
            node(8, 2, "normal_move", "a1a4", 1, 1),
            node(9, 0, "normal_move", "e0e1", 1, 1)
        ]);
        const actual = compareOrdinary(raw);
        assert.deepEqual(actual.children.get(0).map(item => item.id), [1, 9, high]);
        assert.deepEqual(actual.children.get(1).map(item => item.id), [2, high - 1]);
        assert.deepEqual(actual.children.get(2).map(item => item.id), [3, 8]);
        assert.equal(actual.byId.get(3).parent, 2);
        assert.equal(actual.byId.get(4).parent, high);
        assert.deepEqual(model.pathTo(actual, 8).map(item => item.id), [0, 1, 2, 8]);
    });
    await test("counts beyond uint32 and at the safe-integer boundary remain exact", () => {
        const count = 2 ** 32 + 19;
        const raw = report([node(0, null, "root", null, count + 5),
            node(1, 0, "normal_move", "a0a1", count, count),
            node(2, 0, "normal_move", "i0i1", 5, 5)]);
        assert.equal(compareOrdinary(raw).byId.get(1).self, count);
        const largest = report([node(0, null, "root", null, Number.MAX_SAFE_INTEGER),
            node(1, 0, "normal_move", "a0a1", Number.MAX_SAFE_INTEGER,
                Number.MAX_SAFE_INTEGER, {truncated_self: Number.MAX_SAFE_INTEGER})],
        {truncated_samples: Number.MAX_SAFE_INTEGER});
        assert.equal(compareOrdinary(largest).byId.get(1).truncated_self, Number.MAX_SAFE_INTEGER);
    });
    await test("children are sorted by sample count then ID regardless of arrival order", () => {
        const raw = report([node(0, null, "root", null, 8),
            node(9, 0, "normal_move", "a0a1", 2, 2),
            node(3, 0, "normal_move", "i0i1", 2, 2),
            node(8, 0, "normal_move", "e0e1", 4, 4)]);
        const actual = compareOrdinary(raw);
        assert.deepEqual(actual.children.get(0).map(item => item.id), [8, 3, 9]);
        assert.deepEqual(actual.root.children.map(item => item.id), [8, 3, 9]);
    });
    await test("replay and path selection preserve captures and quiescence", () => {
        const raw = fixture(), actual = compact(raw), s = model.parseSession(SESSION);
        const expected = model.parseIteration(raw, s);
        for (const id of [0, 1, 2, 3, 4]) {
            const a = model.replay(s, actual, id), b = model.replay(s, expected, id);
            assert.deepEqual({...a, path: a.path.map(item => item.id)},
                {...b, path: b.path.map(item => item.id)});
        }
        assert.equal(model.replay(s, actual, 1).capture, "r");
        assert.equal(model.replay(s, actual, 3).board[8 * 9 + 4], "k");
        assert.equal(model.replay(s, actual, 3).truncated, true);
        assert.equal(model.replay(s, actual, 4).unrecorded, true);
        assert.throws(() => model.pathTo(actual, 404));
    });
    await test("timeline chunk validation and pixel common-prefix aggregation use compact lookups", async () => {
        const high = 2 ** 24 + 1;
        const raw = report([
            node(0, null, "root", null, 4),
            node(1, 0, "normal_move", "a0a1", 4),
            node(2, 1, "normal_move", "a1a2", 3, 2),
            node(high, 1, "normal_move", "a1a3", 1, 1),
            node(4, 2, "normal_move", "a2a3", 1, 1)
        ]);
        const iteration = compact(raw), s = model.parseSession(SESSION);
        const records = [2, high, 2, 4].map((id, index) => ({
            type: "sample", thread_id: 0, sequence: index + 1,
            time_ms: index + 20, state: "search", request_time_ms: index + 19,
            request_id: index + 1, node_id: id, normal_ply: id === 4 ? 3 : 2,
            quiescence_ply: 0, truncated: false, path_omitted: false
        }));
        const info = {file: "depth-8-timeline-000001.json", start_ms: 0, end_ms: 100, records: records.length};
        const chunk = {timeline_version: 1, search_id: 7, iteration_depth: 8,
            start_ms: 0, end_ms: 100, records};
        assert.equal(timeline.parseChunk(chunk, info, s, iteration, 16).length, 4);
        assert.throws(() => timeline.parseChunk({...chunk,
            records: [{...records[0], node_id: 404}, ...records.slice(1)]}, info, s, iteration, 16));
        const manifest = timeline.manifest({timeline_version: 1, timeline: {
            start_ms: 0, end_ms: 100, finished: true, incomplete: false,
            initial_states: [{thread_id: 0, state: "search", time_ms: 0, sequence: 0}],
            chunks: [info]
        }}, s, iteration);
        const files = new Map([[info.file, {text: async () => JSON.stringify(chunk)}]]);
        const data = new timeline.Data(manifest, files, s, iteration);
        await data.buildOverview();
        const range = await data.range(0, 100, 0, 1);
        assert.equal(data.failed.size, 0);
        assert.equal(range.actualSamples, 4);
        assert.equal(range.samples.length, 1);
        assert.equal(range.samples[0].commonNode, 1);
        assert.equal(range.samples[0].record.node_id, 4);
        assert.equal(range.samples[0].count, 4);
        assert.equal(range.samples[0].firstTime, 20);
        assert.equal(range.samples[0].lastTime, 23);
    });
    await test("zero samples and absent optional node counters match ordinary parsing", () => {
        const actual = compareOrdinary(report([node(0, null, "root", null, 0)],
            {valid_samples: 0, max_normal_ply: 0, max_quiescence_ply: 0}));
        assert.equal(actual.root.truncated_self, 0);
        assert.equal(actual.root.unrecorded_self, 0);
    });
    await test("invalid nodes, IDs, phase transitions and inconsistent metadata are rejected", () => {
        const changes = [
            raw => { raw.nodes[1].id = 0; },
            raw => { raw.nodes[1].id = Number.MAX_SAFE_INTEGER + 1; },
            raw => { raw.nodes[1].parent = 404; },
            raw => { raw.nodes[1].parent = 1; },
            raw => { raw.nodes[1].kind = "script"; },
            raw => { raw.nodes[1].move = "z0a9"; },
            raw => { raw.nodes[2].move = "a0a1"; },
            raw => { raw.nodes[3].kind = "normal_move"; },
            raw => { raw.nodes[2].kind = "quiescence_move"; raw.nodes[2].move = "e9e8"; },
            raw => { raw.nodes[3].self = -1; },
            raw => { raw.nodes[3].inclusive = Number.MAX_SAFE_INTEGER + 1; },
            raw => { raw.nodes[3].truncated_self = 4; },
            raw => { raw.nodes[0].inclusive = 7; },
            raw => { raw.valid_samples = 5; },
            raw => { raw.truncated_samples = 0; },
            raw => { raw.unrecorded_path_samples = 0; },
            raw => { raw.max_normal_ply = 0; },
            raw => { raw.max_quiescence_ply = 0; },
            raw => { raw.search_id = 99; },
            raw => { raw.format_version = 2; },
            raw => { raw.thread_statistics_available = true; raw.threads = [{thread_id: 0, valid_samples: 5}]; }
        ];
        for (const change of changes) {
            const raw = fixture(); change(raw);
            assert.throws(() => compact(raw), "accepted invalid compact input: " + change);
        }
    });
    await test("disconnected cycles and duplicate sibling labels are rejected", () => {
        const cycle = report([node(0, null, "root", null, 0),
            node(1, 2, "normal_move", "a0a1", 0),
            node(2, 1, "normal_move", "i0i1", 0)]);
        assert.throws(() => compact(cycle));
        const repeated = report([node(0, null, "root", null, 2),
            node(1, 0, "normal_move", "a0a1", 1, 1),
            node(2, 0, "normal_move", "a0a1", 1, 1)]);
        assert.throws(() => compact(repeated));
    });
    await test("the 1024-move path limit is enforced without recursive traversal", () => {
        const rows = [node(0, null, "root", null)];
        for (let id = 1; id <= 1025; ++id)
            rows.push(node(id, id - 1, "normal_move", "a0a1", 1, id === 1025 ? 1 : 0));
        assert.throws(() => compact(report(rows,
            {iteration_depth: 1025, max_normal_ply: 1025, max_quiescence_ply: 0})));
    });
    await test("finished builders reject reuse and preserve the detached normalized values", () => {
        const raw = fixture(), pending = builder(raw), result = pending.finish(metadata(raw));
        const before = snapshot(result);
        raw.nodes[3].self = 999;
        assert.deepEqual(snapshot(result), before);
        assert.throws(() => pending.addNode(raw.nodes[0]));
        assert.throws(() => pending.finish(metadata(raw)));
    });
    await test("async finalization reports progress and matches synchronous compact parsing", async () => {
        const raw = pagedFixture(), pending = builder(raw); let progress = 0;
        const result = await pending.finishAsync(metadata(raw), {
            async onProgress() { ++progress; await Promise.resolve(); }, cancelled: () => false
        });
        assert.ok(progress > 0);
        compareOrdinary(raw, result);
        assert.deepEqual(snapshot(result), snapshot(compact(raw)));
    });
    await test("async finalization honors cancellation before work and at progress boundaries", async () => {
        const raw = pagedFixture(); let progress = 0;
        await assert.rejects(builder(raw).finishAsync(metadata(raw), {
            cancelled: () => true, onProgress() { ++progress; }
        }), {name: "AbortError"});
        assert.equal(progress, 0);
        let cancelled = false;
        await assert.rejects(builder(raw).finishAsync(metadata(raw), {
            cancelled: () => cancelled,
            async onProgress() { cancelled = true; await Promise.resolve(); }
        }), {name: "AbortError"});
    });
    await test("async finalization propagates invalid metadata and progress callback errors", async () => {
        const raw = fixture(), invalid = {...metadata(raw), valid_samples: 99};
        await assert.rejects(builder(raw).finishAsync(invalid, {}));
        const expected = new Error("progress consumer failed");
        await assert.rejects(builder(raw).finishAsync(metadata(raw), {
            onProgress() { throw expected; }
        }), error => error === expected);
    });
    console.log("Passed " + checks + " small synthetic compact-model checks.");
}
main().catch(error => { console.error(error); process.exitCode = 1; });
