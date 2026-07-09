export interface GhAsset {
  name: string;
  browser_download_url: string;
}

export interface GhRelease {
  tag_name: string;
  draft: boolean;
  prerelease: boolean;
  published_at: string;
  assets: GhAsset[];
}

export interface FirmwareRelease {
  version: string; // normalized, "fw-" stripped
  binUrl: string;
  sigUrl: string;
  publishedAt: string;
}

/** Newest non-draft `fw-*` release that ships a `.bin` and `.bin.sig`, or null. */
export function selectLatestFirmwareRelease(releases: GhRelease[]): FirmwareRelease | null {
  const candidates = releases
    .filter((r) => !r.draft && r.tag_name.startsWith("fw-"))
    .sort((a, b) => Date.parse(b.published_at) - Date.parse(a.published_at));

  for (const latest of candidates) {
    const bin = latest.assets.find((a) => a.name.endsWith(".bin"));
    const sig = latest.assets.find((a) => a.name.endsWith(".bin.sig"));
    if (!bin || !sig) continue; // an unsigned release is not installable
    return {
      version: latest.tag_name.replace(/^fw-/, ""),
      binUrl: bin.browser_download_url,
      sigUrl: sig.browser_download_url,
      publishedAt: latest.published_at,
    };
  }
  return null;
}

export interface ReleaseWatcher {
  getLatest(): FirmwareRelease | null;
  stop(): void;
}

/**
 * Poll GitHub for the newest firmware release. Caches the result; on any
 * network/HTTP error it keeps the last known value (or null).
 */
export function startReleaseWatcher(
  repo: string,
  opts: { intervalMs?: number } = {},
): ReleaseWatcher {
  const intervalMs = opts.intervalMs ?? 10 * 60 * 1000;
  let latest: FirmwareRelease | null = null;

  async function poll(): Promise<void> {
    try {
      const res = await fetch(`https://api.github.com/repos/${repo}/releases`, {
        headers: { accept: "application/vnd.github+json", "user-agent": "light-control" },
      });
      if (!res.ok) return;
      const releases = (await res.json()) as GhRelease[];
      latest = selectLatestFirmwareRelease(releases);
    } catch {
      // keep last known
    }
  }

  void poll();
  const timer = setInterval(() => void poll(), intervalMs);
  return { getLatest: () => latest, stop: () => clearInterval(timer) };
}
