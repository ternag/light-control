import { execFileSync } from "node:child_process";
import { readFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

/**
 * Version for the server+app ("web") line: `git describe` on web-v* tags,
 * with the prefix stripped. Falls back to package.json, then "unknown".
 */
export function detectWebVersion(): string {
  try {
    const out = execFileSync(
      "git",
      ["describe", "--tags", "--match", "web-v*", "--always", "--dirty"],
      { stdio: ["ignore", "pipe", "ignore"] },
    )
      .toString()
      .trim();
    if (out) return out.replace(/^web-/, "");
  } catch {
    // fall through
  }
  try {
    const pkgPath = resolve(dirname(fileURLToPath(import.meta.url)), "../package.json");
    const pkg = JSON.parse(readFileSync(pkgPath, "utf8")) as { version?: string };
    if (pkg.version) return `v${pkg.version}`;
  } catch {
    // fall through
  }
  return "unknown";
}
