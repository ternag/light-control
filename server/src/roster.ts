import type { PeerInfo, RosterEntry } from "./types.js";

export interface RosterOptions {
  /** Drop a peer that hasn't been seen again within this many ms. */
  ttlMs?: number;
}

interface Tracked {
  info: PeerInfo;
  lastSeenMs: number;
}

/**
 * In-memory roster of peers learned via mDNS, keyed by peer id.
 * `self` is always present and never expires.
 */
export function createRoster(self: PeerInfo, options: RosterOptions = {}) {
  const ttlMs = options.ttlMs ?? Infinity;
  const peers = new Map<string, Tracked>();

  function upsert(info: PeerInfo, nowMs: number): void {
    if (info.id === self.id) return; // never shadow self with a learned record
    peers.set(info.id, { info, lastSeenMs: nowMs });
  }

  function remove(id: string): void {
    peers.delete(id);
  }

  function list(nowMs: number): RosterEntry[] {
    const entry = (info: PeerInfo, lastSeenMs: number, isSelf: boolean): RosterEntry => ({
      ...info,
      lastSeen: new Date(lastSeenMs).toISOString(),
      self: isSelf,
    });

    const rows: RosterEntry[] = [entry(self, nowMs, true)];
    for (const { info, lastSeenMs } of peers.values()) {
      if (nowMs - lastSeenMs > ttlMs) continue; // expired
      rows.push(entry(info, lastSeenMs, false));
    }
    return rows;
  }

  return { self, upsert, remove, list };
}

export type Roster = ReturnType<typeof createRoster>;
