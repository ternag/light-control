import type { Roster } from "./roster.js";
import type { PeerInfo } from "./types.js";

/** Tell a firmware node to fetch + apply the server's cached latest image. */
export async function triggerUpdate(
  roster: Roster,
  self: PeerInfo,
  id: string,
  latestVersion: string | null,
  fetchImpl: typeof fetch = fetch,
): Promise<{ status: number; body: unknown }> {
  const peer = roster.list(Date.now()).find((p) => p.id === id && !p.self);
  if (!peer || peer.type !== "firmware") {
    return { status: 404, body: { error: "no such firmware peer" } };
  }
  const base = `http://${self.address}:${self.port}`;
  try {
    const res = await fetchImpl(`http://${peer.address}:${peer.port}/api/update`, {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({
        url: `${base}/api/firmware/latest/bin`,
        sigUrl: `${base}/api/firmware/latest/sig`,
        version: latestVersion ?? peer.version,
      }),
    });
    let body: unknown = null;
    try { body = await res.json(); } catch { /* board may send no body */ }
    return { status: res.status, body };
  } catch (e) {
    return { status: 502, body: { error: e instanceof Error ? e.message : String(e) } };
  }
}
