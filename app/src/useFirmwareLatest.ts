import { useEffect, useState } from "react";
import type { FirmwareLatest } from "./types";

/** Polls the serving node for the latest published firmware release. */
export function useFirmwareLatest(intervalMs = 30000): FirmwareLatest | null {
  const [latest, setLatest] = useState<FirmwareLatest | null>(null);

  useEffect(() => {
    let cancelled = false;
    async function poll() {
      try {
        const res = await fetch("/api/firmware/latest");
        if (!res.ok) return;
        const data = (await res.json()) as FirmwareLatest | null;
        if (!cancelled) setLatest(data);
      } catch {
        // leave last known
      }
    }
    poll();
    const id = setInterval(poll, intervalMs);
    return () => {
      cancelled = true;
      clearInterval(id);
    };
  }, [intervalMs]);

  return latest;
}
