(() => {
  "use strict";
  const HEADER = 16, MAGIC = 0x3153584d;
  const $ = (id) => document.getElementById(id);
  const wsBase = (location.protocol === "https:" ? "wss://" : "ws://") + location.host;

  let ctx = null, node = null, ws = null, playing = false;
  let fmt = null;                 // {sampleRate, bitDepth, channels} of the stream
  let latencyMs = Number(localStorage.getItem("mxs-latency") || 250);
  let reconnectTimer = null;
  let lastSeq = -1, lost = 0, rxBytes = 0, rxT0 = performance.now();
  const problems = [];   // timestamps of lost-packet / underrun events
  let lastUnderruns = 0;

  // ------------------------------------------------------------ status polling
  async function pollStatus() {
    try {
      const s = await fetch("/api/status", { cache: "no-store" }).then((r) => r.json());
      setSource(s.sourceConnected);
      if (!playing && s.format) showFormat(s.format);
    } catch { setSource(null); }
  }
  setInterval(pollStatus, 2000);
  pollStatus();

  function setSource(on) {
    const dot = $("srcDot"), t = $("srcText");
    dot.className = "dot" + (on ? " on" : on === false ? " wait" : "");
    t.textContent = on ? "Live" : on === false ? "Waiting for DAW" : "Server offline";
  }
  function showFormat(f) {
    $("sr").textContent = (f.sampleRate / 1000).toFixed(f.sampleRate % 1000 ? 1 : 0) + " kHz";
    $("bd").textContent = f.bitDepth + "-bit";
  }

  // ------------------------------------------------------------ latency selector
  const seg = $("latency");
  function markLatency() {
    for (const b of seg.children) b.classList.toggle("active", Number(b.dataset.ms) === latencyMs);
  }
  seg.addEventListener("click", (e) => {
    const b = e.target.closest("button"); if (!b) return;
    latencyMs = Number(b.dataset.ms);
    try { localStorage.setItem("mxs-latency", String(latencyMs)); } catch {}
    markLatency();
    node?.port.postMessage({ type: "latency", ms: latencyMs });
  });
  markLatency();

  // ------------------------------------------------------------ play / stop
  $("play").addEventListener("click", () => (playing ? stop() : start()));

  async function start() {
    playing = true;
    setButton();
    msg("Starting audio…");
    try {
      if (!ctx) {
        // Try to run the context at the stream rate (no resampling); fall back to device rate.
        const want = fmt?.sampleRate || 48000;
        try { ctx = new AudioContext({ sampleRate: want, latencyHint: "playback" }); }
        catch { ctx = new AudioContext({ latencyHint: "playback" }); }
        await ctx.audioWorklet.addModule("worklet.js");
        node = new AudioWorkletNode(ctx, "stream-player", { numberOfInputs: 0, outputChannelCount: [2] });
        node.port.onmessage = onWorklet;
        node.connect(ctx.destination);
      }
      await ctx.resume();
      lost = 0; lastUnderruns = 0; problems.length = 0;
      $("lost").textContent = "0"; $("warn").hidden = true;
      node.port.postMessage({ type: "reset" });
      node.port.postMessage({ type: "latency", ms: latencyMs });
      if (fmt) node.port.postMessage({ type: "format", sampleRate: fmt.sampleRate });
      connect();
    } catch (e) {
      msg("Audio error: " + e.message);
      playing = false; setButton();
    }
  }

  function stop() {
    playing = false;
    setButton();
    clearTimeout(reconnectTimer);
    if (ws) { ws.onclose = null; ws.close(); ws = null; }
    node?.port.postMessage({ type: "reset" });
    ctx?.suspend();
    msg("Stopped");
    $("fill").textContent = "—";
  }

  function setButton() {
    $("play").classList.toggle("stop", playing);
    $("play").setAttribute("aria-label", playing ? "Stop" : "Play");
    $("icoPlay").toggleAttribute("hidden", playing); $("icoStop").toggleAttribute("hidden", !playing);
  }
  const msg = (t) => ($("msg").textContent = t);

  // ------------------------------------------------------------ websocket
  function connect() {
    lastSeq = -1;
    ws = new WebSocket(wsBase + "/listen");
    ws.binaryType = "arraybuffer";
    ws.onopen = () => msg("Connected — buffering…");
    ws.onmessage = (e) => {
      if (typeof e.data === "string") {
        try {
          const s = JSON.parse(e.data);
          if (s.type === "status") { setSource(s.sourceConnected); if (!s.sourceConnected) msg("Waiting for DAW…"); }
        } catch {}
        return;
      }
      onAudio(e.data);
    };
    ws.onclose = () => {
      ws = null;
      if (!playing) return;
      msg("Connection lost — reconnecting…");
      reconnectTimer = setTimeout(connect, 1500);
    };
    ws.onerror = () => {};
  }

  function onAudio(ab) {
    if (ab.byteLength < HEADER) return;
    const dv = new DataView(ab);
    if (dv.getUint32(0, true) !== MAGIC) return;
    const sampleRate = dv.getUint32(4, true), bitDepth = dv.getUint8(8), channels = dv.getUint8(9);
    const seq = dv.getUint32(12, true);
    let gap = false;
    if (lastSeq >= 0 && seq !== ((lastSeq + 1) >>> 0)) {
      const missed = (seq - lastSeq - 1) >>> 0;
      if (missed < 100000) { lost += missed; gap = true; problem(); }
    }
    lastSeq = seq;
    rxBytes += ab.byteLength;

    if (!fmt || fmt.sampleRate !== sampleRate || fmt.bitDepth !== bitDepth || fmt.channels !== channels) {
      fmt = { sampleRate, bitDepth, channels };
      showFormat(fmt);
      node.port.postMessage({ type: "format", sampleRate });
    }

    const bps = bitDepth >> 3;
    const frames = Math.floor((ab.byteLength - HEADER) / (bps * channels));
    const out = new Float32Array(frames * 2);
    const u8 = new Uint8Array(ab, HEADER);
    let o = 0;
    if (bitDepth === 16) {
      const s16 = 1 / 32768;
      for (let f = 0; f < frames; f++) {
        for (let c = 0; c < channels; c++, o += 2) {
          let v = u8[o] | (u8[o + 1] << 8);
          if (v & 0x8000) v -= 0x10000;
          out[f * 2 + c] = v * s16;
        }
        if (channels === 1) out[f * 2 + 1] = out[f * 2];
      }
    } else {
      const s24 = 1 / 8388608;
      for (let f = 0; f < frames; f++) {
        for (let c = 0; c < channels; c++, o += 3) {
          let v = u8[o] | (u8[o + 1] << 8) | (u8[o + 2] << 16);
          if (v & 0x800000) v -= 0x1000000;
          out[f * 2 + c] = v * s24;
        }
        if (channels === 1) out[f * 2 + 1] = out[f * 2];
      }
    }
    node.port.postMessage({ type: "data", samples: out, gap }, [out.buffer]);
  }

  // ------------------------------------------------------------ connection warning
  function problem() { problems.push(performance.now()); }
  function updateWarning() {
    const now = performance.now();
    while (problems.length && now - problems[0] > 30000) problems.shift();
    const show = problems.length >= 3;
    $("warn").toggleAttribute("hidden", !show);
    if (show) $("warn").textContent = latencyMs < 500
      ? "Unstable connection — switch latency to High (500 ms)."
      : "Unstable connection — try a faster network (Wi-Fi / LAN).";
  }

  // ------------------------------------------------------------ meters
  const lvl = { pL: 0, pR: 0, rL: 0, rR: 0, hL: 0, hR: 0, hLt: 0, hRt: 0 };
  let lastState = "";
  function onWorklet(e) {
    const m = e.data;
    if (m.type !== "meter") return;
    lvl.pL = Math.max(lvl.pL, m.peakL); lvl.pR = Math.max(lvl.pR, m.peakR);
    lvl.rL = m.rmsL; lvl.rR = m.rmsR;
    if (playing) {
      $("fill").textContent = Math.round(m.fillMs) + " ms";
      $("xr").textContent = m.underruns;
      $("lost").textContent = lost;
      if (m.underruns > lastUnderruns) problem();
      lastUnderruns = m.underruns;
      const now = performance.now();
      if (now - rxT0 >= 1000) {
        $("rate").textContent = Math.round((rxBytes * 8) / (now - rxT0)) + " kbit/s";
        rxBytes = 0; rxT0 = now;
      }
      updateWarning();
      if (m.state !== lastState && ws && ws.readyState === 1) {
        msg(m.state === "playing" ? "Playing" : "Buffering…");
      }
      lastState = m.state;
    }
  }

  const cv = $("meter"), g = cv.getContext("2d");
  const toDb = (v) => (v > 1e-6 ? 20 * Math.log10(v) : -120);
  const pos = (db) => Math.max(0, Math.min(1, (db + 60) / 60));
  const disp = { pL: -120, pR: -120, rL: -120, rR: -120 };
  let lastT = performance.now();

  function draw(t) {
    const dt = Math.min(0.1, (t - lastT) / 1000); lastT = t;
    const dpr = window.devicePixelRatio || 1;
    const W = cv.clientWidth, H = cv.clientHeight;
    if (cv.width !== W * dpr || cv.height !== H * dpr) { cv.width = W * dpr; cv.height = H * dpr; }
    g.setTransform(dpr, 0, 0, dpr, 0, 0);
    g.clearRect(0, 0, W, H);

    // ballistics: instant attack, 20 dB/s peak fall, 300 ms RMS smoothing, 1.5 s peak hold
    for (const ch of ["L", "R"]) {
      const p = toDb(lvl["p" + ch]), r = toDb(lvl["r" + ch]);
      disp["p" + ch] = Math.max(p, disp["p" + ch] - 20 * dt);
      disp["r" + ch] = r > disp["r" + ch] ? r : disp["r" + ch] + (r - disp["r" + ch]) * Math.min(1, dt / 0.3);
      if (p >= lvl["h" + ch] || t - lvl["h" + ch + "t"] > 1500) { lvl["h" + ch] = p; lvl["h" + ch + "t"] = t; }
    }
    lvl.pL = lvl.pR = 0;

    const barH = (H - 10) / 2;
    [["L", 0], ["R", barH + 10]].forEach(([ch, y]) => {
      g.fillStyle = "#1d222b"; g.fillRect(0, y, W, barH);
      const grad = g.createLinearGradient(0, 0, W, 0);
      grad.addColorStop(0, "#1f9d57"); grad.addColorStop(pos(-18), "#33d17a");
      grad.addColorStop(pos(-9), "#f5c044"); grad.addColorStop(pos(-3), "#ff7a45"); grad.addColorStop(1, "#ff5d5d");
      g.fillStyle = grad;
      g.fillRect(0, y, pos(disp["r" + ch]) * W, barH);                       // RMS bar
      g.globalAlpha = 0.35; g.fillRect(0, y + barH * 0.35, pos(disp["p" + ch]) * W, barH * 0.3); g.globalAlpha = 1; // peak
      const hx = pos(lvl["h" + ch]) * W;                                        // peak hold line
      g.fillStyle = lvl["h" + ch] >= -0.1 ? "#ff5d5d" : "#e7eaf0";
      if (lvl["h" + ch] > -60) g.fillRect(Math.min(W - 2, hx), y, 2, barH);
      g.fillStyle = "rgba(14,16,20,.55)";
      for (let d = -50; d < 0; d += 10) g.fillRect(pos(d) * W, y, 1, barH);   // scale ticks
    });
    const f = (v) => (v > -60 ? v.toFixed(1) : "-∞");
    $("pkL").textContent = `L  pk ${f(lvl.hL)}  rms ${f(disp.rL)}`;
    $("pkR").textContent = `R  pk ${f(lvl.hR)}  rms ${f(disp.rR)}`;
    requestAnimationFrame(draw);
  }
  requestAnimationFrame(draw);
})();
