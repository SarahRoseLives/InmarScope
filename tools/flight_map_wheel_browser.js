// Injected only by the manual browser regression tool, never shipped in the app.
async function runWheelRegression(populate) {
  const check=(condition,message)=>{if(!condition)throw new Error(message);};
  map.setView([20,0],5,{animate:false});
  const data=Array.from({length:500},(_,i)=>({id:String(i+1),lat:20+(i%25)/10,lon:Math.floor(i/25)/10,
    alt:30000,posTime:Date.now()/1000,positionSource:'Decoded ADS-C'}));
  if(populate)window.updateAircraft(data);
  const container=map.getContainer(),rect=container.getBoundingClientRect();
  const point=L.point(rect.width*0.65,rect.height*0.45);
  const anchor=map.containerPointToLatLng(point);
  const steps=[];const onZoom=()=>steps.push({time:performance.now(),zoom:map.getZoom()});map.on('zoom',onZoom);
  const start=performance.now();
  for(let i=0;i<12;i++) {
    container.dispatchEvent(new WheelEvent('wheel',{deltaY:-10,clientX:rect.x+point.x,clientY:rect.y+point.y,bubbles:true,cancelable:true}));
    if(populate&&i%3===0)window.updateAircraft(data);
    await new Promise(r=>setTimeout(r,30));
  }
  await new Promise(r=>setTimeout(r,500));map.off('zoom',onZoom);
  check(steps.length>=10,'wheel must produce continuous fractional steps');
  check(Math.abs(map.getZoom()-5.5)<0.005,'wheel input must accumulate without dropped increments');
  const drift=map.latLngToContainerPoint(anchor).distanceTo(point);
  check(drift<5,'zoom must keep the cursor anchor within 5 pixels');
  if(populate)check(document.querySelectorAll('.leaflet-interactive').length===500,'zoom must preserve received markers');
  // Fit must take over immediately, without an old animation pulling it back.
  container.dispatchEvent(new WheelEvent('wheel',{deltaY:-120,clientX:rect.x+point.x,clientY:rect.y+point.y,bubbles:true,cancelable:true}));
  document.getElementById('fit').click();const fit=map.getZoom();
  await new Promise(r=>setTimeout(r,400));check(map.getZoom()===fit,'Fit must cancel wheel animation');
  const result={updates:steps.length,firstUpdateMs:Math.round(steps[0].time-start),anchorDriftPixels:Math.round(drift*10)/10,
    maximumStepGapMs:Math.round(Math.max(...steps.slice(1).map((s,i)=>s.time-steps[i].time))),markers:document.querySelectorAll('.leaflet-interactive').length};
  if(populate)window.updateAircraft([]);
  return result;
}
