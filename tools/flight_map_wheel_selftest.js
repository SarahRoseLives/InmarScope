// Deterministic regression tests for the production wheel handler, without tiles.
const assert=require('node:assert/strict'),fs=require('node:fs'),vm=require('node:vm');
let zoom=5,clock=0,serial=0,updates=[],frames=new Map();
const listeners=new Map(),events=new Map();
const container={clientHeight:600,addEventListener:(n,f)=>listeners.set(n,f),removeEventListener:n=>listeners.delete(n)};
const fire=n=>{for(const [names,fn] of events)if(names.split(' ').includes(n))fn();};
const map={getContainer:()=>container,getZoom:()=>zoom,getMinZoom:()=>0,getMaxZoom:()=>19,stop(){},
  mouseEventToContainerPoint:e=>({x:e.clientX,y:e.clientY}),
  setZoomAround(point,next,options){assert.equal(options.animate,false);fire('zoomstart');zoom=next;updates.push({point,next});},
  on:(n,f)=>events.set(n,f),off:n=>events.delete(n)};
const context={window:{},requestAnimationFrame:f=>{frames.set(++serial,f);return serial;},cancelAnimationFrame:n=>frames.delete(n)};
vm.runInNewContext(fs.readFileSync('assets/flight-map/wheel_zoom.js','utf8'),context);
const handler=context.window.installSmoothWheelZoom(map);
const tick=()=>{clock+=16;const jobs=[...frames.values()];frames.clear();for(const fn of jobs)fn(clock);};
const settle=()=>{for(let i=0;frames.size&&i<100;i++)tick();assert.equal(frames.size,0);};
const wheel=(delta,mode=0)=>{let prevented=false;listeners.get('wheel')({deltaY:delta,deltaMode:mode,clientX:300,clientY:200,preventDefault(){prevented=true;},stopPropagation(){}});return prevented;};
assert.ok(wheel(-120));tick();assert.ok(zoom>5&&zoom<5.5,'responds on first frame with a fractional step');
wheel(-120);settle();assert.ok(Math.abs(zoom-6)<1e-9,'new wheel input accumulated during animation');
assert.ok(updates.length>10,'continuous intermediate zooms');
assert.ok(updates.every(x=>x.point.x===300&&x.point.y===200));
wheel(3,1);settle();assert.ok(Math.abs(zoom-5.5)<1e-9,'line-mode normalization');
wheel(1,2);settle();assert.ok(Math.abs(zoom-4.5)<1e-9,'page-mode bounded');
wheel(-120);tick();const halfway=zoom;listeners.get('pointerdown')();settle();assert.equal(zoom,halfway,'drag cancels wheel inertia');
wheel(-120);handler.cancel();const before=zoom;settle();assert.equal(zoom,before,'Fit can cancel pending wheel input');
wheel(-120);fire('movestart');assert.equal(frames.size,0,'other navigation cancels wheel');
zoom=19;wheel(-120);assert.equal(frames.size,0);assert.equal(zoom,19);
zoom=0;wheel(120);assert.equal(frames.size,0);assert.equal(zoom,0);
wheel(-120);wheel(120);settle();assert.equal(zoom,0,'direction reversal');
assert.equal(wheel(NaN),false);assert.equal(wheel(0),false);
fire('unload');assert.equal(listeners.size,0);assert.equal(frames.size,0);
console.log('PASS: first-frame response; fractional wheel steps; accumulated input; cursor anchor; line/page deltas; reversal; limits; drag/fit cancellation; cleanup');
