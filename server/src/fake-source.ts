/**
 * Test signal source that behaves like the VST3 plugin:
 * connects to ws://127.0.0.1:8765/ingest and streams a stereo sine
 * (L = 440 Hz, R = 660 Hz, -12 dBFS) in 10 ms frames.
 *
 *   node dist/fake-source.js [sampleRate=48000] [bitDepth=24]
 */
import { WebSocket } from "ws";
import { fileURLToPath } from "node:url";
import { HEADER_SIZE, MAGIC } from "./protocol.js";

export function startFakeSource(opts: {
  url?: string;
  sampleRate?: number;
  bitDepth?: 16 | 24;
  onListeners?: (n: number) => void;
}): { stop: () => void } {
  const url = opts.url || `ws://127.0.0.1:${process.env.INGEST_PORT || 8765}/ingest`;
  const sr = opts.sampleRate || 48000;
  const bits = opts.bitDepth || 24;
  const framesPerChunk = Math.round(sr / 100);
  const bps = bits / 8;
  let phaseL = 0;
  let phaseR = 0;
  let seq = 0;
  let ws: WebSocket | null = null;
  let timer: NodeJS.Timeout | null = null;
  let stopped = false;

  const connect = () => {
    if (stopped) return;
    ws = new WebSocket(url);
    ws.on("open", () => {
      console.log(`[fake-source] connected, streaming ${sr} Hz / ${bits}-bit`);
      const t0 = Date.now();
      let sent = 0;
      timer = setInterval(() => {
        const due = Math.floor(((Date.now() - t0) / 1000) * sr / framesPerChunk);
        while (sent < due && ws?.readyState === WebSocket.OPEN) {
          ws.send(makeChunk());
          sent++;
        }
      }, 5);
    });
    ws.on("message", (d, isBinary) => {
      if (isBinary) return;
      try {
        const m = JSON.parse(d.toString());
        if (m.type === "listeners") opts.onListeners?.(m.count);
      } catch { /* ignore */ }
    });
    ws.on("close", () => {
      if (timer) clearInterval(timer);
      if (!stopped) setTimeout(connect, 1000);
    });
    ws.on("error", () => { /* close follows */ });
  };

  const makeChunk = (): Buffer => {
    const buf = Buffer.alloc(HEADER_SIZE + framesPerChunk * 2 * bps);
    buf.writeUInt32LE(MAGIC, 0);
    buf.writeUInt32LE(sr, 4);
    buf.writeUInt8(bits, 8);
    buf.writeUInt8(2, 9);
    buf.writeUInt16LE(0, 10);
    buf.writeUInt32LE(seq++ >>> 0, 12);
    const amp = 0.25;
    const max = bits === 16 ? 32767 : 8388607;
    let o = HEADER_SIZE;
    for (let i = 0; i < framesPerChunk; i++) {
      const l = Math.round(Math.sin(phaseL) * amp * max);
      const r = Math.round(Math.sin(phaseR) * amp * max);
      phaseL += (2 * Math.PI * 440) / sr;
      phaseR += (2 * Math.PI * 660) / sr;
      if (bits === 16) {
        buf.writeInt16LE(l, o); buf.writeInt16LE(r, o + 2); o += 4;
      } else {
        buf.writeIntLE(l, o, 3); buf.writeIntLE(r, o + 3, 3); o += 6;
      }
    }
    phaseL %= 2 * Math.PI;
    phaseR %= 2 * Math.PI;
    return buf;
  };

  connect();
  return {
    stop: () => {
      stopped = true;
      if (timer) clearInterval(timer);
      ws?.close();
    },
  };
}

if (process.argv[1] === fileURLToPath(import.meta.url)) {
  const sr = Number(process.argv[2] || 48000);
  const bits = Number(process.argv[3] || 24) === 16 ? 16 : 24;
  startFakeSource({ sampleRate: sr, bitDepth: bits, onListeners: (n) => console.log(`[fake-source] listeners: ${n}`) });
}
