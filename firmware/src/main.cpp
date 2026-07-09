#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>

#include "secrets.h"
#include "ota.h"
#include "wifi_connector.h"
#include <esp_ota_ops.h>

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "unknown"
#endif

// ---------------------------------------------------------------------------
// light-control firmware node — v1 "discovery" walking skeleton.
//
//   1. Connects to WiFi (adaptive TX power, non-blocking — see
//      wifi_connector.h). loop() runs from boot regardless of WiFi state, so
//      a board that never sees a known AP still runs its own automations.
//   2. Announces itself on mDNS as a `_lightctrl._tcp` peer (TXT: id/name/
//      type/proto) — the shared discovery contract (proto 1).
//   3. Browses for other peers and keeps a roster in RAM.
//   4. Prints the roster over serial (the node's only "UI" for now).
//   5. Serves GET /api/peers so it's a genuine, curl-able peer.
//
// No PCA9685 light-pattern driving yet — see .planning/ for the full design.
// ---------------------------------------------------------------------------

static const char *PROTO_VERSION = "1";
static const char *SERVICE = "lightctrl"; // -> _lightctrl._tcp
static const uint16_t HTTP_PORT = 80;
static const uint32_t SCAN_INTERVAL_MS = 5000;

// Onboard LED as a WiFi-status indicator (see CLAUDE.md for pin caveats).
// See .planning/2026-07-09-wifi-status-led-design.md for the state table.
static const uint8_t LED_PIN = 8;
static const bool LED_ACTIVE_LOW = true;
static const uint32_t LED_GRACE_MS = 20000;              // solid-on window after connecting
static const uint32_t LED_BLINK_PERIOD_MS = 500;          // connecting
static const uint32_t LED_BLINK_PERIOD_EXHAUSTED_MS = 250; // ladder exhausted, holding at floor

// BOOT button — a runtime long-press forces a TX-power reprobe from full power.
// GPIO9 is the standard ESP32-C3 BOOT pin (also the flash-mode strap, but that
// only matters at reset; a runtime read is safe). Verify on hardware like the
// LED pin. See .planning/2026-07-09-tx-power-self-optimize-design.md.
static const uint8_t BOOT_PIN = 9;
static const uint32_t BUTTON_HOLD_MS = 3000;        // hold this long to trigger a reprobe
static const uint32_t BUTTON_BOOT_GRACE_MS = 3000;  // ignore the button this long after boot

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

static bool g_pendingVerify = false;
static uint32_t g_verifyDeadline = 0;

static AsyncWebServer server(HTTP_PORT);
static WifiConnector g_wifi;
static bool g_networkReady = false; // mDNS + HTTP server started (once, on first connect)
static bool g_ledGrace = false;
static uint32_t g_ledGraceDeadline = 0;

static void setLed(bool on) {
  digitalWrite(LED_PIN, (on ^ LED_ACTIVE_LOW) ? HIGH : LOW);
}

// True exactly once per press, when BOOT has been held for BUTTON_HOLD_MS.
// Ignored during the post-boot grace so it can't be confused with a flash-mode
// strap at reset. Re-arms only after the button is released.
static bool reprobeButtonLongPressed() {
  if (millis() < BUTTON_BOOT_GRACE_MS) return false;
  static uint32_t pressStart = 0;
  static bool armed = true;
  bool down = digitalRead(BOOT_PIN) == LOW;  // INPUT_PULLUP: pressed = LOW
  if (!down) {
    pressStart = 0;
    armed = true;
    return false;
  }
  if (pressStart == 0) pressStart = millis();
  if (armed && millis() - pressStart >= BUTTON_HOLD_MS) {
    armed = false;  // fire once; wait for release before it can fire again
    return true;
  }
  return false;
}

// Morse SOS (··· ——— ···) for the auth-failed state — a wrong compiled-in
// password can't self-heal, so the node literally signals "send help." One
// segment = {LED on?, ms}; dot = 200ms, dash = 600ms, gaps between. The last
// off-segment is the 3s pause before the call repeats.
struct LedSeg { bool on; uint16_t ms; };
static const LedSeg SOS_PATTERN[] = {
    {true, 200}, {false, 200}, {true, 200}, {false, 200}, {true, 200}, {false, 600}, // S
    {true, 600}, {false, 200}, {true, 600}, {false, 200}, {true, 600}, {false, 600}, // O
    {true, 200}, {false, 200}, {true, 200}, {false, 200}, {true, 200}, {false, 3000}, // S + pause
};

static bool sosLevel(uint32_t t) {
  uint32_t total = 0;
  for (const LedSeg &s : SOS_PATTERN) total += s.ms;
  uint32_t x = t % total;
  for (const LedSeg &s : SOS_PATTERN) {
    if (x < s.ms) return s.on;
    x -= s.ms;
  }
  return false;
}

// Derives LED behavior from WifiConnector's state every loop() tick — see
// .planning/2026-07-09-wifi-status-led-design.md for the state table.
static void updateLed() {
  if (g_wifi.isConnected()) {
    if (!g_ledGrace) {
      setLed(false);
    } else if (millis() > g_ledGraceDeadline) {
      g_ledGrace = false;
      setLed(false);
    } else {
      setLed(true);
    }
    return;
  }
  switch (g_wifi.state()) {
    case WifiConnector::State::kAuthFailed:
      setLed(sosLevel(millis()));  // ··· ——— ··· — needs a human
      return;
    case WifiConnector::State::kConnectingExhausted:
      setLed((millis() % LED_BLINK_PERIOD_EXHAUSTED_MS) < (LED_BLINK_PERIOD_EXHAUSTED_MS / 2));
      return;
    default:  // kConnecting (incl. waiting for an absent AP)
      setLed((millis() % LED_BLINK_PERIOD_MS) < (LED_BLINK_PERIOD_MS / 2));
      return;
  }
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
  Serial.printf("  * %-14s %-8s %s:%u (self) [TX %s]\n", g_name.c_str(), "firmware",
                WiFi.localIP().toString().c_str(), HTTP_PORT,
                WifiConnector::currentPowerLabel().c_str());
  for (const Peer &p : g_peers) {
    Serial.printf("    %-14s %-8s %s:%u\n", p.name.c_str(), p.type.c_str(),
                  p.address.c_str(), p.port);
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  pinMode(BOOT_PIN, INPUT_PULLUP);
  setLed(false);
  delay(500); // let the USB-CDC port enumerate on the host
  Serial.println("\nlight-control firmware: boot OK");
  Serial.printf("firmware version: %s\n", FIRMWARE_VERSION);

  // Arm the OTA health check BEFORE connecting: if this is an unverified
  // image whose WiFi is broken, it must roll back within the deadline, which
  // starts at boot. Checked every loop() iteration below.
  const esp_partition_t *running = esp_ota_get_running_partition();
  esp_ota_img_states_t state;
  if (esp_ota_get_state_partition(running, &state) == ESP_OK &&
      state == ESP_OTA_IMG_PENDING_VERIFY) {
    g_pendingVerify = true;
    g_verifyDeadline = millis() + 30000; // 30s to prove healthy
    Serial.println("OTA: running a pending image — will confirm health for 30s");
  }

  // Non-blocking: returns immediately. loop() starts running right away, so
  // this node's own automations aren't held hostage by WiFi ever coming up —
  // g_wifi.update() (called from loop()) carries the connection forward.
  g_wifi.begin(WIFI_SSID, WIFI_PASSWORD);

  // mDNS + server.begin() need an IP, so they're deferred to loop() on first
  // connect. Routes can be registered now regardless.
  server.on("/api/peers", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send(200, "application/json", rosterJson());
  });

  server.on(
      "/api/update", HTTP_POST,
      [](AsyncWebServerRequest *req) {
        // Completion callback: if a body arrived, its handler responded already.
        if (!req->_tempObject) req->send(400, "application/json", "{\"error\":\"missing or oversized body\"}");
      },
      nullptr,
      [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total) {
        // Accumulate into a per-request buffer. _tempObject is freed with the
        // request by the framework; a shared/static buffer would let two
        // in-flight requests corrupt each other's body.
        if (total == 0 || total > 2048) return; // no/oversized body -> completion cb sends 400
        if (index == 0) req->_tempObject = calloc(total + 1, 1);
        char *body = static_cast<char *>(req->_tempObject);
        if (!body) return; // alloc failed -> completion cb sends 400
        memcpy(body + index, data, len);
        if (index + len < total) return; // wait for the whole body
        JsonDocument doc;
        if (deserializeJson(doc, body)) { req->send(400, "application/json", "{\"error\":\"bad json\"}"); return; }
        String url = doc["url"] | "";
        String sigUrl = doc["sigUrl"] | "";
        String version = doc["version"] | "";
        if (url.isEmpty() || sigUrl.isEmpty()) { req->send(400, "application/json", "{\"error\":\"url/sigUrl required\"}"); return; }
        if (!otaRequest(url, sigUrl, version)) { req->send(409, "application/json", "{\"error\":\"update already in progress\"}"); return; }
        req->send(202, "application/json", "{\"status\":\"accepted\"}");
      });
}

void loop() {
  if (otaPending()) {
    otaRun(); // blocks; reboots on success
    return;
  }
  if (g_pendingVerify && millis() > g_verifyDeadline) {
    g_pendingVerify = false;
    if (WiFi.status() == WL_CONNECTED) {
      esp_ota_mark_app_valid_cancel_rollback();
      Serial.println("OTA: new image confirmed healthy — committed");
    } else {
      Serial.println("OTA: health check FAILED — rolling back");
      esp_ota_mark_app_invalid_rollback_and_reboot(); // reboots into previous image
    }
  }

  if (reprobeButtonLongPressed()) g_wifi.forceReprobe();

  bool wasConnected = g_wifi.isConnected();
  g_wifi.update();
  bool nowConnected = g_wifi.isConnected();

  if (nowConnected && !wasConnected) {
    g_ledGrace = true;
    g_ledGraceDeadline = millis() + LED_GRACE_MS;
    if (!g_networkReady) {
      startMdns();
      server.begin();
      g_networkReady = true;
      Serial.printf("HTTP API on http://%s.local/api/peers\n", g_name.c_str());
    }
    // On reconnect we deliberately do NOTHING to mDNS. The ESP-IDF mdns
    // responder registers its own WiFi/IP event handlers (see mdns_init) and
    // re-announces on the new IP automatically. Tearing it down and rebuilding
    // it here (MDNS.end()/begin()) left the service unregistered after a
    // reconnect, so the node vanished from the server's peer list.
  }
  updateLed();

  if (nowConnected) {
    static uint32_t lastScan = 0;
    if (millis() - lastScan >= SCAN_INTERVAL_MS) {
      lastScan = millis();
      scanPeers();
      printRoster();
    }
  }

  delay(50);
}
