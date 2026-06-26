/**
 * The discovery contract, shared across firmware, server, and PWA.
 * Service type: `_lightctrl._tcp.local`. Protocol version 1.
 */

export const SERVICE_TYPE = "lightctrl";
export const PROTO_VERSION = "1";

export type PeerType = "firmware" | "server";

/** Identity + address a node advertises and we learn via mDNS. */
export interface PeerInfo {
  id: string;
  name: string;
  type: PeerType;
  host: string;
  address: string;
  port: number;
  version: string; // e.g. "v0.1.0", "abc123-dirty", or "unknown"
}

/** A roster row as returned by `GET /api/peers`. */
export interface RosterEntry extends PeerInfo {
  lastSeen: string; // ISO-8601
  self: boolean;
}
