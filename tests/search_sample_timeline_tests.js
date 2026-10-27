/* Run with node tests/search_sample_timeline_tests.js. No packages or browser. */
"use strict";
const assert = require("node:assert/strict");
const T = require("../tools/search_sample_viewer/timeline.js");
let checks = 0;
async function test(name, body) { await body(); ++checks; console.log("ok " + checks + " - " + name); }
const session = {search_id:7, thread_capacity:3};
const iteration = {depth:8, byId:new Map([[0,{id:0,pathDepth:1}], [1,{id:1,pathDepth:2}], [2,{id:2,pathDepth:3}]])};
const state = (thread_id,sequence,time_ms,state) => ({type:"state",thread_id,sequence,time_ms,state});
const sample = (thread_id,sequence,time_ms,extra={}) => ({type:"sample",thread_id,sequence,time_ms,state:"search",
  request_time_ms:time_ms === null ? null : Math.max(0,time_ms-2),request_id:sequence,node_id:1,
  normal_ply:1,quiescence_ply:0,truncated:false,path_omitted:false,...extra});
const gap = (thread_id,sequence,time_ms,lost_count=1) => ({type:"gap",thread_id,sequence,time_ms,lost_count});
function fixture(blocks, extra={}, tree=iteration) {
  const files = new Map(), reads = new Map();
  const chunks = blocks.map((block,index) => {
    const info={file:"depth-8-timeline-"+String(index+1).padStart(6,"0")+".json",start_ms:block.start,end_ms:block.end,records:block.records.length};
    const content={timeline_version:1,search_id:7,iteration_depth:8,start_ms:block.start,end_ms:block.end,records:block.records};
    files.set(info.file,{text:async()=>{reads.set(info.file,(reads.get(info.file)||0)+1); return JSON.stringify(content);}});
    return info;
  });
  const raw={timeline_version:1,timeline:{start_ms:0,end_ms:100,finished:true,incomplete:false,cutoff_ms:null,
    stop_reason:null,state_dropped:0,initial_states:[{thread_id:0,state:"search",time_ms:0,sequence:0},
      {thread_id:1,state:"idle",time_ms:0,sequence:0},{thread_id:2,state:"unknown",time_ms:null,sequence:0}],chunks,...extra}};
  const meta=T.manifest(raw,session,tree), data=new T.Data(meta,files,session,tree);
  return {raw,meta,data,files,reads};
}
async function main() {
  await test("old hotspot reports have no synthetic timeline",()=>assert.equal(T.manifest({},session,iteration),null));
  await test("manifest rejects file traversal, duplicate chunks and incompatible versions",()=>{
    const f=fixture([{start:0,end:100,records:[]}]);
    f.raw.timeline.chunks[0].file="../depth-8-timeline-1.json";
    assert.throws(()=>T.manifest(f.raw,session,iteration),/分块清单/);
    f.raw.timeline_version=2; assert.throws(()=>T.manifest(f.raw,session,iteration),/版本/);
  });
  await test("unregistered slots are unknown while reliable idle remains distinct",async()=>{
    const {data}=fixture([]); await data.buildOverview();
    assert.equal((await data.stateAt(1,50)).state,"idle");
    assert.equal((await data.stateAt(2,50)).state,"unknown");
    assert.equal((await data.stateAt(2,50)).observed,false);
  });
  await test("missing boundary flags remain compatible while explicit unknown boundaries are preserved",()=>{
    const legacy=fixture([]); assert.equal(legacy.meta.startKnown,true); assert.equal(legacy.meta.endKnown,true);
    assert.equal(legacy.meta.boundaryUnknown,false);
    const missing=fixture([],{start_known:false,end_known:false,boundary_unknown:true});
    assert.equal(missing.meta.startKnown,false); assert.equal(missing.meta.endKnown,false); assert.equal(missing.meta.boundaryUnknown,true);
    missing.raw.timeline.start_known="no";
    assert.throws(()=>T.manifest(missing.raw,session,iteration),/边界标记/);
  });
  await test("same-millisecond events are ordered by per-thread sequence",async()=>{
    const {data}=fixture([{start:0,end:100,records:[state(0,3,20,"search"),state(0,2,20,"wait_helpers"),state(0,4,60,"idle")]}]);
    await data.buildOverview();
    const at=await data.stateAt(0,30); assert.equal(at.state,"search"); assert.equal(at.start,20); assert.equal(at.end,60);
  });
  await test("wait intervals keep their actual endpoints rather than query-window endpoints",async()=>{
    const {data}=fixture([{start:0,end:100,records:[state(0,1,20,"wait_helpers"),state(0,2,80,"search")]}]);
    await data.buildOverview(); const at=await data.stateAt(0,51);
    assert.deepEqual({state:at.state,start:at.start,end:at.end},{state:"wait_helpers",start:20,end:80});
  });
  await test("a gap remains unknown across path points until an explicit state recovers",async()=>{
    const {data}=fixture([{start:0,end:100,records:[gap(0,1,20,3),sample(0,4,40,{state:"quiescence"}),state(0,5,60,"search"),state(0,6,80,"idle")]}]);
    await data.buildOverview(); assert.equal((await data.stateAt(0,30)).state,"unknown");
    const stillUnknown=await data.stateAt(0,50); assert.equal(stillUnknown.state,"unknown"); assert.equal(stillUnknown.start,20); assert.equal(stillUnknown.end,60);
    assert.equal((await data.stateAt(0,70)).state,"search");
    const r=await data.range(0,100,0,500); assert.equal(r.actualSamples,1); assert.equal(r.samples[0].record.state,"quiescence");
    assert.equal((await data.neighbor(0,null,1)).sequence,4);
  });
  await test("invalid-clock sample marks status unknown without rejecting the chunk",async()=>{
    const {data}=fixture([{start:0,end:100,records:[state(0,1,20,"wait_helpers"),sample(0,2,null,{state:"unknown"}),state(0,3,80,"idle")]}]);
    await data.buildOverview(); assert.equal(data.failed.size,0); assert.equal((await data.stateAt(0,40)).state,"unknown");
    assert.equal((await data.range(0,100,0,500)).actualSamples,0);
  });
  await test("missing chunk creates unknown state until the next reliable event",async()=>{
    const f=fixture([{start:0,end:30,records:[state(0,1,20,"wait_helpers")]},
      {start:31,end:60,records:[]},{start:61,end:100,records:[state(0,3,80,"search")]}]);
    f.files.delete(f.meta.chunks[1].file); await f.data.buildOverview(); assert.equal(f.data.failed.size,1);
    assert.equal((await f.data.stateAt(0,50)).state,"unknown"); assert.equal((await f.data.stateAt(0,90)).state,"search");
  });
  await test("overlapping chunk bounds cannot hide a later collected earlier-time thread state",async()=>{
    const {data}=fixture([
      {start:100,end:1000,records:[state(0,1,100,"search"),state(1,1,1000,"idle")]},
      {start:200,end:1100,records:[state(0,2,200,"search"),state(1,2,1100,"idle")]},
      {start:600,end:600,records:[state(0,3,600,"wait_helpers")]}
    ],{end_ms:1500});
    await data.buildOverview();
    const r=await data.range(700,800,0,500), intervals=r.intervals.filter(i=>i.thread_id===0);
    assert.equal(intervals.length,1); assert.equal(intervals[0].state,"wait_helpers"); assert.equal(intervals[0].start,600);
    const at=await data.stateAt(0,750); assert.equal(at.state,"wait_helpers"); assert.equal(at.start,600);
    assert.ok(data.overview[0].states[data.bin(750)] & (1<<T.STATES.indexOf("wait_helpers")));
    assert.ok(data.cache.size<=2);
  });
  await test("null-clock events in a globally future chunk still invalidate earlier thread state",async()=>{
    const {data}=fixture([
      {start:100,end:1000,records:[state(0,1,100,"wait_helpers"),state(1,1,1000,"idle")]},
      {start:1100,end:1200,records:[sample(0,2,null,{state:"unknown"}),state(0,3,1200,"search"),state(1,2,1100,"idle")]}
    ],{end_ms:1500});
    await data.buildOverview(); const r=await data.range(700,800,0,500);
    assert.equal(r.intervals.find(i=>i.thread_id===0 && i.start<=750 && i.end>=750).state,"unknown");
    const at=await data.stateAt(0,750); assert.equal(at.state,"unknown"); assert.equal(at.end,1200);
    assert.ok(data.cache.size<=2);
  });
  await test("stale cross-chunk sequences never revive state after a newer gap",async()=>{
    const {data}=fixture([{start:0,end:20,records:[gap(0,10,20)]},
      {start:30,end:30,records:[state(0,5,30,"idle"),sample(0,6,30)]},
      {start:40,end:40,records:[sample(0,11,40)]},
      {start:60,end:60,records:[state(0,12,60,"search")]}]);
    await data.buildOverview(); assert.equal((await data.stateAt(0,35)).state,"unknown");
    assert.equal((await data.stateAt(0,50)).state,"unknown");
    assert.equal((await data.stateAt(0,70)).state,"search");
    assert.equal((await data.neighbor(0,null,1)).sequence,11);
    const r=await data.range(30,39,0,500); assert.equal(r.intervals.find(i=>i.thread_id===0).state,"unknown");
    assert.equal(r.actualSamples,0);
  });
  await test("corrupt or foreign chunks are isolated and never treated as known idle",async()=>{
    const f=fixture([{start:0,end:100,records:[]}]); f.files.set(f.meta.chunks[0].file,{text:async()=>"{not json"});
    await f.data.buildOverview(); assert.equal(f.data.failed.size,1); assert.equal((await f.data.stateAt(1,50)).state,"unknown");
  });
  await test("chunk node IDs and request time validity are checked",()=>{
    const f=fixture([{start:0,end:100,records:[]}]); const info=f.meta.chunks[0];
    const raw={timeline_version:1,search_id:7,iteration_depth:8,start_ms:0,end_ms:100,records:[sample(0,1,20,{node_id:99})]};
    assert.throws(()=>T.parseChunk(raw,{...info,records:1},session,iteration,3),/节点引用/);
    raw.records[0]=sample(0,1,20,{request_time_ms:21});
    assert.throws(()=>T.parseChunk(raw,{...info,records:1},session,iteration,3),/路径样本/);
  });
  await test("only manifest references are loaded, and parsed chunk cache stays at two",async()=>{
    const f=fixture(Array.from({length:5},(_,i)=>({start:i*20,end:i*20+19,records:[sample(0,i+1,i*20+10)]})));
    f.files.set("depth-8-timeline-999999.json",{text:async()=>{throw Error("orphan must not load");}});
    await f.data.buildOverview(); assert.equal(f.data.cache.size,2); assert.equal(f.reads.size,5);
    const before=[...f.reads.values()].reduce((a,b)=>a+b,0); await f.data.range(40,58,0,400);
    const after=[...f.reads.values()].reduce((a,b)=>a+b,0); assert.equal(after-before,1); assert.equal(f.data.cache.size,2);
  });
  await test("request history absence remains null instead of invented request time",async()=>{
    const {data}=fixture([{start:0,end:100,records:[sample(0,1,20,{request_time_ms:null})]}]);
    await data.buildOverview(); assert.equal((await data.range(0,100,0,500)).samples[0].record.request_time_ms,null);
  });
  await test("cutoff marks every thread unknown even if the last event was waiting",async()=>{
    const {data}=fixture([{start:0,end:40,records:[state(0,1,20,"wait_helpers")]}],{incomplete:true,cutoff_ms:40,stop_reason:"byte_limit"});
    await data.buildOverview(); assert.equal((await data.stateAt(0,30)).state,"wait_helpers");
    assert.equal((await data.stateAt(0,50)).state,"unknown"); assert.equal((await data.stateAt(1,50)).state,"unknown");
  });
  await test("neighbor navigation preserves multiple observations at the same millisecond",async()=>{
    const {data}=fixture([{start:0,end:100,records:[sample(0,1,20),sample(0,2,20),sample(0,3,40)]}]);
    await data.buildOverview(); const first=await data.neighbor(0,null,1), second=await data.neighbor(0,first,1);
    assert.equal(first.sequence,1); assert.equal(second.sequence,2);
    assert.equal((await data.neighbor(0,second,-1)).sequence,1);
  });
  await test("overview and visible sample density are bounded and preserve exact counts",async()=>{
    const records=Array.from({length:3000},(_,i)=>sample(0,i+1,Math.floor(i/30),{node_id:i===100?2:1,normal_ply:i===100?2:1}));
    const {data}=fixture([{start:0,end:100,records}]); await data.buildOverview(); const r=await data.range(0,100,0,80);
    assert.equal(r.actualSamples,3000); assert.ok(r.samples.length<=80); assert.equal(r.samples.reduce((a,b)=>a+b.count,0),3000);
    assert.equal(data.overview[0].counts.length,640); assert.equal(data.overview[0].counts.reduce((a,b)=>a+b,0),3000);
    assert.ok(r.samples.some(x=>x.record.node_id===2));
  });
  const prefixTree={depth:8,byId:new Map([
    [0,{id:0,parent:null,pathDepth:1}], [1,{id:1,parent:0,pathDepth:2}],
    [2,{id:2,parent:1,pathDepth:3}], [3,{id:3,parent:1,pathDepth:3}],
    [4,{id:4,parent:2,pathDepth:4}]
  ])};
  await test("a pixel preserves the common prefix across returning branches and deeper representatives",async()=>{
    const records=[2,3,2,4].map((node_id,index)=>sample(0,index+1,20+index,{node_id,normal_ply:node_id===4?3:2}));
    const {data}=fixture([{start:0,end:100,records}],{},prefixTree);
    await data.buildOverview();const result=await data.range(0,100,0,1),item=result.samples[0];
    assert.equal(result.actualSamples,4);assert.equal(result.samples.length,1);assert.equal(item.count,4);
    assert.equal(item.record.node_id,4);assert.equal(item.commonNode,1);
    assert.equal(item.firstTime,20);assert.equal(item.lastTime,23);
  });
  await test("unrecorded paths or missing parent links do not claim a shared pixel prefix",async()=>{
    const records=[2,null,4].map((node_id,index)=>sample(0,index+1,20+index,{node_id,normal_ply:node_id===4?3:2}));
    const {data}=fixture([{start:0,end:100,records}],{},prefixTree);
    await data.buildOverview();const item=(await data.range(0,100,0,1)).samples[0];
    assert.equal(item.commonNode,null);assert.equal(item.count,3);assert.equal(item.record.node_id,4);
    const missing=fixture([{start:0,end:100,records:[sample(0,1,20),sample(0,2,21,{node_id:2})]}]);
    await missing.data.buildOverview();assert.equal((await missing.data.range(0,100,0,1)).samples[0].commonNode,null);
  });
  await test("pixel prefix and time bounds accumulate across overlapping chunks independently of collection order",async()=>{
    const {data}=fixture([
      {start:20,end:40,records:[sample(0,1,30,{node_id:2}),sample(0,2,40,{node_id:2})]},
      {start:10,end:50,records:[sample(0,3,10,{node_id:3}),sample(0,4,50,{node_id:4,normal_ply:3})]},
      {start:35,end:35,records:[sample(0,5,35,{node_id:2})]}
    ],{},prefixTree);
    await data.buildOverview();const item=(await data.range(0,100,0,1)).samples[0];
    assert.equal(item.commonNode,1);assert.equal(item.firstTime,10);assert.equal(item.lastTime,50);
    assert.equal(item.count,5);assert.equal(item.record.node_id,4);assert.ok(data.cache.size<=2);
  });
  await test("a cancelled load cannot fill a later report's cache",async()=>{
    const f=fixture([{start:0,end:100,records:[]}]); let release;
    f.files.set(f.meta.chunks[0].file,{text:()=>new Promise(done=>{release=done;})});
    const loading=f.data.load(0); f.data.cancel(); release(JSON.stringify({timeline_version:1,search_id:7,iteration_depth:8,start_ms:0,end_ms:100,records:[]}));
    assert.equal(await loading,null); assert.equal(f.data.cache.size,0);
  });
  console.log("Passed "+checks+" logical timeline model tests."); return {checks};
}
module.exports=main();
