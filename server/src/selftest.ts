/** End-to-end test: server (no tunnel) + fake source + listener. */
import { spawn } from "node:child_process";
import { tmpdir } from "node:os";
import { join, dirname } from "node:path";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { WebSocket } from "ws";
import { startFakeSource } from "./fake-source.js";
import { parseHeader } from "./protocol.js";

const WEB = 13000, ING = 18765;
const urlFile = join(tmpdir(), `mxs-selftest-${process.pid}`);
const here = dirname(fileURLToPath(import.meta.url));
const fail = (m: string) => { console.error("FAIL:", m); process.exit(1); };
const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));

const srv = spawn(process.execPath, [join(here, "index.js"), "--no-tunnel"], {
  env: { ...process.env, WEB_PORT: String(WEB), INGEST_PORT: String(ING), MIX_STREAMER_URL_FILE: urlFile },
  stdio: "inherit",
});
const done = (code: number) => { srv.kill(); process.exit(code); };

await sleep(800);
const html = await fetch(`http://127.0.0.1:${WEB}/`).then((r) => r.text());
if (!html.includes("<html")) fail("index.html not served");
const wl = await fetch(`http://127.0.0.1:${WEB}/worklet.js`);
if (!wl.ok) fail("worklet.js not served");
if ((await fetch(`http://127.0.0.1:${WEB}/../package.json`)).status === 200) fail("path traversal");
if (!readFileSync(urlFile, "utf8").startsWith("http")) fail("url file not written");

let lastCount = -1;
const src = startFakeSource({ url: `ws://127.0.0.1:${ING}/ingest`, sampleRate: 48000, bitDepth: 24, onListeners: (n) => (lastCount = n) });

const ws = new WebSocket(`ws://127.0.0.1:${WEB}/listen`);
let frames = 0, samples = 0, lastSeq = -1, gaps = 0;
ws.on("message", (d, bin) => {
  if (!bin) return;
  const h = parseHeader(d as Buffer);
  if (!h) fail("bad frame");
  if (lastSeq >= 0 && h!.sequence !== lastSeq + 1) gaps++;
  lastSeq = h!.sequence;
  frames++;
  samples += ((d as Buffer).length - 16) / 6;
});

await sleep(2000);
src.stop();
ws.close();
console.log(`listener got ${frames} frames, ${samples} stereo samples, seq gaps=${gaps}, listener count reported to source=${lastCount}`);
if (frames < 150) fail("too few frames");
if (gaps) fail("sequence gaps");
if (lastCount !== 1) fail("listener count not reported");
console.log("SELFTEST OK");
done(0);
