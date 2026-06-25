import { useEffect, useState } from "react";
import type { RosterEntry } from "./types";

export interface PeersState {
  peers: RosterEntry[];
  error: string | null;
  /** False until the first response (success or failure) arrives. */
  loaded: boolean;
}

/** Polls the serving node's roster every `intervalMs` (default 2s). */
export function usePeers(intervalMs = 2000): PeersState {
  const [state, setState] = useState<PeersState>({ peers: [], error: null, loaded: false });

  useEffect(() => {
    let cancelled = false;

    async function poll() {
      try {
        const res = await fetch("/api/peers");
        if (!res.ok) throw new Error(`HTTP ${res.status}`);
        const peers = (await res.json()) as RosterEntry[];
        if (!cancelled) setState({ peers, error: null, loaded: true });
      } catch (e) {
        if (!cancelled) {
          setState((s) => ({ ...s, error: e instanceof Error ? e.message : String(e), loaded: true }));
        }
      }
    }

    poll();
    const id = setInterval(poll, intervalMs);
    return () => {
      cancelled = true;
      clearInterval(id);
    };
  }, [intervalMs]);

  return state;
}
