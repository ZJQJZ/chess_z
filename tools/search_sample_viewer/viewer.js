/* Browser-only UI. All report data stays in memory; no server or network access. */
(function () {
  "use strict";
  const M = window.XqSampleModel;
  const $ = id => document.getElementById(id);
  if (!M) {
    $("notice").hidden = false;
    $("notice").className = "error";
    $("notice").textContent = "无法加载 model.js。请保留查看器目录中的全部文件，再打开 index.html。";
    return;
  }
  const state = { groups: [], group: null, iteration: null, selected: null, zoom: null,
    replay: null, flipped: false, importEpoch: 0, depthEpoch: 0 };
  const nf = new Intl.NumberFormat("zh-CN");
  const count = n => nf.format(n || 0);
  const percent = (n, total) => total ? (100 * n / total).toFixed(1) + "%" : "—";
  const label = node => node.kind === "root" ? "根局面" : node.kind === "quiescence" ? "[静态搜索]" : node.move;
  const phase = node => node.kind === "quiescence" || node.kind === "quiescence_move";
  const svgNS = "http://www.w3.org/2000/svg";
  const pieceNames = { k: "将", a: "士", b: "象", e: "象", n: "馬", h: "馬", r: "車", c: "炮", p: "卒",
    K: "帥", A: "仕", B: "相", E: "相", N: "馬", H: "馬", R: "車", C: "炮", P: "兵" };

  function notice(message, error) {
    $("notice").textContent = message || "";
    $("notice").className = error ? "error" : "";
    $("notice").hidden = !message;
  }
  function busy(message) { $("load-status").textContent = message || ""; }
  function element(tag, text, className) {
    const el = document.createElement(tag);
    if (text !== undefined) el.textContent = text;
    if (className) el.className = className;
    return el;
  }
  function option(select, value, text) {
    const el = element("option", text); el.value = value; select.append(el);
  }
  function tick() { return new Promise(resolve => requestAnimationFrame(resolve)); }
  function latestDepth(group) {
    let depth = 0;
    for (const item of group.depths.keys()) depth = Math.max(depth, item);
    return depth;
  }

  async function readIteration(group, depth) {
    const source = group.depths.get(depth);
    const raw = source.raw || JSON.parse(await source.file.text());
    const iteration = M.parseIteration(raw, group.session);
    if (iteration.depth !== depth) throw new Error("文件名中的深度与报告内容不符：" + source.name);
    return iteration;
  }
  function beginImport() {
    const epoch = ++state.importEpoch; ++state.depthEpoch;
    busy("正在读取报告…");
    notice("");
    return epoch;
  }
  async function importFiles(files, epoch) {
    if (epoch === undefined) epoch = beginImport();
    if (epoch !== state.importEpoch) return;
    try {
      const grouped = new Map();
      for (const entry of files) {
        const path = entry.path.replace(/\\/g, "/");
        const name = path.split("/").pop();
        const match = /^depth-([1-9]\d*)\.json$/.exec(name);
        if (name !== "session.json" && !match) continue;
        const key = path.slice(0, -name.length).replace(/\/$/, "") || "所选报告";
        if (!grouped.has(key)) grouped.set(key, { key, depths: new Map() });
        const group = grouped.get(key);
        if (name === "session.json") {
          if (group.sessionFile) throw new Error("发现多个同名 session.json，请选择一个完整报告目录。");
          group.sessionFile = entry.file;
        } else {
          const depth = Number(match[1]);
          if (!Number.isSafeInteger(depth)) throw new Error("报告文件名中的深度无效。");
          if (group.depths.has(depth)) throw new Error("发现重复的 " + name + "，请使用目录选择以区分不同思考。");
          group.depths.set(depth, { file: entry.file, name });
        }
      }
      const groups = [], skipped = [];
      for (const group of grouped.values()) {
        if (!group.sessionFile || !group.depths.size) { skipped.push(group.key); continue; }
        try { group.session = M.parseSession(await group.sessionFile.text()); }
        catch (error) { skipped.push(group.key + "：" + error.message); continue; }
        groups.push(group);
      }
      if (!groups.length) throw new Error("没有找到完整报告。请同时选择 session.json 和至少一个 depth-N.json，或选择包含这些文件的报告目录。" + (skipped.length ? " " + skipped.join("；") : ""));
      groups.sort((a, b) => a.key.localeCompare(b.key));
      let group = null, iteration = null;
      for (let i = groups.length - 1; i >= 0 && !iteration; --i) {
        const candidate = groups[i];
        for (const depth of [...candidate.depths.keys()].sort((a, b) => b - a)) {
          await tick();
          if (epoch !== state.importEpoch) return;
          try { iteration = await readIteration(candidate, depth); group = candidate; break; }
          catch (error) { skipped.push(candidate.key + "/depth-" + depth + ".json：" + error.message); }
        }
      }
      if (epoch !== state.importEpoch) return;
      if (!iteration) throw new Error("没有可读取的迭代报告。" + skipped.join("；"));
      state.groups = groups;
      commitIteration(group, iteration);
      if (skipped.length) notice("已跳过不完整或无法读取的报告：" + skipped.join("；"));
    } catch (error) {
      if (epoch === state.importEpoch) notice(error.message, true);
    } finally { if (epoch === state.importEpoch) busy(""); }
  }
  async function changeIteration(group, depth) {
    ++state.importEpoch;
    const epoch = ++state.depthEpoch;
    busy("正在加载深度 " + depth + "…"); notice("");
    try {
      await tick();
      const iteration = await readIteration(group, depth);
      if (epoch !== state.depthEpoch) return;
      commitIteration(group, iteration);
    } catch (error) {
      if (epoch !== state.depthEpoch) return;
      notice(error.message, true);
      if (state.group) $("session-select").value = state.group.key;
      if (state.iteration) $("depth-select").value = state.iteration.depth;
    } finally { if (epoch === state.depthEpoch) busy(""); }
  }
  function commitIteration(group, iteration) {
    state.group = group; state.iteration = iteration;
    state.selected = iteration.root.id; state.zoom = iteration.root.id;
    $("empty").hidden = true; $("loaded").hidden = false;
    $("session-select").replaceChildren();
    for (const item of state.groups) option($("session-select"), item.key, item.key);
    $("session-select").value = group.key;
    $("session-picker").hidden = state.groups.length < 2;
    $("session-name").textContent = group.key;
    $("session-name").title = group.key;
    $("depth-select").replaceChildren();
    for (const depth of [...group.depths.keys()].sort((a, b) => a - b)) option($("depth-select"), depth, depth);
    $("depth-select").value = iteration.depth;
    $("iteration-status").textContent = iteration.finished ? "迭代报告已收尾" : "进行中的检查点";
    $("iteration-status").title = "报告收尾不保证所有根走法均已搜索完成。";
    $("sample-integrity").textContent = iteration.incomplete ? "统计存在缺失" : "未报告统计缺失";
    $("sample-integrity").className = "badge" + (iteration.incomplete ? " warn" : "");
    $("total-samples").textContent = count(iteration.validSamples);
    $("sample-threads").textContent = iteration.thread_statistics_available ?
      count(iteration.threads.filter(thread => thread.valid_samples > 0).length) + " / " + group.session.thread_capacity : "未记录";
    $("normal-depth").textContent = count(iteration.max_normal_ply);
    $("q-depth").textContent = count(iteration.max_quiescence_ply);
    $("sample-interval").replaceChildren(document.createTextNode(count(group.session.sample_interval_ms)), element("small", "ms"));
    const details = ["搜索模式：" + group.session.search_mode, "搜索 ID：" + group.session.search_id,
      "路径节点：" + count(iteration.nodes.length), "路径截断样本：" + count(iteration.truncated_samples),
      "容量未收录样本：" + count(iteration.unrecorded_path_samples)];
    const counters = iteration.session_counters || {};
    details.push("本次思考累计槽覆盖：" + count(counters.slot_overwritten), "累计丢弃：" + count(counters.dropped),
      "累计延迟响应：" + count(counters.delayed), "累计重复：" + count(counters.duplicates),
      "累计边界丢弃：" + count(counters.boundaries_dropped), "累计分配丢弃：" + count(counters.allocation_dropped));
    $("report-detail").replaceChildren(...details.map(text => element("span", text)));
    $("move-filter").value = "";
    $("chart-shell").scrollTop = 0;
    selectNode(iteration.root.id);
    renderChart();
  }

  function selectNode(id) {
    const iteration = state.iteration, node = iteration.byId.get(id);
    if (!node) return;
    state.selected = id;
    $("selected-move").textContent = label(node);
    $("node-phase").textContent = node.kind === "root" ? "初始局面" : phase(node) ? "静态搜索" : "普通搜索";
    $("node-phase").className = "badge" + (phase(node) ? " q" : "");
    $("inclusive").textContent = count(node.inclusive);
    $("self").textContent = count(node.self);
    const parent = iteration.byId.get(node.parent);
    $("node-ratio").textContent = "占本轮样本 " + percent(node.inclusive, iteration.validSamples) +
      (parent ? " · 占父分支 " + percent(node.inclusive, parent.inclusive) : "");
    $("node-id").textContent = "节点 #" + node.id;
    $("parent").disabled = !parent;
    const warning = [];
    if (node.truncated_self) warning.push(count(node.truncated_self) + " 个自身样本的路径被截断");
    if (node.unrecorded_self) warning.push(count(node.unrecorded_self) + " 个自身样本因统计容量不足，只保留此前缀");
    $("node-warning").textContent = warning.length ? warning.join("；") + "。棋盘仅对应已记录前缀，实际采样节点可能更深。" : "";
    $("node-warning").hidden = !warning.length;
    $("move-filter").value = "";
    try {
      state.replay = M.replay(state.group.session, iteration, id);
      $("replay-error").hidden = true;
      $("board-title").textContent = node.kind === "root" ? "根局面" : node.kind === "quiescence" ? "进入静态搜索时的棋盘" : "所选着法后的棋盘";
      $("fen").value = state.replay.fen;
      $("copy-fen").disabled = false; $("copy-path").disabled = false;
      renderBoard();
    } catch (error) {
      state.replay = null;
      $("board").replaceChildren(); $("board").setAttribute("aria-label", "无法还原所选节点的棋盘");
      $("side").replaceChildren(); $("fen").value = "";
      $("replay-error").textContent = error.message;
      $("replay-error").hidden = false;
      $("copy-fen").disabled = true; $("copy-path").disabled = true;
    }
    renderPath(); renderBranches(); updateChartSelection();
  }
  function updateChartSelection() {
    for (const frame of $("chart").querySelectorAll(".chart-frame.selected")) frame.classList.remove("selected");
    const selected = $("chart").querySelector('[data-node-id="' + state.selected + '"]');
    if (selected) selected.classList.add("selected");
    $("zoom-selected").disabled = state.selected === state.zoom;
  }
  function renderBoard() {
    const replay = state.replay;
    if (!replay) return;
    const board = document.createDocumentFragment();
    const ranks = Array.from({ length: 10 }, (_, i) => state.flipped ? i : 9 - i);
    const files = Array.from({ length: 9 }, (_, i) => state.flipped ? 8 - i : i);
    for (const rank of ranks) {
      board.append(element("span", rank, "coordinate"));
      for (const file of files) {
        const sq = rank * 9 + file, piece = replay.board[sq];
        let cls = "square " + (!piece ? "is-empty" : piece === piece.toUpperCase() ? "red" : "black");
        if (replay.lastMove && replay.lastMove.from === sq) cls += " from";
        if (replay.lastMove && replay.lastMove.to === sq) cls += " to";
        const cell = element("span", piece ? pieceNames[piece] : "·", cls);
        cell.title = String.fromCharCode(97 + file) + rank + (piece ? " " + pieceNames[piece] : " 空");
        board.append(cell);
      }
    }
    board.append(element("span", ""));
    for (const file of files) board.append(element("span", String.fromCharCode(97 + file), "coordinate"));
    $("board").replaceChildren(board);
    $("board").setAttribute("aria-label", "所选局面：" + replay.fen);
    const side = element("span");
    side.append(element("span", "", "side-dot" + (replay.side === "r" ? " red" : "")),
      document.createTextNode(replay.side === "r" ? "红方行棋" : "黑方行棋"));
    $("side").replaceChildren(side, element("span", "普通 " + replay.normalPly + " · 静态 " + replay.quiescencePly, "muted"));
  }
  function renderPath() {
    const path = M.pathTo(state.iteration, state.selected);
    const fragment = document.createDocumentFragment();
    let move = 0;
    for (const node of path) {
      if (node.move) ++move;
      const button = element("button", node.move ? move + ". " + node.move : label(node));
      button.type = "button";
      button.className = (node.id === state.selected ? "current " : "") + (node.kind === "quiescence" ? "phase" : "");
      button.title = "节点 #" + node.id;
      button.addEventListener("click", () => selectNode(node.id));
      fragment.append(button);
    }
    $("path").replaceChildren(fragment);
    $("path").scrollTop = $("path").scrollHeight;
    $("path-title").textContent = "根到所选节点 · " + move + " 步";
  }
  function renderBranches() {
    const node = state.iteration.byId.get(state.selected), filter = $("move-filter").value.trim().toLowerCase();
    const children = state.iteration.children.get(node.id) || [];
    const fragment = document.createDocumentFragment();
    $("branches-title").textContent = label(node) + " 的后续分支（" + children.length + "）";
    let shown = 0;
    for (const child of children) {
      if (filter && !label(child).toLowerCase().includes(filter)) continue;
      ++shown;
      const row = element("tr");
      const name = element("td"), button = element("button", label(child), "node-link");
      button.type = "button";
      button.addEventListener("click", () => selectNode(child.id));
      name.append(button);
      if (phase(child)) name.append(document.createTextNode(" "), element("span", "静态", "badge q"));
      const inclusive = element("td", count(child.inclusive));
      const track = element("span", undefined, "branch-track"), bar = element("span");
      bar.style.width = (node.inclusive ? 100 * child.inclusive / node.inclusive : 0) + "%";
      track.append(bar); inclusive.append(track);
      row.append(name, inclusive, element("td", percent(child.inclusive, node.inclusive)), element("td", count(child.self)));
      row.addEventListener("click", event => { if (!event.target.closest("button")) selectNode(child.id); });
      fragment.append(row);
    }
    if (!shown) {
      const row = element("tr"), cell = element("td", children.length ? "没有匹配的后续着法。" : "此处没有已收录的后续分支。", "muted");
      cell.colSpan = 4; cell.style.textAlign = "center"; row.append(cell); fragment.append(row);
    }
    $("branches").replaceChildren(fragment);
  }
  function renderChart() {
    const iteration = state.iteration;
    if (!iteration) return;
    const root = iteration.byId.get(state.zoom), chart = $("chart");
    const width = Math.max(240, $("chart-shell").clientWidth), rowHeight = 27;
    chart.replaceChildren();
    $("zoom-parent").disabled = root.parent === null;
    $("zoom-selected").disabled = state.selected === state.zoom;
    $("zoom-root").disabled = root.id === iteration.root.id;
    $("chart-info").textContent = "范围：" + label(root) + " · " + count(root.inclusive) + " 样本";
    $("chart-empty").hidden = iteration.validSamples !== 0;
    $("chart-empty").textContent = "本轮尚无有效样本，仍可查看根局面。较快完成的迭代可能未赶上采样请求。";
    if (!iteration.validSamples) { chart.style.display = "none"; return; }
    chart.style.display = "block";
    const pending = [{ node: root, x: 0, width, level: 0 }];
    const fragment = document.createDocumentFragment();
    let bottom = 1, drawn = 0, limited = false;
    while (pending.length) {
      const item = pending.pop(), node = item.node;
      if (item.width < 1.5) continue;
      if (++drawn > 16000) { limited = true; break; }
      bottom = Math.max(bottom, item.level + 1);
      const g = document.createElementNS(svgNS, "g");
      g.setAttribute("data-node-id", node.id);
      g.setAttribute("class", "chart-frame" + (phase(node) ? " q" : "") + (node.id === state.selected ? " selected" : ""));
      g.setAttribute("role", "button");
      g.setAttribute("tabindex", item.width >= 28 ? "0" : "-1");
      g.setAttribute("aria-label", label(node) + "，前缀 " + node.inclusive + " 个样本，节点 " + node.id);
      const rect = document.createElementNS(svgNS, "rect");
      rect.setAttribute("x", item.x); rect.setAttribute("y", item.level * rowHeight);
      rect.setAttribute("width", item.width); rect.setAttribute("height", rowHeight - 1); rect.setAttribute("rx", 2);
      rect.setAttribute("fill", phase(node) ? ["#f4dba9", "#f6e5c5", "#eed09a"][item.level % 3] :
        node.kind === "root" ? "#dce5ef" : ["#b8ceed", "#cbdcf3", "#d7e4f6", "#bfd6f4"][item.level % 4]);
      const title = document.createElementNS(svgNS, "title");
      title.textContent = label(node) + " · 前缀 " + count(node.inclusive) + "（" + percent(node.inclusive, iteration.validSamples) + "）· 自身 " + count(node.self) + " · 节点 #" + node.id;
      g.append(rect, title);
      if (item.width > 35) {
        const text = document.createElementNS(svgNS, "text");
        text.setAttribute("x", item.x + 5); text.setAttribute("y", item.level * rowHeight + 17);
        const full = node.kind === "root" ? "depth-" + iteration.depth + " · 根局面" : label(node);
        const length = Math.max(1, Math.floor((item.width - 10) / (node.kind === "quiescence" ? 10 : 7)));
        text.textContent = full.length > length ? full.slice(0, Math.max(0, length - 1)) + "…" : full;
        g.append(text);
      }
      g.addEventListener("click", () => selectNode(node.id));
      g.addEventListener("dblclick", () => zoomTo(node.id));
      g.addEventListener("keydown", event => {
        if (event.key === "Enter" || event.key === " ") { event.preventDefault(); selectNode(node.id); }
      });
      fragment.append(g);
      let x = item.x;
      const children = iteration.children.get(node.id) || [], next = [];
      for (const child of children) {
        const childWidth = node.inclusive ? item.width * child.inclusive / node.inclusive : 0;
        if (childWidth >= 1.5) next.push({ node: child, x, width: childWidth, level: item.level + 1 });
        x += childWidth;
      }
      for (let i = next.length - 1; i >= 0; --i) pending.push(next[i]);
    }
    chart.setAttribute("viewBox", "0 0 " + width + " " + bottom * rowHeight);
    chart.setAttribute("height", bottom * rowHeight);
    chart.append(fragment);
    $("chart-caption").textContent = limited ? "图形已达到显示数量上限。可放大分支或使用下方列表查看；统计数据没有删减。" :
      "宽度表示前缀累计样本，右侧留白为节点自身样本；单击查看棋盘，双击放大。窄分支可从下方列表选择。";
  }
  function zoomTo(id) { state.zoom = id; $("chart-shell").scrollTop = 0; renderChart(); }

  async function copyText(text, button) {
    try {
      if (navigator.clipboard && window.isSecureContext) await navigator.clipboard.writeText(text);
      else {
        const area = element("textarea"); area.value = text;
        area.style.position = "fixed"; area.style.opacity = "0";
        document.body.append(area); area.select();
        const okay = document.execCommand("copy"); area.remove();
        if (!okay) throw new Error("请展开“局面 FEN 与路径”，手动选择复制。");
      }
      const old = button.textContent; button.textContent = "已复制";
      setTimeout(() => { button.textContent = old; }, 1400);
    } catch (error) { notice("复制未完成：" + error.message, true); }
  }
  function demo() {
    ++state.importEpoch; ++state.depthEpoch; busy(""); notice("这是内置示例。选择你的报告文件即可查看实际搜索。");
    const session = M.parseSession({ format_version: 1, search_id: 1,
      initial_fen: "rnbakabnr/9/1c5c1/p1p1p1p1p/9/9/P1P1P1P1P/1C5C1/9/RNBAKABNR r - - 0 1",
      search_mode: "parallel", thread_capacity: 2, sample_interval_ms: 20, finished: true });
    const node = (id, parent, kind, move, inclusive, self) => ({ id, parent, kind, move, inclusive, self, truncated_self: 0, unrecorded_self: 0 });
    const raw = { format_version: 1, search_id: 1, iteration_depth: 3, finished: true, incomplete: false,
      valid_samples: 100, truncated_samples: 0, unrecorded_path_samples: 0,
      max_normal_ply: 2, max_quiescence_ply: 1, thread_statistics_available: true,
      threads: [{ thread_id: 0, valid_samples: 55 }, { thread_id: 1, valid_samples: 45 }],
      nodes: [node(0, null, "root", null, 100, 0), node(1, 0, "normal_move", "b2b9", 65, 5),
        node(2, 1, "normal_move", "a9b9", 60, 10), node(3, 2, "quiescence", null, 50, 15),
        node(4, 3, "quiescence_move", "h2h9", 35, 35), node(5, 0, "normal_move", "h2e2", 35, 35)] };
    const group = { key: "内置示例 · 炮吃馬后的搜索", session, depths: new Map([[3, { raw, name: "depth-3.json" }]]) };
    state.groups = [group]; commitIteration(group, M.parseIteration(raw, session));
  }
  async function filesFromDrop(dataTransfer) {
    const entries = Array.from(dataTransfer.items || []).map(item => item.webkitGetAsEntry ? item.webkitGetAsEntry() : null).filter(Boolean);
    if (!entries.length) return Array.from(dataTransfer.files).map(file => ({ file, path: file.name }));
    const output = [];
    async function walk(entry, prefix) {
      const path = prefix + entry.name;
      if (entry.isFile) {
        if (entry.name !== "session.json" && !/^depth-[1-9]\d*\.json$/.test(entry.name)) return;
        const file = await new Promise((resolve, reject) => entry.file(resolve, reject));
        output.push({ file, path });
      } else if (entry.isDirectory) {
        const reader = entry.createReader();
        for (;;) {
          const batch = await new Promise((resolve, reject) => reader.readEntries(resolve, reject));
          if (!batch.length) break;
          for (const child of batch) await walk(child, path + "/");
        }
      }
    }
    for (const entry of entries) await walk(entry, "");
    return output;
  }
  for (const id of ["files", "directory"]) $(id).addEventListener("change", async event => {
    const files = Array.from(event.target.files).map(file => ({ file, path: file.webkitRelativePath || file.name }));
    event.target.value = "";
    if (files.length) await importFiles(files);
  });
  $("session-select").addEventListener("change", event => {
    const group = state.groups.find(item => item.key === event.target.value);
    if (group) changeIteration(group, latestDepth(group));
  });
  $("depth-select").addEventListener("change", event => changeIteration(state.group, Number(event.target.value)));
  $("parent").addEventListener("click", () => {
    const node = state.iteration.byId.get(state.selected);
    if (node.parent !== null) selectNode(node.parent);
  });
  $("flip").addEventListener("click", () => {
    state.flipped = !state.flipped; $("flip").setAttribute("aria-pressed", String(state.flipped)); renderBoard();
  });
  $("zoom-root").addEventListener("click", () => zoomTo(state.iteration.root.id));
  $("zoom-parent").addEventListener("click", () => {
    const node = state.iteration.byId.get(state.zoom); if (node.parent !== null) zoomTo(node.parent);
  });
  $("zoom-selected").addEventListener("click", () => zoomTo(state.selected));
  $("move-filter").addEventListener("input", renderBranches);
  $("copy-fen").addEventListener("click", event => { if (state.replay) copyText(state.replay.fen, event.currentTarget); });
  $("copy-path").addEventListener("click", event => { if (state.replay) copyText(state.replay.moves.join(" "), event.currentTarget); });
  $("demo").addEventListener("click", demo);
  let dragDepth = 0;
  document.addEventListener("dragenter", event => {
    if (Array.from(event.dataTransfer.types).includes("Files")) { event.preventDefault(); ++dragDepth; document.body.classList.add("dragging"); }
  });
  document.addEventListener("dragover", event => { if (Array.from(event.dataTransfer.types).includes("Files")) event.preventDefault(); });
  document.addEventListener("dragleave", event => {
    if (Array.from(event.dataTransfer.types).includes("Files") && --dragDepth <= 0) { dragDepth = 0; document.body.classList.remove("dragging"); }
  });
  document.addEventListener("drop", async event => {
    if (!Array.from(event.dataTransfer.types).includes("Files")) return;
    event.preventDefault(); dragDepth = 0; document.body.classList.remove("dragging");
    const epoch = beginImport();
    try { await importFiles(await filesFromDrop(event.dataTransfer), epoch); }
    catch (error) { if (epoch === state.importEpoch) { busy(""); notice("无法读取拖入的报告：" + error.message, true); } }
  });
  let resizePending = false;
  if (window.ResizeObserver) new ResizeObserver(() => {
    if (!resizePending && state.iteration) {
      resizePending = true;
      requestAnimationFrame(() => { resizePending = false; renderChart(); });
    }
  }).observe($("chart-shell"));
  else window.addEventListener("resize", renderChart);
}());
