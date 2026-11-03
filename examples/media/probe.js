// Test probe: records every media event on the page's <video>/<audio> (capture
// phase on the window, so it also sees loadstart) for tools/mediatest.py.
window.__ev = [];
for (const t of ['loadstart','durationchange','loadedmetadata','loadeddata','canplay','canplaythrough','play','playing',
                 'pause','waiting','seeking','seeked','ended','error','volumechange','ratechange','resize'])
  window.addEventListener(t, (e) => window.__ev.push([t, +(e.target.currentTime || 0).toFixed(3)]), true);
window.__probe = function () {};
