import { describe, expect, test } from "vitest";
import { selectLatestFirmwareRelease, type GhRelease } from "./release.js";

const rel = (tag: string, names: string[], opts?: { published_at?: string }): GhRelease => ({
  tag_name: tag,
  draft: false,
  prerelease: false,
  published_at: opts?.published_at ?? "2026-06-01T00:00:00Z",
  assets: names.map((name) => ({ name, browser_download_url: `https://x/${name}` })),
});

describe("selectLatestFirmwareRelease", () => {
  test("picks the newest fw- release that has a .bin, stripping the prefix", () => {
    const out = selectLatestFirmwareRelease([
      rel("fw-v0.1.0", ["firmware-fw-v0.1.0.bin", "firmware-fw-v0.1.0.bin.sig"], { published_at: "2026-06-20T00:00:00Z" }),
      rel("fw-v0.2.0", ["firmware-fw-v0.2.0.bin", "firmware-fw-v0.2.0.bin.sig"], { published_at: "2026-06-24T00:00:00Z" }),
    ]);
    expect(out).toEqual({
      version: "v0.2.0",
      binUrl: "https://x/firmware-fw-v0.2.0.bin",
      sigUrl: "https://x/firmware-fw-v0.2.0.bin.sig",
      publishedAt: "2026-06-24T00:00:00Z",
    });
  });

  test("ignores web- releases even when newer", () => {
    const out = selectLatestFirmwareRelease([
      rel("web-v9.9.9", ["firmware-web-v9.9.9.bin"], { published_at: "2026-12-31T00:00:00Z" }),
      rel("fw-v0.1.0", ["firmware-fw-v0.1.0.bin", "firmware-fw-v0.1.0.bin.sig"], { published_at: "2026-06-20T00:00:00Z" }),
    ]);
    expect(out?.version).toBe("v0.1.0");
  });

  test("ignores fw- releases that have no .bin asset", () => {
    expect(selectLatestFirmwareRelease([rel("fw-v0.1.0", [])])).toBeNull();
  });

  test("returns null for an empty list", () => {
    expect(selectLatestFirmwareRelease([])).toBeNull();
  });

  test("pairs the .bin with its .sig", () => {
    const r = selectLatestFirmwareRelease([rel("fw-v0.2.0", ["firmware-fw-v0.2.0.bin", "firmware-fw-v0.2.0.bin.sig"])]);
    expect(r?.version).toBe("v0.2.0");
    expect(r?.binUrl).toBe("https://x/firmware-fw-v0.2.0.bin");
    expect(r?.sigUrl).toBe("https://x/firmware-fw-v0.2.0.bin.sig");
  });

  test("rejects a release that has no .sig", () => {
    expect(selectLatestFirmwareRelease([rel("fw-v0.2.0", ["firmware-fw-v0.2.0.bin"])])).toBeNull();
  });

  test("falls back to older signed release when newest is unsigned", () => {
    const out = selectLatestFirmwareRelease([
      rel("fw-v0.2.0", ["firmware-fw-v0.2.0.bin"], { published_at: "2026-06-24T00:00:00Z" }),
      rel("fw-v0.1.0", ["firmware-fw-v0.1.0.bin", "firmware-fw-v0.1.0.bin.sig"], { published_at: "2026-06-20T00:00:00Z" }),
    ]);
    expect(out?.version).toBe("v0.1.0");
  });
});
