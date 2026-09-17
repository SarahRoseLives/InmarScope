'use strict';
// Membership is the native receiver snapshot. Optional online coordinates are
// filtered against received ICAOs by native code; no browser traffic requests.
const map = L.map('map', {
  worldCopyJump: true, scrollWheelZoom: false, zoomSnap: 0, zoomAnimation: false
}).setView([20, 0], 2);
const tiles = L.tileLayer('https://tile.openstreetmap.org/{z}/{x}/{y}.png', {
  maxZoom: 19, attribution: '&copy; <a href="https://www.openstreetmap.org/copyright">OpenStreetMap</a> contributors'
}).addTo(map);
const wheelZoom = window.installSmoothWheelZoom(map);
tiles.on('tileerror', () => {
  document.getElementById('tiles').textContent = 'Background tiles unavailable. Received aircraft positions still update.';
});
const markers = new Map();
function hasPosition(a) {
  return Number.isFinite(a.lat) && Number.isFinite(a.lon) && Math.abs(a.lat) <= 90 && Math.abs(a.lon) <= 180;
}
function label(a) { return [a.flight, a.reg, a.icao, 'AES ' + a.id].filter(Boolean).join(' · '); }
window.updateAircraft = function (aircraft) {
  const keep = new Set();
  const missing = document.getElementById('missing');
  missing.replaceChildren();
  let noPosition = 0;
  for (const a of aircraft) {
    if (!hasPosition(a)) {
      ++noPosition;
      const item = document.createElement('li'); item.textContent = label(a); missing.appendChild(item);
      continue;
    }
    keep.add(a.id);
    let marker = markers.get(a.id);
    if (!marker) {
      marker = L.circleMarker([a.lat, a.lon], {radius: 6, color: '#123c57', weight: 2, fillColor: '#27c5ff', fillOpacity: 0.9}).addTo(map);
      markers.set(a.id, marker);
    } else marker.setLatLng([a.lat, a.lon]);
    const age = Math.max(0, Math.floor(Date.now() / 1000 - (a.posTime || 0)));
    marker.setStyle({fillOpacity: age > 900 ? 0.35 : 0.9, fillColor: a.positionSource === 'ADSB.lol (online)' ? '#ffb347' : '#27c5ff'});
    const text = document.createElement('span');
    text.textContent = label(a) + '\n' + a.lat.toFixed(4) + ', ' + a.lon.toFixed(4) + ' · ' + a.alt + ' ft\n' +
      (a.positionSource || 'Decoded ADS-C') + ' position: ' + Math.floor(age / 60) + ' min ago';
    if (marker.getTooltip()) marker.setTooltipContent(text);
    else marker.bindTooltip(text);
  }
  for (const [id, marker] of markers) if (!keep.has(id)) { map.removeLayer(marker); markers.delete(id); }
  document.getElementById('counts').textContent = aircraft.length + ' received · ' + markers.size + ' with position';
  document.getElementById('missing-count').textContent = 'Received without a known position: ' + noPosition;
  // Deliberately no setView/panTo/fitBounds here: updates preserve the viewport.
};
document.getElementById('fit').addEventListener('click', () => {
  wheelZoom.cancel();
  if (markers.size) map.fitBounds(L.latLngBounds([...markers.values()].map(m => m.getLatLng())), {padding: [25, 25], maxZoom: 9});
});
new ResizeObserver(() => map.invalidateSize({pan: false})).observe(document.getElementById('map'));
