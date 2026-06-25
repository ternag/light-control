// Mirrors the server's RosterEntry (the GET /api/peers contract, proto 1).
export interface RosterEntry {
  id: string;
  name: string;
  type: "firmware" | "server";
  host: string;
  address: string;
  port: number;
  lastSeen: string;
  self: boolean;
}
