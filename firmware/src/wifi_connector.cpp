#include "wifi_connector.h"

#include <Preferences.h>

namespace {

const char *kPrefsNamespace = "wifi";
const char *kPrefsKey = "txpower";

// Reason code from the most recent STA disconnect, written on the WiFi task by
// the event handler and read by update() when a rung times out — lets us tell a
// wrong password apart from a brownout-style handshake failure.
// Cleared at the start of each attempt; 0 means "no disconnect event this try".
volatile uint8_t g_lastDisconnectReason = 0;

// A wrong WPA2 password surfaces as a 4-way-handshake timeout (reason 15) on
// the ESP32-C3 — verified on hardware — NOT as AUTH_FAIL (202), which covers
// other auth rejections. Treat both as a credentials problem: no TX-power rung
// will fix it. (Brownout, the ladder's actual target, reports AUTH_EXPIRE (2)
// instead — see the disconnect-reason note in connect().)
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
  // a wrong password (202 AUTH_FAIL) from a missing/hidden AP (201 NO_AP_FOUND)
  // from a flaky link or weak transmit path (2 AUTH_EXPIRE, the usual symptom of
  // a board that hears the AP but can't complete the handshake). begin() runs
  // exactly once, so this never needs a dedup guard.
  WiFi.onEvent(
      [](WiFiEvent_t, WiFiEventInfo_t info) {
        g_lastDisconnectReason = info.wifi_sta_disconnected.reason;
        Serial.printf("  [disconnect reason: %u]\n",
                      info.wifi_sta_disconnected.reason);
      },
      ARDUINO_EVENT_WIFI_STA_DISCONNECTED);

  Serial.printf("Connecting to WiFi \"%s\"...\n", ssid_);
  int loaded = loadStartRung();
  size_t startRung = loaded >= 0 ? (size_t)loaded : 0;
  if (loaded >= 0) {
    Serial.printf("  starting at last-known-good TX power (rung %u/%u)\n",
                  (unsigned)startRung + 1, (unsigned)kLadderSize);
  }

  connected_ = false;
  ladderExhausted_ = false;
  authFailed_ = false;
  startRungAttempt(startRung);
}

void WifiConnector::update() {
  if (connected_) {
    if (WiFi.status() != WL_CONNECTED) {
      // A drop during a probation bump means the higher power browned out —
      // remember it as this session's ceiling so we don't climb back into it.
      if (probing_) {
        minProbeIndex_ = probeTarget_ + 1;
        probing_ = false;
        Serial.printf("  upward probe to rung %u/%u browned out — ceiling set; falling back.\n",
                      (unsigned)probeTarget_ + 1, (unsigned)kLadderSize);
      }
      Serial.println("WiFi connection lost — reconnecting.");
      connected_ = false;
      ladderExhausted_ = false;
      int loaded = loadStartRung();
      startRungAttempt(loaded >= 0 ? (size_t)loaded : 0);
      return;
    }
    maybeAutoClimb();
    return;
  }

  switch (phase_) {
    case Phase::kAttempting:
      if (WiFi.status() == WL_CONNECTED) {
        onConnected();
      } else if (millis() - rungAttemptStart_ > kPerLevelTimeoutMs) {
        Serial.printf("  timed out (%s) at this power level.\n",
                      wifiStatusName(WiFi.status()));
        WiFi.disconnect(true);
        scanToDiagnose_ = true;  // this scan decides whether to descend
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
  saveGoodRung((int)rung_);
  connected_ = true;
  authFailed_ = false;  // a successful connect clears the "needs a human" latch
  probing_ = false;
  connectedSince_ = millis();  // start the stable-for-6h clock for the auto-climb
  if (ladderExhausted_) {
    Serial.printf("WiFi connected: %s (TX power floor)\n",
                  WiFi.localIP().toString().c_str());
  } else {
    Serial.printf("WiFi connected: %s (TX power rung %u/%u)\n",
                  WiFi.localIP().toString().c_str(), (unsigned)rung_ + 1,
                  (unsigned)kLadderSize);
  }
}

// While connected, reclaim link margin that the descend-only ladder can't:
// after a long stable stretch, try one rung UP (more power) on the live
// connection without reassociating. Keep it only if it survives a probation
// window; a brownout during probation is caught by update()'s loss path, which
// records the ceiling and falls back to the saved (lower) rung. Converges to
// the highest stable rung over a few cycles, then rests.
void WifiConnector::maybeAutoClimb() {
  if (probing_) {
    if (millis() - probeStartedAt_ >= kProbationMs) {
      rung_ = probeTarget_;
      saveGoodRung((int)rung_);
      probing_ = false;
      connectedSince_ = millis();  // earn a fresh stable window before the next climb
      Serial.printf("WiFi TX power climbed to rung %u/%u and held — committed.\n",
                    (unsigned)rung_ + 1, (unsigned)kLadderSize);
    }
    return;
  }
  if (rung_ <= minProbeIndex_) return;  // already at the session ceiling (or full power)
  if (millis() - connectedSince_ < kStableBeforeProbeMs) return;  // not stable long enough yet
  probeTarget_ = rung_ - 1;
  probeStartedAt_ = millis();
  probing_ = true;
  WiFi.setTxPower(kLadder[probeTarget_]);  // live bump — no reassociation
  Serial.printf("Stable — probing TX power up to rung %u/%u (%.1f dBm)...\n",
                (unsigned)probeTarget_ + 1, (unsigned)kLadderSize,
                kLadder[probeTarget_] / 4.0);
}

void WifiConnector::forceReprobe() {
  Serial.println("Reprobe requested — restarting TX power ladder from full power.");
  minProbeIndex_ = 0;  // re-open the upward search for this session
  probing_ = false;
  connected_ = false;
  ladderExhausted_ = false;
  authFailed_ = false;
  WiFi.disconnect(true);
  startRungAttempt(0);
}

// A scan just finished (scanSawSsid_/scanRssi_ are set). Decide what a failed
// rung actually means and act — see the Phase enum comment in the header for
// the full rationale. In short: descend the ladder ONLY on positive evidence
// of brownout (AP audible, strong, and failing for a non-credential reason);
// otherwise hold the current rung.
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
  if (scanRssi_ < kDescendRssiFloorDbm) {
    Serial.printf("  AP present but faint (%d dBm) — descending would weaken TX further; holding rung.\n",
                  (int)scanRssi_);
    startRungAttempt(rung_);
    return;
  }
  descendLadder();  // brownout: AP is present and strong, yet won't associate
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

// Step down one rung, or — once the floor is reached — hold there and keep
// retrying it. If even the lowest TX power can't associate to an AP that is
// present and strong, no rung will, so there's nothing below to try.
void WifiConnector::descendLadder() {
  if (!ladderExhausted_) {
    rung_++;
    if (rung_ >= kLadderSize) {
      ladderExhausted_ = true;
      rung_ = kLadderSize - 1;
      Serial.printf(
          "  exhausted TX power ladder — holding at floor (%.1f dBm), retrying indefinitely.\n",
          kLadder[rung_] / 4.0);
    }
  }
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
