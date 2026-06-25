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

/** Default probe: GET the peer's roster endpoint with a short timeout. */
export async function httpProbe(peer: PeerInfo, timeoutMs = 1000): Promise<boolean> {
  const ac = new AbortController();
  const timer = setTimeout(() => ac.abort(), timeoutMs);
  try {
    const res = await fetch(`http://${peer.address}:${peer.port}/api/peers`, {
      signal: ac.signal,
    });
    return res.ok;
  } catch {
    return false;
  } finally {
    clearTimeout(timer);
  }
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
  const timeoutMs = options.timeoutMs ?? 1000;
  const probe: Probe = (peer) => httpProbe(peer, timeoutMs);
  const tick = () => void checkLiveness(roster, getCandidates(), probe, Date.now());
  const timer = setInterval(tick, intervalMs);
  tick();
  return () => clearInterval(timer);
}
