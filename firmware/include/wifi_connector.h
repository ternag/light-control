#pragma once

#include <Arduino.h>
#include <WiFi.h>

// Connects to WiFi, stepping TX power down a fixed ladder (best signal
// quality first) until a rung produces a stable connection. Some ESP32-C3
// SuperMini clones brown out their radio's regulator at full TX power,
// failing the auth handshake; probing down finds the lowest power that
// still works for THIS board, instead of hardcoding one board's
// known-good value for every board.
// See .planning/2026-07-02-wifi-adaptive-tx-power-design.md.
//
// Runs as a non-blocking state machine: begin() kicks off the first attempt
// and returns immediately; update() must be called every loop() iteration to
// advance it. The initial connect, retrying a timed-out rung, and
// reconnecting after a later WiFi loss all go through the same update() path
// — there is no separate blocking call anywhere. This matters because the
// node's own automations must keep running even on a board that never sees a
// known AP; see .planning/2026-07-09-wifi-status-led-design.md.
//
// The ladder only ever descends on its own. To reclaim link margin when
// conditions improve, update() also runs a gentle upward re-optimization while
// connected (climb one rung after a long stable stretch, keep it only if it
// holds), and forceReprobe() re-walks the whole ladder from full power on
// demand. See .planning/2026-07-09-tx-power-self-optimize-design.md.
class WifiConnector {
 public:
  // kAuthFailed is latched once we see an AUTH_FAIL against a present AP: a
  // wrong compiled-in password can't self-heal, so it stays until a genuine
  // connect. Callers surface it distinctly (an SOS blink — "needs a human").
  enum class State { kConnecting, kConnectingExhausted, kConnected, kAuthFailed };

  WifiConnector() = default;

  // Starts connecting. Non-blocking — returns immediately. Call once, from
  // setup(). Later reconnects (after a WiFi loss) are handled internally by
  // update(); begin() is never called a second time.
  void begin(const char *ssid, const char *password);

  // Advances the connection state machine by one step. Call every loop()
  // iteration; never blocks.
  void update();

  State state() const;
  bool isConnected() const { return connected_; }

  // Restart the TX-power search from full power (rung 1), forgetting this
  // session's brownout ceiling. For a manual "I changed this node's situation,
  // re-optimize now" trigger (e.g. a BOOT-button long-press). Non-blocking:
  // like begin(), it kicks off the ladder and returns; the descent lands
  // directly on the current optimum.
  void forceReprobe();

  // Human-readable label for the TX power the radio is using right now
  // (queries the live hardware value, not Preferences), e.g. "rung 3/10".
  static String currentPowerLabel();

 private:
  // A failed rung means one of several things, and only one of them is fixed
  // by lowering TX power. We descend the ladder ONLY on positive evidence of
  // radio brownout — the AP is audibly present (in the scan) and reasonably
  // strong, yet association still fails for a non-credential reason. Every
  // other failure holds the current rung instead of walking down and
  // corrupting the saved last-known-good:
  //   - AP not in the scan  → it's off / out of range / 5GHz-only. TX power is
  //     irrelevant; poll (kWaiting) until it reappears, then retry this rung.
  //   - AP present, AUTH_FAIL → wrong password. A credential problem, not a
  //     power one; retry this rung (it will keep failing, harmlessly).
  //   - AP present but faint (RSSI below the floor) → the link is range-limited;
  //     descending TX would only weaken our transmit further. Hold this rung.
  // See .planning/2026-07-09-wifi-status-led-design.md.
  enum class Phase { kAttempting, kScanning, kWaiting };

  void startRungAttempt(size_t rung);  // (re)associate at rung; enters kAttempting
  void onConnected();
  void onScanComplete();  // decide: descend, retry-same, or wait
  void descendLadder();   // brownout evidence → step down one rung (or hold at floor)
  void enterWaiting();    // AP absent → schedule the next presence scan
  void maybeAutoClimb();  // while connected: gently probe one rung up after a long stable stretch
  void beginScan();
  bool pollScan();  // true once the scan has finished; sets scanSawSsid_/scanRssi_
  int loadStartRung();
  void saveGoodRung(int rung);

  const char *ssid_ = nullptr;
  const char *password_ = nullptr;
  bool connected_ = false;
  bool ladderExhausted_ = false;
  bool authFailed_ = false;  // latched on AUTH_FAIL; cleared only on a real connect
  Phase phase_ = Phase::kAttempting;
  bool scanToDiagnose_ = false;  // scan follows a failed attempt (may descend) vs. polling for an absent AP
  bool scanSawSsid_ = false;
  int32_t scanRssi_ = -127;
  size_t rung_ = 0;
  uint32_t rungAttemptStart_ = 0;
  uint32_t nextScanAt_ = 0;

  // Upward re-optimization (auto-climb). All RAM-only — the improved rung
  // persists via saveGoodRung(); the ceiling deliberately resets on reboot so a
  // fresh boot re-checks whether more power is now tolerable.
  size_t minProbeIndex_ = 0;    // lowest index (highest power) the climb may try; raised when a probe browns out
  uint32_t connectedSince_ = 0; // millis() the current connection began — gates the stable-for-6h check
  bool probing_ = false;        // an upward probe is in its probation window
  size_t probeTarget_ = 0;      // the rung being probed (one above rung_)
  uint32_t probeStartedAt_ = 0; // millis() the probation began

  static const wifi_power_t kLadder[10];  // exactly 10 rungs (19.5..-1 dBm) — the
                                           // bound itself is the compile-time guard;
                                           // wifi_connector.cpp's initializer must match it.
  static const size_t kLadderSize;
  static const uint32_t kPerLevelTimeoutMs = 7000;
  static const uint32_t kApPollIntervalMs = 3000;   // re-scan cadence while waiting for an absent AP
  static const int32_t kDescendRssiFloorDbm = -80;  // only descend if the AP is at least this strong
  static const uint32_t kStableBeforeProbeMs = 6UL * 60 * 60 * 1000;  // stable this long before an upward probe
  static const uint32_t kProbationMs = 60UL * 1000;  // hold the higher power this long before committing
};
