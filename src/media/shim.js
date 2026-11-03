// Luxium media shim -- injected into every frame of a CEF page before any page
// script runs.  Chromium without proprietary codecs cannot play H.264/HEVC/AAC,
// so <video>/<audio> are re-implemented on top of Luxium's own media engine
// (src/media/player.cpp), which lives in this renderer process and is reached
// through the native object `__luxiumMedia` (src/cef/media_bridge.cpp).
//
// What this file does:
//   * patches HTMLMediaElement.prototype so the element behaves like the real
//     thing (properties, events in spec order, play() promise, TimeRanges...);
//   * draws the decoded NV12 frames into a <canvas> next to the video with
//     WebGL2 (YUV -> RGB in a shader), keeping its geometry synced to the box;
//   * implements text tracks (<track> + WebVTT, cuechange, activeCues) because
//     Chromium never advances cues of an element it is not playing;
//   * exposes the tracks inside the file: audioTracks (switching without reopening) and text
//     subtitles (SRT/ASS/mov_text/WebVTT) as textTracks with engine-decoded cues;
//   * publishes navigator.luxiumMedia, the engine's decoder/demuxer inventory;
//   * draws its own controls when the `controls` attribute is set;
//   * routes fullscreen to the element's container so canvas + controls follow.
// Elements whose source is a blob:/MediaStream (MSE, WebRTC) are left to Chromium.
(function () {
  'use strict';
  const LXM = window.__luxiumMedia;
  if (!LXM || window.__luxiumShim) return;
  try { Object.defineProperty(window, '__luxiumMedia', { value: undefined, configurable: true }); } catch (e) {}
  window.__luxiumShim = true;

  const HME = window.HTMLMediaElement;
  if (!HME) return;
  const proto = HME.prototype;
  const orig = {};                      // original property descriptors / methods
  for (const k of ['src', 'currentSrc', 'currentTime', 'duration', 'paused', 'ended', 'seeking',
                   'readyState', 'networkState', 'buffered', 'seekable', 'played', 'volume', 'muted',
                   'playbackRate', 'error', 'textTracks', 'srcObject'])
    orig[k] = Object.getOwnPropertyDescriptor(proto, k);
  const origPlay = proto.play, origPause = proto.pause, origLoad = proto.load,
        origCanPlay = proto.canPlayType, origAddTrack = proto.addTextTrack;

  const S = new WeakMap();              // element -> state
  const byId = new Map();               // engine player id -> state
  const ours = new WeakSet();           // events we dispatched
  const active = new Set();             // states that need the per-frame loop
  const tracked = new WeakSet();        // elements already scanned
  const MEDIA_EVENTS = ['loadstart', 'durationchange', 'loadedmetadata', 'loadeddata', 'canplay',
    'canplaythrough', 'play', 'playing', 'pause', 'waiting', 'seeking', 'seeked', 'timeupdate',
    'progress', 'ended', 'error', 'volumechange', 'ratechange', 'resize', 'emptied', 'stalled',
    'suspend', 'abort', 'loadeddata'];

  const isProprietary = (t) => /avc[13]|hvc1|hev1|h\.?26[45]|mp4a|ac-3|ec-3|hevc|aac|dvh[1e]/i.test(t);

  // ── small helpers ──────────────────────────────────────────────────────────
  const fmtTime = (t) => {
    if (!isFinite(t)) return '0:00';
    t = Math.max(0, Math.floor(t));
    const h = Math.floor(t / 3600), m = Math.floor(t / 60) % 60, s = t % 60;
    return (h ? h + ':' + String(m).padStart(2, '0') : m) + ':' + String(s).padStart(2, '0');
  };
  class LXTimeRanges {
    constructor(a) { this._a = a; }
    get length() { return this._a.length; }
    start(i) { if (i >= this._a.length) throw new DOMException('Index out of range', 'IndexSizeError'); return this._a[i][0]; }
    end(i) { if (i >= this._a.length) throw new DOMException('Index out of range', 'IndexSizeError'); return this._a[i][1]; }
  }
  const mediaError = (code, message) => {
    const e = Object.create(window.MediaError ? MediaError.prototype : Object.prototype);
    Object.defineProperties(e, { code: { value: code }, message: { value: message || '' },
      MEDIA_ERR_ABORTED: { value: 1 }, MEDIA_ERR_NETWORK: { value: 2 },
      MEDIA_ERR_DECODE: { value: 3 }, MEDIA_ERR_SRC_NOT_SUPPORTED: { value: 4 } });
    return e;
  };
  const fire = (el, type, init) => {
    const e = init ? new Event(type, init) : new Event(type);
    ours.add(e);
    el.dispatchEvent(e);
  };
  const later = (fn) => queueMicrotask(fn);

  // Native media events on elements we own (Chromium's own, e.g. the cancelled
  // load of a src attribute) must never reach the page: only ours do.
  const mediaOwner = (t) => {
    if (t instanceof HME) return t;
    if (t && t.parentNode instanceof HME && (t.localName === 'source' || t.localName === 'track')) return t.parentNode;
    return null;
  };
  for (const type of MEDIA_EVENTS) {
    window.addEventListener(type, (e) => {
      if (ours.has(e)) return;
      const el = mediaOwner(e.target);
      if (!el) return;
      const s = S.get(el);
      if (s && !s.native) e.stopImmediatePropagation();
    }, true);
  }

  // ── per-element state ──────────────────────────────────────────────────────
  function st(el) {
    let s = S.get(el);
    if (!s) {
      s = {
        el, id: -1, native: false, src: '', srcSet: false, cur: '', sources: null, srcIdx: 0,
        paused: true, ended: false, seeking: false, ready: 0, net: 0, dur: NaN, vw: 0, vh: 0,
        hasVideo: false, vol: 1, muted: !!el.defaultMuted, rate: 1, err: null, pendingTime: 0,
        plays: [], autoplayed: false, loopSent: false, surface: null, lastSeq: 0, buf: null,
        tracks: null, lastCueT: -1, frames: 0, dropped: 0, ctl: null, fsFor: null, rvfc: [],
      };
      S.set(el, s);
    }
    return s;
  }
  const isOurs = (el) => { const s = S.get(el); return !!s && !s.native; };

  function refresh(s) {
    if (s.id < 0) return;
    let o;
    try { o = JSON.parse(LXM.state(s.id)); } catch (e) { return; }
    s.paused = o.p; s.ended = o.e; s.seeking = o.s; s.ready = o.r; s.net = o.n;
    s.vw = o.w; s.vh = o.h; s.dur = o.d === null ? Infinity : o.d; s.hasVideo = o.v;
    if (o.d === undefined) s.dur = NaN;
    if (o.c && !s.err) s.err = mediaError(o.c, o.m);
  }

  // The native element knows nothing about the media, so it would stay 300x150.
  // Give it the video's natural size through size containment (zero-specificity
  // rule below: anything the page sets on width/height/aspect-ratio wins).
  function applySize(s) {
    const el = s.el;
    if (!(el instanceof HTMLVideoElement) || !s.vw) return;
    el.style.setProperty('--lx-w', s.vw + 'px');
    el.style.setProperty('--lx-h', s.vh + 'px');
    el.style.setProperty('--lx-ar', s.vw + ' / ' + s.vh);
    el.setAttribute('data-lx-size', '');
  }

  // ── resource selection ─────────────────────────────────────────────────────
  const abs = (u) => { try { return new URL(u, document.baseURI).href; } catch (e) { return u; } };

  function sourceList(el) {
    const a = el.getAttribute('src');
    if (a) return [{ url: a, node: null }];
    const l = [];
    for (const c of el.children)
      if (c.localName === 'source' && c.getAttribute('src')) {
        const t = c.getAttribute('type');
        if (!t || LXM.canPlay(t) !== '') l.push({ url: c.getAttribute('src'), node: c });
      }
    return l;
  }

  function selectResource(el) {
    const s = st(el);
    if (s.native) return;
    closePlayer(s);
    s.err = null; s.ended = false; s.ready = 0; s.dur = NaN; s.vw = s.vh = 0; s.cur = '';
    let list;
    if (s.srcSet) list = s.src ? [{ url: s.src, node: null }] : [];
    else list = sourceList(el);
    s.sources = list; s.srcIdx = 0;
    if (!list.length) { s.net = el.hasAttribute('src') || s.srcSet ? 3 : 0; if (s.srcSet && s.src === '') return; later(() => { if (s.sources === list && list.length === 0 && s.net === 3) failSource(s, 4, 'no supported source'); }); return; }
    openCurrent(s);
  }

  function openCurrent(s) {
    const el = s.el, it = s.sources[s.srcIdx];
    const url = abs(it.url);
    if (/^(blob|data|mediastream):/i.test(url)) { goNative(s, it.url); return; }
    s.cur = url;
    s.net = 2;
    s.id = LXM.create();
    byId.set(s.id, s);
    let h = 'Referer: ' + location.href + '\r\n';
    try { if (new URL(url).origin === location.origin && document.cookie) h += 'Cookie: ' + document.cookie + '\r\n'; } catch (e) {}
    LXM.open(s.id, url, h, location.protocol === 'file:');
    LXM.setVolume(s.id, s.vol); LXM.setMuted(s.id, s.muted); LXM.setRate(s.id, s.rate);
    LXM.setLoop(s.id, s.loopSent = el.loop);
    active.add(s);
    kick();
    tracksNeedLoad(s);
  }

  function failSource(s, code, msg) {
    s.err = mediaError(code, msg);
    s.net = 3;
    fire(s.el, 'error');
  }

  function goNative(s, url) {
    s.native = true;
    closePlayer(s);
    if (s.srcSet && orig.src) orig.src.set.call(s.el, s.src);
    else if (orig.src) orig.src.set.call(s.el, url);
    try { orig.volume.set.call(s.el, s.vol); orig.muted.set.call(s.el, s.muted); } catch (e) {}
  }

  function closePlayer(s) {
    if (s.id >= 0) {
      byId.delete(s.id);
      LXM.close(s.id);
      s.id = -1;
    }
    active.delete(s);
    resetTracks(s);
    for (const p of s.plays.splice(0)) p.reject(new DOMException('The play() request was interrupted by a new load request.', 'AbortError'));
    destroySurface(s);
  }

  // ── engine events -> DOM events ────────────────────────────────────────────
  function onEngineEvent(id, type, detail) {
    const s = byId.get(id);
    if (!s) return;
    const el = s.el;
    refresh(s);
    // play() on an element that is already playing gets no 'playing' event after a seek; settle it once it can run
    if (s.plays.length && type !== 'pause' && type !== 'error' && !s.paused && s.ready >= 3 && !s.seeking && !s.waitingPlay)
      for (const p of s.plays.splice(0)) p.resolve();
    switch (type) {
      case 'volumechange': case 'ratechange': return;         // we fire those from the setters
      case 'resize':
        applySize(s);
        fire(el, type);
        return;
      case 'loadedmetadata':
        applySize(s);
        loadTracks(s);                  // before the event: pages read audioTracks in the handler
        fire(el, type);
        if (s.hasVideo) { ensureSurface(s); if (s.pendingTime > 0) { LXM.seek(s.id, s.pendingTime); refresh(s); } }
        else if (s.pendingTime > 0) { LXM.seek(s.id, s.pendingTime); refresh(s); }
        s.pendingTime = 0;
        updateControls(s);
        return;
      case 'canplay':
        fire(el, type);
        if (el.autoplay && s.paused && !s.autoplayed) { s.autoplayed = true; el.play().catch(() => {}); }
        return;
      case 'playing':
        fire(el, type);
        for (const p of s.plays.splice(0)) p.resolve();
        updateControls(s);
        return;
      case 'pause':
        for (const p of s.plays.splice(0)) p.reject(new DOMException('The play() request was interrupted by a call to pause().', 'AbortError'));
        fire(el, type);
        updateControls(s);
        return;
      case 'error': {
        // MEDIA_ERR_SRC_NOT_SUPPORTED/NETWORK before metadata: try the next <source>
        if (s.sources && s.srcIdx + 1 < s.sources.length && s.ready < 1 && s.sources[s.srcIdx].node) {
          const failed = s.sources[s.srcIdx].node;
          fire(failed, 'error');
          closePlayer(s);
          s.srcIdx++;
          s.err = null;
          openCurrent(s);
          return;
        }
        if (s.sources && s.sources[s.srcIdx] && s.sources[s.srcIdx].node) fire(s.sources[s.srcIdx].node, 'error');
        for (const p of s.plays.splice(0)) p.reject(new DOMException(detail || 'The element has no supported sources.', 'NotSupportedError'));
        fire(el, 'error');
        updateControls(s);
        return;
      }
      default:
        fire(el, type);
        if (type === 'seeked' || type === 'timeupdate' || type === 'play' || type === 'ended' || type === 'waiting' || type === 'loadeddata') updateControls(s);
    }
  }

  // ── the per-frame loop ─────────────────────────────────────────────────────
  let raf = 0, timer = 0;
  function kick() { if (!raf) raf = requestAnimationFrame(loop); if (!timer) timer = setInterval(pollOnly, 250); }
  function pollEvents() {
    const j = LXM.poll();
    if (!j) return;
    for (const [id, type, detail] of JSON.parse(j)) onEngineEvent(id, type, detail);
  }
  function pollOnly() { if (document.hidden) pollEvents(); if (!active.size) { clearInterval(timer); timer = 0; } }
  function loop(ts) {
    raf = 0;
    pollEvents();
    for (const s of active) {
      if (!s.el.isConnected) { if (!s.paused) { LXM.pause(s.id); refresh(s); } syncSurface(s); continue; }
      if (s.el.loop !== s.loopSent) LXM.setLoop(s.id, s.loopSent = s.el.loop);
      if (s.hasVideo) { syncSurface(s); drawFrame(s, ts); }
      tickTracks(s);
      tickControls(s);
    }
    if (active.size) raf = requestAnimationFrame(loop);
  }

  // ── video surface: a WebGL2 canvas next to the <video> ─────────────────────
  const VS = `#version 300 es
in vec2 p; out vec2 v; void main(){ v = vec2(p.x*.5+.5, .5-p.y*.5); gl_Position = vec4(p,0,1); }`;
  const FS = `#version 300 es
precision highp float;
uniform sampler2D uY, uUV; uniform mat3 uM; uniform vec3 uScale, uBias; in vec2 v; out vec4 o;
void main(){
  vec3 yuv = vec3(texture(uY, v).r, texture(uUV, v).rg) * uScale + uBias;
  o = vec4(clamp(uM * yuv, 0., 1.), 1.);
}`;
  const MAT = [   // column-major mat3 for [Y U V] -> RGB
    [1, 1, 1, 0, -0.344136, 1.772, 1.402, -0.714136, 0],          // BT.601
    [1, 1, 1, 0, -0.187324, 1.8556, 1.5748, -0.468124, 0],        // BT.709
    [1, 1, 1, 0, -0.16455, 1.8814, 1.4746, -0.57135, 0],          // BT.2020
  ];

  function ensureSurface(s) {
    if (s.surface || s.native || !(s.el instanceof HTMLVideoElement)) return;
    const c = document.createElement('canvas');
    c.setAttribute('data-luxium-surface', '');
    c.setAttribute('aria-hidden', 'true');
    c.style.cssText = 'position:absolute;left:0;top:0;width:0;height:0;pointer-events:none;margin:0;padding:0;border:0;display:none';
    c.width = c.height = 1;
    const gl = c.getContext('webgl2', { alpha: false, antialias: false, depth: false, stencil: false,
                                         preserveDrawingBuffer: true, powerPreference: 'high-performance' });
    if (!gl) { console.warn('[luxium-media] WebGL2 unavailable: video frames cannot be drawn'); return; }
    const sh = (t, src) => { const x = gl.createShader(t); gl.shaderSource(x, src); gl.compileShader(x);
      if (!gl.getShaderParameter(x, gl.COMPILE_STATUS)) console.warn('[luxium-media] shader:', gl.getShaderInfoLog(x)); return x; };
    const prog = gl.createProgram();
    gl.attachShader(prog, sh(gl.VERTEX_SHADER, VS)); gl.attachShader(prog, sh(gl.FRAGMENT_SHADER, FS));
    gl.bindAttribLocation(prog, 0, 'p');
    gl.linkProgram(prog);
    gl.useProgram(prog);
    const vb = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, vb);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 1, -1, -1, 1, 1, 1]), gl.STATIC_DRAW);
    gl.enableVertexAttribArray(0);
    gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 0, 0);
    const tex = (unit, uni) => {
      const t = gl.createTexture();
      gl.activeTexture(gl.TEXTURE0 + unit);
      gl.bindTexture(gl.TEXTURE_2D, t);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
      gl.uniform1i(gl.getUniformLocation(prog, uni), unit);
      return t;
    };
    s.surface = { c, gl, prog, ty: tex(0, 'uY'), tuv: tex(1, 'uUV'), tw: 0, th: 0, hasFrame: false,
                  u: { m: gl.getUniformLocation(prog, 'uM'), sc: gl.getUniformLocation(prog, 'uScale'), bi: gl.getUniformLocation(prog, 'uBias') },
                  left: 0, top: 0, w: 0, h: 0, matrix: 1, full: false, dirty: true };
    s.el.insertAdjacentElement('afterend', c);
    s.surface.host = c.parentNode;
  }

  function destroySurface(s) {
    const f = s.surface;
    if (!f) return;
    s.surface = null;
    f.c.remove();
    const x = f.gl.getExtension('WEBGL_lose_context');
    if (x) x.loseContext();
  }

  // Object-fit/position of the video box -> the rectangle the picture covers, in CSS px.
  function pictureRect(el, s, cs) {
    const r = el.getBoundingClientRect();
    const bl = parseFloat(cs.borderLeftWidth) + parseFloat(cs.paddingLeft), bt = parseFloat(cs.borderTopWidth) + parseFloat(cs.paddingTop);
    const br = parseFloat(cs.borderRightWidth) + parseFloat(cs.paddingRight), bb = parseFloat(cs.borderBottomWidth) + parseFloat(cs.paddingBottom);
    const bx = r.left + bl, by = r.top + bt, bw = Math.max(0, r.width - bl - br), bh = Math.max(0, r.height - bt - bb);
    const vw = s.vw || 1, vh = s.vh || 1;
    const fit = cs.objectFit || 'contain';
    let w = bw, h = bh;
    const k = Math.min(bw / vw, bh / vh), kc = Math.max(bw / vw, bh / vh);
    if (fit === 'contain') { w = vw * k; h = vh * k; }
    else if (fit === 'cover') { w = vw * kc; h = vh * kc; }
    else if (fit === 'none') { w = vw; h = vh; }
    else if (fit === 'scale-down') { const kk = Math.min(1, k); w = vw * kk; h = vh * kk; }
    const pos = (cs.objectPosition || '50% 50%').split(/\s+/);
    const place = (v, free) => v.endsWith('%') ? free * parseFloat(v) / 100 : (v.endsWith('px') ? parseFloat(v) : free / 2);
    const x = bx + place(pos[0] || '50%', bw - w), y = by + place(pos[1] || pos[0] || '50%', bh - h);
    return { x, y, w, h, bx, by, bw, bh };
  }

  function syncSurface(s) {
    const f = s.surface, el = s.el;
    if (!f) return;
    if (!el.isConnected || !el.getClientRects().length) { f.c.style.display = 'none'; return; }
    if (f.c.previousElementSibling !== el && !(f.host === el.parentNode && el.nextElementSibling === f.c)) {
      if (el.parentNode) el.insertAdjacentElement('afterend', f.c);
    }
    const cs = getComputedStyle(el);
    if (cs.visibility === 'hidden' || cs.display === 'none' || !f.hasFrame) { f.c.style.display = 'none'; return; }
    const pr = pictureRect(el, s, cs);
    // origin of the canvas's containing block = where it sits when left/top are 0
    const o = f.c.getBoundingClientRect();
    const ox = o.left - f.left, oy = o.top - f.top;
    const left = pr.x - ox, top = pr.y - oy;
    const dpr = window.devicePixelRatio || 1;
    const pw = Math.max(1, Math.round(pr.w * dpr)), ph = Math.max(1, Math.round(pr.h * dpr));
    const st2 = f.c.style;
    if (f.c.style.display === 'none') st2.display = 'block';
    if (left !== f.left || top !== f.top) { st2.left = left + 'px'; st2.top = top + 'px'; f.left = left; f.top = top; }
    if (pr.w !== f.w || pr.h !== f.h) { st2.width = pr.w + 'px'; st2.height = pr.h + 'px'; f.w = pr.w; f.h = pr.h; }
    if (f.c.width !== pw || f.c.height !== ph) { f.c.width = pw; f.c.height = ph; f.dirty = true; }
    const z = cs.zIndex;
    if (st2.zIndex !== (z === 'auto' ? '' : z)) st2.zIndex = z === 'auto' ? '' : z;
    if (st2.opacity !== cs.opacity) st2.opacity = cs.opacity;
    const rad = cs.borderRadius;
    if (st2.borderRadius !== rad) st2.borderRadius = rad;
    if (st2.transform !== cs.transform) st2.transform = cs.transform === 'none' ? '' : cs.transform;
  }

  function drawFrame(s, ts) {
    const f = s.surface;
    if (!f) return;
    const gl = f.gl;
    const t0 = performance.now();
    const need = LXM.frame(s.id, s.lastSeq, s.buf || null);
    const t1 = performance.now();
    if (need && need[0] === 'grow') { s.buf = new ArrayBuffer(need[1]); return drawFrame(s, ts); }
    if (need) {
      const [seq, w, h, , pts, matrix, full, bytes] = need;
      s.lastSeq = seq;
      s.msCopy = (s.msCopy || 0) + (t1 - t0);
      const cw = (w + 1) >> 1, ch = (h + 1) >> 1;
      gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
      gl.activeTexture(gl.TEXTURE0);
      gl.bindTexture(gl.TEXTURE_2D, f.ty);
      if (f.tw !== w || f.th !== h) {
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.R8, w, h, 0, gl.RED, gl.UNSIGNED_BYTE, new Uint8Array(s.buf, 0, w * h));
        gl.activeTexture(gl.TEXTURE1);
        gl.bindTexture(gl.TEXTURE_2D, f.tuv);
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RG8, cw, ch, 0, gl.RG, gl.UNSIGNED_BYTE, new Uint8Array(s.buf, w * h, cw * ch * 2));
        f.tw = w; f.th = h;
      } else {
        gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, w, h, gl.RED, gl.UNSIGNED_BYTE, new Uint8Array(s.buf, 0, w * h));
        gl.activeTexture(gl.TEXTURE1);
        gl.bindTexture(gl.TEXTURE_2D, f.tuv);
        gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, cw, ch, gl.RG, gl.UNSIGNED_BYTE, new Uint8Array(s.buf, w * h, cw * ch * 2));
      }
      if (f.matrix !== matrix || f.full !== !!full || f.dirty) {
        f.matrix = matrix; f.full = !!full;
        gl.uniformMatrix3fv(f.u.m, false, MAT[matrix] || MAT[1]);
        if (full) { gl.uniform3f(f.u.sc, 1, 1, 1); gl.uniform3f(f.u.bi, 0, -0.5, -0.5); }
        else { gl.uniform3f(f.u.sc, 255 / 219, 255 / 224, 255 / 224); gl.uniform3f(f.u.bi, -16 / 219, -128 / 224, -128 / 224); }
      }
      s.msUpload = (s.msUpload || 0) + (performance.now() - t1);
      f.dirty = true;
      s.frames++;
      f.hasFrame = true;
      s.lastPts = pts;
      if (s.rvfc.length) { const cbs = s.rvfc.splice(0); for (const [i, cb] of cbs) try { cb(ts, { presentationTime: ts, expectedDisplayTime: ts, width: s.vw, height: s.vh, mediaTime: pts, presentedFrames: s.frames }); } catch (e) { console.error(e); } }
    }
    if (f.dirty && f.hasFrame) {
      gl.viewport(0, 0, f.c.width, f.c.height);
      const t2 = performance.now();
      gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
      s.msDraw = (s.msDraw || 0) + (performance.now() - t2);
      f.dirty = false;
    }
  }

  // ── controls (drawn in a shadow root over the video) ───────────────────────
  const ICON = {
    play: '<path d="M8 5v14l11-7z"/>', pause: '<path d="M6 5h4v14H6zm8 0h4v14h-4z"/>',
    vol: '<path d="M3 9v6h4l5 5V4L7 9zm13.5 3A4.5 4.5 0 0 0 14 8v8a4.5 4.5 0 0 0 2.5-4z"/>',
    mute: '<path d="M3 9v6h4l5 5V4L7 9zm13.6 3 2.7-2.7-1.4-1.4-2.7 2.7-2.7-2.7-1.4 1.4 2.7 2.7-2.7 2.7 1.4 1.4 2.7-2.7 2.7 2.7 1.4-1.4z"/>',
    fs: '<path d="M7 14H5v5h5v-2H7zm-2-4h2V7h3V5H5zm12 7h-3v2h5v-5h-2zM14 5v2h3v3h2V5z"/>',
    fsx: '<path d="M5 16h3v3h2v-5H5zm3-8H5v2h5V5H8zm6 11h2v-3h3v-2h-5zm2-11V5h-2v5h5V8z"/>',
  };
  const CTL_CSS = `
:host{all:initial;position:absolute;pointer-events:none;font:13px/1.2 system-ui,sans-serif;color:#fff;z-index:2147483000}
.bar{position:absolute;left:0;right:0;bottom:0;display:flex;align-items:center;gap:8px;padding:18px 10px 8px;
  background:linear-gradient(transparent,rgba(0,0,0,.75));pointer-events:auto;user-select:none;transition:opacity .2s;box-sizing:border-box}
.bar.hide{opacity:0;pointer-events:none}
button{all:unset;cursor:pointer;width:30px;height:30px;display:flex;align-items:center;justify-content:center;border-radius:4px}
button:hover{background:rgba(255,255,255,.18)}
svg{width:22px;height:22px;fill:#fff}
.time{white-space:nowrap;font-variant-numeric:tabular-nums;min-width:90px;text-align:center}
.seek{position:relative;flex:1;height:20px;cursor:pointer;display:flex;align-items:center}
.track{position:relative;width:100%;height:4px;background:rgba(255,255,255,.3);border-radius:2px}
.buf{position:absolute;top:0;bottom:0;background:rgba(255,255,255,.35);border-radius:2px}
.pos{position:absolute;top:0;bottom:0;left:0;background:#e50914;border-radius:2px}
.knob{position:absolute;top:50%;width:12px;height:12px;margin:-6px 0 0 -6px;background:#fff;border-radius:50%}
.tip{position:absolute;bottom:22px;transform:translateX(-50%);background:rgba(0,0,0,.85);padding:2px 6px;border-radius:3px;display:none}
.seek:hover .tip{display:block}
.vol{width:70px;accent-color:#e50914}
.cues{position:absolute;left:5%;right:5%;bottom:8%;text-align:center;pointer-events:none;transition:bottom .2s}
.cues.up{bottom:56px}
.cues div{display:inline-block;background:rgba(0,0,0,.72);color:#fff;font-size:22px;line-height:1.3;padding:2px 8px;margin:1px 0;white-space:pre-wrap}
`;
  function makeControls(s) {
    if (s.ctl) return s.ctl;
    const host = document.createElement('div');
    host.setAttribute('data-luxium-controls', '');
    const root = host.attachShadow({ mode: 'closed' });
    root.innerHTML = `<style>${CTL_CSS}</style><div class="cues"></div>
      <div class="bar"><button class="pp"><svg viewBox="0 0 24 24"></svg></button><span class="time">0:00 / 0:00</span>
      <div class="seek"><div class="track"><div class="buf"></div><div class="pos"></div><div class="knob"></div></div><div class="tip">0:00</div></div>
      <button class="mute"><svg viewBox="0 0 24 24"></svg></button><input class="vol" type="range" min="0" max="1" step="0.02">
      <button class="fs"><svg viewBox="0 0 24 24"></svg></button></div>`;
    const q = (c) => root.querySelector(c);
    const c = { host, root, bar: q('.bar'), pp: q('.pp'), time: q('.time'), seek: q('.seek'), buf: q('.buf'), pos: q('.pos'),
                knob: q('.knob'), tip: q('.tip'), mute: q('.mute'), vol: q('.vol'), fs: q('.fs'), cues: q('.cues'),
                hideAt: 0, shown: true, key: '' };
    const el = s.el;
    c.pp.onclick = () => { s.paused ? el.play().catch(() => {}) : el.pause(); };
    c.mute.onclick = () => { el.muted = !el.muted; };
    c.vol.oninput = () => { el.volume = +c.vol.value; if (el.muted && +c.vol.value > 0) el.muted = false; };
    c.fs.onclick = () => toggleFullscreen(el);
    const seekTo = (e) => { const r = c.seek.getBoundingClientRect(); return Math.max(0, Math.min(1, (e.clientX - r.left) / r.width)) * (isFinite(s.dur) ? s.dur : 0); };
    let dragging = false;
    c.seek.onpointerdown = (e) => { dragging = true; c.seek.setPointerCapture(e.pointerId); el.currentTime = seekTo(e); };
    c.seek.onpointermove = (e) => {
      const r = c.seek.getBoundingClientRect();
      c.tip.style.left = (e.clientX - r.left) + 'px';
      c.tip.textContent = fmtTime(seekTo(e));
      if (dragging) el.currentTime = seekTo(e);
    };
    c.seek.onpointerup = () => { dragging = false; };
    el.insertAdjacentElement('afterend', host);
    // pointer activity shows the bar; it hides ~2.5 s after the last move while playing
    const poke = () => { c.hideAt = performance.now() + 2500; };
    for (const t of ['pointermove', 'pointerdown', 'keydown']) el.addEventListener(t, poke);
    host.addEventListener('pointermove', poke, true);
    el.addEventListener('click', (e) => {
      if (e.defaultPrevented || !el.hasAttribute('controls')) return;
      if (e.detail <= 2) { s.paused ? el.play().catch(() => {}) : el.pause(); }
    });
    el.addEventListener('dblclick', (e) => { if (!e.defaultPrevented && el.hasAttribute('controls')) toggleFullscreen(el); });
    el.addEventListener('keydown', (e) => {
      if (e.defaultPrevented || !el.hasAttribute('controls') || e.altKey || e.ctrlKey || e.metaKey) return;
      const k = e.key;
      if (k === ' ' || k === 'k') { s.paused ? el.play().catch(() => {}) : el.pause(); }
      else if (k === 'ArrowRight') el.currentTime = el.currentTime + 5;
      else if (k === 'ArrowLeft') el.currentTime = Math.max(0, el.currentTime - 5);
      else if (k === 'ArrowUp') el.volume = Math.min(1, el.volume + 0.05);
      else if (k === 'ArrowDown') el.volume = Math.max(0, el.volume - 0.05);
      else if (k === 'm') el.muted = !el.muted;
      else if (k === 'f') toggleFullscreen(el);
      else return;
      e.preventDefault();
    });
    if (!el.hasAttribute('tabindex')) el.tabIndex = 0;
    c.hideAt = performance.now() + 2500;
    s.ctl = c;
    return c;
  }

  function tickControls(s) {
    const el = s.el;
    const wants = el instanceof HTMLVideoElement && el.hasAttribute('controls') && !/nofullscreen|nodownload/.test('') && s.id >= 0;
    const wantsAudio = !(el instanceof HTMLVideoElement) && el.hasAttribute('controls') && s.id >= 0;
    if (!wants && !wantsAudio) { if (s.ctl) { s.ctl.host.remove(); s.ctl = null; } renderCues(s); return; }
    const c = makeControls(s);
    if (!c.host.isConnected && el.parentNode) el.insertAdjacentElement('afterend', c.host);
    // geometry: cover the video box
    const cs = getComputedStyle(el);
    if (cs.display === 'none' || !el.getClientRects().length) { c.host.style.display = 'none'; return; }
    c.host.style.display = '';
    const r = el.getBoundingClientRect();
    const o = c.host.getBoundingClientRect();
    const hs = c.host.style;
    const ox = o.left - (c.l || 0), oy = o.top - (c.t || 0);
    c.l = r.left - ox; c.t = r.top - oy;
    hs.left = c.l + 'px'; hs.top = c.t + 'px'; hs.width = r.width + 'px'; hs.height = (wantsAudio ? Math.min(r.height, 54) : r.height) + 'px';
    const now = performance.now();
    const show = s.paused || now < c.hideAt || s.ready < 2;
    if (show !== c.shown) { c.shown = show; c.bar.classList.toggle('hide', !show); c.cues.classList.toggle('up', show); }
    updateControls(s, true);
  }

  function updateControls(s, light) {
    const c = s.ctl;
    if (!c) return;
    const el = s.el;
    const t = el.currentTime, d = s.dur;
    const ft = fmtTime(t) + ' / ' + fmtTime(d);
    if (c.time.textContent !== ft) c.time.textContent = ft;
    const frac = isFinite(d) && d > 0 ? Math.min(1, t / d) : 0;
    c.pos.style.width = (frac * 100) + '%';
    c.knob.style.left = (frac * 100) + '%';
    if (!light || !c.bufT || performance.now() - c.bufT > 500) {
      c.bufT = performance.now();
      const b = LXM.ranges(s.id, 0);
      if (b.length && isFinite(d) && d > 0) { c.buf.style.left = (b[0][0] / d * 100) + '%'; c.buf.style.width = ((b[0][1] - b[0][0]) / d * 100) + '%'; }
    }
    const key = (s.paused ? 'p' : 'q') + (s.muted || s.vol === 0 ? 'm' : 'v') + (document.fullscreenElement ? 'x' : 'f');
    if (c.key !== key) {
      c.key = key;
      c.pp.firstChild.innerHTML = s.paused ? ICON.play : ICON.pause;
      c.mute.firstChild.innerHTML = s.muted || s.vol === 0 ? ICON.mute : ICON.vol;
      c.fs.firstChild.innerHTML = document.fullscreenElement ? ICON.fsx : ICON.fs;
    }
    const vv = s.muted ? 0 : s.vol;
    if (+c.vol.value !== vv) c.vol.value = vv;
  }

  // ── fullscreen: go through the container so canvas + controls come along ───
  const fsOf = new WeakMap();           // container -> video we fullscreened it for
  const origRFS = Element.prototype.requestFullscreen;
  function fsTarget(el) {
    const p = el.parentElement;
    return p && p !== document.body && p !== document.documentElement ? p : el;
  }
  function toggleFullscreen(el) {
    if (document.fullscreenElement) return document.exitFullscreen().catch(() => {});
    return el.requestFullscreen().catch(() => {});
  }
  if (origRFS) {
    Element.prototype.requestFullscreen = function (opts) {
      if (this instanceof HME && isOurs(this)) {
        const t = fsTarget(this);
        fsOf.set(t, this);
        return origRFS.call(t, opts);
      }
      return origRFS.call(this, opts);
    };
    const fe = Object.getOwnPropertyDescriptor(Document.prototype, 'fullscreenElement');
    if (fe) Object.defineProperty(Document.prototype, 'fullscreenElement', { configurable: true, enumerable: true,
      get() { const e = fe.get.call(this); return (e && fsOf.get(e)) || e; } });
    if (Element.prototype.webkitRequestFullscreen) Element.prototype.webkitRequestFullscreen = Element.prototype.requestFullscreen;
  }

  // ── text tracks ────────────────────────────────────────────────────────────
  class LXTextTrack extends EventTarget {
    constructor(owner, kind, label, language, id) {
      super();
      this._owner = owner;
      this.kind = kind || 'subtitles'; this.label = label || ''; this.language = language || '';
      this.id = id || ''; this.inBandMetadataTrackDispatchType = '';
      this._mode = 'disabled'; this._cues = []; this._active = []; this.node = null;
      this.readyState = 2; this._loading = false; this._loaded = false;
      this.oncuechange = null;
    }
    get mode() { return this._mode; }
    set mode(m) {
      if (!['disabled', 'hidden', 'showing'].includes(m) || m === this._mode) return;
      this._mode = m;
      if (m !== 'disabled') loadTrack(this);
      else this._active = [];
      later(() => { fire2(this._owner.tracks, 'change'); });
    }
    get cues() { return this._mode === 'disabled' ? null : cueList(this._cues); }
    get activeCues() { return this._mode === 'disabled' ? null : cueList(this._active); }
    addCue(cue) { this._cues.push(cue); this._cues.sort((a, b) => a.startTime - b.startTime); }
    removeCue(cue) { const i = this._cues.indexOf(cue); if (i >= 0) this._cues.splice(i, 1); }
  }
  const cueList = (a) => { const o = Object.create(null); a.forEach((c, i) => o[i] = c); o.length = a.length;
    o.getCueById = (id) => a.find((c) => c.id === id) || null; o[Symbol.iterator] = a[Symbol.iterator].bind(a); return o; };
  class LXTrackList extends EventTarget {
    constructor() { super(); this._a = []; this.onchange = this.onaddtrack = this.onremovetrack = null; }
    get length() { return this._a.length; }
    getTrackById(id) { return this._a.find((t) => t.id === id) || null; }
    [Symbol.iterator]() { return this._a[Symbol.iterator](); }
    _sync() { for (let i = 0; i < Math.max(this._a.length + 1, this._n || 0); i++) { if (i < this._a.length) this[i] = this._a[i]; else delete this[i]; } this._n = this._a.length; }
    _add(t) { this._a.push(t); this._sync(); const e = new Event('addtrack'); e.track = t; ours.add(e); this.dispatchEvent(e); if (this.onaddtrack) this.onaddtrack(e); }
    _remove(t) { const i = this._a.indexOf(t); if (i < 0) return; this._a.splice(i, 1); this._sync(); const e = new Event('removetrack'); e.track = t; ours.add(e); this.dispatchEvent(e); if (this.onremovetrack) this.onremovetrack(e); }
  }
  function fire2(target, type) { const e = new Event(type); ours.add(e); target.dispatchEvent(e); const h = target['on' + type]; if (h) h.call(target, e); }
  function tracksOf(s) { return s.tracks || (s.tracks = new LXTrackList()); }

  function parseTS(t) {
    const m = /^(?:(\d+):)?(\d{1,2}):(\d{2})[.,](\d{3})$/.exec(t.trim());
    return m ? (+(m[1] || 0)) * 3600 + (+m[2]) * 60 + (+m[3]) + (+m[4]) / 1000 : NaN;
  }
  function parseVTT(text) {
    const cues = [];
    const blocks = text.replace(/\r\n?/g, '\n').replace(/^\uFEFF/, '').split(/\n{2,}/);
    for (const b of blocks) {
      const lines = b.split('\n');
      let i = 0;
      if (/^(WEBVTT|NOTE|STYLE|REGION)/.test(lines[0])) continue;
      let id = '';
      if (!lines[0].includes('-->')) { id = lines[0]; i = 1; }
      const m = lines[i] && /^(\S+)\s+-->\s+(\S+)(.*)$/.exec(lines[i]);
      if (!m) continue;
      const start = parseTS(m[1]), end = parseTS(m[2]);
      if (isNaN(start) || isNaN(end)) continue;
      const cue = new VTTCue(start, end, lines.slice(i + 1).join('\n'));
      cue.id = id;
      for (const kv of m[3].trim().split(/\s+/)) {
        const [k, v] = kv.split(':');
        try {
          if (k === 'align' && v) cue.align = v; else if (k === 'line' && v) cue.line = v.endsWith('%') ? parseFloat(v) : (isNaN(+v.split(',')[0]) ? 'auto' : +v.split(',')[0]);
          else if (k === 'position' && v) cue.position = parseFloat(v); else if (k === 'size' && v) cue.size = parseFloat(v);
          else if (k === 'vertical' && v) cue.vertical = v;
        } catch (e) {}
      }
      cues.push(cue);
    }
    return cues;
  }
  function loadTrack(t) {
    if (t._loading || t._loaded || !t.node) return;
    const src = t.node.src || t.node.getAttribute('src');
    if (!src) return;
    t._loading = true; t.readyState = 1;
    fetch(src, { credentials: 'same-origin' }).then((r) => { if (!r.ok) throw new Error(r.status); return r.text(); })
      .then((txt) => { t._cues = parseVTT(txt).sort((a, b) => a.startTime - b.startTime); t._loaded = true; t.readyState = 2; fire(t.node, 'load'); t._owner.lastCueT = -1; })
      .catch(() => { t.readyState = 3; fire(t.node, 'error'); })
      .finally(() => { t._loading = false; });
  }
  // ── tracks inside the file (engine streams) ────────────────────────────────
  class LXAudioTrack {
    constructor(owner, o, idx) {
      this._owner = owner; this._stream = o.i; this._enabled = !!o.sel;
      this.id = String(idx); this.kind = o.def || idx === 0 ? 'main' : 'alternative';
      this.language = o.lang && o.lang !== 'und' ? o.lang : '';
      this.label = o.title || (this.language ? this.language : '') || '';
      this.codec = o.codec; this.channels = o.ch; this.usable = o.ok;
    }
    get enabled() { return this._enabled; }
    set enabled(v) {
      const s = this._owner;
      if (!v || this._enabled || !this.usable || s.id < 0) return;     // exactly one track plays: enable another to switch
      if (!LXM.selectAudio(s.id, this._stream)) return;
      for (const t of s.audioList._a) t._enabled = t === this;
      refresh(s);
      later(() => fire2(s.audioList, 'change'));
    }
  }
  function resetTracks(s) {
    if (s.audioList) { for (const t of [...s.audioList._a]) s.audioList._remove(t); }
    if (s.tracks) for (const t of [...s.tracks._a]) if (t._stream !== undefined) s.tracks._remove(t);
  }
  function loadTracks(s) {
    let o;
    try { o = JSON.parse(LXM.tracks(s.id)); } catch (e) { return; }
    const al = s.audioList || (s.audioList = new LXTrackList());
    resetTracks(s);
    o.audio.forEach((a, i) => al._add(new LXAudioTrack(s, a, i)));
    const tl = tracksOf(s);
    for (const x of o.subs) {
      if (!x.ok) continue;                                  // bitmap subtitles (PGS, VobSub, DVB) are not rendered
      const t = new LXTextTrack(s, 'subtitles', x.title || x.lang || x.codec, x.lang && x.lang !== 'und' ? x.lang : '', 'sub' + x.i);
      t._stream = x.i; t._n = 0; t._loaded = true; t.inBand = true;
      tl._add(t);
    }
  }
  function pullCues(s, tr) {
    if (s.id < 0) return;
    const now = performance.now();
    if (now - (tr._pulled || 0) < 200) return;
    tr._pulled = now;
    const j = LXM.cues(s.id, tr._stream, tr._n);
    if (!j) return;
    const o = JSON.parse(j);
    for (const [a, b, text] of o.c) { const c = new VTTCue(a, b, text); c.id = ''; tr._cues.push(c); }
    tr._cues.sort((x, y) => x.startTime - y.startTime);
    tr._n = o.n; s.cueDirty = true;
  }
  function tracksNeedLoad(s) { /* lazy: tracks load when their mode leaves 'disabled' */ }

  // keep s.tracks in sync with the <track> children
  function syncTrackElements(el) {
    const s = st(el), list = tracksOf(s);
    const nodes = [...el.querySelectorAll(':scope > track')];
    for (const t of [...list._a]) if (t.node && !nodes.includes(t.node)) { list._remove(t); }
    for (const n of nodes) {
      if (list._a.some((t) => t.node === n)) continue;
      const t = new LXTextTrack(s, n.kind || 'subtitles', n.label, n.srclang, n.id);
      t.node = n;
      trackOf.set(n, t);
      list._add(t);
      t._owner = s;
      if (n.hasAttribute('default') && !list._a.some((x) => x !== t && x.mode === 'showing')) t.mode = 'showing';
      n.addEventListener('error', () => {}, false);
    }
  }
  const trackOf = new WeakMap();
  const origTrackDesc = window.HTMLTrackElement && Object.getOwnPropertyDescriptor(HTMLTrackElement.prototype, 'track');
  if (origTrackDesc) Object.defineProperty(HTMLTrackElement.prototype, 'track', { configurable: true, enumerable: true,
    get() { const t = trackOf.get(this); if (t) return t; const p = this.parentNode; if (p instanceof HME) { syncTrackElements(p); const t2 = trackOf.get(this); if (t2) return t2; } return origTrackDesc.get.call(this); } });

  function tickTracks(s) {
    const list = s.tracks;
    if (!list || !list._a.length) return;
    for (const tr of list._a) if (tr._stream !== undefined && tr._mode !== 'disabled') pullCues(s, tr);
    const t = LXM.time(s.id);
    if (t === s.lastCueT && !s.cueDirty) return;
    s.lastCueT = t; s.cueDirty = false;
    for (const tr of list._a) {
      if (tr._mode === 'disabled') continue;
      const now = tr._cues.filter((c) => c.startTime <= t && t < c.endTime);
      const prev = tr._active;
      let changed = now.length !== prev.length || now.some((c, i) => c !== prev[i]);
      if (!changed) continue;
      tr._active = now;
      for (const c of prev) if (!now.includes(c)) { fire(c, 'exit'); if (c.onexit) c.onexit(); }
      for (const c of now) if (!prev.includes(c)) { fire(c, 'enter'); if (c.onenter) c.onenter(); }
      const e = new Event('cuechange'); ours.add(e);
      tr.dispatchEvent(e); if (tr.oncuechange) tr.oncuechange(e);
      if (tr.node) { const e2 = new Event('cuechange'); ours.add(e2); tr.node.dispatchEvent(e2); }
    }
    renderCues(s);
  }

  function renderCues(s) {
    const c = s.ctl;
    if (!c) return;
    const list = s.tracks;
    const showing = list ? list._a.filter((t) => t._mode === 'showing') : [];
    const sig = showing.map((t) => t._active.map((x) => x.startTime + x.text).join('|')).join('#');
    if (sig === c.cueSig) return;
    c.cueSig = sig;
    c.cues.textContent = '';
    for (const tr of showing) for (const cue of tr._active) {
      const d = document.createElement('div');
      try { d.appendChild(cue.getCueAsHTML()); } catch (e) { d.textContent = cue.text; }
      c.cues.appendChild(d);
      c.cues.appendChild(document.createElement('br'));
    }
  }

  // ── HTMLMediaElement.prototype ─────────────────────────────────────────────
  const def = (name, get, set) => Object.defineProperty(proto, name, { configurable: true, enumerable: true, get, set });
  const viaNative = (name, el) => orig[name] && orig[name].get.call(el);

  def('src', function () {
    const s = st(this);
    if (s.native) return viaNative('src', this);
    return s.srcSet ? s.src : (this.getAttribute('src') ? abs(this.getAttribute('src')) : '');
  }, function (v) {
    const s = st(this);
    if (s.native && !s.srcSet) { s.native = false; }
    s.src = String(v); s.srcSet = true; s.autoplayed = false;
    selectResource(this);
  });
  def('currentSrc', function () { const s = st(this); return s.native ? viaNative('currentSrc', this) : s.cur; });
  def('srcObject', function () { return orig.srcObject.get.call(this); }, function (v) {
    const s = st(this);
    closePlayer(s); s.native = true;
    orig.srcObject.set.call(this, v);
  });
  def('currentTime', function () {
    const s = st(this);
    if (s.native) return viaNative('currentTime', this);
    return s.id >= 0 && s.ready >= 1 ? LXM.time(s.id) : s.pendingTime;
  }, function (v) {
    const s = st(this);
    v = +v;
    if (isNaN(v)) throw new TypeError("Failed to set the 'currentTime' property: The provided double value is non-finite.");
    if (s.native) return orig.currentTime.set.call(this, v);
    if (s.id >= 0 && s.ready >= 1) { LXM.seek(s.id, v); refresh(s); s.cueDirty = true; } else s.pendingTime = Math.max(0, v);
  });
  const ro = (name, key, fallback) => def(name, function () { const s = st(this); return s.native ? viaNative(name, this) : key(s); });
  ro('duration', (s) => s.dur);
  ro('paused', (s) => s.paused);
  ro('ended', (s) => s.ended);
  ro('seeking', (s) => s.seeking);
  ro('readyState', (s) => s.ready);
  ro('networkState', (s) => s.net);
  ro('error', (s) => s.err);
  def('buffered', function () { const s = st(this); return s.native ? viaNative('buffered', this) : new LXTimeRanges(s.id >= 0 ? LXM.ranges(s.id, 0) : []); });
  def('seekable', function () { const s = st(this); return s.native ? viaNative('seekable', this) : new LXTimeRanges(s.id >= 0 ? LXM.ranges(s.id, 1) : []); });
  def('played', function () { const s = st(this); return s.native ? viaNative('played', this) : new LXTimeRanges(s.id >= 0 && !s.paused ? [[0, LXM.time(s.id)]] : []); });
  def('volume', function () { const s = st(this); return s.native ? viaNative('volume', this) : s.vol; }, function (v) {
    const s = st(this);
    v = +v;
    if (!(v >= 0 && v <= 1)) throw new DOMException("Failed to set the 'volume' property on 'HTMLMediaElement': The volume provided (" + v + ') is outside the range [0, 1].', 'IndexSizeError');
    if (s.native) return orig.volume.set.call(this, v);
    if (v === s.vol) return;
    s.vol = v; if (s.id >= 0) LXM.setVolume(s.id, v);
    later(() => { fire(this, 'volumechange'); updateControls(s); });
  });
  def('muted', function () { const s = st(this); return s.native ? viaNative('muted', this) : s.muted; }, function (v) {
    const s = st(this);
    if (s.native) return orig.muted.set.call(this, v);
    v = !!v; if (v === s.muted) return;
    s.muted = v; if (s.id >= 0) LXM.setMuted(s.id, v);
    later(() => { fire(this, 'volumechange'); updateControls(s); });
  });
  def('playbackRate', function () { const s = st(this); return s.native ? viaNative('playbackRate', this) : s.rate; }, function (v) {
    const s = st(this);
    if (s.native) return orig.playbackRate.set.call(this, v);
    v = +v; if (!(v > 0) || v === s.rate) return;
    s.rate = Math.min(4, Math.max(0.25, v)); if (s.id >= 0) LXM.setRate(s.id, s.rate);
    later(() => fire(this, 'ratechange'));
  });
  def('audioTracks', function () { const s = st(this); if (s.native) return undefined; return s.audioList || (s.audioList = new LXTrackList()); });
  def('textTracks', function () { const s = st(this); if (s.native) return orig.textTracks.get.call(this); syncTrackElements(this); return tracksOf(s); });
  proto.addTextTrack = function (kind, label, lang) {
    const s = st(this);
    if (s.native) return origAddTrack.call(this, kind, label, lang);
    const t = new LXTextTrack(s, kind, label, lang);
    t._mode = 'hidden'; t._loaded = true;
    tracksOf(s)._add(t);
    return t;
  };
  proto.canPlayType = function (t) { return LXM.canPlay(String(t)); };
  proto.load = function () {
    const s = st(this);
    if (s.native) return origLoad.call(this);
    s.autoplayed = false;
    selectResource(this);
  };
  proto.play = function () {
    const s = st(this);
    if (s.native) return origPlay.call(this);
    if (s.id < 0 && !s.sources) selectResource(this);
    if (s.err && s.err.code === 4 || (s.id < 0 && (!s.sources || !s.sources.length)))
      return Promise.reject(new DOMException('The element has no supported sources.', 'NotSupportedError'));
    if (s.ended) { LXM.seek(s.id, 0); }
    LXM.play(s.id);
    refresh(s);
    return new Promise((resolve, reject) => {
      if (!s.paused && s.ready >= 3 && !s.seeking && !s.waitingPlay) resolve(); else s.plays.push({ resolve, reject });
    });
  };
  proto.pause = function () {
    const s = st(this);
    if (s.native) return origPause.call(this);
    if (s.id < 0) return;
    LXM.pause(s.id);
    refresh(s);
  };
  // pause when the element leaves the DOM (observer below also handles unload)
  if (window.HTMLVideoElement) {
    const vp = HTMLVideoElement.prototype;
    const vw = Object.getOwnPropertyDescriptor(vp, 'videoWidth'), vh = Object.getOwnPropertyDescriptor(vp, 'videoHeight');
    Object.defineProperty(vp, 'videoWidth', { configurable: true, enumerable: true, get() { const s = st(this); return s.native ? vw.get.call(this) : s.vw; } });
    Object.defineProperty(vp, 'videoHeight', { configurable: true, enumerable: true, get() { const s = st(this); return s.native ? vh.get.call(this) : s.vh; } });
    vp.getVideoPlaybackQuality = function () { const s = st(this); return { creationTime: performance.now(), totalVideoFrames: s.frames, droppedVideoFrames: s.dropped, corruptedVideoFrames: 0, totalFrameDelay: 0 }; };
    vp.requestVideoFrameCallback = function (cb) { const s = st(this); const id = ++rvfcId; s.rvfc.push([id, cb]); kick(); return id; };
    vp.cancelVideoFrameCallback = function (id) { const s = st(this); s.rvfc = s.rvfc.filter((x) => x[0] !== id); };
  }
  let rvfcId = 0;

  // `new Audio(url)` would otherwise start a native load of the src attribute.
  const NativeAudio = window.Audio;
  window.Audio = function Audio(src) {
    const a = document.createElement('audio');
    a.preload = 'auto';
    if (src !== undefined) a.src = src;
    return a;
  };
  window.Audio.prototype = NativeAudio.prototype;

  // MSE players must not pick codecs only we can play.
  if (window.MediaSource && MediaSource.isTypeSupported) {
    const o = MediaSource.isTypeSupported.bind(MediaSource);
    MediaSource.isTypeSupported = (t) => (isProprietary(String(t)) ? false : o(t));
  }

  // hls.js 1.6 calls itself supported when MSE takes AV1/VP9, then dies on the
  // first H.264/HEVC segment.  Luxium plays HLS natively (any codec FFmpeg has,
  // seeking, audio-track switching by reloading), so make hls.js step aside and
  // let pages fall back to `video.src = playlist`.
  let hlsRef;
  Object.defineProperty(window, 'Hls', { configurable: true, enumerable: true,
    get() { return hlsRef; },
    set(v) { hlsRef = v; if (v && typeof v.isSupported === 'function') { try { v.isSupported = () => false; } catch (e) {} } } });

  // Engine inventory for apps that decide direct play vs. remux/transcode: names are FFmpeg's
  // (= ffprobe's codec_name / format_name).  Built on first use.
  let caps;
  const getCaps = () => {
    if (caps) return caps;
    const o = JSON.parse(LXM.caps());
    const sets = { video: new Set(o.video), audio: new Set(o.audio), subtitle: new Set(o.subtitle), containers: new Set(o.containers) };
    // ffprobe reports a demuxer family as "matroska,webm" or "mov,mp4,m4a,3gp,3g2,mj2": any member will do
    const hasContainer = (f) => String(f || '').split(',').some((n) => sets.containers.has(n.trim()));
    caps = Object.freeze({
      engine: 'luxium', version: 2,
      decoders: Object.freeze({ video: Object.freeze([...sets.video]), audio: Object.freeze([...sets.audio]), subtitle: Object.freeze([...sets.subtitle]) }),
      containers: Object.freeze([...sets.containers]),
      canDecodeVideo: (codec) => sets.video.has(String(codec).toLowerCase()),
      canDecodeAudio: (codec) => sets.audio.has(String(codec).toLowerCase()),
      canDemux: hasContainer,
      audioTracks: true, embeddedSubtitles: true,
    });
    return caps;
  };
  try { Object.defineProperty(Navigator.prototype, 'luxiumMedia', { configurable: true, enumerable: true, get: getCaps }); } catch (e) {}

  // ── discovering elements the parser created (src/<source>/<track>/controls) ─
  function adopt(el) {
    if (tracked.has(el)) return;
    tracked.add(el);
    const s = st(el);
    if (!s.srcSet && !s.native && (el.getAttribute('src') || el.querySelector('source[src]'))) selectResource(el);
    if (el.querySelector('track')) syncTrackElements(el);
  }
  function scan(root) {
    if (root.nodeType !== 1) return;
    if (root instanceof HME) adopt(root);
    else if (root.querySelectorAll) for (const m of root.querySelectorAll('video,audio')) adopt(m);
  }
  function watch() {
    const mo = new MutationObserver((muts) => {
      for (const m of muts) {
        if (m.type === 'attributes') {
          const t = m.target;
          if (t instanceof HME) {
            const s = st(t);
            if (m.attributeName === 'src' && !s.srcSet && !s.native) selectResource(t);
            if (m.attributeName === 'controls') kick();
          }
          continue;
        }
        for (const n of m.addedNodes) {
          if (n.nodeType !== 1) continue;
          scan(n);
          const p = n.parentNode;
          if (p instanceof HME) {
            const s = st(p);
            if (n.localName === 'source' && !s.srcSet && !s.native && !p.getAttribute('src') && s.id < 0 && (s.net === 0 || s.net === 3)) selectResource(p);
            if (n.localName === 'track') syncTrackElements(p);
          }
        }
        for (const n of m.removedNodes) {
          if (n.nodeType !== 1) continue;
          if (n instanceof HME) {
            const s = S.get(n);
            if (s && !s.native && s.id >= 0 && !s.paused) { LXM.pause(s.id); refresh(s); }
            if (s && s.surface) s.surface.c.style.display = 'none';
            if (s && s.ctl) s.ctl.host.remove();
          } else if (m.target instanceof HME && n.localName === 'track') syncTrackElements(m.target);
        }
      }
    });
    mo.observe(document, { childList: true, subtree: true, attributes: true, attributeFilter: ['src', 'controls'] });
    scan(document.documentElement || document);
    const style = document.createElement('style');
    style.textContent = ':where(video[data-lx-size]){contain:size;contain-intrinsic-size:var(--lx-w) var(--lx-h);aspect-ratio:var(--lx-ar)}' +
      'video::-webkit-media-controls,audio::-webkit-media-controls{display:none!important}' +
      'video::-webkit-media-controls-enclosure{display:none!important}video::-webkit-media-text-track-container{display:none!important}';
    (document.head || document.documentElement).appendChild(style);
  }
  if (document.documentElement) watch();
  else { const w = new MutationObserver(() => { if (document.documentElement) { w.disconnect(); watch(); } }); w.observe(document, { childList: true }); }

  // Page going away: the native side also frees everything when the context dies.
  window.addEventListener('pagehide', () => { for (const s of [...byId.values()]) closePlayer(s); });
  // Debug/test hook: per-player counters (ms are cumulative: native copy of the frame, WebGL upload).
  window.__luxiumMediaStats = () => [...active].map((s) => ({ id: s.id, src: s.cur, frames: s.frames, last: s.lastSeq, t: LXM.time(s.id),
    copyMs: s.msCopy || 0, uploadMs: s.msUpload || 0, drawMs: s.msDraw || 0, size: s.vw + 'x' + s.vh,
    engine: JSON.parse(LXM.stats(s.id)) }));
})();
