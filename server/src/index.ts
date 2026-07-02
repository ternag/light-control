import { hostname, networkInterfaces } from "node:os";
import { randomUUID } from "node:crypto";
import { existsSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { createRoster } from "./roster.js";
import { startMdns } from "./mdns.js";
import { startLiveness } from "./liveness.js";
import { startHttp, type HttpOptions } from "./server.js";
import { startReleaseWatcher } from "./release.js";
import { createFirmwareCache } from "./firmwareCache.js";
import { detectWebVersion } from "./version.js";
import type { PeerInfo } from "./types.js";

const here = dirname(fileURLToPath(import.meta.url));

// --- Config (env overrides; sane defaults so two instances are easy to run) ---
const name = process.env.NODE_NAME ?? `server-${hostname()}`;
const port = Number(process.env.PORT ?? 8080);
// A peer drops off this long after it stops answering liveness probes.
// Guard against an empty/invalid env value: Number("") is 0, and ?? only
// catches null/undefined, so a 0 TTL would expire every peer immediately.
const ttlMs = Number(process.env.PEER_TTL_MS) || 6_000;
const repo = process.env.GH_REPO ?? "ternag/light-control";

const self: PeerInfo = {
  id: randomUUID(),
  name,
  type: "server",
  host: hostname().endsWith(".local") ? hostname() : `${hostname()}.local`,
  address: primaryIpv4(),
  port,
  version: detectWebVersion(),
};

const roster = createRoster(self, { ttlMs });
const mdns = startMdns(self);
// mDNS discovers candidates; HTTP probes decide who's actually alive in the roster.
const stopLiveness = startLiveness(roster, mdns.getCandidates, { intervalMs: 2000, timeoutMs: 1000 });
const releases = startReleaseWatcher(repo);
const firmwareCache = createFirmwareCache(releases.getLatest);
const cacheTimer = setInterval(() => void firmwareCache.refresh(), 30_000);
void firmwareCache.refresh();

// Serve the built PWA if it exists (../app/dist), else run API-only.
const staticDir = resolve(here, "../../app/dist");
const httpOptions: HttpOptions = { port, getLatestFirmware: releases.getLatest, firmwareCache };
if (existsSync(staticDir)) httpOptions.staticDir = staticDir;
const server = startHttp(roster, httpOptions);

console.log(`[light-control] "${name}" (${self.id.slice(0, 8)}) listening on :${port}`);
console.log(
  httpOptions.staticDir
    ? `[light-control] serving PWA from ${staticDir}`
    : `[light-control] API only (no app/dist found) — GET http://localhost:${port}/api/peers`,
);

function shutdown() {
  console.log("\n[light-control] shutting down…");
  stopLiveness();
  mdns.stop();
  releases.stop();
  clearInterval(cacheTimer);
  server.close();
  process.exit(0);
}
process.on("SIGINT", shutdown);
process.on("SIGTERM", shutdown);

function primaryIpv4(): string {
  for (const iface of Object.values(networkInterfaces())) {
    for (const info of iface ?? []) {
      if (info.family === "IPv4" && !info.internal) return info.address;
    }
  }
  return "127.0.0.1";
}
