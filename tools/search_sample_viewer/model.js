/* Offline search-sample model. No DOM, network access, or executable report fields. */
(function (root, factory) {
    "use strict";
    var api = factory();
    if (typeof module === "object" && module.exports) module.exports = api;
    else root.XqSampleModel = api;
}(typeof globalThis !== "undefined" ? globalThis : this, function () {
    "use strict";

    var MAX_MOVES = 1024;
    var MAX_PATH_NODES = MAX_MOVES + 2;
    var PIECE = /^[KABENHRCPkabenhrcp]$/;
    var MOVE = /^[a-i][0-9][a-i][0-9]$/;
    var COUNTERS = ["slot_overwritten", "dropped", "delayed", "duplicates",
        "boundaries_dropped", "allocation_dropped"];

    function fail(message) { throw new Error(message); }
    function record(value, label) {
        if (!value || typeof value !== "object" || Array.isArray(value)) fail(label + " 必须是对象");
        return value;
    }
    function input(raw, label) {
        if (typeof raw === "string") {
            try { raw = JSON.parse(raw); }
            catch (_) { fail(label + " 不是有效 JSON"); }
        }
        return record(raw, label);
    }
    function uint(value, label) {
        if (!Number.isSafeInteger(value) || value < 0) fail(label + " 必须是安全范围内的非负整数");
        return value;
    }
    function add(a, b, label) { return uint(a + b, label); }
    function optionalUint(source, target, names) {
        names.forEach(function (name) {
            if (Object.prototype.hasOwnProperty.call(source, name)) target[name] = uint(source[name], name);
        });
    }
    function optionalBool(source, target, names) {
        names.forEach(function (name) {
            if (!Object.prototype.hasOwnProperty.call(source, name)) return;
            if (typeof source[name] !== "boolean") fail(name + " 必须是布尔值");
            target[name] = source[name];
        });
    }
    function optionalText(source, target, names) {
        names.forEach(function (name) {
            if (!Object.prototype.hasOwnProperty.call(source, name)) return;
            if (typeof source[name] !== "string") fail(name + " 必须是字符串");
            target[name] = source[name];
        });
    }
    function header(raw, label) {
        var source = input(raw, label);
        if (source.format_version !== 1) fail(label + " 的 format_version 必须是 1");
        return { source: source, value: { format_version: 1, search_id: uint(source.search_id, "search_id") } };
    }

    // Index = rank * 9 + file; a0 is the red player's lower-left square.
    function parseFen(fen) {
        if (typeof fen !== "string") fail("初始 FEN 必须是字符串");
        var parts = fen.trim().split(/\s+/);
        if (parts.length < 2 || !/^[rbw]$/.test(parts[1])) fail("FEN 缺少有效行棋方");
        var ranks = parts[0].split("/");
        if (ranks.length !== 10) fail("FEN 必须包含 10 行");
        var board = new Array(90).fill(null);
        ranks.forEach(function (row, rowIndex) {
            var file = 0;
            for (var i = 0; i < row.length; i += 1) {
                var ch = row[i];
                if (/^[1-9]$/.test(ch)) file += Number(ch);
                else {
                    if (!PIECE.test(ch) || file >= 9) fail("FEN 包含无效棋子或行宽");
                    board[(9 - rowIndex) * 9 + file] = ch;
                    file += 1;
                }
                if (file > 9) fail("FEN 每行必须恰好为 9 格");
            }
            if (file !== 9) fail("FEN 每行必须恰好为 9 格");
        });
        return { board: board, side: parts[1] === "b" ? "b" : "r" };
    }

    function boardToFen(board, side) {
        if (!Array.isArray(board) || board.length !== 90 || (side !== "r" && side !== "b"))
            fail("棋盘或行棋方无效");
        var ranks = [];
        for (var rank = 9; rank >= 0; rank -= 1) {
            var row = "", empty = 0;
            for (var file = 0; file < 9; file += 1) {
                var piece = board[rank * 9 + file];
                if (piece === null) { empty += 1; continue; }
                if (typeof piece !== "string" || !PIECE.test(piece)) fail("棋盘包含无效棋子");
                if (empty) { row += empty; empty = 0; }
                row += piece;
            }
            if (empty) row += empty;
            ranks.push(row);
        }
        return ranks.join("/") + " " + side + " - -";
    }

    function parseSession(raw) {
        var parsed = header(raw, "session.json"), source = parsed.source, session = parsed.value;
        if (typeof source.initial_fen !== "string") fail("session.json 缺少 initial_fen");
        session.initial_fen = source.initial_fen;
        session.initialPosition = parseFen(source.initial_fen);
        optionalUint(source, session, ["thread_capacity", "sample_interval_ms", "max_depth", "time_limit_ms",
            "path_limit", "iteration_memory_limit_bytes", "recent_window_ms", "valid_samples"].concat(COUNTERS));
        if (session.thread_capacity === 0) fail("thread_capacity 必须大于 0");
        if (session.sample_interval_ms === 0) fail("sample_interval_ms 必须大于 0");
        if (session.path_limit !== undefined && session.path_limit > MAX_MOVES) fail("path_limit 超过支持的 1024 步");
        optionalBool(source, session, ["finished", "incomplete"]);
        optionalText(source, session, ["search_mode", "directory_timestamp_timezone", "sample_timestamp_clock", "meaning"]);
        return session;
    }

    function parseIteration(raw, session) {
        var parsed = header(raw, "迭代报告"), source = parsed.source, iteration = parsed.value;
        if (!session || iteration.search_id !== session.search_id) fail("迭代报告的 search_id 与 session.json 不一致");
        iteration.iteration_depth = uint(source.iteration_depth, "iteration_depth");
        if (session.max_depth !== undefined && iteration.iteration_depth > session.max_depth)
            fail("iteration_depth 超过会话配置的 max_depth");
        iteration.valid_samples = uint(source.valid_samples, "valid_samples");
        optionalUint(source, iteration, ["truncated_samples", "unrecorded_path_samples", "window_overwritten",
            "max_normal_ply", "max_quiescence_ply", "statistics_bytes"]);
        if (iteration.max_normal_ply !== undefined && iteration.max_normal_ply > iteration.iteration_depth)
            fail("max_normal_ply 超过 iteration_depth");
        optionalBool(source, iteration, ["finished", "incomplete", "thread_statistics_available"]);
        if (source.session_counters !== undefined) {
            iteration.session_counters = {};
            optionalUint(record(source.session_counters, "session_counters"), iteration.session_counters, COUNTERS);
        }
        if (!Array.isArray(source.nodes) || !source.nodes.length) fail("迭代报告缺少路径节点");
        var nodes = [], byId = new Map(), children = new Map(), rootNode = null;
        var totalSelf = 0, totalTruncated = 0, totalUnrecorded = 0;
        source.nodes.forEach(function (rawNode) {
            var item = record(rawNode, "路径节点");
            var node = {
                id: uint(item.id, "节点 id"),
                parent: item.parent === null ? null : uint(item.parent, "节点 parent"),
                kind: item.kind,
                move: item.move,
                inclusive: uint(item.inclusive, "节点 inclusive"),
                self: uint(item.self, "节点 self"),
                truncated_self: item.truncated_self === undefined ? 0 : uint(item.truncated_self, "节点 truncated_self"),
                unrecorded_self: item.unrecorded_self === undefined ? 0 : uint(item.unrecorded_self, "节点 unrecorded_self"),
                children: []
            };
            if (byId.has(node.id)) fail("路径节点 id 重复");
            if (node.kind !== "root" && node.kind !== "normal_move" &&
                node.kind !== "quiescence" && node.kind !== "quiescence_move") fail("路径节点 kind 无效");
            if (node.kind === "root" || node.kind === "quiescence") {
                if (node.move !== null) fail("根节点和静态搜索标记不应包含走法");
            } else if (typeof node.move !== "string" || !MOVE.test(node.move)) fail("路径节点走法坐标无效");
            if ((node.parent === null) !== (node.kind === "root")) fail("只有根节点的 parent 可以为空");
            if (node.parent === null) {
                if (rootNode) fail("路径树必须只有一个根节点");
                rootNode = node;
            }
            if (add(node.truncated_self, node.unrecorded_self, "节点不完整样本数") > node.self)
                fail("节点不完整样本数超过 self");
            totalSelf = add(totalSelf, node.self, "self 总数");
            totalTruncated = add(totalTruncated, node.truncated_self, "截断样本总数");
            totalUnrecorded = add(totalUnrecorded, node.unrecorded_self, "遗漏路径样本总数");
            byId.set(node.id, node);
            children.set(node.id, node.children);
            nodes.push(node);
        });
        if (!rootNode) fail("路径树缺少根节点");
        if (rootNode.inclusive !== iteration.valid_samples || totalSelf !== iteration.valid_samples)
            fail("路径树样本总数与 valid_samples 不一致");
        nodes.forEach(function (node) {
            if (node.parent === null) return;
            var parent = byId.get(node.parent);
            if (!parent) fail("路径节点引用不存在的 parent");
            parent.children.push(node);
            var parentInQ = parent.kind === "quiescence" || parent.kind === "quiescence_move";
            if ((node.kind === "normal_move" || node.kind === "quiescence") && parentInQ)
                fail("静态搜索路径中出现普通走法或重复阶段标记");
            if (node.kind === "quiescence_move" && !parentInQ) fail("静态走法缺少阶段标记");
        });
        // Iterative traversal keeps deep or very large reports off the JS call stack.
        rootNode.pathDepth = 1;
        rootNode.normalPly = 0;
        rootNode.quiescencePly = 0;
        var queue = [rootNode];
        for (var cursor = 0; cursor < queue.length; cursor += 1) {
            var current = queue[cursor], subtotal = current.self, childKeys = new Set();
            current.children.forEach(function (child) {
                var key = child.kind + ":" + child.move;
                if (childKeys.has(key)) fail("同一路径前缀下存在重复走法节点");
                childKeys.add(key);
                subtotal = add(subtotal, child.inclusive, "前缀样本总数");
                child.pathDepth = current.pathDepth + 1;
                child.normalPly = current.normalPly + (child.kind === "normal_move" ? 1 : 0);
                child.quiescencePly = current.quiescencePly + (child.kind === "quiescence_move" ? 1 : 0);
                if (child.pathDepth > MAX_PATH_NODES || child.normalPly + child.quiescencePly > MAX_MOVES)
                    fail("路径超过 1024 步记录上限");
                if (child.normalPly > iteration.iteration_depth ||
                    (iteration.max_normal_ply !== undefined && child.normalPly > iteration.max_normal_ply) ||
                    (iteration.max_quiescence_ply !== undefined && child.quiescencePly > iteration.max_quiescence_ply))
                    fail("路径深度与迭代深度统计不一致");
                queue.push(child);
            });
            if (subtotal !== current.inclusive) fail("节点 inclusive 不等于 self 与子节点 inclusive 之和");
            current.children.sort(function (a, b) { return b.inclusive - a.inclusive || a.id - b.id; });
        }
        if (queue.length !== nodes.length) fail("路径树含有环或与根断开的节点");
        if (iteration.truncated_samples !== undefined && totalTruncated > iteration.truncated_samples)
            fail("节点截断样本数超过迭代统计");
        if (iteration.unrecorded_path_samples !== undefined && totalUnrecorded !== iteration.unrecorded_path_samples)
            fail("节点遗漏样本数与迭代统计不一致");
        iteration.threads = [];
        if (source.threads !== undefined) {
            if (!Array.isArray(source.threads)) fail("threads 必须是数组");
            var threadIds = new Set(), threadTotal = 0;
            source.threads.forEach(function (rawThread) {
                var item = record(rawThread, "线程统计");
                var thread = { thread_id: uint(item.thread_id, "thread_id"),
                    valid_samples: uint(item.valid_samples, "线程 valid_samples") };
                if (threadIds.has(thread.thread_id)) fail("thread_id 重复");
                if (session.thread_capacity !== undefined && thread.thread_id >= session.thread_capacity)
                    fail("thread_id 超过 thread_capacity");
                threadIds.add(thread.thread_id);
                threadTotal = add(threadTotal, thread.valid_samples, "线程样本总数");
                optionalUint(item, thread, ["max_quiescence_ply", "last_timestamp_ms"]);
                iteration.threads.push(thread);
            });
            if (iteration.thread_statistics_available !== false && threadTotal !== iteration.valid_samples)
                fail("线程样本总数与 valid_samples 不一致");
        }
        iteration.nodes = nodes;
        iteration.byId = byId;
        iteration.children = children;
        iteration.root = rootNode;
        iteration.depth = iteration.iteration_depth;
        iteration.validSamples = iteration.valid_samples;
        return iteration;
    }

    function pathTo(iteration, nodeId) {
        if (!iteration || !(iteration.byId instanceof Map)) fail("迭代报告尚未解析");
        var node = iteration.byId.get(uint(nodeId, "所选节点 id"));
        if (!node) fail("找不到所选路径节点");
        var path = [];
        while (node) {
            path.push(node);
            if (path.length > MAX_PATH_NODES) fail("路径含环或超过记录上限");
            node = node.parent === null ? null : iteration.byId.get(node.parent);
        }
        return path.reverse();
    }

    function replay(session, iteration, nodeId) {
        if (!session || session.search_id !== iteration.search_id) fail("会话与迭代不匹配");
        var initial = parseFen(session.initial_fen), board = initial.board, side = initial.side;
        var path = pathTo(iteration, nodeId), moves = [], lastMove = null;
        var normalPly = 0, quiescencePly = 0;
        path.forEach(function (node) {
            if (node.kind !== "normal_move" && node.kind !== "quiescence_move") return;
            var move = node.move;
            var from = Number(move[1]) * 9 + move.charCodeAt(0) - 97;
            var to = Number(move[3]) * 9 + move.charCodeAt(2) - 97;
            var piece = board[from], captured = board[to];
            if (piece === null) fail("路径无法回放：" + move + " 的起点没有棋子");
            // Match the engine's raw make_move, including pseudo-legal search paths.
            board[from] = null;
            board[to] = piece;
            side = side === "r" ? "b" : "r";
            lastMove = { move: move, from: from, to: to, piece: piece, capture: captured };
            moves.push(move);
            if (node.kind === "normal_move") normalPly += 1;
            else quiescencePly += 1;
        });
        var leaf = path[path.length - 1];
        return { board: board, side: side, fen: boardToFen(board, side), path: path, moves: moves,
            normalPly: normalPly, quiescencePly: quiescencePly, lastMove: lastMove,
            capture: lastMove ? lastMove.capture : null,
            truncated: leaf.truncated_self > 0, unrecorded: leaf.unrecorded_self > 0 };
    }

    return Object.freeze({ parseSession: parseSession, parseIteration: parseIteration, pathTo: pathTo,
        replay: replay, parseFen: parseFen, boardToFen: boardToFen,
        MAX_MOVES: MAX_MOVES, MAX_PATH_NODES: MAX_PATH_NODES });
}));
