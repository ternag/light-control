import { describe, expect, test } from "vitest";
import type { Service } from "bonjour-service";
import { toPeerInfo } from "./mdns.js";

function service(txt: Record<string, string>): Service {
  return {
    name: txt.name ?? "node",
    host: "node.local",
    port: 80,
    addresses: ["192.168.0.5"],
    txt,
  } as unknown as Service;
}

describe("toPeerInfo", () => {
  test("reads the firmware version from the fw TXT key", () => {
    const peer = toPeerInfo(service({ id: "a1", type: "firmware", name: "tractor", fw: "v0.2.0" }));
    expect(peer?.version).toBe("v0.2.0");
  });

  test("defaults version to empty string when fw is absent", () => {
    const peer = toPeerInfo(service({ id: "a1", type: "firmware", name: "tractor" }));
    expect(peer?.version).toBe("");
  });
});
