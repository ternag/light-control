import { describe, expect, test } from "vitest";
import { createRoster } from "./roster.js";
import type { PeerInfo } from "./types.js";

const self: PeerInfo = {
  id: "self-1",
  name: "server-1",
  type: "server",
  host: "server-1.local",
  address: "192.168.1.10",
  port: 8080,
  version: "v0.1.0",
};

const tractor: PeerInfo = {
  id: "tractor-mac",
  name: "tractor",
  type: "firmware",
  host: "tractor.local",
  address: "192.168.1.42",
  port: 80,
  version: "v0.2.0",
};

describe("roster", () => {
  test("a new roster lists only self, marked self:true", () => {
    const roster = createRoster(self);
    const list = roster.list(0);
    expect(list).toHaveLength(1);
    expect(list[0]).toMatchObject({ id: "self-1", self: true });
    expect(list[0].lastSeen).toBeTypeOf("string");
  });

  test("upsert adds a peer; self stays first, peer is not self", () => {
    const roster = createRoster(self);
    roster.upsert(tractor, 1000);
    const list = roster.list(1000);
    expect(list).toHaveLength(2);
    expect(list[0].id).toBe("self-1");
    expect(list[0].self).toBe(true);
    expect(list[1]).toMatchObject({ id: "tractor-mac", self: false });
  });

  test("upsert with an existing id updates in place, no duplicate", () => {
    const roster = createRoster(self);
    roster.upsert(tractor, 1000);
    roster.upsert({ ...tractor, address: "192.168.1.99" }, 2000);
    const list = roster.list(2000);
    expect(list).toHaveLength(2);
    expect(list[1].address).toBe("192.168.1.99");
  });

  test("remove drops a peer", () => {
    const roster = createRoster(self);
    roster.upsert(tractor, 1000);
    roster.remove("tractor-mac");
    expect(roster.list(1000)).toHaveLength(1);
  });

  test("a peer not seen within the TTL is dropped from the list", () => {
    const roster = createRoster(self, { ttlMs: 5000 });
    roster.upsert(tractor, 1000);
    expect(roster.list(5999)).toHaveLength(2); // still within TTL
    expect(roster.list(6001)).toHaveLength(1); // expired
  });

  test("self never expires", () => {
    const roster = createRoster(self, { ttlMs: 5000 });
    const list = roster.list(1_000_000);
    expect(list).toHaveLength(1);
    expect(list[0].id).toBe("self-1");
  });
});
