import { useEffect, useState } from "react";
import { usePeers } from "./usePeers";
import { useFirmwareLatest } from "./useFirmwareLatest";
import type { FirmwareLatest, RosterEntry } from "./types";

export function App() {
  const { peers, error, loaded } = usePeers();
  const latestFirmware = useFirmwareLatest();

  return (
    <main>
      <header>
        <h1>light-control</h1>
        <p className="subtitle">Peers on the network</p>
      </header>

      {error && <p className="error">Can’t reach the node: {error}</p>}
      {!loaded && <p className="muted">Discovering…</p>}
      {loaded && peers.length === 0 && !error && <p className="muted">No peers yet.</p>}

      <ul className="roster">
        {peers.map((p) => (
          <PeerRow key={p.id} peer={p} latestFirmware={latestFirmware} />
        ))}
      </ul>
    </main>
  );
}

function isUpdateAvailable(peer: RosterEntry, latest: FirmwareLatest | null): boolean {
  if (peer.type !== "firmware" || !latest) return false;
  if (!peer.version || peer.version === "unknown") return false;
  return peer.version !== latest.version;
}

// How long a triggered update may run before we assume it failed node-side.
// A normal OTA (download + flash + reboot) finishes well within this.
const UPDATE_TIMEOUT_MS = 90_000;

function PeerRow({ peer, latestFirmware }: { peer: RosterEntry; latestFirmware: FirmwareLatest | null }) {
  const updateAvailable = isUpdateAvailable(peer, latestFirmware);
  const [busy, setBusy] = useState(false);
  const [err, setErr] = useState<string | null>(null);

  // On success the node reboots off the roster (unmounting this row) and comes
  // back on the new version. But a node-side failure after the 202 is silent —
  // the node just stays on the old version — so give up after a timeout to
  // bring the update button back instead of showing "updating…" forever.
  useEffect(() => {
    if (!busy) return;
    const timer = setTimeout(() => {
      setBusy(false);
      setErr("update did not complete — node still on the old version");
    }, UPDATE_TIMEOUT_MS);
    return () => clearTimeout(timer);
  }, [busy]);

  async function onUpdate() {
    if (!confirm(`Update ${peer.name} to ${latestFirmware?.version}? The node will reboot.`)) return;
    setBusy(true);
    setErr(null);
    try {
      const res = await fetch(`/api/peers/${encodeURIComponent(peer.id)}/update`, { method: "POST" });
      if (res.status !== 202) throw new Error(`HTTP ${res.status}`);
    } catch (e) {
      setErr(e instanceof Error ? e.message : String(e));
      setBusy(false);
    }
  }

  return (
    <li className={`peer ${peer.self ? "is-self" : ""}`}>
      <span className={`badge badge-${peer.type}`}>{peer.type}</span>
      <span className="name">{peer.name}</span>
      {peer.self && <span className="you">you</span>}
      <span className="version">{peer.version || "—"}</span>
      {updateAvailable && !busy && (
        <button className="update-btn" onClick={onUpdate}>update available →</button>
      )}
      {updateAvailable && busy && <span className="update-tag">updating…</span>}
      {updateAvailable && err && <span className="error">{err}</span>}
      <span className="addr">{peer.address}:{peer.port}</span>
      <span className="seen" title={peer.lastSeen}>{timeAgo(peer.lastSeen)}</span>
    </li>
  );
}

function timeAgo(iso: string): string {
  const secs = Math.max(0, Math.round((Date.now() - new Date(iso).getTime()) / 1000));
  if (secs < 2) return "just now";
  if (secs < 60) return `${secs}s ago`;
  return `${Math.round(secs / 60)}m ago`;
}
