/* Run with: node tests/search_sample_viewer_tests.js */
"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const modelPath = path.join(__dirname, "../tools/search_sample_viewer/model.js");
const model = require(modelPath);
let checks = 0;

function test(name, body) {
    body();
    checks++;
    console.log("ok " + checks + " - " + name);
}

const startFen = "r3k4/9/9/9/9/9/9/9/9/R3K3R r - -";
function session(overrides = {}) {
    return {
        format_version: 1, search_id: 7, initial_fen: startFen,
        search_mode: "parallel", thread_capacity: 16, sample_interval_ms: 20,
        max_depth: 100, time_limit_ms: 0, path_limit: 1024,
        iteration_memory_limit_bytes: 67108864, recent_window_ms: 5000,
        finished: true, valid_samples: 1, incomplete: false,
        slot_overwritten: 0, dropped: 0, delayed: 0, duplicates: 0,
        boundaries_dropped: 0, allocation_dropped: 0,
        directory_timestamp_timezone: "UTC", sample_timestamp_clock: "monotonic_ms",
        meaning: "cooperative logical-path sample counts; not CPU percentages or node visits",
        ...overrides
    };
}

function node(id, parent, kind, move, inclusive = 1, self = 0, extra = {}) {
    return {id, parent, kind, move, inclusive, self, truncated_self: 0,
        unrecorded_self: 0, ...extra};
}

function report(nodes, extra = {}) {
    return {
        format_version: 1, search_id: 7, iteration_depth: 8, finished: true,
        valid_samples: nodes[0].inclusive, truncated_samples: 0,
        unrecorded_path_samples: 0, window_overwritten: 0, incomplete: false,
        max_normal_ply: 8, max_quiescence_ply: 8, statistics_bytes: 4096,
        session_counters: {slot_overwritten: 0, dropped: 0, delayed: 0,
            duplicates: 0, boundaries_dropped: 0, allocation_dropped: 0},
        thread_statistics_available: false, threads: [], nodes, ...extra
    };
}

function simpleReport() {
    return report([
        node(0, null, "root", null),
        node(1, 0, "normal_move", "a0a9"),
        node(2, 1, "quiescence", null),
        node(3, 2, "quiescence_move", "e9e8", 1, 1)
    ]);
}

function parsed(raw = simpleReport(), rawSession = session()) {
    const s = model.parseSession(rawSession);
    return [s, model.parseIteration(raw, s)];
}

function square(file, rank) { return rank * 9 + file; }

test("CommonJS and isolated browser exports work without DOM or network", () => {
    const context = {};
    vm.runInNewContext(fs.readFileSync(modelPath, "utf8"), context, {filename: modelPath});
    assert.equal(typeof context.XqSampleModel.parseSession, "function");
    assert.equal(typeof model.replay, "function");
});

test("FEN coordinates, red/black side and compatible piece letters round-trip", () => {
    const position = model.parseFen(startFen);
    assert.equal(position.board.length, 90);
    assert.equal(position.board[square(0, 0)], "R");
    assert.equal(position.board[square(0, 9)], "r");
    assert.equal(position.side, "r");
    assert.equal(model.boardToFen(position.board, position.side), startFen);
    for (const side of ["r", "b", "w"]) {
        const p = model.parseFen("4k4/9/9/9/9/9/9/9/9/HEBNK4 " + side + " - - 0 1");
        assert.equal(p.side, side === "b" ? "b" : "r");
        assert.equal(p.board[0], "H");
        assert.equal(p.board[1], "E");
        assert.equal(p.board[2], "B");
        assert.equal(p.board[3], "N");
    }
});

test("root selection leaves initial position and input arrays unchanged", () => {
    const raw = simpleReport();
    const before = JSON.stringify(raw);
    const [s, iteration] = parsed(raw);
    const p = model.replay(s, iteration, 0);
    assert.equal(p.fen, startFen);
    assert.equal(p.side, "r");
    assert.deepEqual(p.moves, []);
    assert.equal(p.lastMove, null);
    assert.equal(p.normalPly, 0);
    assert.equal(p.quiescencePly, 0);
    assert.deepEqual(model.pathTo(iteration, 0).map(n => n.id), [0]);
    assert.equal(JSON.stringify(raw), before);
});

test("incremental iteration building matches ordinary parsing without retaining or changing raw nodes", () => {
    const raw = simpleReport(), before = JSON.stringify(raw), s = model.parseSession(session());
    const expected = model.parseIteration(raw, s), builder = model.createIterationBuilder(s);
    const {nodes, ...metadata} = raw;
    // A parent need not arrive before its child, just as with ordinary parsing.
    for (const item of nodes.slice().reverse()) builder.addNode(item);
    const incremental = builder.finish(metadata);
    const reordered = model.parseIteration({...metadata, nodes:nodes.slice().reverse()}, s);
    assert.deepEqual(incremental, reordered);
    assert.deepEqual(model.replay(s, incremental, 3), model.replay(s, expected, 3));
    assert.equal(JSON.stringify(raw), before);
    assert.notEqual(incremental.byId.get(3), nodes[3]);
    assert.throws(() => builder.addNode(nodes[0]), /完成建模/);
    assert.throws(() => builder.finish(metadata), /完成建模/);
});

test("incremental node input rejects invalid nodes before finalization", () => {
    const s = model.parseSession(session());
    for (const change of [item => {item.kind="script";}, item => {item.move="z0a1";},
        item => {item.self=-1;}, item => {item.parent=null;}]) {
        const builder = model.createIterationBuilder(s), item = simpleReport().nodes[1];
        change(item);assert.throws(() => builder.addNode(item));
    }
    const duplicate = model.createIterationBuilder(s), item = simpleReport().nodes[0];
    duplicate.addNode(item);assert.throws(() => duplicate.addNode(item), /id 重复/);
});

test("incremental finalization retains metadata, parent, depth and thread validation", () => {
    const s = model.parseSession(session());
    const mutations = [
        raw => {raw.search_id=99;}, raw => {raw.format_version=2;},
        raw => {raw.valid_samples=2;}, raw => {raw.iteration_depth=0;raw.max_normal_ply=0;},
        raw => {raw.nodes[1].parent=404;}, raw => {raw.nodes[1].parent=3;},
        raw => {raw.thread_statistics_available=true;raw.threads=[{thread_id:16,valid_samples:1}];},
        raw => {raw.thread_statistics_available=true;raw.threads=[{thread_id:0,valid_samples:2}];}
    ];
    for (const change of mutations) {
        const raw = simpleReport();change(raw);
        const {nodes,...metadata} = raw, builder = model.createIterationBuilder(s);
        nodes.forEach(builder.addNode);
        assert.throws(() => builder.finish(metadata), "incremental metadata was accepted: "+change);
    }
    assert.throws(() => model.createIterationBuilder(s).finish(simpleReport()), /缺少路径节点/);
});

test("capture and quiescence marker restore the selected position precisely", () => {
    const [s, iteration] = parsed();
    const capture = model.replay(s, iteration, 1);
    assert.equal(capture.board[0], null);
    assert.equal(capture.board[81], "R");
    assert.equal(capture.side, "b");
    assert.equal(capture.capture, "r");
    assert.equal(capture.lastMove.move, "a0a9");
    const marker = model.replay(s, iteration, 2);
    assert.deepEqual(marker.board, capture.board);
    assert.equal(marker.side, capture.side);
    assert.equal(marker.normalPly, 1);
    assert.equal(marker.quiescencePly, 0);
    const q = model.replay(s, iteration, 3);
    assert.equal(q.board[square(4, 9)], null);
    assert.equal(q.board[square(4, 8)], "k");
    assert.equal(q.side, "r");
    assert.equal(q.normalPly, 1);
    assert.equal(q.quiescencePly, 1);
    assert.deepEqual(q.moves, ["a0a9", "e9e8"]);
    assert.deepEqual(model.pathTo(iteration, 3).map(n => n.kind),
        ["root", "normal_move", "quiescence", "quiescence_move"]);
    // Browsing a different branch or returning to the root cannot mutate earlier snapshots.
    model.replay(s, iteration, 0);
    assert.equal(capture.board[81], "R");
    assert.equal(s.initialPosition.board[81], "r");
});

test("identical move labels at different prefixes produce different boards", () => {
    const raw = report([
        node(0, null, "root", null, 2),
        node(1, 0, "normal_move", "a0a1"),
        node(2, 1, "normal_move", "e9e8", 1, 1),
        node(3, 0, "normal_move", "i0i1"),
        node(4, 3, "normal_move", "e9e8", 1, 1)
    ]);
    const [s, iteration] = parsed(raw);
    const a = model.replay(s, iteration, 2);
    const b = model.replay(s, iteration, 4);
    assert.equal(a.board[square(0, 1)], "R");
    assert.equal(a.board[square(8, 0)], "R");
    assert.equal(b.board[square(0, 0)], "R");
    assert.equal(b.board[square(8, 1)], "R");
    assert.notEqual(a.fen, b.fen);
});

test("replay follows C make_move semantics without rejecting pseudo-legal search paths", () => {
    const fen = "4k4/9/9/9/9/9/9/9/R8/NR2K4 b - -";
    const raw = report([
        node(0, null, "root", null),
        node(1, 0, "normal_move", "a0a1"), // Knight moves straight and captures own rook.
        node(2, 1, "normal_move", "a1a1", 1, 1) // make_move also accepts same-square moves.
    ]);
    const [s, iteration] = parsed(raw, session({initial_fen: fen}));
    const p = model.replay(s, iteration, 2);
    assert.equal(p.board[square(0, 1)], "N");
    assert.equal(p.board[0], null);
    assert.equal(p.side, "b");
});

test("truncated and capacity-limited samples identify the known prefix", () => {
    for (const [field, total, output] of [
        ["truncated_self", "truncated_samples", "truncated"],
        ["unrecorded_self", "unrecorded_path_samples", "unrecorded"]
    ]) {
        const raw = report([
            node(0, null, "root", null),
            node(1, 0, "normal_move", "a0a1", 1, 1, {[field]: 1})
        ], {[total]: 1, incomplete: true});
        const [s, iteration] = parsed(raw);
        const p = model.replay(s, iteration, 1);
        assert.equal(p[output], true);
        assert.equal(p.board[square(0, 1)], "R");
        assert.equal(p.side, "b");
        assert.deepEqual(p.moves, ["a0a1"]);
    }
    // A truncated sample can also hit the statistics capacity before its recorded
    // path ends. The reporter counts it globally as truncated but marks only the
    // capacity stop at this prefix, so these two totals need not be equal.
    const raw = report([
        node(0, null, "root", null),
        node(1, 0, "normal_move", "a0a1", 1, 1, {unrecorded_self: 1})
    ], {truncated_samples: 1, unrecorded_path_samples: 1, incomplete: true});
    const [s, iteration] = parsed(raw);
    assert.equal(model.replay(s, iteration, 1).unrecorded, true);
});

test("1024 recorded moves and a static phase marker do not need recursive traversal", () => {
    const nodes = [node(0, null, "root", null)];
    const cycle = ["a0a1", "e9e8", "a1a0", "e8e9"];
    let parent = 0;
    for (let ply = 0; ply < 1024; ply++) {
        if (ply === 8) {
            nodes.push(node(nodes.length, parent, "quiescence", null));
            parent = nodes.length - 1;
        }
        nodes.push(node(nodes.length, parent, ply < 8 ? "normal_move" : "quiescence_move",
            cycle[ply % 4], 1, ply === 1023 ? 1 : 0,
            ply === 1023 ? {truncated_self: 1} : {}));
        parent = nodes.length - 1;
    }
    const raw = report(nodes, {truncated_samples: 1, incomplete: true,
        max_normal_ply: 8, max_quiescence_ply: 1016});
    const [s, iteration] = parsed(raw);
    const p = model.replay(s, iteration, parent);
    assert.equal(p.moves.length, 1024);
    assert.equal(p.path.length, 1026);
    assert.equal(p.normalPly, 8);
    assert.equal(p.quiescencePly, 1016);
    assert.equal(p.fen, startFen);
    assert.equal(p.truncated, true);
});

test("JSON strings, sparse node IDs and shuffled node order remain valid", () => {
    const raw = report([
        node(42, 0, "normal_move", "a0a1", 1, 1),
        node(0, null, "root", null)
    ]);
    raw.valid_samples = 1;
    const [s, iteration] = parsed(JSON.stringify(raw), JSON.stringify(session()));
    assert.equal(model.replay(s, iteration, 42).side, "b");
    assert.deepEqual(model.pathTo(iteration, 42).map(n => n.id), [0, 42]);
});

test("malformed JSON, FEN, sessions and mismatched search IDs fail clearly", () => {
    for (const invalid of ["{", "null", [], null, session({format_version: 2}),
        session({initial_fen: "9/9 r - -"}), session({thread_capacity: -1}),
        session({sample_interval_ms: 0})]) {
        assert.throws(() => model.parseSession(invalid));
    }
    for (const fen of ["", "9/9/9/9/9/9/9/9/9/9 x - -",
        "9/9/9/9/9/9/9/9/9/8 r - -", "9/9/9/9/9/9/9/9/9/8Z r - -"]) {
        assert.throws(() => model.parseFen(fen));
    }
    const s = model.parseSession(session());
    assert.throws(() => model.parseIteration("{", s));
    assert.throws(() => model.parseIteration({...simpleReport(), search_id: 8}, s));
    assert.throws(() => model.parseIteration({...simpleReport(), format_version: 99}, s));
});

test("bad parent graphs, duplicate IDs and inconsistent statistics are rejected", () => {
    const mutations = [
        r => {r.nodes[0].parent = 1;},
        r => {r.nodes[1].id = 0;},
        r => {r.nodes[1].parent = 404;},
        r => {r.nodes[1].parent = 3;},
        r => {r.nodes[1].parent = 1;},
        r => {r.nodes[1].kind = "script";},
        r => {r.nodes[1].move = "z0a9";},
        r => {r.nodes[2].move = "a0a1";},
        r => {r.nodes[0].inclusive = 2;},
        r => {r.nodes[3].self = 0;},
        r => {r.nodes[3].self = -1;},
        r => {r.nodes[3].inclusive = Number.MAX_SAFE_INTEGER + 1;},
        r => {r.nodes[3].truncated_self = 2;},
        r => {r.valid_samples = 2;},
        r => {r.nodes[3].truncated_self = 1; r.truncated_samples = 0;},
        r => {r.thread_statistics_available = true; r.threads = [
            {thread_id: 0, valid_samples: 2, max_quiescence_ply: 1, last_timestamp_ms: 100}
        ];}
    ];
    const s = model.parseSession(session());
    for (const mutate of mutations) {
        const raw = simpleReport();
        mutate(raw);
        assert.throws(() => model.parseIteration(raw, s), "mutation was accepted: " + mutate);
    }
    const [, iteration] = parsed();
    assert.throws(() => model.pathTo(iteration, 999));
    const emptySource = report([node(0, null, "root", null),
        node(1, 0, "normal_move", "b0b1", 1, 1)]);
    const [s2, i2] = parsed(emptySource);
    assert.throws(() => model.replay(s2, i2, 1));
});

test("zero-sample iterations are viewable without percentages or synthetic moves", () => {
    const [s, iteration] = parsed(report([node(0, null, "root", null, 0)],
        {valid_samples: 0, max_normal_ply: 0, max_quiescence_ply: 0}));
    assert.equal(iteration.validSamples, 0);
    assert.equal(model.replay(s, iteration, 0).fen, startFen);
});

test("per-thread samples preserve counts and reject duplicate or out-of-range IDs", () => {
    const raw = simpleReport();
    raw.thread_statistics_available = true;
    raw.threads = [
        {thread_id: 0, valid_samples: 0, max_quiescence_ply: 0, last_timestamp_ms: 0},
        {thread_id: 15, valid_samples: 1, max_quiescence_ply: 1, last_timestamp_ms: 100}
    ];
    const [s, iteration] = parsed(raw);
    assert.equal(iteration.threads.length, 2);
    assert.equal(iteration.threads[1].valid_samples, 1);
    for (const id of [0, 16, -1]) {
        const bad = JSON.parse(JSON.stringify(raw));
        bad.threads[1].thread_id = id;
        assert.throws(() => model.parseIteration(bad, s));
    }
});

test("real saved reports replay every sampled endpoint and retain count invariants", () => {
    const reportRoot = path.join(__dirname, "../build/search_samples");
    if (!fs.existsSync(reportRoot)) {
        console.log("  saved-report replay skipped: build/search_samples does not exist");
        return;
    }
    let endpoints = 0;
    let iterations = 0, skippedLarge = 0;
    for (const directory of fs.readdirSync(reportRoot)) {
        const dir = path.join(reportRoot, directory);
        const sessionPath = path.join(dir, "session.json");
        if (!fs.existsSync(sessionPath)) continue;
        const s = model.parseSession(fs.readFileSync(sessionPath, "utf8"));
        for (const file of fs.readdirSync(dir).filter(f => /^depth-\d+\.json$/.test(f))) {
            const filename = path.join(dir, file);
            // Huge reports have separate streaming-reader validation; this
            // synchronous replay suite must stay below whole-string limits.
            if (fs.statSync(filename).size > 64 * 1024 * 1024) { skippedLarge++; continue; }
            const raw = JSON.parse(fs.readFileSync(filename, "utf8"));
            const iteration = model.parseIteration(raw, s);
            let total = 0;
            for (const n of iteration.nodes) {
                total += n.self;
                if (!n.self) continue;
                const p = model.replay(s, iteration, n.id);
                const moveCount = p.path.filter(entry => entry.move !== null).length;
                assert.equal(p.moves.length, moveCount);
                const initialSide = s.initialPosition.side;
                assert.equal(p.side, moveCount % 2 ? (initialSide === "r" ? "b" : "r") : initialSide);
                assert.equal(p.normalPly + p.quiescencePly, moveCount);
                assert.equal(model.boardToFen(p.board, p.side), p.fen);
                endpoints++;
            }
            assert.equal(total, raw.valid_samples);
            if (directory === "search-20261020-225129-84380-1-0" && file === "depth-11.json") {
                assert.equal(total, 5318);
                const roots = iteration.children.get(iteration.root.id);
                const counts = new Map(roots.map(n => [n.move, n.inclusive]));
                assert.equal(counts.get("i8i7"), 1976);
                assert.equal(counts.get("i8h8"), 1105);
            }
            iterations++;
        }
    }
    console.log("  replayed " + endpoints + " sampled endpoints in " + iterations + " saved iterations");
    if (skippedLarge) console.log("  skipped " + skippedLarge + " reports over 64 MiB (covered by streaming-reader checks)");
});

console.log("Passed " + checks + " offline viewer tests.");
