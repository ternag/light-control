import { Bonjour, type Service } from "bonjour-service";
import { PROTO_VERSION, SERVICE_TYPE, type PeerInfo } from "./types.js";

export interface Mdns {
  /** Peers currently advertised on the network (excludes self). */
  getCandidates(): PeerInfo[];
  stop(): void;
}

/**
 * Announce `self` on `_lightctrl._tcp` and browse for peers. mDNS only tells us
 * which peers *exist* and where to reach them — liveness is decided separately
 * by probing them (bonjour caches a vanished peer well past its disappearance,
 * so it can't be trusted for "is it still up?").
 */
export function startMdns(self: PeerInfo): Mdns {
  const bonjour = new Bonjour();

  bonjour.publish({
    name: self.name,
    type: SERVICE_TYPE,
    protocol: "tcp",
    port: self.port,
    txt: {
      id: self.id,
      name: self.name,
      type: self.type,
      proto: PROTO_VERSION,
    },
  });

  const browser = bonjour.find({ type: SERVICE_TYPE, protocol: "tcp" });

  // The initial query only catches peers that announce afterwards, so re-query
  // periodically — this also lets a late-joining node discover peers already up.
  const requery = setInterval(() => browser.update(), 3000);

  function getCandidates(): PeerInfo[] {
    const peers: PeerInfo[] = [];
    for (const service of browser.services) {
      const peer = toPeerInfo(service);
      if (peer && peer.id !== self.id) peers.push(peer);
    }
    return peers;
  }

  return {
    getCandidates,
    stop() {
      clearInterval(requery);
      browser.stop();
      bonjour.unpublishAll(() => bonjour.destroy());
    },
  };
}

function toPeerInfo(service: Service): PeerInfo | null {
  const txt = (service.txt ?? {}) as Record<string, string>;
  if (!txt.id || !txt.type) return null;
  const address = firstIpv4(service.addresses) ?? service.host;
  return {
    id: txt.id,
    name: txt.name || service.name,
    type: txt.type === "firmware" ? "firmware" : "server",
    host: service.host,
    address,
    port: service.port,
  };
}

function firstIpv4(addresses?: string[]): string | undefined {
  return addresses?.find((a) => /^\d+\.\d+\.\d+\.\d+$/.test(a));
}
