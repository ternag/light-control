import { describe, expect, test } from "vitest";
import { createRoster } from "./roster.js";
import { checkLiveness } from "./liveness.js";
import type { PeerInfo } from "./types.js";

const self: PeerInfo = {
  id: "self-1", name: "server-1", type: "server",
  host: "server-1.local", address: "192.168.0.10", port: 8080,
};
const esp: PeerInfo = {
  id: "esp32-ab", name: "esp32-ab", type: "firmware",
  host: "esp32-ab.local", address: "192.168.0.42", port: 80,
};

const has = (roster: ReturnType<typeof createRoster>, id: string, now: number) =>
  roster.list(now).some((p) => p.id === id);

describe("checkLiveness", () => {
  test("a reachable candidate is added to the roster", async () => {
    const roster = createRoster(self, { ttlMs: 5000 });
    await checkLiveness(roster, [esp], async () => true, 1000);
    expect(has(roster, esp.id, 1000)).toBe(true);
  });

  test("an unreachable candidate is not added", async () => {
    const roster = createRoster(self, { ttlMs: 5000 });
    await checkLiveness(roster, [esp], async () => false, 1000);
    expect(has(roster, esp.id, 1000)).toBe(false);
  });

  test("a candidate that stops responding expires from the roster", async () => {
    const roster = createRoster(self, { ttlMs: 5000 });
    await checkLiveness(roster, [esp], async () => true, 1000); // alive
    expect(has(roster, esp.id, 2000)).toBe(true); // still within TTL
    await checkLiveness(roster, [esp], async () => false, 7000); // gone, not refreshed
    expect(has(roster, esp.id, 7000)).toBe(false); // expired
  });
});
