import type { FirmwareRelease } from "./release.js";

/** LAN-serving routes for the cached image — shared by the HTTP server (which
 * serves them) and the update trigger (which embeds them in URLs sent to nodes). */
export const FIRMWARE_BIN_PATH = "/api/firmware/latest/bin";
export const FIRMWARE_SIG_PATH = "/api/firmware/latest/sig";

export interface CachedFirmware {
  version: string;
  bin: Buffer;
  sig: Buffer;
}

/**
 * Downloads the latest signed image (bin + sig) once per version and keeps it in
 * memory so the LAN can fetch it without hitting GitHub each time.
 */
export function createFirmwareCache(
  getLatest: () => FirmwareRelease | null,
  fetchImpl: typeof fetch = fetch,
) {
  let cached: CachedFirmware | null = null;

  async function download(url: string): Promise<Buffer> {
    const res = await fetchImpl(url);
    if (!res.ok) throw new Error(`fetch ${url} -> ${res.status}`);
    return Buffer.from(await res.arrayBuffer());
  }

  async function refresh(): Promise<void> {
    const latest = getLatest();
    if (!latest) return;
    if (cached?.version === latest.version) return; // already have it
    try {
      const [bin, sig] = await Promise.all([download(latest.binUrl), download(latest.sigUrl)]);
      cached = { version: latest.version, bin, sig };
    } catch {
      // keep whatever we had; try again next tick
    }
  }

  return { refresh, get: (): CachedFirmware | null => cached };
}

export type FirmwareCache = ReturnType<typeof createFirmwareCache>;
