import { describe, expect, test } from "vitest";
import { selectLatestFirmwareRelease, type GhRelease } from "./release.js";

const rel = (tag: string, opts: Partial<GhRelease> & { bin?: boolean } = {}): GhRelease => ({
  tag_name: tag,
  draft: opts.draft ?? false,
  prerelease: opts.prerelease ?? false,
  published_at: opts.published_at ?? "2026-06-25T00:00:00Z",
  assets: opts.bin === false ? [] : [{ name: `firmware-${tag}.bin`, browser_download_url: `https://x/${tag}.bin` }],
});

describe("selectLatestFirmwareRelease", () => {
  test("picks the newest fw- release that has a .bin, stripping the prefix", () => {
    const out = selectLatestFirmwareRelease([
      rel("fw-v0.1.0", { published_at: "2026-06-20T00:00:00Z" }),
      rel("fw-v0.2.0", { published_at: "2026-06-24T00:00:00Z" }),
    ]);
    expect(out).toEqual({
      version: "v0.2.0",
      binUrl: "https://x/fw-v0.2.0.bin",
      publishedAt: "2026-06-24T00:00:00Z",
    });
  });

  test("ignores web- releases even when newer", () => {
    const out = selectLatestFirmwareRelease([
      rel("web-v9.9.9", { published_at: "2026-12-31T00:00:00Z" }),
      rel("fw-v0.1.0", { published_at: "2026-06-20T00:00:00Z" }),
    ]);
    expect(out?.version).toBe("v0.1.0");
  });

  test("ignores fw- releases that have no .bin asset", () => {
    expect(selectLatestFirmwareRelease([rel("fw-v0.1.0", { bin: false })])).toBeNull();
  });

  test("returns null for an empty list", () => {
    expect(selectLatestFirmwareRelease([])).toBeNull();
  });
});
