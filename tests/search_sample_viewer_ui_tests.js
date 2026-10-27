/* Run with: node tests/search_sample_viewer_ui_tests.js
 * DOM simulation exercises the shipped event handlers; it is not a browser or
 * a visual/layout test. No packages, engine process, network or server required.
 */
"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const viewerDir = path.join(__dirname, "../tools/search_sample_viewer");
const index = fs.readFileSync(path.join(viewerDir, "index.html"), "utf8");
const modelSource = fs.readFileSync(path.join(viewerDir, "model.js"), "utf8");
const readerSource = fs.readFileSync(path.join(viewerDir, "report_reader.js"), "utf8");
const timelineSource = fs.readFileSync(path.join(viewerDir, "timeline.js"), "utf8");
const viewerSource = fs.readFileSync(path.join(viewerDir, "viewer.js"), "utf8");

// The small DOM below implements only element creation, text, attributes,
// fragments, simple selectors and events. It deliberately provides no layout,
// fetch, file-system access or internal viewer state to the application.
class Element {
    constructor(tag, nodeType = 1) {
        this.tagName = tag.toUpperCase(); this.nodeType = nodeType;
        this.children = []; this.parentNode = null; this.attributes = new Map();
        this.listeners = new Map(); this.style = {}; this.className = "";
        this.hidden = false; this.disabled = false; this.clientWidth = 980;
        this.scrollTop = 0; this._text = ""; this._value = "";
        this.classList = {
            add: (...names) => { this.className = [...new Set([...this.classes(), ...names])].join(" "); },
            remove: (...names) => { this.className = this.classes().filter(n => !names.includes(n)).join(" "); },
            contains: name => this.classes().includes(name)
        };
    }
    classes() { return this.className.split(/\s+/).filter(Boolean); }
    get value() { return this._value; }
    set value(value) { this._value = String(value); }
    get textContent() { return this._text + this.children.map(c => c.textContent).join(""); }
    set textContent(value) { this.replaceChildren(); this._text = String(value); }
    append(...nodes) {
        for (let item of nodes) {
            if (typeof item !== "object") { const n = new Element("#text", 3); n._text = String(item); item = n; }
            if (item.nodeType === 11) {
                this.append(...item.children.slice()); item.children = []; continue;
            }
            if (item.parentNode) item.remove();
            item.parentNode = this; this.children.push(item);
        }
    }
    replaceChildren(...nodes) {
        for (const child of this.children) child.parentNode = null;
        this.children = []; this._text = ""; this.append(...nodes);
    }
    remove() {
        if (this.parentNode) this.parentNode.children = this.parentNode.children.filter(child => child !== this);
        this.parentNode = null;
    }
    setAttribute(name, value) {
        if (name === "class") this.className = String(value);
        else this.attributes.set(name, String(value));
    }
    getAttribute(name) { return name === "class" ? this.className : this.attributes.get(name) ?? null; }
    addEventListener(type, handler) {
        if (!this.listeners.has(type)) this.listeners.set(type, []);
        this.listeners.get(type).push(handler);
    }
    async fire(type, properties = {}) {
        const event = {type, target: this, currentTarget: this, defaultPrevented: false,
            preventDefault() { this.defaultPrevented = true; }, ...properties};
        for (const handler of this.listeners.get(type) || []) await handler(event);
        return event;
    }
    matches(selector) {
        if (selector.startsWith(".")) return selector.slice(1).split(".").every(name => this.classes().includes(name));
        const attribute = /^\[([^=]+)="([^"]*)"\]$/.exec(selector);
        if (attribute) return this.getAttribute(attribute[1]) === attribute[2];
        return this.tagName === selector.toUpperCase();
    }
    querySelectorAll(selector) {
        const result = [], pending = this.children.slice().reverse();
        while (pending.length) {
            const child = pending.pop();
            if (child.matches(selector)) result.push(child);
            pending.push(...child.children.slice().reverse());
        }
        return result;
    }
    querySelector(selector) { return this.querySelectorAll(selector)[0] || null; }
    closest(selector) {
        for (let el = this; el; el = el.parentNode) if (el.matches(selector)) return el;
        return null;
    }
    select() { /* Clipboard fallback selects the textarea. */ }
}

function browser(loadModel = true) {
    const document = new Element("#document", 9), ids = new Map();
    document.body = new Element("body");
    document.createElement = tag => new Element(tag);
    document.createElementNS = (_namespace, tag) => new Element(tag);
    document.createDocumentFragment = () => new Element("#fragment", 11);
    document.createTextNode = text => { const n = new Element("#text", 3); n.textContent = text; return n; };
    document.execCommand = command => command === "copy";
    document.getElementById = id => {
        assert.ok(ids.has(id), "viewer referenced missing HTML id: " + id);
        return ids.get(id);
    };
    for (const match of index.matchAll(/<([a-z][\w-]*)\b([^<>]*\bid="([^"]+)"[^<>]*)>/gi)) {
        const el = new Element(match[1]);
        el.hidden = /\bhidden(?:\s|$)/.test(match[2]);
        el.disabled = /\bdisabled(?:\s|$)/.test(match[2]);
        ids.set(match[3], el); document.body.append(el);
    }
    const window = {document, isSecureContext: false, addEventListener() {}};
    const context = vm.createContext({window, document, navigator: {}, Intl, Blob, TextDecoder, Uint8Array, ArrayBuffer,
        requestAnimationFrame: callback => { queueMicrotask(() => callback(0)); return 1; },
        setTimeout: callback => { queueMicrotask(callback); return 1; }});
    if (loadModel) {
        vm.runInContext(modelSource, context, {filename: "model.js"});
        window.XqSampleModel = context.XqSampleModel;
    }
    vm.runInContext(readerSource, context, {filename: "report_reader.js"});
    window.XqSampleReportReader = context.XqSampleReportReader;
    vm.runInContext(timelineSource, context, {filename: "timeline.js"});
    window.XqSampleTimeline = context.XqSampleTimeline;
    vm.runInContext(viewerSource, context, {filename: "viewer.js"});
    return {
        document, $: id => document.getElementById(id),
        window,
        frame(id) {
            const frame = document.getElementById("chart").querySelector('[data-node-id="' + id + '"]');
            assert.ok(frame, "chart has no visible frame for node " + id); return frame;
        }
    };
}

function file(name, raw, directory = "fixture", text) {
    return {name, webkitRelativePath: directory + "/" + name,
        text: text || (() => Promise.resolve(typeof raw === "string" ? raw : JSON.stringify(raw)))};
}
async function importFiles(ui, files) { ui.$("files").files = files; await ui.$("files").fire("change"); }
function deferred() {
    let resolve;
    const promise = new Promise(done => { resolve = done; });
    return {promise, resolve};
}
function streamedFile(name, raw, directory = "fixture", firstRead) {
    // Exceed the streaming threshold without keeping a large report fixture on disk.
    const json = typeof raw === "string" ? raw : JSON.stringify(raw);
    const blob = new Blob([" ".repeat(16 * 1024 * 1024), json]);
    const reads = {text: 0, buffers: 0, maximumSlice: 0};
    return {name, webkitRelativePath: directory + "/" + name, size: blob.size, reads,
        text() { ++reads.text; throw new Error("A large report must not use file.text()."); },
        slice(start, end) {
            const part = blob.slice(start, end);
            reads.maximumSlice = Math.max(reads.maximumSlice, part.size);
            return {size: part.size, async arrayBuffer() {
                ++reads.buffers;
                if (reads.buffers === 1 && firstRead) await firstRead();
                return part.arrayBuffer();
            }};
        }};
}
async function settle() { for (let i = 0; i < 15; i++) await Promise.resolve(); }
function boardCell(ui, coordinate) {
    const cell = ui.$("board").querySelectorAll(".square").find(el => el.title.startsWith(coordinate + " "));
    assert.ok(cell, "board missing square " + coordinate); return cell;
}
function rootFen(ui) { return ui.$("fen").value; }
function nodes(ui) { return ui.$("chart").querySelectorAll(".chart-frame").map(el => Number(el.getAttribute("data-node-id"))); }

const initialFen = "r3k4/9/9/9/9/9/9/9/9/R3K3R r - -";
function fixture(depth = 2, overrides = {}) {
    const session = {format_version: 1, search_id: 7, initial_fen: initialFen,
        search_mode: "parallel", thread_capacity: 2, sample_interval_ms: 20};
    const n = (id, parent, kind, move, self = 0) => ({id, parent, kind, move,
        inclusive: 1, self, truncated_self: 0, unrecorded_self: 0});
    const report = {format_version: 1, search_id: 7, iteration_depth: depth,
        finished: true, valid_samples: 1, max_normal_ply: 1, max_quiescence_ply: 1,
        truncated_samples: 0, unrecorded_path_samples: 0, incomplete: false,
        thread_statistics_available: false, threads: [],
        nodes: [n(0, null, "root", null), n(1, 0, "normal_move", "a0a9"),
            n(2, 1, "quiescence", null), n(3, 2, "quiescence_move", "e9e8", 1)], ...overrides};
    return {session, report};
}
function filesFor(data, directory = "fixture") {
    return [file("session.json", data.session, directory),
        file("depth-" + data.report.iteration_depth + ".json", data.report, directory)];
}

let checks = 0;
async function test(name, body) {
    await body(); ++checks; console.log("ok " + checks + " - " + name);
}

async function main() {
    await test("missing model fails visibly and demo loads Chinese board through its event handler", async () => {
        const missing = browser(false);
        assert.equal(missing.$("notice").hidden, false);
        assert.match(missing.$("notice").textContent, /无法加载 model.js/);
        const ui = browser();
        assert.equal(ui.$("loaded").hidden, true);
        await ui.$("demo").fire("click");
        assert.equal(ui.$("loaded").hidden, false);
        assert.equal(ui.$("empty").hidden, true);
        assert.equal(ui.$("total-samples").textContent, "100");
        assert.equal(ui.$("sample-threads").textContent, "2 / 2");
        assert.equal(ui.$("board").querySelectorAll(".square").length, 90);
        assert.equal(boardCell(ui, "b9").textContent, "馬");
        assert.equal(boardCell(ui, "b9").classList.contains("black"), true);
        assert.match(ui.$("side").textContent, /红方行棋/);
        assert.equal(ui.$("parent").disabled, true);
        assert.equal(ui.$("depth-select").value, "3");
    });

    await test("ordinary and static selections replay captures, side and path without altering root", async () => {
        const ui = browser(); await ui.$("demo").fire("click");
        const initial = rootFen(ui);
        await ui.frame(1).fire("click");
        assert.equal(ui.$("selected-move").textContent, "b2b9");
        assert.equal(boardCell(ui, "b9").textContent, "炮");
        assert.equal(boardCell(ui, "b9").classList.contains("red"), true);
        assert.equal(boardCell(ui, "b2").textContent, "·");
        assert.equal(ui.$("inclusive").textContent, "65");
        assert.equal(ui.$("self").textContent, "5");
        assert.equal(ui.$("path").querySelectorAll("button").length, 2);
        assert.match(ui.$("side").textContent, /黑方行棋/);
        assert.equal(rootFen(ui), "rCbakabnr/9/1c5c1/p1p1p1p1p/9/9/P1P1P1P1P/7C1/9/RNBAKABNR b - -");
        await ui.frame(2).fire("click"); const beforeQ = rootFen(ui);
        await ui.frame(3).fire("click");
        assert.equal(rootFen(ui), beforeQ);
        assert.equal(ui.$("board-title").textContent, "进入静态搜索时的棋盘");
        assert.equal(ui.$("node-phase").textContent, "静态搜索");
        await ui.frame(4).fire("keydown", {key: "Enter"});
        assert.equal(boardCell(ui, "h9").textContent, "炮");
        assert.equal(boardCell(ui, "h2").textContent, "·");
        assert.match(ui.$("side").textContent, /普通 2 · 静态 1/);
        assert.equal(rootFen(ui), "1rbakabCr/9/1c5c1/p1p1p1p1p/9/9/P1P1P1P1P/9/9/RNBAKABNR b - -");
        await ui.$("parent").fire("click"); assert.equal(rootFen(ui), beforeQ);
        await ui.$("path").querySelector("button").fire("click");
        assert.equal(rootFen(ui), initial);
        assert.equal(boardCell(ui, "b9").textContent, "馬");
    });

    await test("flip changes orientation only and double-click or toolbar zoom changes chart scope", async () => {
        const ui = browser(); await ui.$("demo").fire("click");
        const fen = rootFen(ui);
        assert.ok(ui.$("board").querySelectorAll(".square")[0].title.startsWith("a9 "));
        await ui.$("flip").fire("click");
        assert.equal(ui.$("flip").getAttribute("aria-pressed"), "true");
        assert.ok(ui.$("board").querySelectorAll(".square")[0].title.startsWith("i0 "));
        assert.equal(rootFen(ui), fen);
        const doubleClickTarget = ui.frame(1);
        await doubleClickTarget.fire("click");
        assert.equal(ui.frame(1), doubleClickTarget, "single click must retain the element that receives the subsequent double click");
        await doubleClickTarget.fire("dblclick");
        assert.equal(nodes(ui).join(","), "1,2,3,4");
        assert.match(ui.$("chart-info").textContent, /范围：b2b9/);
        await ui.frame(4).fire("click"); await ui.$("zoom-selected").fire("click");
        assert.equal(nodes(ui).join(","), "4");
        await ui.$("zoom-parent").fire("click"); assert.equal(nodes(ui).join(","), "3,4");
        await ui.$("zoom-root").fire("click"); assert.equal(nodes(ui).length, 6);
        assert.equal(ui.$("selected-move").textContent, "h2h9");
    });

    await test("file input imports reports, preserves a valid view on errors and falls back from a bad latest depth", async () => {
        const ui = browser(); const data = fixture(); await importFiles(ui, filesFor(data));
        assert.equal(ui.$("session-name").textContent, "fixture");
        assert.equal(rootFen(ui), initialFen);
        await ui.frame(1).fire("click"); const selectedFen = rootFen(ui);
        await importFiles(ui, [file("session.json", "{", "broken"), file("depth-2.json", data.report, "broken")]);
        assert.equal(ui.$("notice").className, "error");
        assert.equal(ui.$("session-name").textContent, "fixture");
        assert.equal(rootFen(ui), selectedFen);
        await importFiles(ui, [...filesFor(data, "fallback"), file("depth-3.json", "{", "fallback")]);
        assert.equal(ui.$("session-name").textContent, "fallback");
        assert.equal(ui.$("depth-select").value, "2");
        assert.match(ui.$("notice").textContent, /已跳过.*depth-3.json/);
        ui.$("depth-select").value = "3"; await ui.$("depth-select").fire("change");
        assert.equal(ui.$("depth-select").value, "2");
        assert.equal(rootFen(ui), initialFen);
        assert.equal(ui.$("notice").className, "error");
    });

    await test("large sliced reports load and switch depths without reading one whole text string", async () => {
        const ui = browser(), latest = fixture(4), older = fixture(2);
        assert.equal(typeof ui.window.XqSampleReportReader.read, "function", "the browser reader module was not loaded");
        const large = streamedFile("depth-4.json", latest.report);
        await importFiles(ui, [file("session.json", latest.session), file("depth-2.json", older.report), large]);
        assert.equal(ui.$("session-name").textContent, "fixture");
        assert.equal(ui.$("depth-select").value, "4");
        assert.equal(rootFen(ui), initialFen);
        assert.equal(large.reads.text, 0);
        assert.ok(large.reads.buffers > 1, "the report was not read in separate byte slices");
        assert.ok(large.reads.maximumSlice < large.size, "the reader allocated the entire report as one slice");
        await ui.frame(1).fire("click");assert.notEqual(rootFen(ui), initialFen);
        ui.$("depth-select").value = "2";await ui.$("depth-select").fire("change");
        assert.equal(ui.$("depth-select").value, "2");assert.equal(rootFen(ui), initialFen);
        ui.$("depth-select").value = "4";await ui.$("depth-select").fire("change");
        assert.equal(ui.$("depth-select").value, "4");assert.equal(rootFen(ui), initialFen);
        assert.equal(ui.$("selected-move").textContent, "根局面");
        assert.equal(large.reads.text, 0);assert.equal(ui.$("load-status").textContent, "");
    });

    await test("truncated large reports name the damaged file and preserve the displayed iteration", async () => {
        const ui = browser(), current = fixture(4), broken = fixture(2);
        const damaged = streamedFile("depth-2.json", JSON.stringify(broken.report).slice(0, -8));
        await importFiles(ui, [...filesFor(current), damaged]);
        await ui.frame(1).fire("click");const selectedFen = rootFen(ui);
        ui.$("depth-select").value = "2";await ui.$("depth-select").fire("change");
        assert.equal(ui.$("notice").className, "error");
        assert.match(ui.$("notice").textContent, /depth-2\.json/);
        assert.match(ui.$("notice").textContent, /损坏|截断|不完整/);
        assert.equal(ui.$("depth-select").value, "4");assert.equal(rootFen(ui), selectedFen);
        assert.equal(ui.$("selected-move").textContent, "a0a9");
        assert.equal(damaged.reads.text, 0);
        const importedDamage = streamedFile("depth-2.json", JSON.stringify(broken.report).slice(0, -8), "damaged-import");
        await importFiles(ui, [file("session.json", broken.session, "damaged-import"), importedDamage]);
        assert.equal(ui.$("notice").className, "error");
        assert.match(ui.$("notice").textContent, /depth-2\.json/);
        assert.match(ui.$("notice").textContent, /损坏|截断|不完整/);
        assert.equal(ui.$("session-name").textContent, "fixture");
        assert.equal(ui.$("depth-select").value, "4");assert.equal(rootFen(ui), selectedFen);
        assert.equal(importedDamage.reads.text, 0);assert.equal(ui.$("load-status").textContent, "");
    });

    await test("session picker keeps different initial FENs separate even when search IDs match", async () => {
        const ui = browser(), a = fixture(), b = fixture();
        b.session.initial_fen = "4k3r/9/9/9/9/9/9/9/9/R3K4 b - -";
        assert.equal(a.session.search_id, b.session.search_id);
        await importFiles(ui, [...filesFor(a, "a-position"), ...filesFor(b, "z-position")]);
        assert.equal(ui.$("session-picker").hidden, false);
        assert.equal(ui.$("session-select").querySelectorAll("option").length, 2);
        assert.equal(ui.$("session-name").textContent, "z-position");
        assert.equal(rootFen(ui), b.session.initial_fen);
        assert.equal(boardCell(ui, "i9").classList.contains("black"), true);
        ui.$("session-select").value = "a-position";
        await ui.$("session-select").fire("change"); await settle();
        assert.equal(ui.$("session-name").textContent, "a-position");
        assert.equal(rootFen(ui), a.session.initial_fen);
        assert.equal(boardCell(ui, "a9").textContent, "車");
        assert.equal(boardCell(ui, "i9").textContent, "·");
        await ui.frame(1).fire("click");
        assert.match(ui.$("side").textContent, /黑方行棋/);
        ui.$("session-select").value = "z-position";
        await ui.$("session-select").fire("change"); await settle();
        assert.equal(rootFen(ui), b.session.initial_fen);
        assert.equal(ui.$("selected-move").textContent, "根局面");
        await ui.frame(1).fire("click");
        assert.equal(boardCell(ui, "i9").classList.contains("black"), true);
        assert.match(ui.$("side").textContent, /红方行棋/);
    });

    await test("branch list and move filter select narrow paths through real click handlers", async () => {
        const ui = browser(); await ui.$("demo").fire("click");
        ui.$("move-filter").value = "h2"; await ui.$("move-filter").fire("input");
        const buttons = ui.$("branches").querySelectorAll("button");
        assert.equal(buttons.length, 1); assert.equal(buttons[0].textContent, "h2e2");
        await buttons[0].fire("click");
        assert.equal(ui.$("selected-move").textContent, "h2e2");
        assert.equal(boardCell(ui, "e2").textContent, "炮");
        assert.equal(boardCell(ui, "h2").textContent, "·");
        assert.equal(ui.$("move-filter").value, "");
    });

    await test("late imports cannot overwrite a newer demo or a newer imported report", async () => {
        for (const newer of ["demo", "import"]) {
            const ui = browser(), wait = deferred(), old = fixture();
            const loading = importFiles(ui, [file("session.json", null, "old", () => wait.promise),
                file("depth-2.json", old.report, "old")]);
            await settle();
            if (newer === "demo") await ui.$("demo").fire("click");
            else await importFiles(ui, filesFor(fixture(), "new"));
            const name = ui.$("session-name").textContent, fen = rootFen(ui), samples = ui.$("total-samples").textContent;
            wait.resolve(JSON.stringify(old.session)); await loading; await settle();
            assert.equal(ui.$("session-name").textContent, name);
            assert.equal(rootFen(ui), fen);
            assert.equal(ui.$("total-samples").textContent, samples);
            assert.equal(ui.$("load-status").textContent, "");
        }
    });

    await test("a pending depth change cannot replace a newer demo", async () => {
        const ui = browser(), wait = deferred(), data = fixture();
        const older = fixture(1);
        await importFiles(ui, [...filesFor(data), file("depth-1.json", null, "fixture", () => wait.promise)]);
        ui.$("depth-select").value = "1";
        const changing = ui.$("depth-select").fire("change"); await settle();
        await ui.$("demo").fire("click");
        wait.resolve(JSON.stringify(older.report)); await changing; await settle();
        assert.match(ui.$("session-name").textContent, /内置示例/);
        assert.equal(ui.$("depth-select").value, "3");
        assert.equal(ui.$("total-samples").textContent, "100");
        assert.equal(ui.$("load-status").textContent, "");
    });

    await test("cancelled sliced imports and depth changes stop reading and cannot replace a newer demo", async () => {
        for (const mode of ["import", "depth"]) {
            const ui = browser(), wait = deferred(), entered = deferred(), older = fixture(2);
            const large = streamedFile("depth-2.json", older.report, "streaming-old", async () => {
                entered.resolve();await wait.promise;
            });
            let loading;
            if (mode === "import") {
                loading = importFiles(ui, [file("session.json", older.session, "streaming-old"), large]);
            } else {
                const latest = fixture(4);
                await importFiles(ui, [...filesFor(latest, "streaming-old"), large]);
                ui.$("depth-select").value = "2";loading = ui.$("depth-select").fire("change");
            }
            await Promise.race([entered.promise, loading.then(() => { throw new Error(mode + " ended without reading a byte slice"); })]);
            await ui.$("demo").fire("click");
            wait.resolve();await loading;await settle();
            assert.match(ui.$("session-name").textContent, /内置示例/);
            assert.equal(ui.$("depth-select").value, "3");assert.equal(ui.$("total-samples").textContent, "100");
            assert.match(ui.$("notice").textContent, /这是内置示例/);
            assert.equal(ui.$("load-status").textContent, "");assert.equal(large.reads.text, 0);
            assert.equal(large.reads.buffers, 1, mode + " continued reading after cancellation");
        }
    });

    await test("a slow dropped directory cannot replace a newer demo after directory traversal", async () => {
        const ui = browser(), data = fixture();
        let releaseEntries, batches = 0;
        const folder = {name: "dropped-old", isDirectory: true, createReader: () => ({
            readEntries(resolve) { if (batches++ === 0) releaseEntries = resolve; else resolve([]); }
        })};
        const dropping = ui.document.fire("drop", {dataTransfer: {
            types: ["Files"], files: [], items: [{webkitGetAsEntry: () => folder}]
        }});
        await settle(); assert.equal(typeof releaseEntries, "function");
        await ui.$("demo").fire("click");
        releaseEntries(filesFor(data).map(item => ({name: item.name, isFile: true, file: done => done(item)})));
        await dropping; await settle();
        assert.match(ui.$("session-name").textContent, /内置示例/);
        assert.match(ui.$("notice").textContent, /这是内置示例/);
        assert.equal(ui.$("total-samples").textContent, "100");
        assert.equal(ui.document.body.classList.contains("dragging"), false);
    });

    await test("zero samples and limited prefixes show honest empty-state and board warnings", async () => {
        const ui = browser(); const empty = fixture(1, {valid_samples: 0, max_normal_ply: 0, max_quiescence_ply: 0,
            nodes: [{id: 0, parent: null, kind: "root", move: null, inclusive: 0, self: 0}]});
        await importFiles(ui, filesFor(empty));
        assert.equal(ui.$("chart-empty").hidden, false);
        assert.match(ui.$("chart-empty").textContent, /尚无有效样本/);
        assert.equal(ui.$("chart").style.display, "none");
        assert.equal(rootFen(ui), initialFen);
        assert.match(ui.$("node-ratio").textContent, /—/);
        assert.doesNotMatch(ui.$("node-ratio").textContent, /NaN|Infinity/);
        const limited = fixture(2, {valid_samples: 2, incomplete: true,
            truncated_samples: 1, unrecorded_path_samples: 1, max_quiescence_ply: 0,
            nodes: [{id: 0, parent: null, kind: "root", move: null, inclusive: 2, self: 0},
                {id: 1, parent: 0, kind: "normal_move", move: "a0a9", inclusive: 2, self: 2,
                    truncated_self: 1, unrecorded_self: 1}]});
        await importFiles(ui, filesFor(limited)); await ui.frame(1).fire("click");
        assert.equal(ui.$("node-warning").hidden, false);
        assert.match(ui.$("node-warning").textContent, /路径被截断/);
        assert.match(ui.$("node-warning").textContent, /统计容量不足/);
        assert.match(ui.$("node-warning").textContent, /棋盘仅对应已记录前缀/);
        assert.equal(boardCell(ui, "a9").textContent, "車");
        assert.equal(ui.$("sample-integrity").textContent, "统计存在缺失");
        assert.equal(ui.$("chart").style.display, "block");
    });

    await test("unreplayable path reports the error without displaying a stale board", async () => {
        const ui = browser(), data = fixture(); data.report.nodes[1].move = "b0b1";
        await importFiles(ui, filesFor(data)); await ui.frame(1).fire("click");
        assert.equal(ui.$("replay-error").hidden, false);
        assert.equal(ui.$("board").querySelectorAll(".square").length, 0);
        assert.equal(rootFen(ui), "");
        assert.equal(ui.$("copy-fen").disabled, true);
        await ui.$("parent").fire("click");
        assert.equal(ui.$("replay-error").hidden, true);
        assert.equal(rootFen(ui), initialFen);
        assert.equal(ui.$("copy-fen").disabled, false);
    });

    await test("existing reports import through the UI and switch iteration while retaining the initial board", async () => {
        const reportRoot = path.join(__dirname, "../build/search_samples");
        if (!fs.existsSync(reportRoot)) { console.log("  saved-report UI import skipped: no local reports"); return; }
        const directory = fs.readdirSync(reportRoot).find(name => {
            const dir = path.join(reportRoot, name);
            return fs.existsSync(path.join(dir, "session.json")) &&
                fs.existsSync(path.join(dir, "depth-10.json")) && fs.existsSync(path.join(dir, "depth-11.json"));
        });
        if (!directory) { console.log("  saved-report UI import skipped: no depth-10/11 fixture"); return; }
        const dir = path.join(reportRoot, directory);
        const read = name => fs.readFileSync(path.join(dir, name), "utf8");
        const ui = browser();
        await importFiles(ui, ["session.json", "depth-10.json", "depth-11.json"].map(name => file(name, read(name), directory)));
        assert.equal(ui.$("depth-select").value, "11");
        const expected = JSON.parse(read("depth-11.json"));
        assert.equal(ui.$("total-samples").textContent.replace(/,/g, ""), String(expected.valid_samples));
        assert.equal(ui.$("sample-threads").textContent, "16 / 16");
        const fen = rootFen(ui);
        const rootBranches = ui.$("branches").querySelectorAll("button");
        assert.ok(rootBranches.length > 0); await rootBranches[0].fire("click");
        assert.notEqual(rootFen(ui), fen);
        ui.$("depth-select").value = "10"; await ui.$("depth-select").fire("change");
        assert.equal(ui.$("depth-select").value, "10");
        assert.equal(rootFen(ui), fen);
        assert.equal(ui.$("selected-move").textContent, "根局面");
        assert.equal(ui.$("total-samples").textContent.replace(/,/g, ""), String(JSON.parse(read("depth-10.json")).valid_samples));
        assert.equal(ui.$("replay-error").hidden, true);
    });
    console.log("Passed " + checks + " offline viewer DOM smoke tests (simulated DOM; not a visual browser test).");
}

module.exports = main();
module.exports.harness = {browser, file, importFiles, fixture, filesFor, boardCell, rootFen, settle};
