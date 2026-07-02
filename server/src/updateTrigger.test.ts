import { expect, test } from "vitest";
import { triggerUpdate } from "./updateTrigger.js";
import { createRoster } from "./roster.js";
import type { PeerInfo } from "./types.js";

const self: PeerInfo = { id: "srv", name: "srv", type: "server", host: "h", address: "192.168.1.101", port: 8080, version: "web" };
const node: PeerInfo = { id: "esp1", name: "esp1", type: "firmware", host: "e.local", address: "192.168.1.100", port: 80, version: "v0.1.0" };

test("posts the LAN image URLs to the target board", async () => {
  const roster = createRoster(self); roster.upsert(node, Date.now());
  let captured: any = null;
  const fetchImpl = (async (url: string, init: any) => { captured = { url, body: JSON.parse(init.body) }; return { status: 202, json: async () => ({ ok: true }) }; }) as unknown as typeof fetch;
  const r = await triggerUpdate(roster, self, "esp1", fetchImpl);
  expect(captured.url).toBe("http://192.168.1.100:80/api/update");
  expect(captured.body.url).toBe("http://192.168.1.101:8080/api/firmware/latest/bin");
  expect(captured.body.sigUrl).toBe("http://192.168.1.101:8080/api/firmware/latest/sig");
  expect(r.status).toBe(202);
});

test("404 for an unknown or non-firmware peer", async () => {
  const roster = createRoster(self);
  const r = await triggerUpdate(roster, self, "nope", (async () => ({})) as unknown as typeof fetch);
  expect(r.status).toBe(404);
});
