import { expect, test } from "vitest";
import { createFirmwareCache } from "./firmwareCache.js";
import type { FirmwareRelease } from "./release.js";

const latest: FirmwareRelease = {
  version: "v0.2.0", binUrl: "https://x/f.bin", sigUrl: "https://x/f.bin.sig",
  publishedAt: "2026-06-01T00:00:00Z",
};

test("downloads and caches bin+sig for the latest version", async () => {
  const fetchImpl = (async (url: string) => ({
    ok: true,
    arrayBuffer: async () => new TextEncoder().encode(url.endsWith(".sig") ? "SIG" : "BIN").buffer,
  })) as unknown as typeof fetch;
  const cache = createFirmwareCache(() => latest, fetchImpl);
  expect(cache.get()).toBeNull();
  await cache.refresh();
  const got = cache.get();
  expect(got?.version).toBe("v0.2.0");
  expect(got?.bin.toString()).toBe("BIN");
  expect(got?.sig.toString()).toBe("SIG");
});

test("refresh is a no-op when the version is already cached", async () => {
  let calls = 0;
  const fetchImpl = (async () => { calls++; return { ok: true, arrayBuffer: async () => new ArrayBuffer(1) }; }) as unknown as typeof fetch;
  const cache = createFirmwareCache(() => latest, fetchImpl);
  await cache.refresh();
  await cache.refresh();
  expect(calls).toBe(2); // two assets fetched once, not four
});
