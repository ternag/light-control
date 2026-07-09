#include "wifi_connector.h"

#include <Preferences.h>

namespace {

const char *kPrefsNamespace = "wifi";
const char *kPrefsKey = "txpower";

const char *wifiStatusName(wl_status_t s) {
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

}  // namespace

const wifi_power_t WifiConnector::kLadder[] = {
    WIFI_POWER_19_5dBm, WIFI_POWER_17dBm,     WIFI_POWER_15dBm,
    WIFI_POWER_13dBm,   WIFI_POWER_11dBm,     WIFI_POWER_8_5dBm,
    WIFI_POWER_7dBm,    WIFI_POWER_5dBm,      WIFI_POWER_2dBm,
    WIFI_POWER_MINUS_1dBm,
};
const size_t WifiConnector::kLadderSize =
    sizeof(WifiConnector::kLadder) / sizeof(WifiConnector::kLadder[0]);

WifiConnector::WifiConnector(void (*onTick)()) : onTick_(onTick) {}

void WifiConnector::connect(const char *ssid, const char *password) {
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

  Serial.printf("Connecting to WiFi \"%s\"...\n", ssid);
  int loaded = loadStartRung();
  size_t startRung = loaded >= 0 ? (size_t)loaded : 0;
  if (loaded >= 0) {
    Serial.printf("  starting at last-known-good TX power (rung %u/%u)\n",
                  (unsigned)startRung + 1, (unsigned)kLadderSize);
  }

  for (size_t rung = startRung; rung < kLadderSize; rung++) {
    Serial.printf("  [%u/%u] trying %.1f dBm...\n", (unsigned)rung + 1,
                  (unsigned)kLadderSize, kLadder[rung] / 4.0);
    if (tryConnectAtPower(ssid, password, kLadder[rung], kPerLevelTimeoutMs)) {
      saveGoodRung((int)rung);
      Serial.printf("WiFi connected: %s (TX power rung %u/%u)\n",
                    WiFi.localIP().toString().c_str(), (unsigned)rung + 1,
                    (unsigned)kLadderSize);
      return;
    }
  }

  // Exhausted the ladder — hold at the floor and keep retrying forever. If
  // the lowest TX power still can't connect, the problem isn't TX power
  // (wrong password, AP down, out of range) — climbing back up won't help.
  size_t floor = kLadderSize - 1;
  Serial.printf("  exhausted TX power ladder — holding at floor (%.1f dBm), retrying indefinitely.\n",
                kLadder[floor] / 4.0);
  while (!tryConnectAtPower(ssid, password, kLadder[floor], kPerLevelTimeoutMs)) {
  }
  saveGoodRung((int)floor);
  Serial.printf("WiFi connected: %s (TX power floor)\n",
                WiFi.localIP().toString().c_str());
}

bool WifiConnector::tryConnectAtPower(const char *ssid, const char *password,
                                       wifi_power_t power, uint32_t timeoutMs) {
  WiFi.begin(ssid, password);
  WiFi.setTxPower(power);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (onTick_) onTick_();
    delay(250);
    if (millis() - start > timeoutMs) {
      Serial.printf("  timed out (%s) at this power level.\n",
                    wifiStatusName(WiFi.status()));
      scanNetworks(ssid);
      WiFi.disconnect(true);
      delay(200);
      return false;
    }
  }
  return true;
}

// Diagnostic, only run when a connection attempt stalls: list the 2.4GHz
// networks the C3 can see (including hidden ones). Helps tell a missing/hidden
// SSID apart from a wrong password. Note: this gateway must broadcast a plain
// 2.4GHz SSID — band-steered single-SSID networks hide the 2.4 radio and won't
// onboard the C3 (see project notes).
void WifiConnector::scanNetworks(const char *ssid) {
  Serial.println("Scanning for 2.4GHz networks (including hidden)...");
  int n = WiFi.scanNetworks(/*async=*/false, /*show_hidden=*/true);
  if (n <= 0) {
    Serial.println("  (none found)");
    return;
  }
  for (int i = 0; i < n; i++) {
    String foundSsid = WiFi.SSID(i);
    bool hidden = foundSsid.length() == 0;
    Serial.printf("  %s%-24s ch%-3d %4ddBm\n", foundSsid == ssid ? "-> " : "   ",
                  hidden ? "<hidden>" : foundSsid.c_str(), WiFi.channel(i),
                  WiFi.RSSI(i));
  }
  WiFi.scanDelete();
}

// Returns the persisted rung index, or -1 if nothing is stored yet (or the
// stored value no longer matches any rung in the ladder). -1 is distinct
// from 0 so callers can tell "persisted at full power" apart from "never
// persisted" when logging.
int WifiConnector::loadStartRung() {
  Preferences prefs;
  prefs.begin(kPrefsNamespace, /*readOnly=*/true);
  bool has = prefs.isKey(kPrefsKey);
  int32_t stored = has ? prefs.getInt(kPrefsKey) : 0;
  prefs.end();
  if (!has) return -1;
  for (size_t i = 0; i < kLadderSize; i++) {
    if ((int32_t)kLadder[i] == stored) return (int)i;
  }
  return -1;  // stored value no longer in the ladder — start fresh
}

void WifiConnector::saveGoodRung(int rung) {
  Preferences prefs;
  prefs.begin(kPrefsNamespace, /*readOnly=*/false);
  int32_t want = (int32_t)kLadder[rung];
  if (!prefs.isKey(kPrefsKey) || prefs.getInt(kPrefsKey) != want) {
    prefs.putInt(kPrefsKey, want);
  }
  prefs.end();
}
