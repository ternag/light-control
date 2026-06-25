import { createServer, type IncomingMessage, type ServerResponse } from "node:http";
import { readFile, stat } from "node:fs/promises";
import { extname, join, normalize } from "node:path";
import type { Roster } from "./roster.js";

export interface HttpOptions {
  port: number;
  /** Directory of the built PWA to serve at `/`. Optional. */
  staticDir?: string;
}

const MIME: Record<string, string> = {
  ".html": "text/html; charset=utf-8",
  ".js": "text/javascript; charset=utf-8",
  ".css": "text/css; charset=utf-8",
  ".json": "application/json; charset=utf-8",
  ".svg": "image/svg+xml",
  ".ico": "image/x-icon",
  ".png": "image/png",
};

export function startHttp(roster: Roster, options: HttpOptions) {
  const server = createServer((req, res) => {
    void handle(req, res, roster, options);
  });
  server.listen(options.port);
  return server;
}

async function handle(
  req: IncomingMessage,
  res: ServerResponse,
  roster: Roster,
  options: HttpOptions,
): Promise<void> {
  const url = new URL(req.url ?? "/", "http://localhost");

  if (url.pathname === "/api/peers") {
    sendJson(res, 200, roster.list(Date.now()));
    return;
  }

  if (options.staticDir) {
    await serveStatic(res, options.staticDir, url.pathname);
    return;
  }

  sendJson(res, 404, { error: "not found" });
}

async function serveStatic(res: ServerResponse, dir: string, pathname: string): Promise<void> {
  // Confine resolved paths to the static dir; fall back to index.html (SPA).
  const rel = normalize(pathname).replace(/^(\.\.[/\\])+/, "");
  let file = join(dir, rel === "/" || rel === "" ? "index.html" : rel);
  try {
    if (!(await stat(file)).isFile()) throw new Error("not a file");
  } catch {
    file = join(dir, "index.html"); // SPA fallback
  }
  try {
    const body = await readFile(file);
    res.writeHead(200, { "content-type": MIME[extname(file)] ?? "application/octet-stream" });
    res.end(body);
  } catch {
    sendJson(res, 404, { error: "not found" });
  }
}

function sendJson(res: ServerResponse, status: number, body: unknown): void {
  res.writeHead(status, { "content-type": "application/json; charset=utf-8" });
  res.end(JSON.stringify(body));
}
