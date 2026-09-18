'use strict';
// Keep wheel input responsive instead of queuing whole-level CSS transitions.
// Only public Leaflet APIs are used; aircraft updates never enter this handler.
window.installSmoothWheelZoom = function (map) {
  const container = map.getContainer();
  let frame = 0, target = map.getZoom(), anchor, previous = 0, applying = false;
  const clamp = value => Math.max(map.getMinZoom(), Math.min(map.getMaxZoom(), value));
  function cancel() {
    if (frame) cancelAnimationFrame(frame);
    frame = 0; previous = 0; target = map.getZoom();
  }
  function step(time) {
    frame = 0;
    const elapsed = previous ? Math.min(64, time - previous) : 16;
    previous = time;
    const current = map.getZoom();
    let next = current + (target - current) * (1 - Math.exp(-elapsed / 55));
    const done = Math.abs(target - next) < 0.002;
    if (done) next = target;
    applying = true;
    try { map.setZoomAround(anchor, next, {animate: false}); }
    finally { applying = false; }
    if (!done) frame = requestAnimationFrame(step);
    else previous = 0;
  }
  function wheel(event) {
    if (!Number.isFinite(event.deltaY) || event.deltaY === 0) return;
    event.preventDefault(); event.stopPropagation();
    // DOM wheel deltas can be pixels, lines, or pages. One mouse notch
    // (typically 120 pixels / 3 lines) moves half a level, with no integer snap.
    const unit = event.deltaMode === 1 ? 40 : event.deltaMode === 2 ? container.clientHeight : 1;
    const delta = Math.max(-240, Math.min(240, event.deltaY * unit));
    if (!frame) { map.stop(); target = map.getZoom(); }
    target = clamp(target - delta / 240);
    anchor = map.mouseEventToContainerPoint(event);
    if (!frame && target !== map.getZoom()) frame = requestAnimationFrame(step);
  }
  function otherMove() { if (!applying) cancel(); }
  container.addEventListener('wheel', wheel, {passive: false});
  container.addEventListener('pointerdown', cancel, true);
  container.addEventListener('keydown', cancel, true);
  map.on('movestart zoomstart resize', otherMove);
  map.on('unload', () => {
    cancel();
    container.removeEventListener('wheel', wheel);
    container.removeEventListener('pointerdown', cancel, true);
    container.removeEventListener('keydown', cancel, true);
    map.off('movestart zoomstart resize', otherMove);
  });
  return {cancel};
};
