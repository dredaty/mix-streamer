import http from "node:http";
import { readFile, stat } from "node:fs/promises";
import { networkInterfaces } from "node:os";
import { dirname, extname, join, normalize, sep } from "node:path";
import { fileURLToPath } from "node:url";
import { WebSocketServer, WebSocket, type RawData } from "ws";
import { parseHeader, type StreamFormat } from "./protocol.js";
import { CloudflareTunnel, URL_FILE, removeUrlFile, writeUrlFile } from "./tunnel.js";

const args = new Set(process.argv.slice(2));
const WEB_PORT = Number(process.env.WEB_PORT || 3000);
const INGEST_PORT = Number(process.env.INGEST_PORT || 8765);
const INGEST_HOST = process.env.INGEST_HOST || "127.0.0.1";
const USE_TUNNEL = !args.has("--no-tunnel") && process.env.NO_TUNNEL !== "1";
/** Drop frames for a listener whose socket send queue exceeds this (slow client). */
const MAX_LISTENER_BACKLOG = 1024 * 1024;

const PUBLIC_DIR = join(dirname(fileURLToPath(import.meta.url)), "..", "public");

const MIME: Record<string, string> = {
  ".html": "text/html; charset=utf-8",
  ".js": "text/javascript; charset=utf-8",
  ".css": "text/css; charset=utf-8",
  ".svg": "image/svg+xml",
  ".json": "application/json",
  ".ico": "image/x-icon",
  ".png": "image/png",
};

// ---------------------------------------------------------------- state
let source: WebSocket | null = null;
let currentFormat: StreamFormat | null = null;
let framesIn = 0;
let bytesIn = 0;
let publicUrl: string | null = null;

const listeners = new Set<WebSocket>();

function statusPayload() {
  return {
    type: "status",
    sourceConnected: source !== null && source.readyState === WebSocket.OPEN,
    listeners: listeners.size,
    format: currentFormat,
    publicUrl,
  };
}

function broadcastStatus(): void {
  const msg = JSON.stringify(statusPayload());
  for (const l of listeners) if (l.readyState === WebSocket.OPEN) l.send(msg);
  if (source && source.readyState === WebSocket.OPEN) {
    source.send(JSON.stringify({ type: "listeners", count: listeners.size }));
  }
}

// ---------------------------------------------------------------- HTTP + /listen
const httpServer = http.createServer(async (req, res) => {
  try {
    const url = new URL(req.url || "/", "http://localhost");
    if (url.pathname === "/api/status") {
      res.writeHead(200, { "Content-Type": "application/json", "Cache-Control": "no-store" });
      res.end(JSON.stringify(statusPayload()));
      return;
    }

    let rel = decodeURIComponent(url.pathname);
    if (rel.endsWith("/")) rel += "index.html";
    const filePath = normalize(join(PUBLIC_DIR, rel));
    if (!filePath.startsWith(PUBLIC_DIR + sep)) {
      res.writeHead(403).end("Forbidden");
      return;
    }
    const st = await stat(filePath).catch(() => null);
    if (!st || !st.isFile()) {
      res.writeHead(404).end("Not found");
      return;
    }
    const body = await readFile(filePath);
    res.writeHead(200, {
      "Content-Type": MIME[extname(filePath)] || "application/octet-stream",
      "Cache-Control": "no-cache",
    });
    res.end(body);
  } catch (e) {
    res.writeHead(500).end("Server error");
    console.error("[http]", e);
  }
});

const listenWss = new WebSocketServer({ server: httpServer, path: "/listen", perMessageDeflate: false });

listenWss.on("connection", (ws, req) => {
  listeners.add(ws);
  const who = (req.headers["cf-connecting-ip"] as string) || req.socket.remoteAddress;
  console.log(`[listen] + ${who}  (listeners: ${listeners.size})`);
  ws.send(JSON.stringify(statusPayload()));
  broadcastStatus();

  // Keep-alive through Cloudflare (idle WS connections get closed after ~100 s).
  const ping = setInterval(() => ws.readyState === WebSocket.OPEN && ws.ping(), 20000);

  ws.on("close", () => {
    clearInterval(ping);
    listeners.delete(ws);
    console.log(`[listen] - ${who}  (listeners: ${listeners.size})`);
    broadcastStatus();
  });
  ws.on("error", () => ws.terminate());
  ws.on("message", () => {
    /* listeners are receive-only */
  });
});

// ---------------------------------------------------------------- /ingest (plugin)
const ingestWss = new WebSocketServer({
  host: INGEST_HOST,
  port: INGEST_PORT,
  path: "/ingest",
  perMessageDeflate: false,
  maxPayload: 4 * 1024 * 1024,
});

ingestWss.on("connection", (ws, req) => {
  if (source && source !== ws) {
    console.log("[ingest] new source connected – replacing previous one");
    source.close(4000, "replaced");
  }
  source = ws;
  console.log(`[ingest] source connected from ${req.socket.remoteAddress}`);
  broadcastStatus();

  ws.on("message", (data: RawData, isBinary: boolean) => {
    if (!isBinary) return; // plugin currently sends no text messages
    const buf = Array.isArray(data) ? Buffer.concat(data) : Buffer.isBuffer(data) ? data : Buffer.from(data);
    const hdr = parseHeader(buf);
    if (!hdr) return;

    framesIn++;
    bytesIn += buf.length;
    if (
      !currentFormat ||
      currentFormat.sampleRate !== hdr.sampleRate ||
      currentFormat.bitDepth !== hdr.bitDepth ||
      currentFormat.channels !== hdr.channels
    ) {
      currentFormat = { sampleRate: hdr.sampleRate, bitDepth: hdr.bitDepth, channels: hdr.channels };
      console.log(`[ingest] format: ${hdr.sampleRate} Hz / ${hdr.bitDepth}-bit / ${hdr.channels} ch`);
      broadcastStatus();
    }

    for (const l of listeners) {
      if (l.readyState !== WebSocket.OPEN) continue;
      if (l.bufferedAmount > MAX_LISTENER_BACKLOG) continue; // slow client: drop, player re-buffers
      l.send(buf, { binary: true });
    }
  });

  ws.on("close", () => {
    if (source === ws) {
      source = null;
      console.log("[ingest] source disconnected");
      broadcastStatus();
    }
  });
  ws.on("error", (e) => console.warn("[ingest] error:", e.message));
});

ingestWss.on("error", (e: NodeJS.ErrnoException) => {
  console.error(`[ingest] cannot listen on ${INGEST_HOST}:${INGEST_PORT}: ${e.message}`);
  process.exit(1);
});

// periodic listener-count refresh for the plugin + stats
setInterval(() => {
  if (source && source.readyState === WebSocket.OPEN) {
    source.send(JSON.stringify({ type: "listeners", count: listeners.size }));
  }
}, 2000);

setInterval(() => {
  if (process.env.STATS && framesIn > 0) {
    console.log(`[stats] ${framesIn} frames, ${(bytesIn / 1024).toFixed(0)} KiB in last 10 s`);
  }
  framesIn = 0;
  bytesIn = 0;
}, 10000);

// ---------------------------------------------------------------- startup
function lanUrl(): string {
  for (const ifs of Object.values(networkInterfaces())) {
    for (const i of ifs || []) {
      if (i.family === "IPv4" && !i.internal) return `http://${i.address}:${WEB_PORT}`;
    }
  }
  return `http://localhost:${WEB_PORT}`;
}

let tunnel: CloudflareTunnel | null = null;

httpServer.on("error", (e: NodeJS.ErrnoException) => {
  console.error(`[http] cannot listen on port ${WEB_PORT}: ${e.message}`);
  process.exit(1);
});

httpServer.listen(WEB_PORT, () => {
  console.log(`[http]   web player:   http://localhost:${WEB_PORT}`);
  console.log(`[ingest] plugin socket: ws://${INGEST_HOST}:${INGEST_PORT}/ingest`);

  removeUrlFile();
  if (USE_TUNNEL) {
    tunnel = new CloudflareTunnel(WEB_PORT, (url) => {
      publicUrl = url;
      writeUrlFile(url);
      console.log("");
      console.log("  ==============================================================");
      console.log(`   PUBLIC LISTEN URL:  ${url}`);
      console.log(`   (saved to ${URL_FILE})`);
      console.log("  ==============================================================");
      console.log("");
      broadcastStatus();
    });
    tunnel.start();
  } else {
    publicUrl = lanUrl();
    writeUrlFile(publicUrl);
    console.log(`[tunnel] disabled – LAN URL ${publicUrl} saved to ${URL_FILE}`);
  }
});

function shutdown(): void {
  console.log("\nshutting down…");
  tunnel?.stop();
  removeUrlFile();
  for (const l of listeners) l.terminate();
  source?.terminate();
  ingestWss.close();
  httpServer.close();
  setTimeout(() => process.exit(0), 300).unref();
}
process.on("SIGINT", shutdown);
process.on("SIGTERM", shutdown);
