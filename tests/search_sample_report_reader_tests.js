/* Run with: node tests/search_sample_report_reader_tests.js */
"use strict";
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const {Blob} = require("node:buffer");
const readerPath = path.join(__dirname, "../tools/search_sample_viewer/report_reader.js");
const reader = require(readerPath);
let checks = 0;
async function test(name, body) {
    await body(); ++checks; console.log("ok report-reader " + checks + " - " + name);
}
function boundedFile(text, limit) {
    const blob = text instanceof Blob ? text : new Blob([text]);
    const slices = [];
    return {
        size: blob.size, slices,
        text() { throw Error("whole-file text() is forbidden"); },
        arrayBuffer() { throw Error("whole-file arrayBuffer() is forbidden"); },
        slice(start, end) {
            assert.ok(end - start <= limit, "reader requested an oversized slice");
            slices.push([start, end]);
            return blob.slice(start, end);
        }
    };
}
async function collect(text, chunkBytes = 13) {
    const nodes = [], file = boundedFile(text, chunkBytes);
    const metadata = await reader.read(file, {chunkBytes, onNode: node => nodes.push(node)});
    return {nodes, metadata, file};
}
async function invalid(text, pattern = /报告 JSON/) {
    await assert.rejects(collect(text, 3), pattern);
}

async function main() {
    await test("CommonJS and browser exports need no DOM", async () => {
        const context = {TextDecoder};
        vm.runInNewContext(fs.readFileSync(readerPath, "utf8"), context, {filename: readerPath});
        assert.equal(typeof context.XqSampleReportReader.read, "function");
        assert.equal(typeof reader.read, "function");
    });
    await test("all byte boundaries preserve Chinese, escaped strings and nested metadata", async () => {
        const source = {title: "中文棋谱 😀", escaped: 'quote " slash \\ newline\n', nodes: [
            {id: 0, parent: null, move: "根局面", note: '}] , "nodes": [ \\"', values: [true, false, null, -1.2e3]},
            {id: 1, parent: 0, move: "a0a9", child: {nested: [1, {name: "马进二"}]}}
        ], timeline: {chunks: [{file: "深度.json"}], text: "嵌套", nodes: ["ordinary metadata"]}};
        const text = "\n\t" + JSON.stringify(source, null, 2) + "\r\n";
        for (const chunkBytes of [1, 2, 3, 7, 16, 31, 128, 1024]) {
            const result = await collect(text, chunkBytes);
            assert.deepEqual(result.nodes, source.nodes);
            const {nodes, ...metadata} = source;
            assert.deepEqual(result.metadata, metadata);
            assert.equal(Object.hasOwn(result.metadata, "nodes"), false);
        }
    });
    await test("nodes may appear first or last and empty arrays are valid", async () => {
        assert.deepEqual((await collect('{"nodes":[],"x":1}')).metadata, {x: 1});
        assert.deepEqual((await collect('{"x":1,"nodes":[0,"text",null,true,[],{}]}')).nodes,
            [0, "text", null, true, [], {}]);
        assert.deepEqual((await collect('\ufeff{"nodes":[]}')).metadata, {});
        const result = await collect('{"__proto__":{"safe":true},"nodes":[]}');
        assert.equal(Object.getPrototypeOf(result.metadata), Object.prototype);
        assert.deepEqual(Object.getOwnPropertyDescriptor(result.metadata, "__proto__").value, {safe: true});
    });
    await test("invalid syntax, duplicates, truncation and trailing junk are rejected", async () => {
        for (const text of ["", " ", "[]", "null", "{}", '{"nodes":{}}', '{"nodes":null}',
            '{"nodes":[],"nodes":[]}', '{"nodes":[],"no\\u0064es":[]}',
            '{"nodes":[1,]}', '{"nodes":[],}', '{"nodes":[{"x":1,}]}',
            '{"nodes":[truefalse]}', '{"nodes":[01]}', '{"nodes":["\\q"]}',
            '{"nodes":["raw\nnewline"]}', '{"nodes":[{]}', '{"nodes":[1 2]}',
            '{"nodes":[]}junk', '{"nodes":[]}{}', '{"nodes":[] "x":1}',
            '{"nodes" []}', '{"nodes":', '{"nodes":[', '{"nodes":[{"x":',
            '{"nodes":["escaped\\', '{"nodes":[],"tail":"unfinished', '{"nodes":[]']) {
            await invalid(text);
        }
        await invalid('{"nodes":[],"nodes":[]}', /重复的 nodes/);
        await invalid('{"nodes":[]', /结尾处不完整/);
        await invalid('{"nodes":[]}junk', /多余内容/);
    });
    await test("UTF-8 errors and incomplete slice reads have useful errors", async () => {
        await invalid(new Blob([new Uint8Array([123, 34, 120, 34, 58, 34, 0xff, 34, 125])]), /UTF-8/);
        await invalid(new Blob(['{"nodes":[],"x":"', new Uint8Array([0xe4, 0xb8])]), /UTF-8.*结尾处不完整/);
        await assert.rejects(reader.read({size: 10, slice: () => new Blob(["short"])}), /读取内容不完整/);
    });
    await test("progress is awaited at chunk boundaries and byte counts reach the file size", async () => {
        const file = boundedFile('{"nodes":[{"id":0},{"id":1}],"text":"棋"}', 7);
        const progress = [], nodes = []; let pending = false;
        const metadata = await reader.read(file, {chunkBytes: 7,
            onNode(node) { assert.equal(pending, false); nodes.push(node); },
            async onProgress(bytes, total) {
                assert.equal(pending, false); pending = true;
                progress.push([bytes, total]); await Promise.resolve(); pending = false;
            }});
        assert.deepEqual(metadata, {text: "棋"}); assert.equal(nodes.length, 2);
        assert.deepEqual(progress.at(-1), [file.size, file.size]);
        assert.equal(progress.length, file.slices.length);
        progress.forEach(([bytes, total], index) => {
            assert.equal(bytes, Math.min((index + 1) * 7, file.size)); assert.equal(total, file.size);
        });
    });
    await test("cancellation aborts before reads, after progress and during node callbacks", async () => {
        const source = '{"nodes":[' + Array.from({length: 100}, (_, id) => JSON.stringify({id})).join(",") + "]}";
        const before = boundedFile(source, 64);
        await assert.rejects(reader.read(before, {cancelled: () => true}), {name: "AbortError"});
        assert.equal(before.slices.length, 0);
        const after = boundedFile(source, 64); let cancelled = false;
        await assert.rejects(reader.read(after, {chunkBytes: 64, cancelled: () => cancelled,
            onProgress() { cancelled = true; }}), {name: "AbortError"});
        assert.equal(after.slices.length, 1);
        const during = boundedFile(source, 4096); let count = 0;
        await assert.rejects(reader.read(during, {chunkBytes: 4096, cancelled: () => count === 3,
            onNode() { ++count; }}), {name: "AbortError"});
        assert.equal(count, 3); assert.equal(during.slices.length, 1);
    });
    await test("callback validation errors propagate without being relabeled as JSON errors", async () => {
        const expected = Error("builder rejected invalid parent");
        await assert.rejects(reader.read(new Blob(['{"nodes":[{}]}']), {onNode() { throw expected; }}),
            error => error === expected);
        await assert.rejects(reader.read(new Blob(['{"nodes":[]}']), {onProgress() { throw expected; }}),
            error => error === expected);
    });
    await test("a large node array uses bounded slices without retaining a raw nodes array", async () => {
        const count = 40000, rows = Array.from({length: count}, (_, id) => JSON.stringify({
            id, parent: id ? id - 1 : null, kind: "normal_move", move: "a0a9", inclusive: 1, self: 1
        }));
        const file = boundedFile('{"format_version":1,"nodes":[' + rows.join(",") + '],"finished":true}', 16384);
        let next = 0, progressCalls = 0;
        const metadata = await reader.read(file, {chunkBytes: 16384,
            onNode(node) { assert.equal(node.id, next++); }, onProgress() { ++progressCalls; }});
        assert.equal(next, count); assert.ok(progressCalls > 100);
        assert.deepEqual(metadata, {format_version: 1, finished: true});
        assert.equal(Object.hasOwn(metadata, "nodes"), false);
        const defaultFile = boundedFile('{"nodes":[],"padding":"' + "x".repeat(2 * 1024 * 1024) + '"}', 1024 * 1024);
        assert.equal((await reader.read(defaultFile)).padding.length, 2 * 1024 * 1024);
        assert.equal(defaultFile.slices.length, 3);
    });
    console.log("Passed " + checks + " incremental report-reader checks.");
}
main().catch(error => { console.error(error); process.exitCode = 1; });
