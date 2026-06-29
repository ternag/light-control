import { Socket } from "node:net";
import type { PeerInfo } from "./types.js";
import type { Roster } from "./roster.js";

/** Resolves true if the peer answered, false if unreachable. */
export type Probe = (peer: PeerInfo) => Promise<boolean>;

/**
 * Refresh each reachable candidate in the roster. Unreachable candidates are
 * left untouched, so the roster's TTL drops them shortly after they stop
 * answering. mDNS supplies the candidate addresses; HTTP decides who's alive.
 */
export async function checkLiveness(
  roster: Roster,
  candidates: PeerInfo[],
  probe: Probe,
  now: number,
): Promise<void> {
  await Promise.all(
    candidates.map(async (peer) => {
      if (await probe(peer)) roster.upsert(peer, now);
    }),
  );
}

/**
 * Default probe: open a TCP connection to the peer's port and close it at once.
 *
 * We deliberately do NOT make an HTTP request here. A firmware node's tiny
 * async web server can't service Node's HTTP client (undici keep-alive), and
 * piling up half-open requests can exhaust its connection pool and crash it.
 * "The port accepts a connection" is a sufficient and gentle liveness signal.
 */
export function tcpProbe(peer: PeerInfo, timeoutMs = 1500): Promise<boolean> {
  return new Promise((resolve) => {
    const socket = new Socket();
    let settled = false;
    const finish = (ok: boolean) => {
      if (settled) return;
      settled = true;
      socket.destroy();
      resolve(ok);
    };
    socket.setTimeout(timeoutMs);
    socket.once("connect", () => finish(true));
    socket.once("timeout", () => finish(false));
    socket.once("error", () => finish(false));
    socket.connect(peer.port, peer.address);
  });
}

export interface LivenessOptions {
  intervalMs?: number;
  timeoutMs?: number;
}

/** Poll candidate liveness on a timer. Returns a stop() function. */
export function startLiveness(
  roster: Roster,
  getCandidates: () => PeerInfo[],
  options: LivenessOptions = {},
): () => void {
  const intervalMs = options.intervalMs ?? 2000;
  const timeoutMs = options.timeoutMs ?? 1500;
  const probe: Probe = (peer) => tcpProbe(peer, timeoutMs);
  const tick = () => {
    void checkLiveness(roster, getCandidates(), probe, Date.now());
  };
  const timer = setInterval(tick, intervalMs);
  tick();
  return () => clearInterval(timer);
}
