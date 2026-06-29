#include <Arduino.h>
#include <WiFi.h>

#include "secrets.h" // only for WIFI_SSID, to flag the target network in output

// ---------------------------------------------------------------------------
// RSSI scanner — a standalone diagnostic build for checking a board's WiFi
// receive path. It does NOT connect to anything; it just runs a passive 2.4GHz
// scan every few seconds and prints every network it sees, with signal level.
//
// Use it to tell a healthy board from one with a bad antenna / RF section by
// comparing boards at the SAME spot:
//   * Healthy board near the AP: target SSID around -30..-55 dBm, plus many
//     neighbour networks.
//   * Dead RF path: no networks at all, or the target only at -80 dBm and weak.
//
// This is more reliable than the main firmware's scan, which runs mid-connect
// (a busy radio returns 0 networks). Here the radio is idle before scanning.
//
// Build / flash (does not disturb the main firmware):
//   ./flash.sh diag
//   # or:  pio run -d firmware -e diag -t upload -t monitor
// Flash the real firmware again with:  ./flash.sh -m
// ---------------------------------------------------------------------------

#ifndef WIFI_SSID
#define WIFI_SSID "" // scanner still works; just won't flag a target row
#endif

static const uint32_t SCAN_INTERVAL_MS = 3000;

static const char *authName(wifi_auth_mode_t a) {
  switch (a) {
    case WIFI_AUTH_OPEN: return "open";
    case WIFI_AUTH_WEP: return "WEP";
    case WIFI_AUTH_WPA_PSK: return "WPA";
    case WIFI_AUTH_WPA2_PSK: return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/2";
    case WIFI_AUTH_WPA3_PSK: return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/3";
    default: return "?";
  }
}

void setup() {
  Serial.begin(115200);
  delay(500); // let the USB-CDC port enumerate on the host
  Serial.println("\nlight-control RSSI scanner");
  Serial.printf("target SSID: \"%s\"\n", WIFI_SSID);
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true); // ensure the radio is idle, not mid-connect
  delay(200);
}

void loop() {
  Serial.println("scanning 2.4GHz (incl. hidden)...");
  int n = WiFi.scanNetworks(/*async=*/false, /*show_hidden=*/true);
  if (n <= 0) {
    Serial.println("  (no networks found — suspect a dead RX/antenna path)");
  } else {
    int best = -127;
    bool targetSeen = false;
    int targetRssi = 0;
    for (int i = 0; i < n; i++) {
      String ssid = WiFi.SSID(i);
      bool hidden = ssid.length() == 0;
      bool isTarget = !hidden && ssid == WIFI_SSID;
      int rssi = WiFi.RSSI(i);
      if (rssi > best) best = rssi;
      if (isTarget) {
        targetSeen = true;
        targetRssi = rssi;
      }
      Serial.printf("  %s%-24s ch%-3d %4d dBm  %-7s %s\n", isTarget ? "-> " : "   ",
                    hidden ? "<hidden>" : ssid.c_str(), WiFi.channel(i), rssi,
                    authName(WiFi.encryptionType(i)), WiFi.BSSIDstr(i).c_str());
    }
    Serial.printf("  --- %d network(s); strongest %d dBm; ", n, best);
    if (targetSeen) Serial.printf("\"%s\" %d dBm ---\n", WIFI_SSID, targetRssi);
    else Serial.printf("\"%s\" NOT seen ---\n", WIFI_SSID);
  }
  WiFi.scanDelete();
  delay(SCAN_INTERVAL_MS);
}
