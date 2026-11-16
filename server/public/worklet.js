/**
 * Jitter-buffered stereo stream player.
 * - ring buffer filled from the main thread (Float32 interleaved)
 * - waits for `target` ms of audio before starting, re-buffers on underrun
 * - compensates clock drift between DAW and browser by a tiny (≤ ±0.2 %)
 *   resampling-ratio correction, hard resync if the buffer runs away
 * - 4-point Hermite interpolation (also handles stream SR != context SR)
 * - posts Peak/RMS meters ~30x per second
 */
class StreamPlayer extends AudioWorkletProcessor {
  constructor() {
    super();
    this.capacity = 0;
    this.buf = null;            // interleaved L/R
    this.writePos = 0;          // absolute frames written
    this.readPos = 0;           // absolute integer frame index
    this.frac = 0;              // fractional read offset
    this.srcRate = sampleRate;
    this.baseRatio = 1;
    this.targetMs = 250;
    this.state = "buffering";
    this.fillAvg = 0;
    this.gain = 0;              // de-click ramp
    this.underruns = 0;
    this.meter = { pL: 0, pR: 0, sL: 0, sR: 0, n: 0 };
    this.meterEvery = Math.round(sampleRate / 30);
    this.alloc(48000);

    this.port.onmessage = (e) => {
      const m = e.data;
      if (m.type === "data") this.push(m.samples);
      else if (m.type === "format") this.setFormat(m.sampleRate);
      else if (m.type === "latency") { this.targetMs = m.ms; this.rebuffer(); }
      else if (m.type === "reset") this.rebuffer(true);
    };
  }

  alloc(rate) {
    this.capacity = Math.ceil(rate * 4); // 4 s
    this.buf = new Float32Array(this.capacity * 2);
    this.writePos = this.readPos = 0;
    this.frac = 0;
  }

  setFormat(rate) {
    if (rate === this.srcRate && this.buf) return;
    this.srcRate = rate;
    this.baseRatio = rate / sampleRate;
    this.alloc(rate);
    this.state = "buffering";
  }

  rebuffer(clear) {
    if (clear) { this.writePos = this.readPos = 0; this.frac = 0; }
    this.state = "buffering";
  }

  get targetFrames() { return (this.targetMs / 1000) * this.srcRate; }
  fill() { return this.writePos - this.readPos - this.frac; }

  push(s) {
    const frames = s.length >> 1;
    const cap = this.capacity;
    for (let i = 0; i < frames; i++) {
      const w = (this.writePos % cap) * 2;
      this.buf[w] = s[2 * i];
      this.buf[w + 1] = s[2 * i + 1];
      this.writePos++;
    }
    // overflow: keep newest audio
    if (this.writePos - this.readPos > cap - 8) {
      this.readPos = this.writePos - Math.floor(this.targetFrames);
      this.frac = 0;
    }
  }

  sample(idx, ch) {
    return this.buf[((idx % this.capacity) * 2) + ch];
  }

  process(_inputs, outputs) {
    const out = outputs[0];
    const L = out[0], R = out[1] || out[0];
    const n = L.length;
    const target = this.targetFrames;
    let fill = this.fill();

    if (this.state === "buffering") {
      if (fill >= target) {
        this.state = "playing";
        this.fillAvg = fill;
        this.gain = 0;
      } else {
        L.fill(0); if (R !== L) R.fill(0);
        this.meterTick(L, R, n);
        return true;
      }
    }

    // drift control
    this.fillAvg += (fill - this.fillAvg) * 0.002;
    const err = (this.fillAvg - target) / target;
    let corr = 0;
    if (Math.abs(err) > 0.15) corr = Math.max(-0.002, Math.min(0.002, err * 0.004));
    if (this.fillAvg > target * 3 + this.srcRate * 0.2) {
      // way behind (e.g. tab was suspended): jump to target latency
      this.readPos = this.writePos - Math.floor(target);
      this.frac = 0;
      this.fillAvg = target;
    }
    const step = this.baseRatio * (1 + corr);

    for (let i = 0; i < n; i++) {
      if (this.writePos - this.readPos < 3) {
        // underrun → silence, go back to buffering (fade-in on restart)
        for (let j = i; j < n; j++) { L[j] = 0; if (R !== L) R[j] = 0; }
        this.underruns++;
        this.state = "buffering";
        break;
      }
      const t = this.frac, i0 = this.readPos;
      for (let ch = 0; ch < 2; ch++) {
        const ym1 = i0 > 0 ? this.sample(i0 - 1, ch) : this.sample(i0, ch);
        const y0 = this.sample(i0, ch), y1 = this.sample(i0 + 1, ch), y2 = this.sample(i0 + 2, ch);
        const c1 = 0.5 * (y1 - ym1);
        const c2 = ym1 - 2.5 * y0 + 2 * y1 - 0.5 * y2;
        const c3 = 0.5 * (y2 - ym1) + 1.5 * (y0 - y1);
        const v = ((c3 * t + c2) * t + c1) * t + y0;
        if (ch === 0) L[i] = v * this.gain; else if (R !== L) R[i] = v * this.gain;
      }
      if (this.gain < 1) this.gain = Math.min(1, this.gain + 1 / 256);
      this.frac += step;
      const adv = Math.floor(this.frac);
      this.readPos += adv;
      this.frac -= adv;
    }
    this.meterTick(L, R, n);
    return true;
  }

  meterTick(L, R, n) {
    const m = this.meter;
    for (let i = 0; i < n; i++) {
      const a = L[i], b = R[i];
      const aa = a < 0 ? -a : a, bb = b < 0 ? -b : b;
      if (aa > m.pL) m.pL = aa;
      if (bb > m.pR) m.pR = bb;
      m.sL += a * a; m.sR += b * b;
    }
    m.n += n;
    if (m.n >= this.meterEvery) {
      this.port.postMessage({
        type: "meter",
        peakL: m.pL, peakR: m.pR,
        rmsL: Math.sqrt(m.sL / m.n), rmsR: Math.sqrt(m.sR / m.n),
        fillMs: (this.fill() / this.srcRate) * 1000,
        state: this.state, underruns: this.underruns,
      });
      m.pL = m.pR = m.sL = m.sR = 0; m.n = 0;
    }
  }
}
registerProcessor("stream-player", StreamPlayer);
