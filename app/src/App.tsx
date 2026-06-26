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

function PeerRow({ peer, latestFirmware }: { peer: RosterEntry; latestFirmware: FirmwareLatest | null }) {
  const updateAvailable = isUpdateAvailable(peer, latestFirmware);
  return (
    <li className={`peer ${peer.self ? "is-self" : ""}`}>
      <span className={`badge badge-${peer.type}`}>{peer.type}</span>
      <span className="name">{peer.name}</span>
      {peer.self && <span className="you">you</span>}
      <span className="version">{peer.version || "—"}</span>
      {updateAvailable && <span className="update-tag">update available</span>}
      <span className="addr">
        {peer.address}:{peer.port}
      </span>
      <span className="seen" title={peer.lastSeen}>
        {timeAgo(peer.lastSeen)}
      </span>
    </li>
  );
}

function timeAgo(iso: string): string {
  const secs = Math.max(0, Math.round((Date.now() - new Date(iso).getTime()) / 1000));
  if (secs < 2) return "just now";
  if (secs < 60) return `${secs}s ago`;
  return `${Math.round(secs / 60)}m ago`;
}
