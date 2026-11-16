import { spawn, type ChildProcess } from "node:child_process";
import { writeFileSync, unlinkSync, existsSync } from "node:fs";
import { homedir } from "node:os";
import { join } from "node:path";

export const URL_FILE = process.env.MIX_STREAMER_URL_FILE || join(homedir(), ".mix-streamer-url");
const URL_RE = /https:\/\/[a-z0-9-]+\.trycloudflare\.com/i;

export function writeUrlFile(url: string): void {
  try {
    writeFileSync(URL_FILE, url + "\n", "utf8");
  } catch (e) {
    console.error(`[tunnel] cannot write ${URL_FILE}:`, (e as Error).message);
  }
}

export function removeUrlFile(): void {
  try {
    if (existsSync(URL_FILE)) unlinkSync(URL_FILE);
  } catch {
    /* ignore */
  }
}

/**
 * Starts `cloudflared tunnel --url http://localhost:<port>` (quick tunnel),
 * parses the public trycloudflare.com URL from its log output and restarts
 * it with back-off if it exits.
 */
export class CloudflareTunnel {
  private proc: ChildProcess | null = null;
  private stopped = false;
  private restartDelay = 2000;
  public url: string | null = null;

  constructor(
    private readonly localPort: number,
    private readonly onUrl: (url: string) => void,
    private readonly binary = process.env.CLOUDFLARED_BIN || "cloudflared",
  ) {}

  start(): void {
    if (this.stopped) return;
    console.log(`[tunnel] starting ${this.binary} tunnel --url http://localhost:${this.localPort}`);

    const proc = spawn(
      this.binary,
      ["tunnel", "--no-autoupdate", "--url", `http://localhost:${this.localPort}`],
      { stdio: ["ignore", "pipe", "pipe"], windowsHide: true },
    );
    this.proc = proc;

    const scan = (chunk: Buffer) => {
      const text = chunk.toString("utf8");
      if (process.env.TUNNEL_DEBUG) process.stderr.write(`[cloudflared] ${text}`);
      const m = text.match(URL_RE);
      if (m && m[0] !== this.url) {
        this.url = m[0];
        this.restartDelay = 2000;
        this.onUrl(m[0]);
      }
    };
    proc.stdout?.on("data", scan);
    proc.stderr?.on("data", scan);

    proc.on("error", (err: NodeJS.ErrnoException) => {
      if (err.code === "ENOENT") {
        console.error(
          `[tunnel] '${this.binary}' not found. Install cloudflared ` +
            `(macOS: brew install cloudflared, Windows: winget install --id Cloudflare.cloudflared) ` +
            `or set CLOUDFLARED_BIN. Running in local-only mode.`,
        );
        this.stopped = true;
      } else {
        console.error("[tunnel] spawn error:", err.message);
      }
    });

    proc.on("exit", (code, signal) => {
      this.proc = null;
      this.url = null;
      if (this.stopped) return;
      console.warn(`[tunnel] cloudflared exited (code=${code}, signal=${signal}); restarting in ${this.restartDelay} ms`);
      setTimeout(() => this.start(), this.restartDelay);
      this.restartDelay = Math.min(this.restartDelay * 2, 60000);
    });
  }

  stop(): void {
    this.stopped = true;
    if (this.proc && !this.proc.killed) this.proc.kill();
  }
}
