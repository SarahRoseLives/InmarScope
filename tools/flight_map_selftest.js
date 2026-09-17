// Run the production update code with a map spy: no network or tile downloads.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const elements = new Map();
const element = () => ({textContent:'', children:[], replaceChildren(){this.children=[];}, appendChild(x){this.children.push(x);}, addEventListener(_,fn){this.click=fn;}});
const document = {getElementById(id){if(!elements.has(id))elements.set(id,element());return elements.get(id);}, createElement:element};
const pins = new Set();
let viewChanges=0;
const map = {setView(){++viewChanges;return this;},fitBounds(){++viewChanges;},removeLayer(x){pins.delete(x);},invalidateSize(){}};
const L = {map:()=>map,tileLayer:()=>({addTo(){return this;},on(){}}),latLngBounds:x=>x,
  circleMarker(pos){return {pos, addTo(){pins.add(this);return this;},setLatLng(x){this.pos=x;},getLatLng(){return this.pos;},setStyle(){},getTooltip(){return this.tip;},bindTooltip(x){this.tip=x;},setTooltipContent(x){this.tip=x;}};}};
const context={L,document,window:{installSmoothWheelZoom:()=>({cancel(){}})},ResizeObserver:class{observe(){}}};
vm.runInNewContext(fs.readFileSync('assets/flight-map/map.js','utf8'),context);
const update=context.window.updateAircraft;
assert.equal(pins.size,0);
const aircraft={id:'000001',lat:12,lon:34,alt:30000,flight:'<img onerror=alert(1)>',posTime:Date.now()/1000};
update([aircraft,{id:'000002'}]);assert.equal(pins.size,1);assert.equal(viewChanges,1);
assert.match([...pins][0].tip.textContent,/<img onerror/); // text, never HTML
assert.equal(elements.get('missing').children.length,1);
update([{...aircraft,lat:13},{id:'000003',lat:10,lon:20,alt:25000}]);
assert.equal(pins.size,2);assert.equal([...pins][0].pos[0],13);assert.equal(viewChanges,1);
elements.get('fit').click();assert.equal(viewChanges,2);
update([]);assert.equal(pins.size,0);assert.equal(viewChanges,2);
update([{id:'bad',lat:91,lon:0},{id:'unknown'},{id:'zero',lat:0,lon:0,alt:0}]);assert.equal(pins.size,1);
assert.equal(elements.get('missing').children.length,2);
update([{id:'online',lat:10,lon:20,alt:30000,positionSource:'ADSB.lol (online)',posTime:Date.now()/1000}]);
assert.match([...pins][0].tip.textContent,/ADSB.lol \(online\)/);
assert.equal(viewChanges,2);
update([]);assert.equal(pins.size,0);
if (process.argv[2]) {
  // JSON emitted by the native ADS-C -> AircraftTable -> received-ID lookup test.
  const native=JSON.parse(fs.readFileSync(process.argv[2],'utf8'));
  assert.equal(native.length,2);
  update(native);assert.equal(pins.size,2);
  assert.ok([...pins].some(p=>p.tip.textContent.includes('Decoded ADS-C')));
  assert.ok([...pins].some(p=>p.tip.textContent.includes('ADSB.lol (online)')));
  assert.equal(viewChanges,2);
  update([]);assert.equal(pins.size,0);
}
console.log('PASS: only supplied aircraft, no invented positions, marker updates/removal, safe labels, no automatic recenter');
