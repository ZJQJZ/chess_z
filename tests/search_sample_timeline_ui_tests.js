/* Canvas handler smoke checks with a simulated DOM; Chrome still validates layout. */
"use strict";
const assert=require("node:assert/strict");
const base=require("./search_sample_viewer_ui_tests.js");
const {browser,file,importFiles,fixture,filesFor,boardCell,rootFen,settle}=base.harness;
let checks=0;
async function test(name,fn) { await fn(); ++checks; console.log("ok timeline-ui "+checks+" - "+name); }
function canvas(ui) {
  const create=ui.document.createElement;
  ui.document.createElement=tag=>{
    const el=create(tag);
    if(tag==="canvas") {
      const operations=[], saved=[];let transform=[1,0,0,1,0,0];
      const properties={fillStyle:"#000",strokeStyle:"#000",font:"10px sans-serif",lineWidth:1,
        textAlign:"start",textBaseline:"alphabetic",globalAlpha:1};
      const record=(method,values)=>{
        const args=[...values],rawArgs=[...values];
        if(["fillText","strokeText"].includes(method)) {
          const x=args[1],y=args[2];args[1]=transform[0]*x+transform[2]*y+transform[4];
          args[2]=transform[1]*x+transform[3]*y+transform[5];
        } else if(["fillRect","strokeRect","clearRect","rect","roundRect","moveTo","lineTo","arc"].includes(method)) {
          const x=args[0],y=args[1];args[0]=transform[0]*x+transform[2]*y+transform[4];
          args[1]=transform[1]*x+transform[3]*y+transform[5];
          if(["fillRect","strokeRect","clearRect","rect","roundRect"].includes(method)) {
            args[2]*=transform[0];args[3]*=transform[3];
          }
        }
        operations.push({method,args,rawArgs,...properties});
      };
      const context=new Proxy(properties, {
        get:(obj,key)=>{
          if(key==="measureText") return text=>({width:String(text).length*7.2});
          if(key==="save") return ()=>{saved.push({properties:{...properties},transform:[...transform]});record("save",[]);};
          if(key==="restore") return ()=>{const state=saved.pop();if(state){Object.assign(properties,state.properties);transform=state.transform;}record("restore",[]);};
          if(key==="setTransform") return (...matrix)=>{transform=matrix;record("setTransform",matrix);};
          if(key==="resetTransform") return ()=>{transform=[1,0,0,1,0,0];record("resetTransform",[]);};
          if(key==="translate") return (x,y)=>{transform[4]+=transform[0]*x+transform[2]*y;transform[5]+=transform[1]*x+transform[3]*y;record("translate",[x,y]);};
          if(key==="scale") return (x,y)=>{transform[0]*=x;transform[1]*=x;transform[2]*=y;transform[3]*=y;record("scale",[x,y]);};
          if(key==="canvas") return el;
          if(key in obj) return obj[key];
          return (...args)=>record(String(key),args);
        },
        set:(obj,key,value)=>{obj[key]=value;return true;}
      });
      el.drawOperations=operations;
      el.getContext=()=>context; el.getBoundingClientRect=()=>({left:0,top:0});
    }
    return el;
  };
  ui.$("timeline-plot").clientWidth=980; ui.$("timeline-plot").clientHeight=510;
}
async function ready(ui) {
  for(let n=0;n<200;++n) {
    await settle();
    if(ui.$("timeline-range-info").textContent && !ui.$("timeline-loading").textContent) return;
  }
  throw Error("timeline did not finish loading");
}
async function openDemo() {
  const ui=browser(); canvas(ui); await ui.$("demo").fire("click");
  ui.demoRootFen=rootFen(ui);
  await ui.$("view-timeline").fire("click"); await ready(ui); return ui;
}
function getCanvas(ui) { return ui.$("timeline-canvas-host").children[0]; }
function lastFrame(ui) {
  const operations=getCanvas(ui).drawOperations;
  const index=operations.findLastIndex(operation=>operation.method==="clearRect");
  return operations.slice(index<0?0:index);
}
function assertSampleCaptions(ui,selectedTime) {
  const ratio=ui.window.devicePixelRatio||1;
  const captions=lastFrame(ui).filter(op=>op.method==="fillText"&&op.rawArgs[2]===72);
  assert.ok(captions.some(op=>op.args[0]===selectedTime&&op.fillStyle==="#245cc5"),
    "the selected sample lost its time caption");
  const boxes=captions.map(op=>({start:op.args[1],end:op.args[1]+String(op.args[0]).length*7.3*ratio}))
    .sort((a,b)=>a.start-b.start);
  for(let index=1;index<boxes.length;++index)
    assert.ok(boxes[index-1].end<=boxes[index].start,"sample-time captions overlap");
  return captions;
}
function timelineFixture(count=20,{sameTime=false,deep=false,timeOffset=0}={}) {
  const f=fixture(deep?8:2);
  if(deep) {
    const moves=["a0a9","e9e8","a9b9","e8e9","b9a9","e9e8","a9b9","e8e9"];
    f.report.nodes=[{id:0,parent:null,kind:"root",move:null,inclusive:count,self:0,truncated_self:0,unrecorded_self:0},
      ...moves.map((move,index)=>({id:index+1,parent:index,kind:"normal_move",move,
        inclusive:count,self:index===moves.length-1?count:0,truncated_self:0,unrecorded_self:0}))];
    f.report.max_normal_ply=8;f.report.max_quiescence_ply=0;
  } else {
    f.report.nodes.forEach(node=>{node.inclusive=count;node.self=node.id===3?count:0;});
  }
  f.report.valid_samples=count;
  const name="depth-"+f.report.iteration_depth+"-timeline-000001.json";
  const duration=Math.max(100,count*20+20);
  const records=Array.from({length:count},(_,i)=>({type:"sample",thread_id:0,sequence:i+2,
    time_ms:timeOffset+(sameTime?50:(i+1)*20),state:deep?"search":"quiescence",request_id:i+1,
    request_time_ms:timeOffset+(sameTime?40:i*20),node_id:deep?8:3,normal_ply:deep?8:1,
    quiescence_ply:deep?0:1,truncated:false,path_omitted:false}));
  f.report.timeline_version=1;f.report.timeline={start_ms:timeOffset,end_ms:timeOffset+duration,finished:true,incomplete:false,
    initial_states:[{thread_id:0,state:"search",time_ms:timeOffset,sequence:1},{thread_id:1,state:"idle",time_ms:timeOffset,sequence:1}],
    chunks:[{file:name,start_ms:timeOffset,end_ms:timeOffset+duration,records:records.length}]};
  return {f,files:[...filesFor(f),file(name,{timeline_version:1,search_id:7,
    iteration_depth:f.report.iteration_depth,start_ms:timeOffset,end_ms:timeOffset+duration,records})]};
}
function runFixture(times,{nodes,nodeIds=times.map(()=>3),states=[],end=times.at(-1)+20}={}) {
  const f=fixture();
  f.report.nodes=(nodes||f.report.nodes).map(node=>({...node,inclusive:0,self:0,truncated_self:0,unrecorded_self:0}));
  const byId=new Map(f.report.nodes.map(node=>[node.id,node]));
  const records=times.map((time_ms,index)=>{
    let node=byId.get(nodeIds[index]),normal_ply=0,quiescence_ply=0;
    node.self++;
    while(node) {
      node.inclusive++;
      if(node.kind==="normal_move")normal_ply++;
      if(node.kind==="quiescence_move")quiescence_ply++;
      node=byId.get(node.parent);
    }
    return {type:"sample",thread_id:0,time_ms,state:quiescence_ply?"quiescence":"search",
      request_id:index+1,request_time_ms:Math.max(0,time_ms-1),node_id:nodeIds[index],
      normal_ply,quiescence_ply,truncated:false,path_omitted:false};
  });
  f.report.valid_samples=times.length;
  f.report.max_normal_ply=Math.max(...records.map(r=>r.normal_ply));
  f.report.max_quiescence_ply=Math.max(...records.map(r=>r.quiescence_ply));
  records.push(...states.map(state=>({type:"state",thread_id:0,...state})));
  records.sort((a,b)=>a.time_ms-b.time_ms);records.forEach((r,index)=>{r.sequence=index+2;});
  const name="depth-2-timeline-000001.json";
  f.report.timeline_version=1;f.report.timeline={start_ms:0,end_ms:end,finished:true,incomplete:false,
    initial_states:[{thread_id:0,state:"search",time_ms:0,sequence:1},{thread_id:1,state:"idle",time_ms:0,sequence:1}],
    chunks:[{file:name,start_ms:0,end_ms:end,records:records.length}]};
  return {f,files:[...filesFor(f),file(name,{timeline_version:1,search_id:7,iteration_depth:2,start_ms:0,end_ms:end,records})]};
}
async function openFixture(data,plotHeight=510) {
  const ui=browser();canvas(ui);ui.$("timeline-plot").clientHeight=plotHeight;
  await importFiles(ui,data.files);await ui.$("view-timeline").fire("click");await ready(ui);return ui;
}
async function clickAt(ui,x,y) {
  const c=getCanvas(ui); await c.fire("pointerdown",{clientX:x,clientY:y,pointerId:1});
  await c.fire("pointerup",{clientX:x,clientY:y,pointerId:1}); await settle();
}
function timeRange(ui) {
  return [Number(ui.$("timeline-start").value),Number(ui.$("timeline-end").value)];
}
async function applyRange(ui,start=400,end=1200) {
  ui.$("timeline-start").value=start;ui.$("timeline-end").value=end;
  await ui.$("timeline-apply").fire("click");await settle();
}
async function wheelAt(ui,properties) {
  const event=await getCanvas(ui).fire("wheel",{clientX:490,clientY:210,deltaX:0,deltaY:0,deltaMode:0,...properties});
  await settle();return event;
}
function stackRects(ui) {
  return lastFrame(ui).filter(op=>op.method==="fillRect"&&["#e8f0fc","#fff0d7"].includes(op.fillStyle));
}
function plotBounds(ui) {
  const grid=lastFrame(ui).filter(op=>op.method==="moveTo"&&op.strokeStyle==="#edf1f5");
  return {left:Math.min(...grid.map(op=>op.rawArgs[0])),right:Math.max(...grid.map(op=>op.rawArgs[0]))};
}
function timeX(ui,time) {
  const {left,right}=plotBounds(ui),[start,end]=timeRange(ui);
  return left+(time-start)/(end-start)*(right-left);
}
async function main() {
  await base;
  await test("timeline remains lazy in default hotspot mode",async()=>{
    const ui=browser(); await ui.$("demo").fire("click");
    assert.equal(ui.$("timeline-canvas-host").children.length,0);
    assert.equal(ui.$("hotspot-view").hidden,false);
  });
  await test("legacy report keeps hotspots and explains missing timeline",async()=>{
    const ui=browser(); canvas(ui); await importFiles(ui,filesFor(fixture()));
    await ui.$("view-timeline").fire("click");
    assert.match(ui.$("timeline-message").textContent,/未记录时间线/);
    assert.equal(ui.$("timeline-canvas-host").children.length,0);
    await ui.$("view-hotspot").fire("click"); assert.equal(ui.$("hotspot-view").hidden,false);
  });
  await test("unknown iteration boundaries are explicitly warned without hiding valid samples",async()=>{
    const ui=browser(); canvas(ui); const f=fixture();
    f.report.timeline_version=1; f.report.timeline={start_ms:0,end_ms:100,start_known:false,end_known:false,boundary_unknown:true,
      initial_states:[],chunks:[]};
    await importFiles(ui,filesFor(f)); await ui.$("view-timeline").fire("click"); await ready(ui);
    assert.match(ui.$("timeline-message").textContent,/迭代边界记录缺失/);
    assert.equal(ui.$("timeline-plot").hidden,false);
  });
  await test("a running checkpoint without its future end boundary is not reported as lost",async()=>{
    const ui=browser(); canvas(ui); const f=fixture();
    f.report.timeline_version=1; f.report.timeline={start_ms:0,end_ms:100,start_known:true,end_known:false,boundary_unknown:false,
      finished:false,initial_states:[],chunks:[]};
    await importFiles(ui,filesFor(f)); await ui.$("view-timeline").fire("click"); await ready(ui);
    assert.doesNotMatch(ui.$("timeline-message").textContent,/边界记录缺失/);
  });
  await test("a sample navigation replays its path and waits clear every board action",async()=>{
    const ui=await openDemo();
    assert.equal(ui.$("selected-move").textContent,"b2b9"); assert.notEqual(rootFen(ui),ui.demoRootFen);
    const waits=lastFrame(ui).filter(op=>op.method==="fillRect"&&op.fillStyle==="#9ba4b0");
    const band=waits.reduce((best,op)=>!best||op.args[2]>best.args[2]?op:best,null);
    assert.ok(band,"waiting state has no visible status band");
    await clickAt(ui,band.args[0]+band.args[2]/2,band.args[1]+band.args[3]/2);
    assert.equal(ui.$("selected-move").textContent,"等待辅助线程");
    assert.equal(ui.$("board").querySelectorAll(".square").length,0); assert.equal(rootFen(ui),"");
    assert.equal(ui.$("copy-fen").disabled,true); assert.equal(ui.$("copy-path").disabled,true);
    assert.equal(ui.$("parent").disabled,true); assert.equal(ui.$("flip").disabled,true);
    assert.equal(ui.$("path").children.length,0); assert.equal(ui.$("node-stats").hidden,true);
    assert.match(ui.$("timeline-selection").textContent,/50 ms.*102 ms.*52 ms/);
    await ui.$("view-hotspot").fire("click");
    assert.notEqual(rootFen(ui),""); assert.equal(ui.$("flip").disabled,false);
  });
  await test("waiting fills only the fixed status band and never a full-height stack background",async()=>{
    const ui=await openDemo();
    const waits=lastFrame(ui).filter(op=>op.method==="fillRect"&&op.fillStyle==="#9ba4b0");
    assert.ok(waits.length);
    for(const wait of waits) {
      assert.ok(wait.args[3]<=28,"WAIT background extends into the stack");
      assert.ok(wait.args[1]>=28&&wait.args[1]+wait.args[3]<=65,"WAIT is outside the fixed status band");
    }
    assert.ok(lastFrame(ui).some(op=>op.method==="fillText"&&op.args[0]==="b2b9"),"move names were not painted");
    assert.ok(lastFrame(ui).some(op=>op.method==="arc"),"sampling instants are not marked");
    assert.ok(lastFrame(ui).some(op=>op.method==="setLineDash"&&op.args[0].length),"sample guides are missing");
  });
  await test("range, thread and nearest-sample controls preserve iteration identity",async()=>{
    const ui=await openDemo(); ui.$("timeline-thread").value="1"; await ui.$("timeline-thread").fire("change"); await settle();
    assert.equal(ui.$("selected-move").textContent,"h2h9"); assert.match(ui.$("timeline-selection").textContent,/线程 1/);
    ui.$("timeline-start").value="35"; ui.$("timeline-end").value="55"; await ui.$("timeline-apply").fire("click"); await settle();
    assert.match(ui.$("timeline-range-info").textContent,/35 ms.*55 ms/);
    await ui.$("timeline-reset").fire("click"); await settle(); assert.equal(ui.$("timeline-start").value,"0");
    assert.equal(ui.$("timeline-end").value,"180");
  });
  await test("a failed import or depth change restores the old timeline and its sample navigation",async()=>{
    for(const mode of ["import","depth"]) {
      let ui,fail,expected;
      if(mode==="import") {
        ui=await openDemo();
        const bad=fixture();
        fail=()=>importFiles(ui,[file("session.json",bad.session,"broken"),file("depth-2.json","{","broken")]);
        expected={move:"h2e2",request:6,destination:"e2",origin:"h2",piece:"炮"};
      } else {
        const nodes=[{id:0,parent:null,kind:"root",move:null},{id:1,parent:0,kind:"normal_move",move:"a0a9"},
          {id:2,parent:0,kind:"normal_move",move:"i0i9"}];
        const data=runFixture([20,40],{nodes,nodeIds:[1,2]});
        data.files.push(file("depth-1.json","{"));ui=await openFixture(data);
        fail=async()=>{ui.$("depth-select").value="1";await ui.$("depth-select").fire("change");};
        expected={move:"i0i9",request:2,destination:"i9",origin:"i0",piece:"車"};
      }
      const name=ui.$("session-name").textContent,depth=ui.$("depth-select").value,fen=rootFen(ui);
      await fail();await ready(ui);
      assert.equal(ui.$("notice").className,"error");assert.match(ui.$("notice").textContent,/depth-[12]\.json/);
      assert.equal(ui.$("session-name").textContent,name);assert.equal(ui.$("depth-select").value,depth);
      assert.equal(rootFen(ui),fen,"the failed load changed the old timeline's displayed sample");
      await ui.$("timeline-next").fire("click");await settle();
      assert.match(ui.$("timeline-selection").textContent,new RegExp("请求 #"+expected.request+"\\b"));
      assert.equal(ui.$("selected-move").textContent,expected.move);
      assert.equal(boardCell(ui,expected.destination).textContent,expected.piece);
      assert.equal(boardCell(ui,expected.origin).textContent,"·");
      assert.notEqual(rootFen(ui),fen,"the restored timeline did not replay the next sample");
    }
  });
  await test("painted move bands and phase prefixes replay the selected sample and expose a tooltip",async()=>{
    const ui=await openDemo();ui.$("timeline-thread").value="1";await ui.$("timeline-thread").fire("change");await settle();
    assert.equal(ui.$("selected-move").textContent,"h2h9");
    const move=lastFrame(ui).find(op=>op.method==="fillText"&&op.args[0]==="b2b9"&&op.args[1]>110);
    assert.ok(move,"readable move band was not painted");
    await getCanvas(ui).fire("pointermove",{clientX:move.args[1]+2,clientY:move.args[2]-6});
    assert.equal(ui.$("timeline-tooltip").hidden,false);assert.match(ui.$("timeline-tooltip").textContent,/b2b9.*43 ms.*线程 1/);
    await clickAt(ui,move.args[1]+2,move.args[2]-6);
    assert.equal(ui.$("selected-move").textContent,"b2b9");
    const phase=lastFrame(ui).find(op=>op.method==="fillText"&&op.args[0]==="静态阶段"&&op.args[1]>110);
    assert.ok(phase);await clickAt(ui,phase.args[1]+2,phase.args[2]-6);
    assert.equal(ui.$("board-title").textContent,"进入静态搜索时的棋盘");
    await getCanvas(ui).fire("pointerleave");assert.equal(ui.$("timeline-tooltip").hidden,true);
  });
  await test("vertical path scrolling leaves the time axis and selected-thread state header fixed",async()=>{
    const ui=await openFixture(timelineFixture(20,{deep:true}),300);
    assert.ok(parseFloat(ui.$("timeline-spacer").style.height)>300);
    const before=lastFrame(ui),header=before.filter(op=>op.method==="fillText"&&(/ ms$/.test(op.args[0])&&op.args[2]===18||op.args[0]==="思考时间"));
    const path=before.find(op=>op.method==="fillText"&&op.args[0]==="4. e8e9");
    assert.ok(path);assert.ok(header.length>2);
    ui.$("timeline-plot").scrollTop=56;await ui.$("timeline-plot").fire("scroll");await settle();
    const after=lastFrame(ui),newHeader=after.filter(op=>op.method==="fillText"&&(/ ms$/.test(op.args[0])&&op.args[2]===18||op.args[0]==="思考时间"));
    assert.deepEqual(newHeader.map(op=>op.args),header.map(op=>op.args));
    const shifted=after.find(op=>op.method==="fillText"&&op.args[0]==="4. e8e9");
    assert.ok(shifted);assert.equal(path.args[2]-shifted.args[2],56);
    const stateLabel=after.find(op=>op.method==="fillText"&&op.args[0]==="线程 0");assert.equal(stateLabel.args[2],50);
    ui.window.devicePixelRatio=2;await ui.$("timeline-plot").fire("scroll");await settle();
    assert.equal(getCanvas(ui).width,1960);assert.equal(getCanvas(ui).style.width,"980px");
    const highDpi=lastFrame(ui),axis=highDpi.find(op=>op.method==="fillText"&&op.args[0]==="思考时间");
    assert.equal(axis.rawArgs[2],18);assert.equal(axis.args[2],36);
    const band=highDpi.find(op=>op.method==="fillText"&&op.args[0]==="a9b9"&&op.args[1]>236&&op.args[2]>224);
    assert.ok(band);await clickAt(ui,band.args[1]/2+2,band.args[2]/2-6);
    assert.equal(ui.$("selected-move").textContent,"a9b9","high-DPI pointer coordinates missed a move band");
    assertSampleCaptions(ui,"20 ms");
  });
  await test("dense reports open a readable window and controls separate scrolling from time zoom",async()=>{
    const data=timelineFixture(80),ui=await openFixture(data);
    const span=()=>Number(ui.$("timeline-end").value)-Number(ui.$("timeline-start").value);
    assert.ok(span()<data.f.report.timeline.end_ms/2,"initial view did not focus approximately ten samples");
    assert.match(ui.$("timeline-range-info").textContent,/10 样本/);
    assert.match(ui.$("timeline-selection").textContent,/请求 #1/);
    const moveLabels=lastFrame(ui).filter(op=>op.method==="fillText"&&op.args[0]==="a0a9"&&op.args[1]>118);
    assert.equal(moveLabels.length,1,"an unchanged move prefix should have one label across neighboring samples");
    for(const text of moveLabels) assert.ok(lastFrame(ui).some(op=>op.method==="fillRect"&&
      op.fillStyle==="#e8f0fc"&&op.args[2]>500&&text.args[1]>op.args[0]&&
      text.args[1]<op.args[0]+op.args[2]&&text.args[2]>op.args[1]&&
      text.args[2]<op.args[1]+op.args[3]),"the unchanged prefix was not painted as a continuous readable band");
    await ui.$("timeline-reset").fire("click");await settle();assert.match(ui.$("timeline-density").textContent,/点较密/);
    await ui.$("timeline-readable").fire("click");await settle();assert.ok(span()<data.f.report.timeline.end_ms/2);
    const readableSpan=span();await ui.$("timeline-zoom-out").fire("click");await settle();assert.ok(span()>readableSpan);
    assertSampleCaptions(ui,"20 ms");
    await ui.$("timeline-zoom-in").fire("click");await settle();assert.ok(span()<=readableSpan+2);
    assertSampleCaptions(ui,"20 ms");
    const oldRange=[ui.$("timeline-start").value,ui.$("timeline-end").value];
    const wheel=await getCanvas(ui).fire("wheel",{clientX:420,clientY:210,deltaY:90});await settle();
    assert.equal(wheel.defaultPrevented,false);assert.deepEqual([ui.$("timeline-start").value,ui.$("timeline-end").value],oldRange);
    const zoom=await getCanvas(ui).fire("wheel",{clientX:420,clientY:210,deltaY:-120,ctrlKey:true});await settle();
    assert.equal(zoom.defaultPrevented,true);assert.ok(span()<readableSpan);
    const start=Number(ui.$("timeline-start").value);
    const pan=await getCanvas(ui).fire("wheel",{clientX:420,clientY:210,deltaY:90,shiftKey:true});await settle();
    assert.equal(pan.defaultPrevented,true);assert.ok(Number(ui.$("timeline-start").value)>start);
    const longTimes=await openFixture(timelineFixture(20,{timeOffset:1000000000}));
    assert.ok(assertSampleCaptions(longTimes,"1,000,000,020 ms").length>1,
      "collision regression must compare more than one long timestamp caption");
    longTimes.$("timeline-start").value="1000000020";longTimes.$("timeline-end").value="1000000200";
    await longTimes.$("timeline-apply").fire("click");await settle();
    const rows=new Map();
    for(const op of lastFrame(longTimes).filter(op=>op.method==="fillRect"&&["#e8f0fc","#fff0d7"].includes(op.fillStyle))) {
      if(!rows.has(op.args[1]))rows.set(op.args[1],[]);
      rows.get(op.args[1]).push({start:op.args[0],end:op.args[0]+op.args[2]});
    }
    assert.ok(rows.size>1);
    for(const row of rows.values()) {
      row.sort((a,b)=>a.start-b.start);
      for(let i=1;i<row.length;++i)assert.ok(row[i-1].end<=row[i].start+1e-7,
        "clamping the time range made neighboring move bands overlap");
    }
    assertSampleCaptions(longTimes,"1,000,000,020 ms");
  });
  await test("neighboring samples share one named band per prefix while markers and stepping remain individual",async()=>{
    const ui=await openFixture(runFixture([20,40,60,80]));
    await ui.$("timeline-reset").fire("click");await settle();
    const rows=new Map();
    for(const rect of stackRects(ui)) {
      if(!rows.has(rect.rawArgs[1]))rows.set(rect.rawArgs[1],[]);
      rows.get(rect.rawArgs[1]).push(rect);
    }
    assert.equal(rows.size,4,"the ordinary and quiescence prefix levels must all remain visible");
    for(const row of rows.values()) {
      assert.equal(row.length,1,"an unchanged prefix was split into separate sample boxes");
      assert.ok(row[0].rawArgs[2]>600,"adjacent samples did not form a long band");
    }
    const labels=lastFrame(ui).filter(op=>op.method==="fillText"&&op.rawArgs[1]>118);
    for(const text of ["a0a9","静态阶段","e9e8"])
      assert.equal(labels.filter(op=>op.args[0]===text).length,1,"a connected prefix should be named once: "+text);
    assert.equal(lastFrame(ui).filter(op=>op.method==="arc").length,4,"merging prefixes erased real sampling markers");
    await ui.$("timeline-next").fire("click");await settle();
    assert.match(ui.$("timeline-selection").textContent,/请求 #2\b/);
    await ui.$("timeline-prev").fire("click");await settle();
    assert.match(ui.$("timeline-selection").textContent,/请求 #1\b/);
    const dense=await openFixture(timelineFixture(1600));
    await dense.$("timeline-reset").fire("click");await settle();
    const denseNames=lastFrame(dense).filter(op=>op.method==="fillText"&&op.args[0]==="a0a9"&&op.rawArgs[1]>118);
    assert.equal(denseNames.length,1,"pixel-binned identical prefixes lost their readable band name");
    assert.ok(stackRects(dense).some(op=>op.rawArgs[2]>800),"dense samples did not keep a continuous prefix band");
  });
  await test("the same move text under different parent prefixes never merges into one band",async()=>{
    const node=(id,parent,kind,move)=>({id,parent,kind,move});
    const nodes=[node(0,null,"root",null),node(1,0,"normal_move","a0a9"),node(2,1,"normal_move","e9e8"),
      node(3,0,"normal_move","i0i9"),node(4,3,"normal_move","e9e8")];
    const ui=await openFixture(runFixture([20,40,60,80],{nodes,nodeIds:[2,2,4,4]}));
    await ui.$("timeline-reset").fire("click");await settle();
    const labels=lastFrame(ui).filter(op=>op.method==="fillText"&&op.args[0]==="e9e8"&&op.rawArgs[1]>118);
    assert.equal(labels.length,2,"matching move names with different ancestors were falsely merged");
    const y=labels[0].rawArgs[2]-16,row=stackRects(ui).filter(op=>op.rawArgs[1]===y);
    assert.equal(row.length,2);assert.ok(row[0].rawArgs[0]+row[0].rawArgs[2]<=row[1].rawArgs[0]+1e-7);
    const rootRow=Math.min(...stackRects(ui).map(op=>op.rawArgs[1]));
    assert.equal(stackRects(ui).filter(op=>op.rawArgs[1]===rootRow).length,1,"the shared root prefix should still connect");
  });
  await test("waiting, idle, unknown intervals and long sampling gaps leave breaks in every prefix band",async()=>{
    for(const state of ["wait_helpers","idle","unknown"]) {
      const ui=await openFixture(runFixture([20,40,60,80],{states:[{time_ms:45,state},{time_ms:55,state:"search"}]}));
      await ui.$("timeline-reset").fire("click");await settle();
      const rows=new Map(),gap=timeX(ui,50);
      for(const rect of stackRects(ui)) {
        assert.ok(rect.rawArgs[0]+rect.rawArgs[2]<=gap||rect.rawArgs[0]>=gap,
          state+" was covered by a fabricated connected path band");
        if(!rows.has(rect.rawArgs[1]))rows.set(rect.rawArgs[1],[]);
        rows.get(rect.rawArgs[1]).push(rect);
      }
      assert.equal(rows.size,4);for(const row of rows.values())assert.equal(row.length,2,state+" did not split a prefix");
    }
    const ui=await openFixture(runFixture([20,40,180,200]));
    await ui.$("timeline-reset").fire("click");await settle();
    const gap=timeX(ui,110),rects=stackRects(ui);
    assert.equal(rects.length,8,"a long unobserved gap should divide all four prefix levels");
    for(const rect of rects)assert.ok(rect.rawArgs[0]+rect.rawArgs[2]<gap||rect.rawArgs[0]>gap,
      "the long sampling gap was filled by a path band");
  });
  await test("clicks and tooltips on either end of one band choose the nearest sample and the clicked prefix",async()=>{
    const ui=await openFixture(runFixture([20,40,60,80]));
    await ui.$("timeline-reset").fire("click");await settle();
    const label=lastFrame(ui).find(op=>op.method==="fillText"&&op.args[0]==="a0a9"&&op.rawArgs[1]>118);
    assert.ok(label);const y=label.rawArgs[2]-6,left=timeX(ui,24),right=timeX(ui,76);
    await getCanvas(ui).fire("pointermove",{clientX:left,clientY:y});
    assert.match(ui.$("timeline-tooltip").textContent,/a0a9.*20 ms.*线程 0/);
    await clickAt(ui,left,y);
    assert.match(ui.$("timeline-selection").textContent,/请求 #1\b/);
    assert.equal(ui.$("selected-move").textContent,"a0a9");
    assert.equal(ui.$("path").querySelectorAll("button").length,2,"clicking a prefix replayed the sample's deepest endpoint");
    const prefixFen=rootFen(ui);
    await ui.$("timeline-next").fire("click");await settle();
    assert.match(ui.$("timeline-selection").textContent,/请求 #2\b/);
    assert.equal(ui.$("selected-move").textContent,"e9e8","sample stepping should still replay the full sampled path");
    await getCanvas(ui).fire("pointermove",{clientX:right,clientY:y});
    assert.match(ui.$("timeline-tooltip").textContent,/a0a9.*80 ms.*线程 0/);
    await clickAt(ui,right,y);
    assert.match(ui.$("timeline-selection").textContent,/请求 #4\b/);
    assert.equal(ui.$("selected-move").textContent,"a0a9");assert.equal(rootFen(ui),prefixFen);
  });
  await test("horizontal trackpad gestures pan both ways without swallowing vertical scrolling",async()=>{
    const data=timelineFixture(80),ui=await openFixture(data);await applyRange(ui);
    const right=await wheelAt(ui,{deltaX:120,deltaY:3}),[from,to]=timeRange(ui);
    assert.equal(right.defaultPrevented,true);assert.ok(from>400);assert.equal(to-from,800);
    const left=await wheelAt(ui,{deltaX:-120,deltaY:-3});
    assert.equal(left.defaultPrevented,true);assert.deepEqual(timeRange(ui),[400,1200]);
    const vertical=await wheelAt(ui,{deltaX:4,deltaY:90});
    assert.equal(vertical.defaultPrevented,false);assert.deepEqual(timeRange(ui),[400,1200]);
    await wheelAt(ui,{deltaX:100000});
    assert.deepEqual(timeRange(ui),[data.f.report.timeline.end_ms-800,data.f.report.timeline.end_ms]);
    await wheelAt(ui,{deltaX:-100000});assert.deepEqual(timeRange(ui),[0,800]);
    await applyRange(ui);await wheelAt(ui,{shiftKey:true,deltaX:-90,deltaY:3});
    const shifted=timeRange(ui);assert.ok(shifted[0]<400);assert.equal(shifted[1]-shifted[0],800);
    await applyRange(ui);await wheelAt(ui,{deltaX:-90,deltaY:3});
    assert.deepEqual(timeRange(ui),shifted,"Shift chose minor-axis jitter instead of the horizontal gesture");
    await applyRange(ui);
    const c=getCanvas(ui);
    await c.fire("pointerdown",{clientX:500,clientY:210,pointerId:1,shiftKey:true});
    await c.fire("pointermove",{clientX:400,clientY:210,pointerId:1,shiftKey:true});
    await c.fire("pointerup",{clientX:400,clientY:210,pointerId:1,shiftKey:true});await settle();
    const dragged=timeRange(ui);assert.ok(dragged[0]>400);assert.equal(dragged[1]-dragged[0],800);
    await wheelAt(ui,{deltaX:40});assert.ok(timeRange(ui)[0]>dragged[0],"released drag blocked the next gesture");
  });
  await test("wheel pan normalizes line and page deltas to the visible time range",async()=>{
    const ui=await openFixture(timelineFixture(80));await applyRange(ui,400,1000);
    await wheelAt(ui,{deltaX:48});const pixels=timeRange(ui);
    await applyRange(ui,400,1000);await wheelAt(ui,{deltaX:3,deltaMode:1});
    assert.deepEqual(timeRange(ui),pixels,"line-mode horizontal movement was treated as pixels");
    await applyRange(ui,400,1000);await wheelAt(ui,{deltaX:1,deltaMode:2});
    assert.deepEqual(timeRange(ui),[1000,1600],"one page should pan one visible time window");
  });
  await test("modified wheel zoom responds gently to gesture size and ignores zero movement",async()=>{
    const ui=await openFixture(timelineFixture(80));await applyRange(ui);
    const span=()=>timeRange(ui)[1]-timeRange(ui)[0];
    const tiny=await wheelAt(ui,{ctrlKey:true,deltaY:-1}),tinySpan=span();
    assert.equal(tiny.defaultPrevented,true);assert.ok(tinySpan<800&&tinySpan>790,"tiny wheel movement caused a zoom jump");
    for(let n=0;n<7;++n)await wheelAt(ui,{ctrlKey:true,deltaY:-1});
    assert.ok(span()<tinySpan&&span()>740,"a short sequence of pinch events zoomed too quickly");
    await applyRange(ui);await wheelAt(ui,{ctrlKey:true,deltaY:-20});const largerSpan=span();
    assert.ok(largerSpan<730&&largerSpan>690,"a moderate gesture should zoom noticeably without jumping");
    await applyRange(ui);await wheelAt(ui,{metaKey:true,deltaY:-20});assert.equal(span(),largerSpan);
    await applyRange(ui);await wheelAt(ui,{ctrlKey:true,deltaY:-10000});
    assert.ok(span()<640&&span()>560,"a large wheel packet bypassed the per-event zoom limit");
    await applyRange(ui);await wheelAt(ui,{ctrlKey:true,deltaY:10000});
    assert.ok(span()>1000&&span()<1120,"zoom-out made a large discontinuous jump");
    const before=timeRange(ui);await wheelAt(ui,{ctrlKey:true,deltaY:0});
    await wheelAt(ui,{metaKey:true,deltaX:50,deltaY:0});assert.deepEqual(timeRange(ui),before);
  });
  await test("wheel zoom keeps the pointed time fixed and respects wheel delta units",async()=>{
    const ui=await openFixture(timelineFixture(80));await applyRange(ui);
    const grid=lastFrame(ui).filter(op=>op.method==="moveTo"&&op.strokeStyle==="#edf1f5");
    const left=Math.min(...grid.map(op=>op.rawArgs[0])),right=Math.max(...grid.map(op=>op.rawArgs[0]));
    const fraction=.25,clientX=left+(right-left)*fraction,pointedTime=400+800*fraction;
    await wheelAt(ui,{ctrlKey:true,deltaY:-48,clientX});
    const pixels=timeRange(ui),newPointedTime=pixels[0]+(pixels[1]-pixels[0])*fraction;
    assert.ok(Math.abs(newPointedTime-pointedTime)<=1,"zoom moved the time under the pointer");
    await applyRange(ui);await wheelAt(ui,{ctrlKey:true,deltaY:-3,deltaMode:1,clientX});
    assert.deepEqual(timeRange(ui),pixels,"line-mode zoom was treated as pixels");
    await applyRange(ui);await wheelAt(ui,{ctrlKey:true,deltaY:-.05*ui.$("timeline-plot").clientHeight,clientX});const pagePixels=timeRange(ui);
    await applyRange(ui);await wheelAt(ui,{ctrlKey:true,deltaY:-.05,deltaMode:2,clientX});
    assert.deepEqual(timeRange(ui),pagePixels,"page-mode zoom was treated as pixels");
    await applyRange(ui);await ui.$("timeline-zoom-in").fire("click");await settle();
    const buttonSpan=timeRange(ui)[1]-timeRange(ui)[0];
    assert.ok(buttonSpan>500&&buttonSpan<560,"zoom-in button should change the range by a clear, bounded step");
    await ui.$("timeline-zoom-out").fire("click");await settle();
    assert.ok(Math.abs(timeRange(ui)[1]-timeRange(ui)[0]-800)<=1,"zoom buttons are not reciprocal");
  });
  await test("other threads use a separate collapsed overview and switching its row loads that thread",async()=>{
    const ui=await openDemo();assert.equal(Boolean(ui.$("timeline-overview").open),false);
    assert.equal(ui.$("timeline-overview-canvas-host").children.length,0);
    assert.equal(lastFrame(ui).some(op=>op.method==="fillText"&&op.args[0]==="线程 1"),false);
    ui.$("timeline-overview").open=true;await ui.$("timeline-overview").fire("toggle");await settle();
    const overview=ui.$("timeline-overview-canvas-host").children[0];assert.ok(overview);assert.notEqual(overview,getCanvas(ui));
    assert.ok(overview.drawOperations.some(op=>op.method==="fillText"&&op.args[0]==="线程 1"));
    await overview.fire("click",{clientX:30,clientY:53});await settle();
    assert.equal(ui.$("timeline-thread").value,"1");assert.equal(ui.$("selected-move").textContent,"h2h9");
  });
  await test("same-millisecond samples remain individually navigable even when their columns overlap",async()=>{
    const ui=await openFixture(timelineFixture(20,{sameTime:true}));
    assert.match(ui.$("timeline-selection").textContent,/请求 #1\b/);
    for(let request=2;request<=12;++request) {
      await ui.$("timeline-next").fire("click");await settle();
      assert.match(ui.$("timeline-selection").textContent,new RegExp("请求 #"+request+"\\b"));
      assert.match(ui.$("timeline-selection").textContent,/50 ms/);
    }
    await ui.$("timeline-prev").fire("click");await settle();assert.match(ui.$("timeline-selection").textContent,/请求 #11\b/);
    assert.notEqual(rootFen(ui),"");assert.equal(ui.$("copy-fen").disabled,false);
    assert.ok(lastFrame(ui).some(op=>op.method==="arc"&&op.args[2]===5),"selected same-time sample lost its marker");
  });
  await test("only referenced chunks load and a slow old read cannot replace a new iteration",async()=>{
    const ui=browser(); canvas(ui); const f=fixture(); let release, reads=0;
    f.report.timeline_version=1; f.report.timeline={start_ms:0,end_ms:100,initial_states:[],chunks:[{file:"depth-2-timeline-000001.json",start_ms:0,end_ms:100,records:0}]};
    const slow=file("depth-2-timeline-000001.json",null,"fixture",()=>{++reads;return new Promise(done=>{release=done;});});
    const orphan=file("depth-2-timeline-000002.json",null,"fixture",()=>{throw Error("orphan read");});
    await importFiles(ui,[...filesFor(f),slow,orphan]); assert.equal(reads,0);
    await ui.$("view-timeline").fire("click"); await settle(); assert.equal(reads,1);
    await ui.$("demo").fire("click"); await ready(ui);
    release(JSON.stringify({timeline_version:1,search_id:7,iteration_depth:2,start_ms:0,end_ms:100,records:[]})); await settle();
    assert.equal(ui.$("depth-select").value,"3"); assert.match(ui.$("session-name").textContent,/内置示例/);
    assert.equal(ui.$("timeline-end").value,"180");
  });
  console.log("Passed "+checks+" timeline UI handler checks (simulated Canvas/DOM)."); return {checks};
}
module.exports=main();
