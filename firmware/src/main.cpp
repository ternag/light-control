#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>

#include "secrets.h"
#include "ota.h"

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "unknown"
#endif

// ---------------------------------------------------------------------------
// light-control firmware node — v1 "discovery" walking skeleton.
//
//   1. Connects to WiFi.
//   2. Announces itself on mDNS as a `_lightctrl._tcp` peer (TXT: id/name/
//      type/proto) — the shared discovery contract (proto 1).
//   3. Browses for other peers and keeps a roster in RAM.
//   4. Prints the roster over serial (the node's only "UI" for now).
//   5. Serves GET /api/peers so it's a genuine, curl-able peer.
//
// No LEDs / config / persistence yet — see .planning/ for the full design.
// ---------------------------------------------------------------------------

static const char *PROTO_VERSION = "1";
static const char *SERVICE = "lightctrl"; // -> _lightctrl._tcp
static const uint16_t HTTP_PORT = 80;
static const uint32_t SCAN_INTERVAL_MS = 5000;

// Onboard LED as a simple WiFi-status indicator (see CLAUDE.md for pin caveats).
static const uint8_t LED_PIN = 8;
static const bool LED_ACTIVE_LOW = true;

struct Peer {
  String id;
  String name;
  String type;
  String host;
  String address;
  uint16_t port;
};

static String g_id;   // this node's stable id (MAC-derived)
static String g_name; // this node's friendly name
static std::vector<Peer> g_peers;

static AsyncWebServer server(HTTP_PORT);

static void setLed(bool on) {
  digitalWrite(LED_PIN, (on ^ LED_ACTIVE_LOW) ? HIGH : LOW);
}

// Serialize self + discovered peers into the roster JSON contract.
static String rosterJson() {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();

  JsonObject self = arr.add<JsonObject>();
  self["id"] = g_id;
  self["name"] = g_name;
  self["type"] = "firmware";
  self["host"] = String(g_name) + ".local";
  self["address"] = WiFi.localIP().toString();
  self["port"] = HTTP_PORT;
  self["version"] = FIRMWARE_VERSION;
  self["self"] = true;

  for (const Peer &p : g_peers) {
    JsonObject o = arr.add<JsonObject>();
    o["id"] = p.id;
    o["name"] = p.name;
    o["type"] = p.type;
    o["host"] = p.host;
    o["address"] = p.address;
    o["port"] = p.port;
    o["self"] = false;
  }

  String out;
  serializeJson(doc, out);
  return out;
}

static const char *wifiStatusName(wl_status_t s) {
  switch (s) {
    case WL_NO_SSID_AVAIL: return "NO_SSID_AVAIL (network not found — wrong SSID, out of range, or 5GHz-only)";
    case WL_CONNECT_FAILED: return "CONNECT_FAILED (auth rejected — wrong password?)";
    case WL_CONNECTION_LOST: return "CONNECTION_LOST";
    case WL_DISCONNECTED: return "DISCONNECTED (still trying)";
    case WL_IDLE_STATUS: return "IDLE";
    case WL_CONNECTED: return "CONNECTED";
    default: return "UNKNOWN";
  }
}

// Diagnostic, only run when a connection attempt stalls: list the 2.4GHz
// networks the C3 can see (including hidden ones). Helps tell a missing/hidden
// SSID apart from a wrong password. Note: this gateway must broadcast a plain
// 2.4GHz SSID — band-steered single-SSID networks hide the 2.4 radio and won't
// onboard the C3 (see project notes).
static void scanNetworks() {
  Serial.println("Scanning for 2.4GHz networks (including hidden)...");
  int n = WiFi.scanNetworks(/*async=*/false, /*show_hidden=*/true);
  if (n <= 0) {
    Serial.println("  (none found)");
    return;
  }
  for (int i = 0; i < n; i++) {
    String ssid = WiFi.SSID(i);
    bool hidden = ssid.length() == 0;
    Serial.printf("  %s%-24s ch%-3d %4ddBm\n", ssid == WIFI_SSID ? "-> " : "   ",
                  hidden ? "<hidden>" : ssid.c_str(), WiFi.channel(i), WiFi.RSSI(i));
  }
  WiFi.scanDelete();
}

static void connectWifi() {
  WiFi.mode(WIFI_STA);
  // Log the disconnect reason code on every drop. Invaluable for telling apart
  // a wrong password (202 AUTH_FAIL) from a missing/hidden AP (201 NO_AP_FOUND)
  // from a flaky link or weak transmit path (2 AUTH_EXPIRE, the usual symptom of
  // a board that hears the AP but can't complete the handshake).
  WiFi.onEvent(
      [](WiFiEvent_t, WiFiEventInfo_t info) {
        Serial.printf("  [disconnect reason: %u]\n",
                      info.wifi_sta_disconnected.reason);
      },
      ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  Serial.printf("Connecting to WiFi \"%s\"...\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  // Cap TX power LOW (8.5 dBm; the default is ~19.5 dBm). These cheap C3 SuperMini
  // clones have a weak regulator that browns out on the current spike when the
  // radio transmits at full power, failing the auth handshake (reason 2). Backing
  // the power off keeps the rail stable. Known board quirk — not a faulty board.
  WiFi.setTxPower(WIFI_POWER_8_5dBm);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    setLed(true);
    delay(250);
    setLed(false);
    delay(250);
    if (millis() - start > 15000) {
      // Stuck: report the status, scan to diagnose, then start the attempt over.
      Serial.printf("  still not connected (%s) — re-scanning and retrying.\n",
                    wifiStatusName(WiFi.status()));
      scanNetworks();
      WiFi.disconnect(true);
      delay(200);
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
      start = millis();
    }
  }
  setLed(true); // solid = connected
  Serial.printf("WiFi connected: %s\n", WiFi.localIP().toString().c_str());
}

static void startMdns() {
  // Friendly name + stable id derived from the MAC (last 3 bytes).
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char suffix[7];
  snprintf(suffix, sizeof(suffix), "%02x%02x%02x", mac[3], mac[4], mac[5]);
  g_name = String("esp32-") + suffix;
  g_id = String("esp32-") + suffix;

  if (!MDNS.begin(g_name.c_str())) {
    Serial.println("ERROR: mDNS start failed");
    return;
  }
  MDNS.addService(SERVICE, "tcp", HTTP_PORT);
  MDNS.addServiceTxt(SERVICE, "tcp", "id", g_id);
  MDNS.addServiceTxt(SERVICE, "tcp", "name", g_name);
  MDNS.addServiceTxt(SERVICE, "tcp", "type", "firmware");
  MDNS.addServiceTxt(SERVICE, "tcp", "proto", PROTO_VERSION);
  MDNS.addServiceTxt(SERVICE, "tcp", "fw", FIRMWARE_VERSION);
  Serial.printf("Announced as %s.local (id %s)\n", g_name.c_str(), g_id.c_str());
}

// Query the network and rebuild the in-RAM roster (excluding self).
static void scanPeers() {
  int n = MDNS.queryService(SERVICE, "tcp");
  std::vector<Peer> found;
  for (int i = 0; i < n; i++) {
    Peer p;
    p.host = MDNS.hostname(i);
    p.address = MDNS.IP(i).toString();
    p.port = MDNS.port(i);
    p.name = p.host; // fallback if no TXT name
    int nTxt = MDNS.numTxt(i);
    for (int t = 0; t < nTxt; t++) {
      String key = MDNS.txtKey(i, t);
      String val = MDNS.txt(i, t);
      if (key == "id") p.id = val;
      else if (key == "name") p.name = val;
      else if (key == "type") p.type = val;
    }
    if (p.id == g_id) continue; // skip our own advertisement
    found.push_back(p);
  }
  g_peers = found;
}

static void printRoster() {
  Serial.printf("--- roster: %u peer(s) + self ---\n", (unsigned)g_peers.size());
  Serial.printf("  * %-14s %-8s %s:%u (self)\n", g_name.c_str(), "firmware",
                WiFi.localIP().toString().c_str(), HTTP_PORT);
  for (const Peer &p : g_peers) {
    Serial.printf("    %-14s %-8s %s:%u\n", p.name.c_str(), p.type.c_str(),
                  p.address.c_str(), p.port);
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  setLed(false);
  delay(500); // let the USB-CDC port enumerate on the host
  Serial.println("\nlight-control firmware: boot OK");
  Serial.printf("firmware version: %s\n", FIRMWARE_VERSION);

  connectWifi();
  startMdns();

  server.on("/api/peers", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send(200, "application/json", rosterJson());
  });

  server.on(
      "/api/update", HTTP_POST,
      [](AsyncWebServerRequest *req) {
        // Completion callback: the body handler already responded.
        if (!req->_tempObject) req->send(400, "application/json", "{\"error\":\"empty body\"}");
      },
      nullptr,
      [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total) {
        static String body;
        if (index == 0) body = "";
        body.concat(reinterpret_cast<const char *>(data), len);
        if (index + len < total) return; // wait for the whole body
        req->_tempObject = (void *)1; // mark handled
        JsonDocument doc;
        if (deserializeJson(doc, body)) { req->send(400, "application/json", "{\"error\":\"bad json\"}"); return; }
        String url = doc["url"] | "";
        String sigUrl = doc["sigUrl"] | "";
        String version = doc["version"] | "";
        if (url.isEmpty() || sigUrl.isEmpty()) { req->send(400, "application/json", "{\"error\":\"url/sigUrl required\"}"); return; }
        if (!otaRequest(url, sigUrl, version)) { req->send(409, "application/json", "{\"error\":\"update already in progress\"}"); return; }
        req->send(202, "application/json", "{\"status\":\"accepted\"}");
      });

  server.begin();
  Serial.printf("HTTP API on http://%s.local/api/peers\n", g_name.c_str());
}

void loop() {
  if (otaPending()) {
    otaRun(); // blocks; reboots on success
    return;
  }
  static uint32_t lastScan = 0;
  if (millis() - lastScan >= SCAN_INTERVAL_MS) {
    lastScan = millis();
    scanPeers();
    printRoster();
  }
  delay(50);
}
