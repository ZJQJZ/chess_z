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
    var EMPTY_CHILDREN = Object.freeze([]);

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

    function iterationHeader(raw, session) {
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
        return parsed;
    }

    function normalizeNode(rawNode) {
        var item = record(rawNode, "路径节点");
        var node = {
            id: uint(item.id, "节点 id"),
            parent: item.parent === null ? null : uint(item.parent, "节点 parent"),
            kind: item.kind, move: item.move,
            inclusive: uint(item.inclusive, "节点 inclusive"),
            self: uint(item.self, "节点 self"),
            truncated_self: item.truncated_self === undefined ? 0 : uint(item.truncated_self, "节点 truncated_self"),
            unrecorded_self: item.unrecorded_self === undefined ? 0 : uint(item.unrecorded_self, "节点 unrecorded_self"),
            children: EMPTY_CHILDREN, pathDepth: 0, normalPly: 0, quiescencePly: 0
        };
        if (node.kind !== "root" && node.kind !== "normal_move" &&
            node.kind !== "quiescence" && node.kind !== "quiescence_move") fail("路径节点 kind 无效");
        if (node.kind === "root" || node.kind === "quiescence") {
            if (node.move !== null) fail("根节点和静态搜索标记不应包含走法");
        } else if (typeof node.move !== "string" || !MOVE.test(node.move)) fail("路径节点走法坐标无效");
        if ((node.parent === null) !== (node.kind === "root")) fail("只有根节点的 parent 可以为空");
        if (add(node.truncated_self, node.unrecorded_self, "节点不完整样本数") > node.self)
            fail("节点不完整样本数超过 self");
        return node;
    }

    function finishMetadata(iteration, source, session, totalTruncated, totalUnrecorded) {
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
        iteration.depth = iteration.iteration_depth;
        iteration.validSamples = iteration.valid_samples;
        return iteration;
    }

    // Small reports keep ordinary objects. Large reports use numeric pages and
    // bounded, on-demand views instead of two Map entries and an object per node.
    function createIterationBuilder(session, options) {
        if (options && options.compact) return createCompactIterationBuilder(session, options);
        var nodes = [], byId = new Map(), children = new Map(), rootNode = null;
        var totalSelf = 0, totalTruncated = 0, totalUnrecorded = 0;
        var finished = false;
        function addNode(rawNode) {
            if (finished) fail("迭代报告已完成建模");
            var node = normalizeNode(rawNode);
            if (byId.has(node.id)) fail("路径节点 id 重复");
            if (node.parent === null) {
                if (rootNode) fail("路径树必须只有一个根节点");
                rootNode = node;
            }
            totalSelf = add(totalSelf, node.self, "self 总数");
            totalTruncated = add(totalTruncated, node.truncated_self, "截断样本总数");
            totalUnrecorded = add(totalUnrecorded, node.unrecorded_self, "遗漏路径样本总数");
            byId.set(node.id, node);
            children.set(node.id, node.children);
            nodes.push(node);
        }
        function finish(rawMetadata) {
            if (finished) fail("迭代报告已完成建模");
            var parsed = iterationHeader(rawMetadata, session), source = parsed.source, iteration = parsed.value;
            if (!nodes.length) fail("迭代报告缺少路径节点");
            // Parent links are attached only once; a failed finish is not reusable.
            finished = true;
            if (!rootNode) fail("路径树缺少根节点");
            if (rootNode.inclusive !== iteration.valid_samples || totalSelf !== iteration.valid_samples)
                fail("路径树样本总数与 valid_samples 不一致");
            nodes.forEach(function (node) {
                if (node.parent === null) return;
                var parent = byId.get(node.parent);
                if (!parent) fail("路径节点引用不存在的 parent");
                if (parent.children === EMPTY_CHILDREN) {
                    parent.children = [];
                    children.set(parent.id, parent.children);
                }
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
                var current = queue[cursor], subtotal = current.self;
                var childKeys = current.children.length > 1 ? new Set() : null;
                current.children.forEach(function (child) {
                    var key = child.kind + ":" + child.move;
                    if (childKeys) {
                        if (childKeys.has(key)) fail("同一路径前缀下存在重复走法节点");
                        childKeys.add(key);
                    }
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
                if (current.children.length > 1)
                    current.children.sort(function (a, b) { return b.inclusive - a.inclusive || a.id - b.id; });
            }
            if (queue.length !== nodes.length) fail("路径树含有环或与根断开的节点");
            iteration.nodes = nodes;
            iteration.byId = byId;
            iteration.children = children;
            iteration.root = rootNode;
            return finishMetadata(iteration, source, session, totalTruncated, totalUnrecorded);
        }
        return { addNode: addNode, finish: finish };
    }

    function createCompactIterationBuilder(session, options) {
        var pageSize = options.pageSize === undefined ? 65536 : options.pageSize;
        if (!Number.isSafeInteger(pageSize) || pageSize < 1 || pageSize > 65536)
            fail("节点存储页大小必须为 1 至 65536");
        // Dense engine IDs cost 34 bytes/node, plus optional nonzero loss counts.
        // The Map contains pages, never individual nodes. Links normally use
        // uint32 (id + 1, zero = absent); only pages with larger external IDs
        // promote that link column to Float64, with -1 as the absent sentinel.
        var pages = new Map(), views = new Map(), count = 0, rootId = null;
        var totalSelf = 0, totalTruncated = 0, totalUnrecorded = 0, finished = false;
        var MAX_LINK_ID = 4294967294, VIEW_LIMIT = 4096, BATCH = 16384;
        function pageAt(id) { return pages.get(Math.floor(id / pageSize)); }
        function has(id) {
            if (!Number.isSafeInteger(id) || id < 0) return false;
            var page = pageAt(id);
            return !!page && page.code[id % pageSize] !== 0;
        }
        function link(column, offset) {
            var value = column[offset];
            if (column instanceof Uint32Array) return value === 0 ? null : value - 1;
            return value === -1 ? null : value;
        }
        function putLink(page, name, offset, value) {
            var column = page[name];
            if (column instanceof Uint32Array && value !== null && value > MAX_LINK_ID) {
                var promoted = new Float64Array(pageSize);
                for (var i = 0; i < pageSize; ++i) promoted[i] = column[i] - 1;
                column = page[name] = promoted;
            }
            if (column instanceof Uint32Array) column[offset] = value === null ? 0 : value + 1;
            else column[offset] = value === null ? -1 : value;
        }
        function encode(node) {
            if (node.kind === "root") return 1;
            if (node.kind === "quiescence") return 2;
            var move = node.move, from = Number(move[1]) * 9 + move.charCodeAt(0) - 97;
            var to = Number(move[3]) * 9 + move.charCodeAt(2) - 97;
            return 3 + from * 90 + to + (node.kind === "quiescence_move" ? 8100 : 0);
        }
        function kind(code) {
            return code === 1 ? "root" : code === 2 ? "quiescence" : code < 8103 ? "normal_move" : "quiescence_move";
        }
        function inQ(code) { return code === 2 || code >= 8103; }
        function moveText(code) {
            if (code <= 2) return null;
            var squares = (code - 3) % 8100, from = Math.floor(squares / 90), to = squares % 90;
            return String.fromCharCode(97 + from % 9) + Math.floor(from / 9) +
                String.fromCharCode(97 + to % 9) + Math.floor(to / 9);
        }
        function addNode(rawNode) {
            if (finished) fail("迭代报告已完成建模");
            var node = normalizeNode(rawNode), index = Math.floor(node.id / pageSize), offset = node.id % pageSize;
            var page = pages.get(index);
            if (!page) {
                page = {code: new Uint16Array(pageSize), parent: new Uint32Array(pageSize),
                    first: new Uint32Array(pageSize), next: new Uint32Array(pageSize),
                    inclusive: new Float64Array(pageSize), self: new Float64Array(pageSize),
                    depth: new Uint32Array(pageSize), truncated: null, unrecorded: null};
                pages.set(index, page);
            }
            if (page.code[offset]) fail("路径节点 id 重复");
            if (node.parent === null) {
                if (rootId !== null) fail("路径树必须只有一个根节点");
                rootId = node.id;
            }
            totalSelf = add(totalSelf, node.self, "self 总数");
            totalTruncated = add(totalTruncated, node.truncated_self, "截断样本总数");
            totalUnrecorded = add(totalUnrecorded, node.unrecorded_self, "遗漏路径样本总数");
            putLink(page, "parent", offset, node.parent);
            page.code[offset] = encode(node);
            page.inclusive[offset] = node.inclusive; page.self[offset] = node.self;
            if (node.truncated_self) {
                if (!page.truncated) page.truncated = new Float64Array(pageSize);
                page.truncated[offset] = node.truncated_self;
            }
            if (node.unrecorded_self) {
                if (!page.unrecorded) page.unrecorded = new Float64Array(pageSize);
                page.unrecorded[offset] = node.unrecorded_self;
            }
            ++count;
        }
        function childViews(id) {
            if (!has(id)) return undefined;
            var page = pageAt(id), child = link(page.first, id % pageSize);
            if (child === null) return EMPTY_CHILDREN;
            var result = [];
            while (child !== null) {
                result.push(get(child));
                page = pageAt(child); child = link(page.next, child % pageSize);
            }
            result.sort(function (a, b) { return b.inclusive - a.inclusive || a.id - b.id; });
            return result;
        }
        function get(id) {
            if (!has(id)) return undefined;
            if (views.has(id)) {
                var cached = views.get(id); views.delete(id); views.set(id, cached); return cached;
            }
            var page = pageAt(id), offset = id % pageSize, code = page.code[offset];
            var depth = page.depth[offset] - 1, normal = depth % 2048, q = Math.floor(depth / 2048);
            var node = {id: id, parent: link(page.parent, offset), kind: kind(code), move: moveText(code),
                inclusive: page.inclusive[offset], self: page.self[offset],
                truncated_self: page.truncated ? page.truncated[offset] : 0,
                unrecorded_self: page.unrecorded ? page.unrecorded[offset] : 0,
                pathDepth: normal + q + 1 + (inQ(code) ? 1 : 0), normalPly: normal, quiescencePly: q};
            Object.defineProperty(node, "children", {enumerable: true, get: function () { return childViews(id); }});
            if (views.size >= VIEW_LIMIT) views.delete(views.keys().next().value);
            views.set(id, node);
            return node;
        }
        function* ids() {
            for (var entry of pages) {
                var base = entry[0] * pageSize, page = entry[1];
                for (var offset = 0; offset < pageSize; ++offset)
                    if (page.code[offset]) yield base + offset;
            }
        }
        function beginFinish(rawMetadata) {
            if (finished) fail("迭代报告已完成建模");
            var parsed = iterationHeader(rawMetadata, session);
            if (!count) fail("迭代报告缺少路径节点");
            finished = true;
            if (rootId === null) fail("路径树缺少根节点");
            var rootPage = pageAt(rootId);
            if (rootPage.inclusive[rootId % pageSize] !== parsed.value.valid_samples || totalSelf !== parsed.value.valid_samples)
                fail("路径树样本总数与 valid_samples 不一致");
            return validate(parsed);
        }
        function* validate(parsed) {
            var iteration = parsed.value, done = 0, total = count * 2;
            // Numeric adjacency links allow parents to arrive after children.
            for (var entry of pages) {
                var page = entry[1], base = entry[0] * pageSize;
                for (var offset = 0; offset < pageSize; ++offset) {
                    var code = page.code[offset];
                    if (!code) continue;
                    var id = base + offset, parent = link(page.parent, offset);
                    if (parent !== null) {
                        if (!has(parent)) fail("路径节点引用不存在的 parent");
                        var parentPage = pageAt(parent), parentOffset = parent % pageSize;
                        var parentInQ = inQ(parentPage.code[parentOffset]);
                        if ((code === 2 || code >= 3 && code < 8103) && parentInQ)
                            fail("静态搜索路径中出现普通走法或重复阶段标记");
                        if (code >= 8103 && !parentInQ) fail("静态走法缺少阶段标记");
                        putLink(page, "next", offset, link(parentPage.first, parentOffset));
                        putLink(parentPage, "first", parentOffset, id);
                    }
                    if (++done % BATCH === 0) yield {done: done, total: total};
                }
            }
            yield {done: done, total: total};
            // Resolve each parent chain once, with at most MAX_PATH_NODES local
            // entries. No breadth-first queue or full-tree node views are kept.
            for (var item of pages) {
                page = item[1]; base = item[0] * pageSize;
                for (offset = 0; offset < pageSize; ++offset) {
                    code = page.code[offset];
                    if (!code) continue;
                    id = base + offset;
                    if (!page.depth[offset]) {
                        var chain = [], cursor = id;
                        while (true) {
                            var currentPage = pageAt(cursor), currentOffset = cursor % pageSize;
                            if (currentPage.depth[currentOffset]) break;
                            chain.push(cursor);
                            if (chain.length > MAX_PATH_NODES) fail("路径含环或超过 1024 步记录上限");
                            parent = link(currentPage.parent, currentOffset);
                            if (parent === null) { currentPage.depth[currentOffset] = 1; chain.pop(); break; }
                            cursor = parent;
                        }
                        while (chain.length) {
                            cursor = chain.pop(); currentPage = pageAt(cursor); currentOffset = cursor % pageSize;
                            parent = link(currentPage.parent, currentOffset); parentPage = pageAt(parent);
                            var packed = parentPage.depth[parent % pageSize] - 1, currentCode = currentPage.code[currentOffset];
                            var normal = packed % 2048 + (currentCode >= 3 && currentCode < 8103 ? 1 : 0);
                            var q = Math.floor(packed / 2048) + (currentCode >= 8103 ? 1 : 0);
                            if (normal + q > MAX_MOVES || normal + q + 1 + (inQ(currentCode) ? 1 : 0) > MAX_PATH_NODES)
                                fail("路径超过 1024 步记录上限");
                            if (normal > iteration.iteration_depth ||
                                (iteration.max_normal_ply !== undefined && normal > iteration.max_normal_ply) ||
                                (iteration.max_quiescence_ply !== undefined && q > iteration.max_quiescence_ply))
                                fail("路径深度与迭代深度统计不一致");
                            currentPage.depth[currentOffset] = normal + q * 2048 + 1;
                        }
                    }
                    var child = link(page.first, offset), subtotal = page.self[offset];
                    var keys = child !== null && link(pageAt(child).next, child % pageSize) !== null ? new Set() : null;
                    while (child !== null) {
                        var childPage = pageAt(child), childOffset = child % pageSize, key = childPage.code[childOffset];
                        if (keys) {
                            if (keys.has(key)) fail("同一路径前缀下存在重复走法节点");
                            keys.add(key);
                        }
                        subtotal = add(subtotal, childPage.inclusive[childOffset], "前缀样本总数");
                        child = link(childPage.next, childOffset);
                    }
                    if (subtotal !== page.inclusive[offset]) fail("节点 inclusive 不等于 self 与子节点 inclusive 之和");
                    if (++done % BATCH === 0) yield {done: done, total: total};
                }
            }
            yield {done: done, total: total};
            iteration.nodes = {length: count, forEach: function (callback) {
                var index = 0; for (var id of ids()) callback(get(id), index++, iteration.nodes);
            }};
            iteration.nodes[Symbol.iterator] = function* () { for (var id of ids()) yield get(id); };
            iteration.byId = {get: get, has: has, size: count};
            iteration.children = {get: childViews, has: has, size: count};
            iteration.root = get(rootId);
            iteration.storage = "compact";
            return finishMetadata(iteration, parsed.source, session, totalTruncated, totalUnrecorded);
        }
        function finish(rawMetadata) {
            var work = beginFinish(rawMetadata), result;
            do { result = work.next(); } while (!result.done);
            return result.value;
        }
        async function finishAsync(rawMetadata, callbacks) {
            callbacks = callbacks || {};
            function checkCancelled() {
                if (callbacks.cancelled && callbacks.cancelled()) {
                    var error = new Error("已取消读取报告。"); error.name = "AbortError"; throw error;
                }
            }
            checkCancelled();
            var work = beginFinish(rawMetadata);
            while (true) {
                checkCancelled();
                var result = work.next();
                if (result.done) return result.value;
                if (callbacks.onProgress) await callbacks.onProgress(result.value.done, result.value.total);
            }
        }
        return {addNode: addNode, finish: finish, finishAsync: finishAsync};
    }

    function parseIteration(raw, session) {
        var parsed = iterationHeader(raw, session), source = parsed.source;
        if (!Array.isArray(source.nodes) || !source.nodes.length) fail("迭代报告缺少路径节点");
        var builder = createIterationBuilder(session);
        source.nodes.forEach(builder.addNode);
        return builder.finish(source);
    }

    function pathTo(iteration, nodeId) {
        if (!iteration || !iteration.byId || typeof iteration.byId.get !== "function") fail("迭代报告尚未解析");
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

    return Object.freeze({ parseSession: parseSession, parseIteration: parseIteration,
        createIterationBuilder: createIterationBuilder, pathTo: pathTo,
        replay: replay, parseFen: parseFen, boardToFen: boardToFen,
        MAX_MOVES: MAX_MOVES, MAX_PATH_NODES: MAX_PATH_NODES });
}));
