/* Logical search timeline. Files are read lazily; no network or worker access. */
(function (root, factory) {
  "use strict";
  const api = factory();
  if (typeof module === "object" && module.exports) module.exports = api;
  else root.XqSampleTimeline = api;
}(typeof globalThis !== "undefined" ? globalThis : this, function () {
  "use strict";
  const STATES = ["unknown", "search", "quiescence", "wait_helpers", "idle"];
  const NAMES = { unknown: "未知", search: "普通搜索", quiescence: "静态搜索", wait_helpers: "等待辅助线程", idle: "空闲" };
  const COLORS = { unknown: "#edf0f4", search: "#aac8ed", quiescence: "#efd09a", wait_helpers: "#9ba4b0", idle: "#e3e7ed" };
  const safe = n => Number.isSafeInteger(n) && n >= 0;
  const object = n => n && typeof n === "object" && !Array.isArray(n);
  const known = s => STATES.includes(s);
  const validFile = s => typeof s === "string" && /^depth-[1-9]\d*-timeline-\d+\.json$/.test(s);
  const clamp = (n, lo, hi) => Math.max(lo, Math.min(hi, n));
  function parse(raw) {
    if (typeof raw === "string") raw = JSON.parse(raw);
    if (!object(raw)) throw new Error("时间线必须是 JSON 对象。");
    return raw;
  }
  function manifest(raw, session, iteration) {
    if (raw.timeline_version === undefined) return null;
    if (raw.timeline_version !== 1 || !object(raw.timeline)) throw new Error("无法识别时间线版本。");
    const t = raw.timeline;
    for (const key of ["start_known", "end_known", "boundary_unknown"]) {
      if (t[key] !== undefined && typeof t[key] !== "boolean") throw new Error("时间线边界标记无效。");
    }
    if (!safe(t.start_ms) || !safe(t.end_ms) || t.end_ms < t.start_ms || !Array.isArray(t.chunks) ||
        !Array.isArray(t.initial_states)) throw new Error("时间线的范围或分块清单无效。");
    const capacity = session.thread_capacity || 1;
    if (capacity > 1024) throw new Error("时间线线程容量超过查看器上限 1024。");
    const seen = new Set(), initial = Array.from({length: capacity}, (_, thread_id) =>
      ({thread_id, state: "unknown", time_ms: t.start_ms, sequence: 0, observed: false}));
    for (const item of t.initial_states) {
      if (!object(item) || !safe(item.thread_id) || item.thread_id >= capacity || seen.has(item.thread_id) ||
          !known(item.state) || (item.time_ms !== null && !safe(item.time_ms)) || !safe(item.sequence))
        throw new Error("时间线初始线程状态无效。");
      seen.add(item.thread_id);
      initial[item.thread_id] = {...item, observed: item.time_ms !== null && item.state !== "unknown"};
    }
    const files = new Set();
    const chunks = t.chunks.map((chunk, index) => {
      if (!object(chunk) || !validFile(chunk.file) || !chunk.file.startsWith("depth-" + iteration.depth + "-") ||
          files.has(chunk.file) || !safe(chunk.start_ms) || !safe(chunk.end_ms) || chunk.end_ms < chunk.start_ms ||
          !safe(chunk.records)) throw new Error("时间线分块清单包含无效或重复的文件。");
      files.add(chunk.file);
      return {...chunk, index};
    });
    return {start: t.start_ms, end: t.end_ms, initial, chunks, capacity,
      incomplete: t.incomplete === true, finished: t.finished === true,
      cutoff: safe(t.cutoff_ms) ? clamp(t.cutoff_ms, t.start_ms, t.end_ms) : null,
      stopReason: typeof t.stop_reason === "string" ? t.stop_reason : null,
      dropped: safe(t.state_dropped) ? t.state_dropped : 0,
      startKnown: t.start_known !== false, endKnown: t.end_known !== false, boundaryUnknown: t.boundary_unknown === true};
  }
  function compare(a, b) { return a.sequence - b.sequence || a.time_ms - b.time_ms || (a.type === "sample" ? 1 : -1); }
  function copyStates(states) { return states.map(item => ({...item})); }
  function commonAncestor(iteration, first, second) {
    if (first === null || second === null) return null;
    let a = iteration.byId.get(first), b = iteration.byId.get(second);
    if (!a || !b) return null;
    // Validated trees decrease pathDepth at each parent; partial test fixtures
    // without those links must never invent a prefix or loop on a broken chain.
    const parent = node => {
      if (!safe(node.parent)) return null;
      const next = iteration.byId.get(node.parent);
      return next && safe(next.pathDepth) && next.pathDepth === node.pathDepth - 1 ? next : null;
    };
    while (a.id !== b.id) {
      if (!safe(a.pathDepth) || !safe(b.pathDepth) || !a.pathDepth || !b.pathDepth) return null;
      const aDepth = a.pathDepth, bDepth = b.pathDepth;
      if (aDepth >= bDepth) a = parent(a);
      if (bDepth >= aDepth) b = parent(b);
      if (!a || !b) return null;
    }
    return a.id;
  }
  function parseChunk(raw, info, session, iteration, capacity) {
    const chunk = parse(raw);
    if (chunk.timeline_version !== 1 || chunk.search_id !== session.search_id || chunk.iteration_depth !== iteration.depth ||
        chunk.start_ms !== info.start_ms || chunk.end_ms !== info.end_ms || !Array.isArray(chunk.records) ||
        chunk.records.length !== info.records || chunk.records.length > 500000)
      throw new Error("分块的版本、范围、迭代或记录数不匹配。");
    const records = [], seen = new Set();
    for (const rawRecord of chunk.records) {
      const r = rawRecord;
      if (!object(r) || !["sample", "state", "gap"].includes(r.type) || !safe(r.thread_id) ||
          r.thread_id >= capacity || !safe(r.sequence) || (r.time_ms !== null && !safe(r.time_ms)) ||
          (r.time_ms !== null && (r.time_ms < info.start_ms || r.time_ms > info.end_ms)))
        throw new Error("分块包含无效事件。");
      if (seen.has(r.thread_id + ":" + r.sequence)) throw new Error("分块包含重复的线程序号。");
      seen.add(r.thread_id + ":" + r.sequence);
      if (r.type === "state" && !known(r.state)) throw new Error("线程状态无效。");
      if (r.type === "gap" && !safe(r.lost_count)) throw new Error("丢失事件计数无效。");
      if (r.type === "sample" && (!safe(r.request_id) || !safe(r.normal_ply) || !safe(r.quiescence_ply) ||
          (r.node_id !== null && (!safe(r.node_id) || !iteration.byId.has(r.node_id))) ||
          (r.request_time_ms !== null && (!safe(r.request_time_ms) || r.time_ms === null || r.request_time_ms > r.time_ms)) ||
          !(r.time_ms === null ? r.state === "unknown" : ["search", "quiescence"].includes(r.state)))) throw new Error("路径样本或节点引用无效。");
      records.push({...r});
    }
    records.sort((a, b) => a.thread_id - b.thread_id || compare(a, b));
    return records;
  }
  class Data {
    constructor(meta, files, session, iteration) {
      this.meta = meta; this.files = files; this.session = session; this.iteration = iteration;
      this.cache = new Map(); this.failed = new Map(); this.before = [];
      this.afterChunks = []; this.effects = []; this.sequenceBefore = [];
      this.bins = 640; this.ready = false; this.cancelled = false;
      this.overview = Array.from({length: meta.capacity}, () => ({states: new Uint8Array(this.bins),
        counts: new Uint32Array(this.bins), depths: new Uint16Array(this.bins)}));
    }
    cancel() { this.cancelled = true; this.cache.clear(); }
    async load(index) {
      if (this.cancelled) return null;
      if (this.cache.has(index)) {
        const found = this.cache.get(index); this.cache.delete(index); this.cache.set(index, found); return found;
      }
      if (this.failed.has(index)) return null;
      const info = this.meta.chunks[index];
      try {
        const file = this.files.get(info.file);
        if (!file) throw new Error("未选择该文件");
        if (file.size > 128 * 1024 * 1024) throw new Error("单个分块超过 128MiB");
        const records = parseChunk(await file.text(), info, this.session, this.iteration, this.meta.capacity);
        if (this.cancelled) return null;
        this.cache.set(index, records);
        while (this.cache.size > 2) this.cache.delete(this.cache.keys().next().value);
        return records;
      } catch (error) {
        if (!this.cancelled) this.failed.set(index, info.file + "：" + error.message);
        return null;
      }
    }
    bin(time) { return clamp(Math.floor((time - this.meta.start) / Math.max(1, this.meta.end - this.meta.start) * this.bins), 0, this.bins - 1); }
    paint(thread, start, end, state) {
      if (end <= start) return;
      const row = this.overview[thread], bit = 1 << STATES.indexOf(state);
      const a = this.bin(start), b = this.bin(Math.max(start, end - 0.001));
      for (let i = a; i <= b; ++i) row.states[i] |= bit;
    }
    transition(states, r, fallback, intervals) {
      const prior = states[r.thread_id], time = r.time_ms === null ? prior.time_ms : r.time_ms;
      if (r.sequence <= prior.sequence) return null;
      const valid = r.time_ms !== null && time >= prior.time_ms;
      const end = clamp(Math.max(prior.time_ms, time), this.meta.start, this.meta.end);
      const state = valid && r.type === "state" ? r.state : "unknown";
      if (intervals) intervals.push({thread_id: r.thread_id, start: prior.time_ms, end,
        state: prior.state, observed: prior.observed, sequence: prior.sequence});
      else this.paint(r.thread_id, prior.time_ms, end, prior.state);
      states[r.thread_id] = {thread_id: r.thread_id, state, time_ms: end,
        sequence: r.sequence, observed: valid && r.type === "state"};
      return end;
    }
    recover(states, record, fallback, intervals) {
      if (record.time_ms === null) {
        return this.transition(states, {...record,type:"gap"}, fallback, intervals);
      }
      // A path is a point observation, never a replacement state boundary.
      // A dropped-state burst may continue after this sample, so extending its
      // running phase would hide an unrecorded WAIT. Only explicit states end gaps.
      return null;
    }
    markMissing(states, info, intervals) {
      for (let thread_id = 0; thread_id < states.length; ++thread_id) {
        const prior = states[thread_id], time = Math.max(prior.time_ms, info.start_ms);
        if (intervals) intervals.push({thread_id, start: prior.time_ms, end: time, state: prior.state, observed: prior.observed});
        else this.paint(thread_id, prior.time_ms, time, prior.state);
        states[thread_id] = {thread_id, state: "unknown", time_ms: time, sequence: prior.sequence, observed: false};
      }
    }
    buildOverview(progress) {
      if (!this.building) this.building = this._buildOverview(progress);
      return this.building;
    }
    async _buildOverview(progress) {
      if (this.ready) return;
      const states = copyStates(this.meta.initial).map(s => ({...s, time_ms: this.meta.start}));
      const sequences = states.map(s => s.sequence);
      for (let i = 0; i < this.meta.chunks.length && !this.cancelled; ++i) {
        this.before[i] = copyStates(states);
        this.sequenceBefore[i] = sequences.slice();
        const effects = new Map(); this.effects[i] = effects;
        const recordEffect = (thread, time) => {
          if (time === null) return;
          const prior = effects.get(thread);
          if (prior) { prior.min = Math.min(prior.min, time); prior.max = Math.max(prior.max, time); }
          else effects.set(thread, {min:time,max:time});
        };
        const info = this.meta.chunks[i], records = await this.load(i);
        if (!records) {
          this.markMissing(states, info);
          for (const s of states) recordEffect(s.thread_id, s.time_ms);
        }
        else for (const r of records) {
          if (r.sequence <= sequences[r.thread_id]) continue;
          sequences[r.thread_id] = r.sequence;
          if (r.type !== "sample") recordEffect(r.thread_id, this.transition(states, r, info.start_ms));
          else {
            recordEffect(r.thread_id, this.recover(states, r, info.start_ms));
            if (r.time_ms === null) continue;
            const row = this.overview[r.thread_id], bin = this.bin(r.time_ms);
            row.counts[bin]++; row.depths[bin] = Math.max(row.depths[bin], Math.min(65535, r.normal_ply + r.quiescence_ply));
          }
        }
        this.afterChunks[i] = copyStates(states);
        if (progress) await progress(i + 1, this.meta.chunks.length);
      }
      for (const s of states) {
        const end = this.meta.incomplete && this.meta.cutoff !== null ? this.meta.cutoff : this.meta.end;
        this.paint(s.thread_id, s.time_ms, end, s.state);
        if (end < this.meta.end) this.paint(s.thread_id, end, this.meta.end, "unknown");
      }
      this.after = copyStates(states);
      this.ready = !this.cancelled;
    }
    usable(index, record) {
      return !this.sequenceBefore[index] || record.sequence > this.sequenceBefore[index][record.thread_id];
    }
    statesAtStart(time) {
      const states = copyStates(this.meta.initial).map(s => ({...s, time_ms:this.meta.start}));
      for (const snapshot of this.afterChunks) for (const s of snapshot) {
        const prior = states[s.thread_id];
        if (s.time_ms <= time && (s.sequence > prior.sequence || s.sequence === prior.sequence && !s.observed))
          states[s.thread_id] = {...s};
      }
      return states;
    }
    async stateAt(thread, time, cancelled) {
      let current = this.statesAtStart(time)[thread];
      let next = this.meta.end;
      for (let index = 0; index < this.meta.chunks.length; ++index) {
        if (cancelled && cancelled()) return null;
        const effect = this.effects[index] && this.effects[index].get(thread);
        if (!effect || effect.max <= time) continue;
        const info = this.meta.chunks[index], records = await this.load(index);
        if (!records) {
          if (effect.min <= time) current = {state:"unknown",time_ms:effect.min,observed:false,sequence:current.sequence};
          else { next = Math.min(next, effect.min); break; }
        } else for (const record of records) {
          if (record.thread_id !== thread || !this.usable(index,record) || record.sequence <= current.sequence) continue;
          if (record.type === "sample" && record.time_ms !== null) continue;
          const at = record.time_ms === null ? current.time_ms : record.time_ms;
          if (at > time) { next = Math.min(next, at); break; }
          current = {state:record.time_ms === null || record.type === "gap" ? "unknown" : record.state,
            time_ms:at,observed:record.type !== "gap" && record.time_ms !== null,sequence:record.sequence};
        }
        if (next < this.meta.end) break;
      }
      if (this.meta.incomplete && this.meta.cutoff !== null) {
        if (time >= this.meta.cutoff) current = {state:"unknown",time_ms:this.meta.cutoff,observed:false};
        else next = Math.min(next, this.meta.cutoff);
      }
      return {thread_id:thread,state:current.state,start:current.time_ms,end:Math.max(current.time_ms,next),observed:current.observed};
    }
    async range(start, end, thread, pixelWidth, cancelled) {
      const indices = [];
      for (let i = 0; i < this.meta.chunks.length; ++i) {
        const chunk = this.meta.chunks[i];
        const pathOverlap = chunk.start_ms <= end && chunk.end_ms >= start;
        const stateOverlap = this.effects[i] && [...this.effects[i].values()].some(bounds => bounds.min <= end && bounds.max >= start);
        if (pathOverlap || stateOverlap) indices.push(i);
      }
      // Chunks are published in collection order, not global timestamp order.
      // Seed each thread independently from every completed chunk snapshot.
      const states = this.statesAtStart(start);
      const intervals = [], sampleBins = new Map();
      let actualSamples = 0;
      for (const index of indices) {
        if (cancelled && cancelled()) return null;
        const info = this.meta.chunks[index], records = await this.load(index);
        if (!records) this.markMissing(states, info, intervals);
        else for (const r of records) {
          if (!this.usable(index,r)) continue;
          if (r.type !== "sample") this.transition(states, r, info.start_ms, intervals);
          else {
            this.recover(states, r, info.start_ms, intervals);
            if (r.thread_id === thread && r.time_ms !== null && r.time_ms >= start && r.time_ms <= end) {
            ++actualSamples;
            const bin = clamp(Math.floor((r.time_ms - start) / Math.max(1, end - start) * pixelWidth), 0, pixelWidth - 1);
            const prev = sampleBins.get(bin);
            if (!prev) sampleBins.set(bin, {record:r,count:1,commonNode:r.node_id,firstTime:r.time_ms,lastTime:r.time_ms});
            else {
              ++prev.count;
              prev.commonNode = commonAncestor(this.iteration, prev.commonNode, r.node_id);
              prev.firstTime = Math.min(prev.firstTime, r.time_ms);
              prev.lastTime = Math.max(prev.lastTime, r.time_ms);
              if (r.normal_ply + r.quiescence_ply > prev.record.normal_ply + prev.record.quiescence_ply) prev.record = r;
            }
            }
          }
        }
        // Detailed state geometry also has a hard bound; broad views use fixed bins.
        if (intervals.length > 20000) intervals.splice(0, intervals.length - 20000);
      }
      for (const s of states) {
        const limit = this.meta.incomplete && this.meta.cutoff !== null ? Math.min(end, this.meta.cutoff) : end;
        intervals.push({thread_id: s.thread_id, start: s.time_ms, end: limit, state: s.state, observed: s.observed});
        if (limit < end) intervals.push({thread_id: s.thread_id, start: Math.max(start, limit), end, state: "unknown", observed: true});
      }
      return {intervals: intervals.filter(i => i.end > start && i.start < end && i.end > i.start),
        samples: [...sampleBins.values()].sort((a, b) => a.record.time_ms - b.record.time_ms || compare(a.record, b.record)), actualSamples};
    }
    async neighbor(thread, current, direction, cancelled) {
      let best = null;
      const time = current ? current.time_ms : direction > 0 ? this.meta.start - 1 : this.meta.end + 1;
      const indices = this.meta.chunks.map((_, i) => i);
      if (direction < 0) indices.reverse();
      for (const index of indices) {
        if (cancelled && cancelled()) return null;
        const info = this.meta.chunks[index];
        if (direction > 0 && info.end_ms < time || direction < 0 && info.start_ms > time) continue;
        if (best && (direction > 0 && info.start_ms > best.time_ms || direction < 0 && info.end_ms < best.time_ms)) continue;
        const records = await this.load(index);
        for (const r of records || []) {
          if (r.type !== "sample" || r.thread_id !== thread || r.time_ms === null || !this.usable(index, r)) continue;
          const delta = r.time_ms - time || (current ? r.sequence - current.sequence : direction);
          if (delta * direction <= 0) continue;
          if (!best || ((r.time_ms - best.time_ms || r.sequence - best.sequence) * direction < 0)) best = r;
        }
      }
      return best;
    }
  }
  const ms = n => Number(n).toLocaleString("zh-CN", {maximumFractionDigits: 2}) + " ms";
  const describe = record => "线程 " + record.thread_id + " · " + ms(record.time_ms) + " · " + NAMES[record.state];
  function create(options) {
    const $ = options.$, host = $("timeline-view");
    const LEFT = 118, HEADER = 112, ROW = 28, STATUS_Y = 34, STATUS_H = 24;
    let data = null, meta = null, iteration = null, session = null, canvas = null, ctx = null;
    let overviewCanvas = null, overviewCtx = null, overviewGeometry = null;
    let thread = 0, start = 0, end = 1, epoch = 0, active = false, selected = null, selectedNode = null;
    let range = null, geometry = null, drag = null, initialFocus = true;
    const label = n => n.kind === "root" ? "根局面" : n.kind === "quiescence" ? "静态阶段" : n.move;
    const sameSample = (a, b) => a && b && a.thread_id === b.thread_id && a.sequence === b.sequence;
    const message = text => { $("timeline-message").textContent = text || ""; $("timeline-message").hidden = !text; };
    const hideTooltip = () => { $("timeline-tooltip").hidden = true; };
    function pathFor(nodeId) {
      const path = [];
      let node = iteration && iteration.byId.get(nodeId);
      while (node) { path.push(node); node = node.parent === null ? null : iteration.byId.get(node.parent); }
      return path.reverse();
    }
    function stop() { ++epoch; if (data) data.cancel(); data = null; range = null; selected = null; selectedNode = null; hideTooltip(); }
    function attach(s, it, raw, files) {
      stop(); session = s; iteration = it; thread = 0; initialFocus = true;
      $("timeline-selection").textContent = "点击带着法文字的方块，回放根到该步的棋盘；上方状态条单独显示等待。";
      $("timeline-range-info").textContent = ""; $("timeline-density").textContent = "";
      $("timeline-plot").scrollTop = 0;
      try { meta = manifest(raw, s, it); }
      catch (error) { meta = null; message("时间线无法读取：" + error.message); }
      if (!meta) { message(raw.timeline_version === undefined ? "该报告未记录时间线。请使用 --sample-timeline 重新采集。热点分布仍然可用。" : $("timeline-message").textContent); }
      else {
        data = new Data(meta, files || new Map(), s, it); start = meta.start; end = Math.max(start + 1, meta.end);
        $("timeline-thread").replaceChildren();
        for (let i = 0; i < meta.capacity; ++i) {
          const option = document.createElement("option"); option.value = i; option.textContent = "线程 " + i; $("timeline-thread").append(option);
        }
        $("timeline-thread").value = "0"; updateInputs();
        $("timeline-overview-title").textContent = "全部 " + meta.capacity + " 个线程的状态概览（点击展开）";
      }
      $("timeline-controls").hidden = !meta; $("timeline-plot").hidden = !meta; $("timeline-overview").hidden = !meta;
    }
    function pathHit(p) {
      if (!geometry || p.y < HEADER) return null;
      const y = p.y + geometry.offset;
      const hit = geometry.hits.find(hit => p.x >= hit.x && p.x < hit.x + hit.width && y >= hit.y && y < hit.y + hit.height);
      if (!hit) return null;
      // A connected frame can cover many samples. Replay the one nearest the pointer.
      const columns = geometry.columns;
      let lo = hit.first, hi = hit.last;
      while (lo < hi) {
        const mid = Math.floor((lo + hi) / 2);
        if (columns[mid].x < p.x) lo = mid + 1; else hi = mid;
      }
      if (lo > hit.first && p.x - columns[lo - 1].x < columns[lo].x - p.x) --lo;
      const item = columns[lo];
      return {...hit, record:item.record, count:item.count};
    }
    function markerHit(p) {
      if (!geometry || p.y < 73 || p.y > 107) return null;
      let best = null;
      for (const hit of geometry.markers) if (Math.abs(p.x - hit.x) <= 6 && (!best || Math.abs(p.x - hit.x) < Math.abs(p.x - best.x))) best = hit;
      return best;
    }
    function tooltip(p) {
      const hit = pathHit(p) || markerHit(p), tip = $("timeline-tooltip"), plot = $("timeline-plot");
      if (!hit) { hideTooltip(); return; }
      const node = iteration.byId.get(hit.node);
      tip.textContent = (node ? label(node) : "路径未收录") + " · " + ms(hit.record.time_ms) + " · 线程 " + thread +
        (hit.last > hit.first ? "\n相同前缀 · " + ms(geometry.columns[hit.first].record.time_ms) + " — " +
          ms(geometry.columns[hit.last].record.time_ms) : "") +
        "\n点击回放到这一步" + (hit.count > 1 ? " · 此点合并 " + hit.count + " 个样本，请放大逐点查看" : "");
      tip.style.left = clamp(p.x + 12, 5, Math.max(5, plot.clientWidth - 300)) + "px";
      tip.style.top = (Math.min(p.y + 16, Math.max(HEADER, plot.clientHeight - 72)) + plot.scrollTop) + "px";
      tip.hidden = false;
    }
    function canvasReady() {
      if (canvas) return !!ctx;
      canvas = document.createElement("canvas"); canvas.id = "timeline-canvas";
      canvas.setAttribute("role", "img"); canvas.setAttribute("aria-label", "所选线程的时间轴、状态条和带着法文字的采样路径，点击方块回放棋盘");
      canvas.tabIndex = 0; $("timeline-canvas-host").append(canvas);
      ctx = canvas.getContext ? canvas.getContext("2d") : null;
      if (!ctx) { message("当前浏览器无法提供 Canvas 2D 绘图，请使用支持 Canvas 的浏览器。热点视图仍可使用。"); return false; }
      canvas.addEventListener("pointerdown", event => {
        if (!geometry) return;
        const point = position(event); hideTooltip();
        drag = {...point, shift: event.shiftKey, start, end};
        if (canvas.setPointerCapture) canvas.setPointerCapture(event.pointerId);
      });
      canvas.addEventListener("pointermove", event => {
        if (!geometry) return;
        const p = position(event);
        if (drag && drag.shift) {
          const delta = (drag.x - p.x) / geometry.width * (drag.end - drag.start);
          setRange(drag.start + delta, drag.end + delta, false); draw();
        } else if (drag && Math.abs(p.x - drag.x) > 4) {
          draw(); ctx.fillStyle = "#245cc530"; ctx.fillRect(Math.min(p.x, drag.x), 0, Math.abs(p.x - drag.x), geometry.height);
        } else tooltip(p);
      });
      canvas.addEventListener("pointerup", event => {
        if (!drag) return;
        const p = position(event), old = drag; drag = null;
        if (old.shift) return refresh();
        if (Math.abs(p.x - old.x) > 5) setRange(xTime(Math.min(p.x, old.x)), xTime(Math.max(p.x, old.x)));
        else return pick(p);
      });
      canvas.addEventListener("pointercancel", () => { drag = null; hideTooltip(); draw(); });
      canvas.addEventListener("pointerleave", hideTooltip);
      canvas.addEventListener("wheel", event => {
        if (!geometry) return;
        hideTooltip();
        // Trackpads emit many small pixel deltas; mouse wheels can use lines or pages.
        const unit = event.deltaMode === 1 ? 16 : 1;
        const dx = (event.deltaX || 0) * (event.deltaMode === 2 ? geometry.width : unit);
        const dy = (event.deltaY || 0) * (event.deltaMode === 2 ? geometry.height : unit);
        if (event.ctrlKey || event.metaKey) {
          if (!dy) return;
          event.preventDefault();
          const p = position(event);
          return zoom(Math.exp(clamp(dy * .006, -.3, .3)), xTime(p.x));
        } else if (event.shiftKey || dx && Math.abs(dx) >= Math.abs(dy)) {
          const pixels = Math.abs(dx) >= Math.abs(dy) ? dx : dy;
          if (!pixels) return;
          event.preventDefault();
          const delta = pixels / geometry.width * (end - start);
          return setRange(start + delta, end + delta);
        }
        // Predominantly vertical, unmodified gestures keep the browser's path scrolling.
      }, {passive: false});
      canvas.addEventListener("keydown", event => {
        if (event.key === "ArrowLeft" || event.key === "ArrowRight") { event.preventDefault(); return navigate(event.key === "ArrowLeft" ? -1 : 1); }
      });
      $("timeline-plot").addEventListener("scroll", () => { hideTooltip(); draw(); });
      return true;
    }
    function position(event) { const box = canvas.getBoundingClientRect(); return {x: event.clientX - box.left, y: event.clientY - box.top}; }
    function xTime(x) { return clamp(start + (x - geometry.left) / geometry.width * (end - start), meta.start, meta.end); }
    function updateInputs() { $("timeline-start").value = Math.round(start); $("timeline-end").value = Math.round(end); }
    function setRange(a, b, redraw = true) {
      if (!meta) return;
      hideTooltip();
      const span = Math.max(1, Math.min(Math.max(1, meta.end - meta.start), b - a));
      start = clamp(a, meta.start, Math.max(meta.start, meta.end - span)); end = start + span;
      if (selected && (selected.time_ms < start || selected.time_ms > end)) {
        selected = null; selectedNode = null;
        options.onState({thread_id:thread,state:"unknown",start,end,observed:false,reason:"已调整时间范围，请重新选择可见样本查看棋盘。"});
        $("timeline-selection").textContent = "已调整时间范围，请选择可见样本。";
      }
      updateInputs(); if (redraw) return refresh();
    }
    function zoom(factor, center = selected ? selected.time_ms : (start + end) / 2) {
      const fraction = clamp((center - start) / Math.max(1, end - start), 0, 1), span = (end - start) * factor;
      return setRange(center - span * fraction, center + span * (1 - fraction));
    }
    function warnings() {
      const items = [];
      if (meta.incomplete) items.push("时间线不完整" + (meta.stopReason ? "（" + meta.stopReason + "）" : "") + (meta.cutoff !== null ? "，可靠截止 " + ms(meta.cutoff) : ""));
      if (meta.dropped) items.push("状态事件丢失 " + meta.dropped + " 条");
      if (meta.boundaryUnknown || !meta.startKnown || meta.finished && !meta.endKnown) items.push("迭代边界记录缺失，起止时间或边界附近的线程状态不完整；未知区间已单独标出");
      if (data.failed.size) items.push("缺失或损坏分块 " + data.failed.size + " 个；受影响区间标为未知。" + [...data.failed.values()].slice(0, 2).join("；"));
      message(items.join("；"));
    }
    async function readableWindow(current, anchor, cancelled) {
      let first = anchor || await current.neighbor(thread, null, 1, cancelled);
      if (!first || cancelled()) return null;
      if (anchor) for (let i = 0; i < 4; ++i) {
        const prior = await current.neighbor(thread, first, -1, cancelled);
        if (!prior || cancelled()) break;
        first = prior;
      }
      let last = first, minimumGap = Infinity;
      for (let i = 0; i < 9; ++i) {
        const next = await current.neighbor(thread, last, 1, cancelled);
        if (!next || cancelled()) break;
        if (next.time_ms > last.time_ms) minimumGap = Math.min(minimumGap,next.time_ms - last.time_ms);
        last = next;
      }
      const pad = Math.max(4, (session.sample_interval_ms || 20) * .6);
      const usable = Math.max(100,$("timeline-plot").clientWidth - LEFT - 14);
      const maxSpan = Number.isFinite(minimumGap) ? Math.max(1,usable * .78 * minimumGap / 64) : Infinity;
      const span = Math.min(Math.max(pad * 2,last.time_ms - first.time_ms + pad * 2),maxSpan);
      const from = anchor ? anchor.time_ms - span * .4 : first.time_ms - Math.min(pad,span * .1);
      return {first,start:from,end:from + span,maxSpan};
    }
    async function show() {
      if (!data || !meta || !active || !canvasReady()) return;
      const token = ++epoch, current = data, wantedThread = thread;
      const cancelled = () => token !== epoch || current !== data || !active;
      try {
        if (!current.ready) {
          message("正在逐块建立有界时间概览…");
          await current.buildOverview(async (n, total) => {
            if (cancelled()) return;
            message("正在建立时间概览 " + n + " / " + total + "…");
            await new Promise(resolve => requestAnimationFrame(resolve));
          });
        }
        if (cancelled()) return;
        const counts = current.overview.map(row => row.counts.reduce((a, b) => a + b, 0));
        Array.from($("timeline-thread").children).forEach((option, i) => { option.textContent = "线程 " + i + " · " + counts[i] + " 样本"; });
        let first = null;
        if (initialFocus) {
          initialFocus = false;
          first = await current.neighbor(thread, null, 1, cancelled);
          if (cancelled()) return;
          if (first) {
            const window = await readableWindow(current, first, cancelled);
            if (cancelled()) return;
            if (window && (counts[thread] > 14 || end - start > window.maxSpan)) setRange(window.start, window.end, false);
          }
        }
        warnings(); const result = await refresh();
        if (result && current === data && wantedThread === thread && active && first && first.time_ms >= start && first.time_ms <= end) {
          const informative = result.samples.find(item => pathFor(item.record.node_id).length > 1);
          const item = informative || {record:first,count:1};
          choose(item.record,item.record.node_id,item.count);
        }
      } catch (error) { if (!cancelled()) message("时间图读取失败：" + error.message); }
    }
    async function refresh() {
      if (!active || !data || !data.ready || !ctx) return null;
      const token = ++epoch, current = data;
      $("timeline-loading").textContent = "读取可见范围…";
      const width = Math.max(100, $("timeline-plot").clientWidth - LEFT - 14);
      const result = await current.range(start, end, thread, width, () => token !== epoch);
      if (!result || token !== epoch || current !== data || !active) return null;
      range = result; $("timeline-loading").textContent = "";
      $("timeline-stack-title").textContent = "线程 " + thread + " · 采样走法栈";
      $("timeline-range-info").textContent = ms(start) + " — " + ms(end) + " · 线程 " + thread + " · " + result.actualSamples.toLocaleString("zh-CN") + " 样本";
      const density = result.actualSamples > Math.max(1, width / 38);
      const combined = result.samples.some(item => item.count > 1);
      $("timeline-density").textContent = !result.actualSamples ? "此范围没有该线程的路径样本。可查看状态条，或切换到有样本的线程。" :
        density ? "当前点较密，相邻相同前缀已连成横条。点击“看清着法”或放大可逐点查看；点击横条回放附近样本。" :
        combined ? "标有 ×N 的点合并了同一时刻或像素的多个样本。可用前／后样本逐点查看，点击方块回放到该步。" :
        "相邻相同前缀连成横条；悬停查看着法与采样时刻，点击回放附近样本。左侧列出已选样本的完整走法。";
      warnings(); draw(); drawOverview(); return result;
    }
    function fillStatus(state, x, y, width, height, text, context = ctx) {
      const c = context;
      c.fillStyle = state === "mixed" ? "#cbd7e6" : COLORS[state] || COLORS.unknown; c.fillRect(x, y, width, height);
      if (state === "unknown") {
        c.save(); c.beginPath(); c.rect(x, y, width, height); c.clip(); c.strokeStyle = "#b8c1ce"; c.lineWidth = 1;
        for (let z = x - height; z < x + width; z += 8) { c.beginPath(); c.moveTo(z, y + height); c.lineTo(z + height, y); c.stroke(); }
        c.restore();
      } else if (state === "mixed") {
        c.fillStyle = "#839dbd"; c.fillRect(x, y + height / 2, width, height / 2);
      }
      if (text && width > Math.max(68, text.length * 12 + 8)) { c.fillStyle = "#39485c"; c.fillText(text, x + 6, y + height / 2 + 4, width - 12); }
    }
    function stateBand(context, threadId, y, height, left, usable, text) {
      const tx = t => left + (t - start) / Math.max(1, end - start) * usable;
      const row = data.overview[threadId];
      for (let b = data.bin(start); b <= data.bin(end); ++b) {
        const bs = meta.start + b / data.bins * (meta.end - meta.start), be = meta.start + (b + 1) / data.bins * (meta.end - meta.start);
        const x = tx(Math.max(start, bs)), w = tx(Math.min(end, be)) - x, bits = row.states[b];
        const single = STATES.find((_, k) => bits === 1 << k);
        fillStatus(single || (bits & 1 ? "unknown" : bits ? "mixed" : "unknown"), x, y, Math.max(0, w), height, null, context);
      }
      for (const interval of range ? range.intervals : []) {
        if (interval.thread_id !== threadId) continue;
        const x = tx(Math.max(start, interval.start)), w = tx(Math.min(end, interval.end)) - x;
        if (w > 0) fillStatus(interval.state, x, y, w, height, text ? NAMES[interval.state] : null, context);
      }
    }
    function setupCanvas(element, context, width, height) {
      const ratio = window.devicePixelRatio || 1;
      element.width = Math.round(width * ratio); element.height = Math.round(height * ratio);
      element.style.width = width + "px"; element.style.height = height + "px";
      context.setTransform(ratio, 0, 0, ratio, 0, 0); context.clearRect(0, 0, width, height);
      context.font = "12px ui-monospace, SFMono-Regular, monospace"; context.textBaseline = "alphabetic";
    }
    function stackFrames(columns, firstLevel, lastLevel) {
      const interval = session.sample_interval_ms || 20, frames = [], rows = [];
      const blocked = [];
      for (const part of (range ? range.intervals : []).filter(part => part.thread_id === thread &&
        !["search","quiescence"].includes(part.state)).sort((a,b) => a.start - b.start)) {
        const prior = blocked[blocked.length - 1];
        if (prior && part.start <= prior.end) prior.end = Math.max(prior.end,part.end);
        else blocked.push({start:part.start,end:part.end});
      }
      columns.forEach((item,index) => {
        const firstTime = item.firstTime ?? item.record.time_ms, lastTime = item.lastTime ?? item.record.time_ms;
        const prev = columns[index - 1], next = columns[index + 1];
        const prevTime = prev ? prev.lastTime ?? prev.record.time_ms : -Infinity;
        const nextTime = next ? next.firstTime ?? next.record.time_ms : Infinity;
        let a = Math.max(start, firstTime - (firstTime - prevTime <= interval * 3 ? (firstTime - prevTime) / 2 : interval / 2));
        let b = Math.min(end, lastTime + (nextTime - lastTime <= interval * 3 ? (nextTime - lastTime) / 2 : interval / 2));
        let reliable = true;
        // Samples remain selectable inside an unknown state, but cannot bridge it.
        let lo = 0, hi = blocked.length;
        while (lo < hi) {
          const mid = Math.floor((lo + hi) / 2);
          if (blocked[mid].end <= a) lo = mid + 1; else hi = mid;
        }
        for (let k = lo; k < blocked.length && blocked[k].start < b; ++k) {
          const part = blocked[k], time = item.record.time_ms;
          if (part.start <= lastTime && part.end >= firstTime) reliable = false;
          if (part.end <= time) a = Math.max(a,part.end);
          else if (part.start > time) b = Math.min(b,part.start);
          else { a = b = time; reliable = false; break; }
        }
        const commonNode = iteration.byId.get(item.commonNode);
        const commonDepth = item.commonNode === undefined ? item.path.length : commonNode ? commonNode.pathDepth : 0;
        const path = item.path.length ? item.path : [null];
        for (let level = firstLevel; level < Math.min(path.length,lastLevel); ++level) {
          const node = path[level];
          const prior = rows[level], join = reliable && !!node && level < commonDepth;
          if (join && prior && prior.join && prior.node === node.id && prior.last === index - 1 && Math.abs(prior.to - a) < 1e-7) {
            prior.to = b; prior.last = index;
          } else {
            const frame = {node:node ? node.id : null,level,from:a,to:b,first:index,last:index,join};
            frames.push(frame); rows[level] = frame;
          }
        }
      });
      return frames;
    }
    function frameText(text, width) {
      if (ctx.measureText(text).width <= width) return text;
      while (text && ctx.measureText(text + "…").width > width) text = text.slice(0,-1);
      return text ? text + "…" : "";
    }
    function draw() {
      if (!active || !ctx || !meta || !data || !data.ready) return;
      const plot = $("timeline-plot"), width = Math.max(260, plot.clientWidth), height = Math.max(300, plot.clientHeight || 560);
      setupCanvas(canvas, ctx, width, height);
      const left = LEFT, usable = Math.max(1, width - left - 14), stackTop = HEADER + 4, offset = plot.scrollTop || 0;
      const items = range ? range.samples.slice() : [], selectedPath = selected ? pathFor(selected.node_id) : [];
      // Same-millisecond samples share a pixel. Stepping must still show the
      // exact selected sample, rather than the bin's deepest representative.
      if (selected && selected.time_ms >= start && selected.time_ms <= end && !items.some(item => sameSample(item.record,selected))) {
        const bins = Math.max(100,plot.clientWidth - LEFT - 14);
        const bin = time => clamp(Math.floor((time - start) / Math.max(1,end - start) * bins),0,bins - 1);
        const pixel = bin(selected.time_ms);
        const index = items.findIndex(item => bin(item.record.time_ms) === pixel);
        if (index >= 0) items[index] = {...items[index],record:selected};
        else items.push({record:selected,count:1});
        items.sort((a,b) => a.record.time_ms - b.record.time_ms || a.record.sequence - b.record.sequence);
      }
      let maxDepth = Math.max(1, selectedPath.length);
      const columns = items.map(item => {
        const path = pathFor(item.record.node_id); maxDepth = Math.max(maxDepth, path.length); return {...item, path};
      });
      const fullHeight = stackTop + maxDepth * ROW + 22;
      $("timeline-spacer").style.height = Math.max(height, fullHeight) + "px";
      geometry = {left, width:usable, height, stackTop, offset, columns, hits:[], markers:[], status:{x:left,y:STATUS_Y,width:usable,height:STATUS_H}};
      const tx = t => left + (t - start) / Math.max(1, end - start) * usable;
      ctx.fillStyle = "#fff"; ctx.fillRect(0, 0, width, height);
      ctx.save(); ctx.beginPath(); ctx.rect(0, HEADER, width, height - HEADER); ctx.clip();
      const ticks = Math.max(2, Math.min(6, Math.floor(usable / 105)));
      for (let i = 0; i <= ticks; ++i) {
        const x = left + i / ticks * usable;
        ctx.strokeStyle = "#edf1f5"; ctx.beginPath(); ctx.moveTo(x, HEADER); ctx.lineTo(x, height); ctx.stroke();
      }
      if (!items.length) {
        ctx.fillStyle = "#718096"; ctx.fillText("这个时间范围没有路径采样点。", left + 15, HEADER + 42);
        ctx.fillText("等待状态请看上方状态条，或选择其他线程。", left + 15, HEADER + 66);
      }
      columns.forEach((item, index) => {
        const x = tx(item.record.time_ms), prev = index ? tx(columns[index - 1].record.time_ms) : -Infinity;
        const next = index + 1 < columns.length ? tx(columns[index + 1].record.time_ms) : Infinity;
        const nearest = Math.min(x - prev, next - x);
        item.x = x; item.width = Math.max(1,Math.min(112,Number.isFinite(nearest) ? nearest : 112));
      });
      const selectedIndex = columns.findIndex(item => sameSample(selected,item.record));
      const firstLevel = Math.max(0,Math.floor((HEADER + offset - stackTop) / ROW));
      const lastLevel = Math.ceil((height + offset - stackTop) / ROW);
      for (const frame of stackFrames(columns,firstLevel,lastLevel)) {
        const y = stackTop + frame.level * ROW, viewY = y - offset;
        if (viewY + ROW < HEADER || viewY > height) continue;
        const node = iteration.byId.get(frame.node), q = node && ["quiescence","quiescence_move"].includes(node.kind);
        const x = clamp(tx(frame.from),left,width - 15), frameWidth = Math.max(1,Math.min(width - 14 - x,tx(frame.to) - x));
        const isSelected = selectedIndex >= frame.first && selectedIndex <= frame.last;
        ctx.fillStyle = node ? q ? "#fff0d7" : "#e8f0fc" : "#f2f4f7";
        ctx.fillRect(x,viewY,frameWidth,ROW - 4);
        ctx.fillStyle = q ? "#c89036" : "#7399cc"; ctx.fillRect(x,viewY,Math.min(3,frameWidth),ROW - 4);
        ctx.strokeStyle = isSelected ? "#285f9f" : q ? "#e6cc9f" : "#c7d8ef";
        ctx.lineWidth = isSelected && node && node.id === selectedNode ? 2 : 1;
        ctx.strokeRect(x,viewY,frameWidth,ROW - 4);
        const text = frameText(node ? label(node) : "未收录",frameWidth - 12);
        if (text) { ctx.fillStyle = q ? "#805318" : "#27486f"; ctx.fillText(text,x + 7,viewY + 16); }
        geometry.hits.push({...frame,x,y,width:frameWidth,height:ROW - 4});
      }
      if (selectedIndex >= 0) {
        const item = columns[selectedIndex];
        ctx.strokeStyle = "#4477b9"; ctx.lineWidth = 1; ctx.setLineDash([2,4]);
        ctx.beginPath(); ctx.moveTo(item.x,HEADER);
        ctx.lineTo(item.x,Math.min(height,stackTop + Math.max(1,item.path.length) * ROW - offset));
        ctx.stroke(); ctx.setLineDash([]);
      }
      // The gutter spells out the selected sample even when nearby points are dense.
      ctx.fillStyle = "#f8fafc"; ctx.fillRect(0, HEADER, left - 7, height - HEADER);
      for (let level = 0; level < maxDepth; ++level) {
        const y = stackTop + level * ROW - offset;
        if (y + ROW < HEADER || y > height) continue;
        const node = selectedPath[level];
        ctx.fillStyle = node ? ["quiescence","quiescence_move"].includes(node.kind) ? "#95611e" : "#36587e" : "#8a96a6";
        ctx.fillText(node ? (level ? level + ". " : "") + label(node) : level ? "第 " + level + " 层" : "根局面", 10, y + 16, left - 20);
      }
      ctx.restore();
      // Fixed header is drawn last and never translated by the path scroll offset.
      ctx.fillStyle = "#fff"; ctx.fillRect(0, 0, width, HEADER);
      ctx.fillStyle = "#64748b"; ctx.fillText("思考时间", 10, 18);
      for (let i = 0; i <= ticks; ++i) {
        const x = left + i / ticks * usable, text = ms(start + i / ticks * (end - start));
        ctx.fillStyle = "#64748b"; ctx.fillText(text, Math.min(width - text.length * 7.1 - 12, x + 2), 18);
      }
      stateBand(ctx, thread, STATUS_Y, STATUS_H, left, usable, true);
      ctx.fillStyle = "#36587e"; ctx.fillText("线程 " + thread, 10, STATUS_Y + 16);
      ctx.fillStyle = "#8290a1"; ctx.fillText("采样时刻", 10, 89);
      for (const item of columns) {
        const r = item.record, isSelected = sameSample(selected,r), x = item.x;
        ctx.strokeStyle = isSelected ? "#285f9f" : "#c6d4e5"; ctx.beginPath(); ctx.moveTo(x, 90); ctx.lineTo(x, HEADER); ctx.stroke();
        ctx.fillStyle = isSelected ? "#245cc5" : "#809bbd"; ctx.beginPath(); ctx.arc(x, 83, isSelected ? 5 : 3.5, 0, Math.PI * 2); ctx.fill();
        if (item.count > 1 && item.width > 36) { ctx.fillStyle = "#6b7c91"; ctx.fillText("×" + item.count,x + 7,99,item.width / 2); }
        geometry.markers.push({x, node:r.node_id,record:r,count:item.count});
      }
      const captions = [];
      const captionOrder = columns.slice().sort((a,b) => Number(!!sameSample(selected,b.record)) - Number(!!sameSample(selected,a.record)));
      for (const item of captionOrder) {
        if (item.width < 62 && !sameSample(selected,item.record)) continue;
        const text = ms(item.record.time_ms), textWidth = text.length * 7.3;
        const x = clamp(item.x - textWidth / 2,left,Math.max(left,width - textWidth - 14));
        if (captions.some(box => x < box.end + 7 && x + textWidth > box.start - 7)) continue;
        captions.push({start:x,end:x + textWidth});
        ctx.fillStyle = sameSample(selected,item.record) ? "#245cc5" : "#6d7f95";
        ctx.fillText(text,x,72);
      }
      ctx.strokeStyle = "#dfe6ef"; ctx.beginPath(); ctx.moveTo(0, HEADER - 1); ctx.lineTo(width, HEADER - 1); ctx.stroke();
    }
    async function selectState(threadId, time, current = data) {
      const token = ++epoch, interval = await current.stateAt(threadId, time, () => token !== epoch);
      if (token !== epoch || current !== data || !active) return;
      selected = null; selectedNode = null;
      const chosen = interval || {thread_id:threadId,state:"unknown",start:time,end:time,observed:false};
      options.onState(chosen);
      $("timeline-selection").textContent = "线程 " + threadId + " · " + NAMES[chosen.state] + " · " + ms(chosen.start) + " — " + ms(chosen.end) +
        " · " + ms(chosen.end - chosen.start) + (["search","quiescence"].includes(chosen.state) ? " · 此时没有路径样本，不能还原具体走法。" : "");
      draw();
    }
    async function pick(p) {
      if (!range || !geometry) return;
      const band = geometry.status;
      if (p.y >= band.y && p.y < band.y + band.height && p.x >= band.x) return selectState(thread, xTime(p.x));
      const hit = pathHit(p) || markerHit(p);
      if (hit) choose(hit.record, hit.node, hit.count);
      else if (p.y >= HEADER) return selectState(thread, xTime(p.x));
    }
    function choose(record, node, density) {
      selected = record; selectedNode = node;
      const delay = record.request_time_ms === null ? "请求时间未知" : "请求 " + ms(record.request_time_ms) + " · 延迟 " + ms(record.time_ms - record.request_time_ms);
      const chosen = iteration.byId.get(node), notes = ["已选 " + (chosen ? label(chosen) : "未收录路径"), describe(record), "请求 #" + record.request_id, "序号 " + record.sequence, delay,
        "普通 " + record.normal_ply + " · 静态 " + record.quiescence_ply];
      if (density > 1) notes.push("此像素合并 " + density + " 个样本，当前为最深路径；点击“看清着法”或放大逐点查看");
      if (record.truncated || record.path_omitted) notes.push("路径不完整，仅回放已记录前缀");
      $("timeline-selection").textContent = notes.join(" · ");
      if (node !== null && iteration.byId.has(node)) options.onNode(node, record);
      else options.onState({thread_id:thread,state:"unknown",start:record.time_ms,end:record.time_ms,observed:false,reason:"该样本的路径未收录。"});
      draw(); drawOverview();
    }
    async function navigate(direction) {
      if (!data || !data.ready) return;
      const token = ++epoch, current = data, wantedThread = thread;
      $("timeline-loading").textContent = "查找相邻样本…";
      const record = await current.neighbor(thread, selected, direction, () => token !== epoch);
      if (token !== epoch || current !== data || !active) return;
      $("timeline-loading").textContent = "";
      if (!record) { $("timeline-selection").textContent = direction > 0 ? "没有后续路径样本。" : "没有更早的路径样本。"; return; }
      if (record.time_ms < start || record.time_ms > end) {
        const span = end - start; setRange(record.time_ms - span / 2, record.time_ms + span / 2, false);
        if (!await refresh()) return;
      }
      if (current === data && wantedThread === thread && active) choose(record, record.node_id, 1);
    }
    async function readable() {
      if (!data || !data.ready) return;
      const token = ++epoch, current = data, wantedThread = thread, window = await readableWindow(current, selected, () => token !== epoch || current !== data || !active);
      if (!window || token !== epoch || current !== data || !active) return;
      const record = selected || window.first;
      const node = selected ? selectedNode : window.first.node_id;
      setRange(window.start, window.end, false); $("timeline-plot").scrollTop = 0;
      const result = await refresh(); if (result && current === data && wantedThread === thread && active) choose(record, node, 1);
    }
    function changeThread(value) {
      thread = value; $("timeline-thread").value = value; selected = null; selectedNode = null; initialFocus = true;
      $("timeline-plot").scrollTop = 0;
      options.onState({thread_id:thread,state:"unknown",start,end,observed:false,reason:"已切换线程，请选择该线程的路径样本或状态区间。"});
      $("timeline-selection").textContent = "线程 " + thread + " · 请选取路径样本或状态区间。";
      return show();
    }
    function overviewReady() {
      if (overviewCanvas) return !!overviewCtx;
      overviewCanvas = document.createElement("canvas"); overviewCanvas.id = "timeline-overview-canvas";
      overviewCanvas.setAttribute("role","img"); overviewCanvas.setAttribute("aria-label","全部线程状态概览，点击行名切换所选线程");
      $("timeline-overview-canvas-host").append(overviewCanvas); overviewCtx = overviewCanvas.getContext ? overviewCanvas.getContext("2d") : null;
      $("timeline-overview-plot").addEventListener("scroll",drawOverview);
      overviewCanvas.addEventListener("click", event => {
        if (!overviewGeometry) return;
        const box = overviewCanvas.getBoundingClientRect(), x = event.clientX - box.left, y = event.clientY - box.top;
        if (y < 28) return;
        const t = Math.floor((y + overviewGeometry.offset - 28) / 17);
        if (t < 0 || t >= meta.capacity) return;
        if (x < LEFT) return changeThread(t);
        if (t !== thread) { thread = t; $("timeline-thread").value = t; $("timeline-plot").scrollTop = 0; }
        const time = clamp(start + (x - LEFT) / overviewGeometry.width * (end - start), start, end);
        return selectState(t,time).then(refresh);
      });
      return !!overviewCtx;
    }
    function drawOverview() {
      if (!active || !data || !data.ready || !$("timeline-overview").open || !overviewReady()) return;
      const plot = $("timeline-overview-plot"), width = Math.max(260,plot.clientWidth || $("timeline-plot").clientWidth), height = Math.max(100,plot.clientHeight || 230);
      setupCanvas(overviewCanvas, overviewCtx, width,height);
      const c = overviewCtx, offset = plot.scrollTop || 0, usable = width - LEFT - 14;
      overviewGeometry = {offset,width:usable}; $("timeline-overview-spacer").style.height = Math.max(height,28 + meta.capacity * 17) + "px";
      c.fillStyle = "#fff"; c.fillRect(0,0,width,height);
      c.save(); c.beginPath(); c.rect(0,28,width,height - 28); c.clip();
      for (let t = 0; t < meta.capacity; ++t) {
        const y = 28 + t * 17 - offset;
        if (y + 17 < 28 || y > height) continue;
        c.fillStyle = t === thread ? "#e8f0ff" : "#f8fafc"; c.fillRect(0,y,LEFT - 7,16);
        c.fillStyle = t === thread ? "#245cc5" : "#64748b"; c.fillText("线程 " + t,10,y + 12);
        stateBand(c,t,y + 2,12,LEFT,usable,false);
      }
      c.restore(); c.fillStyle = "#fff"; c.fillRect(0,0,width,28); c.fillStyle = "#64748b";
      c.fillText("线程",10,18); c.fillText(ms(start),LEFT,18); c.fillText(ms(end),Math.max(LEFT,width - 105),18);
    }
    $("timeline-overview").addEventListener("toggle",drawOverview);
    $("timeline-thread").addEventListener("change",event => changeThread(Number(event.target.value)));
    $("timeline-apply").addEventListener("click",() => {
      const a = Number($("timeline-start").value), b = Number($("timeline-end").value);
      if (!Number.isFinite(a) || !Number.isFinite(b) || b <= a) { message("时间范围须为有效数字，且结束时间大于开始时间。"); return; }
      return setRange(a,b);
    });
    $("timeline-readable").addEventListener("click",readable);
    $("timeline-zoom-in").addEventListener("click",() => zoom(1 / 1.5));
    $("timeline-zoom-out").addEventListener("click",() => zoom(1.5));
    $("timeline-reset").addEventListener("click",() => { if (meta) return setRange(meta.start,Math.max(meta.start + 1,meta.end)); });
    $("timeline-pan-left").addEventListener("click",() => setRange(start - (end - start) / 2,end - (end - start) / 2));
    $("timeline-pan-right").addEventListener("click",() => setRange(start + (end - start) / 2,end + (end - start) / 2));
    $("timeline-prev").addEventListener("click",() => navigate(-1));
    $("timeline-next").addEventListener("click",() => navigate(1));
    window.addEventListener("resize",() => { if (active) return refresh(); });
    return {attach,stop,selectMode(showTimeline) {
      active = showTimeline; host.hidden = !active;
      if (active) return show(); else { ++epoch; $("timeline-loading").textContent = ""; hideTooltip(); }
    }};
  }
  return {manifest, parseChunk, Data, create, STATES, NAMES};
}));
