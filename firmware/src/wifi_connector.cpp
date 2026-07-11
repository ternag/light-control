#include "wifi_connector.h"

namespace {

// Reason code from the most recent STA disconnect, written on the WiFi task by
// the event handler and read by update() when a rung times out — lets us tell a
// wrong password apart from an unreached-AP handshake failure.
// Cleared at the start of each attempt; 0 means "no disconnect event this try".
volatile uint8_t g_lastDisconnectReason = 0;

// A wrong WPA2 password surfaces as a 4-way-handshake timeout (reason 15) on
// the ESP32-C3 — verified on hardware — NOT as AUTH_FAIL (202), which covers
// other auth rejections. Treat both as a credentials problem: no TX-power rung
// will fix it. (An AP that's present but out of transmit reach reports
// AUTH_EXPIRE (2) or times out instead — that's what climbing addresses.)
bool isAuthFailure(uint8_t reason) {
  return reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
         reason == WIFI_REASON_AUTH_FAIL;
}

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

/*
1/10 = 19.5 dBm  (full power)
2/10 = 17 dBm
3/10 = 15 dBm
4/10 = 13 dBm
5/10 = 11 dBm
6/10 = 8.5 dBm
7/10 = 7 dBm
8/10 = 5 dBm
9/10 = 2 dBm
10/10 = -1 dBm   (floor)
*/

const wifi_power_t WifiConnector::kLadder[] = {
    WIFI_POWER_19_5dBm, WIFI_POWER_17dBm,     WIFI_POWER_15dBm,
    WIFI_POWER_13dBm,   WIFI_POWER_11dBm,     WIFI_POWER_8_5dBm,
    WIFI_POWER_7dBm,    WIFI_POWER_5dBm,      WIFI_POWER_2dBm,
    WIFI_POWER_MINUS_1dBm,
};
const size_t WifiConnector::kLadderSize =
    sizeof(WifiConnector::kLadder) / sizeof(WifiConnector::kLadder[0]);

WifiConnector::State WifiConnector::state() const {
  if (connected_) return State::kConnected;
  if (authFailed_) return State::kAuthFailed;
  return ladderExhausted_ ? State::kConnectingExhausted : State::kConnecting;
}

void WifiConnector::begin(const char *ssid, const char *password) {
  ssid_ = ssid;
  password_ = password;

  WiFi.mode(WIFI_STA);
  // Own reconnection fully via update() — the ladder logic must be the only
  // thing that ever re-associates, or the built-in auto-reconnect races it
  // (silently retrying at the power that just failed) and the LED ends up
  // lying about what's actually happening.
  WiFi.setAutoReconnect(false);
  // Log the disconnect reason code on every drop. Invaluable for telling apart
  // a wrong password (15 4WAY_HANDSHAKE_TIMEOUT / 202 AUTH_FAIL) from a
  // missing/hidden AP (201 NO_AP_FOUND) from an AP that's present but out of
  // transmit reach (2 AUTH_EXPIRE — the usual symptom of a handshake we can't
  // complete because our transmit doesn't reach). begin() runs exactly once, so
  // this never needs a dedup guard.
  WiFi.onEvent(
      [](WiFiEvent_t, WiFiEventInfo_t info) {
        g_lastDisconnectReason = info.wifi_sta_disconnected.reason;
        Serial.printf("  [disconnect reason: %u]\n",
                      info.wifi_sta_disconnected.reason);
      },
      ARDUINO_EVENT_WIFI_STA_DISCONNECTED);

  // The climb starts (and rests) at the cool default. Resolve its ladder index
  // once; fall back to the lowest-power rung if kStartTxPower isn't on the
  // ladder (a misconfiguration).
  startRung_ = kLadderSize - 1;
  for (size_t i = 0; i < kLadderSize; i++) {
    if (kLadder[i] == kStartTxPower) {
      startRung_ = i;
      break;
    }
  }

  Serial.printf("Connecting to WiFi \"%s\"...\n", ssid_);
  connected_ = false;
  ladderExhausted_ = false;
  authFailed_ = false;
  startRungAttempt(startRung_);
}

void WifiConnector::update() {
  if (connected_) {
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi connection lost — reconnecting.");
      connected_ = false;
      ladderExhausted_ = false;
      startRungAttempt(startRung_);  // restart the climb from the cool default
      return;
    }
    return;  // connected and holding — the start rung is already cool, nothing to tune
  }

  switch (phase_) {
    case Phase::kAttempting:
      if (WiFi.status() == WL_CONNECTED) {
        onConnected();
      } else if (millis() - rungAttemptStart_ > kPerLevelTimeoutMs) {
        Serial.printf("  timed out (%s) at this power level.\n",
                      wifiStatusName(WiFi.status()));
        WiFi.disconnect(true);
        scanToDiagnose_ = true;  // this scan decides whether to climb
        beginScan();
        phase_ = Phase::kScanning;
      }
      break;

    case Phase::kScanning:
      if (pollScan()) onScanComplete();  // pollScan sets scanSawSsid_/scanRssi_
      break;

    case Phase::kWaiting:
      if (millis() >= nextScanAt_) {
        scanToDiagnose_ = false;  // just polling for the AP to reappear
        beginScan();
        phase_ = Phase::kScanning;
      }
      break;
  }
}

void WifiConnector::onConnected() {
  connected_ = true;
  authFailed_ = false;      // a successful connect clears the "needs a human" latch
  ladderExhausted_ = false; // ...and the "climbed out without connecting" latch
  Serial.printf("WiFi connected: %s (TX power rung %u/%u)\n",
                WiFi.localIP().toString().c_str(), (unsigned)rung_ + 1,
                (unsigned)kLadderSize);
}

void WifiConnector::forceReprobe() {
  Serial.println("Reprobe requested — restarting the climb from the cool default.");
  connected_ = false;
  ladderExhausted_ = false;
  authFailed_ = false;
  WiFi.disconnect(true);
  startRungAttempt(startRung_);
}

// A scan just finished (scanSawSsid_/scanRssi_ are set). Decide what a failed
// rung actually means and act — see the Phase enum comment in the header for
// the full rationale. In short: climb the ladder (add power) ONLY when the AP
// is audibly present yet association failed for a non-credential reason;
// otherwise hold the current rung (wait for an absent AP, SOS on bad password).
void WifiConnector::onScanComplete() {
  if (!scanToDiagnose_) {
    // Was polling for an absent AP to come back.
    if (scanSawSsid_) {
      Serial.printf("  AP \"%s\" back (%d dBm) — retrying at rung %u/%u.\n",
                    ssid_, (int)scanRssi_, (unsigned)rung_ + 1,
                    (unsigned)kLadderSize);
      startRungAttempt(rung_);
    } else {
      enterWaiting();  // still gone — poll again shortly
    }
    return;
  }

  // Diagnosing a rung that just timed out.
  if (!scanSawSsid_) {
    Serial.printf("  AP \"%s\" not on the air — TX power can't help; holding rung, waiting for it.\n",
                  ssid_);
    enterWaiting();
    return;
  }
  if (isAuthFailure(g_lastDisconnectReason)) {
    Serial.printf("  auth failed (reason %u, wrong password?) — not a TX-power problem; holding rung (needs a human).\n",
                  (unsigned)g_lastDisconnectReason);
    authFailed_ = true;  // latched → SOS blink; cleared only by a real connect
    startRungAttempt(rung_);
    return;
  }
  // AP is present and it isn't a credentials problem, yet we didn't associate:
  // our transmit didn't reach it at this power. Climb toward more power.
  climbLadder();
}

void WifiConnector::startRungAttempt(size_t rung) {
  rung_ = rung;
  g_lastDisconnectReason = 0;  // reflect only THIS attempt's outcome
  WiFi.begin(ssid_, password_);
  WiFi.setTxPower(kLadder[rung_]);
  rungAttemptStart_ = millis();
  phase_ = Phase::kAttempting;
  Serial.printf("  [%u/%u] trying %.1f dBm...\n", (unsigned)rung_ + 1,
                (unsigned)kLadderSize, kLadder[rung_] / 4.0);
}

// Step one rung toward more power (a lower index). At full power (index 0) there
// is nowhere higher to go: the AP is present but unreachable even at max power
// (out of range, or a weak-regulator board that browns out up here). Latch
// "exhausted" for the fast double-blink, then restart the climb from the cool
// default so we keep re-probing — and stay cool between sweeps — until the AP
// becomes reachable at some rung.
void WifiConnector::climbLadder() {
  if (rung_ == 0) {
    ladderExhausted_ = true;
    Serial.printf(
        "  climbed to full power without connecting — AP present but unreachable; re-probing from rung %u/%u.\n",
        (unsigned)startRung_ + 1, (unsigned)kLadderSize);
    startRungAttempt(startRung_);
    return;
  }
  rung_--;  // one rung up the ladder = more power
  startRungAttempt(rung_);
}

void WifiConnector::enterWaiting() {
  phase_ = Phase::kWaiting;
  nextScanAt_ = millis() + kApPollIntervalMs;
}

String WifiConnector::currentPowerLabel() {
  wifi_power_t current = WiFi.getTxPower();
  for (size_t i = 0; i < kLadderSize; i++) {
    if (kLadder[i] == current) {
      char buf[16];
      snprintf(buf, sizeof(buf), "rung %u/%u", (unsigned)i + 1, (unsigned)kLadderSize);
      return String(buf);
    }
  }
  char buf[16];
  snprintf(buf, sizeof(buf), "%.1f dBm", current / 4.0);
  return String(buf);
}

// Scan the 2.4GHz band (including hidden SSIDs) whenever a connection attempt
// times out, or on a cadence while waiting for an absent AP. Beyond the human
// diagnostic, this is the load-bearing signal for the failure-mode decision in
// onScanComplete(): whether the target SSID is on the air at all, and how
// strong it is. Note: this gateway must broadcast a plain 2.4GHz SSID —
// band-steered single-SSID networks hide the 2.4 radio and won't onboard the
// C3 (see project notes).
//
// Runs async (scanNetworks(true, ...)) and is polled from update() via
// pollScan() rather than blocked on — a synchronous scan takes a couple of
// seconds, and a string of failed rungs during a bad-signal period must not
// stall loop().
void WifiConnector::beginScan() {
  Serial.println("Scanning for 2.4GHz networks (including hidden)...");
  WiFi.scanNetworks(/*async=*/true, /*show_hidden=*/true);
}

bool WifiConnector::pollScan() {
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return false;
  scanSawSsid_ = false;
  scanRssi_ = -127;  // strongest sighting of the target SSID (dBm)
  if (n == WIFI_SCAN_FAILED) {
    Serial.println("  (scan failed)");
    WiFi.scanDelete();
    return true;
  }
  if (n <= 0) {
    Serial.println("  (none found)");
  } else {
    for (int i = 0; i < n; i++) {
      String foundSsid = WiFi.SSID(i);
      bool hidden = foundSsid.length() == 0;
      bool isTarget = foundSsid == ssid_;
      if (isTarget) {
        scanSawSsid_ = true;
        if (WiFi.RSSI(i) > scanRssi_) scanRssi_ = WiFi.RSSI(i);
      }
      Serial.printf("  %s%-24s ch%-3d %4ddBm\n", isTarget ? "-> " : "   ",
                    hidden ? "<hidden>" : foundSsid.c_str(), WiFi.channel(i),
                    WiFi.RSSI(i));
    }
  }
  WiFi.scanDelete();
  return true;
}
