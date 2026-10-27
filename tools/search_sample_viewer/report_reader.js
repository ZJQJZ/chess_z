/* Incremental offline report reader. Retains one JSON value, never the nodes array. */
(function (root, factory) {
    "use strict";
    var api = factory();
    if (typeof module === "object" && module.exports) module.exports = api;
    else root.XqSampleReportReader = api;
}(typeof globalThis !== "undefined" ? globalThis : this, function () {
    "use strict";

    var DEFAULT_CHUNK_BYTES = 1024 * 1024;
    var MAX_VALUE_CHARS = 16 * 1024 * 1024;
    function fail(message) { throw new Error("报告 JSON " + message); }
    function whitespace(code) { return code === 32 || code === 9 || code === 10 || code === 13; }
    function abort() { var error = new Error("已取消读取报告。"); error.name = "AbortError"; throw error; }

    function Scanner(options) {
        this.options = options;
        this.metadata = {};
        this.state = "root";
        this.key = null;
        this.nodesSeen = false;
        this.token = null;
    }
    Scanner.prototype.checkCancelled = function () {
        if (this.options.cancelled && this.options.cancelled()) abort();
    };
    Scanner.prototype.begin = function (kind, first) {
        this.token = {kind: kind, parts: [], length: 0, string: first === '"', escape: false,
            stack: first === "{" || first === "[" ? [first] : []};
    };
    Scanner.prototype.append = function (text) {
        this.token.length += text.length;
        if (this.token.length > MAX_VALUE_CHARS) fail("中的单个字段或节点过大（上限 16 MiB 字符）。");
        this.token.parts.push(text);
    };
    Scanner.prototype.complete = function (text) {
        this.append(text);
        var token = this.token, value;
        try { value = JSON.parse(token.parts.length === 1 ? token.parts[0] : token.parts.join("")); }
        catch (_) { fail("包含无效的" + (token.kind === "node" ? "节点" : token.kind === "key" ? "字段名" : "字段值") + "。"); }
        this.token = null;
        if (token.kind === "key") {
            this.key = value;
            if (value === "nodes") {
                if (this.nodesSeen) fail("包含重复的 nodes 字段。");
                this.nodesSeen = true;
            }
            this.state = "colon";
        } else if (token.kind === "node") {
            this.checkCancelled();
            if (this.options.onNode) this.options.onNode(value);
            this.checkCancelled();
            this.state = "node-after";
        } else {
            // Define rather than assign so a report's __proto__ remains ordinary data.
            Object.defineProperty(this.metadata, this.key, {value: value, enumerable: true,
                configurable: true, writable: true});
            this.state = "after";
        }
    };
    Scanner.prototype.consume = function (text) {
        var i = 0, segment = 0;
        while (i < text.length) {
            var ch = text[i], code = text.charCodeAt(i), token = this.token;
            if (token) {
                if (token.string) {
                    if (token.escape) token.escape = false;
                    else if (ch === "\\") token.escape = true;
                    else if (ch === '"') {
                        token.string = false;
                        if (!token.stack.length) { this.complete(text.slice(segment, i + 1)); segment = ++i; continue; }
                    }
                } else if (ch === '"') token.string = true;
                else if (ch === "{" || ch === "[") token.stack.push(ch);
                else if (ch === "}" || ch === "]") {
                    if (!token.stack.length) { this.complete(text.slice(segment, i)); segment = i; continue; }
                    var opener = token.stack.pop();
                    if (opener !== (ch === "}" ? "{" : "[")) fail("的括号不匹配。");
                    if (!token.stack.length) { this.complete(text.slice(segment, i + 1)); segment = ++i; continue; }
                } else if (!token.stack.length && (ch === "," || whitespace(code))) {
                    this.complete(text.slice(segment, i)); segment = i; continue;
                }
                ++i;
                continue;
            }
            if (whitespace(code)) { ++i; continue; }
            if (this.state === "root") {
                if (ch !== "{") fail("顶层必须是对象。");
                this.state = "key-first";
            } else if (this.state === "key-first" || this.state === "key") {
                if (ch === "}" && this.state === "key-first") this.state = "done";
                else {
                    if (ch !== '"') fail("字段名必须是字符串，且不能有多余逗号。");
                    this.begin("key", ch); segment = i;
                }
            } else if (this.state === "colon") {
                if (ch !== ":") fail("字段名后缺少冒号。");
                this.state = "value";
            } else if (this.state === "value" && this.key === "nodes") {
                if (ch !== "[") fail("的 nodes 字段必须是数组。");
                this.state = "node-first";
            } else if (this.state === "value" || this.state === "node-first" || this.state === "node") {
                if (ch === "]" && this.state === "node-first") this.state = "after";
                else {
                    if (ch === "," || ch === "]" || ch === "}" || ch === ":") fail("缺少字段值或节点，或包含多余逗号。");
                    this.begin(this.state === "value" ? "value" : "node", ch); segment = i;
                }
            } else if (this.state === "node-after") {
                if (ch === ",") this.state = "node";
                else if (ch === "]") this.state = "after";
                else fail("节点之间缺少逗号或数组结束符。");
            } else if (this.state === "after") {
                if (ch === ",") this.state = "key";
                else if (ch === "}") this.state = "done";
                else fail("字段之间缺少逗号或对象结束符。");
            } else fail("结束后仍有多余内容。");
            ++i;
        }
        if (this.token) this.append(text.slice(segment));
    };
    Scanner.prototype.finish = function () {
        this.checkCancelled();
        if (this.token || this.state !== "done") fail("在结尾处不完整，文件可能已被截断。");
        if (!this.nodesSeen) fail("缺少 nodes 数组。");
        return this.metadata;
    };

    async function read(file, options) {
        options = options || {};
        if (!file || !Number.isSafeInteger(file.size) || file.size < 0 || typeof file.slice !== "function")
            fail("文件无法读取。");
        var chunkBytes = options.chunkBytes === undefined ? DEFAULT_CHUNK_BYTES : options.chunkBytes;
        if (!Number.isSafeInteger(chunkBytes) || chunkBytes <= 0 || chunkBytes > 4 * DEFAULT_CHUNK_BYTES)
            fail("读取块大小必须是 1 至 4194304 字节的整数。");
        if (typeof TextDecoder !== "function") fail("读取需要浏览器支持 TextDecoder。");
        var scanner = new Scanner(options), decoder = new TextDecoder("utf-8", {fatal: true});
        scanner.checkCancelled();
        for (var offset = 0; offset < file.size;) {
            scanner.checkCancelled();
            var next = Math.min(file.size, offset + chunkBytes), part = file.slice(offset, next);
            if (!part || typeof part.arrayBuffer !== "function") fail("文件分块无法读取。");
            var bytes = await part.arrayBuffer();
            scanner.checkCancelled();
            if (bytes.byteLength !== next - offset) fail("读取内容不完整，请重新选择文件。");
            var text;
            try { text = decoder.decode(bytes, {stream: true}); }
            catch (_) { fail("不是有效的 UTF-8 文本。"); }
            scanner.consume(text);
            offset = next;
            if (options.onProgress) await options.onProgress(offset, file.size);
            scanner.checkCancelled();
        }
        var tail;
        try { tail = decoder.decode(); }
        catch (_) { fail("的 UTF-8 字符在结尾处不完整，文件可能已被截断。"); }
        scanner.consume(tail);
        return scanner.finish();
    }

    return {read: read};
}));
