#pragma once

#include <Arduino.h>
#include <WiFi.h>

// Connects to WiFi by CLIMBING a fixed TX-power ladder from a cool default
// upward, using the lowest power that reaches the AP. It starts at kStartTxPower
// (a modest, low-heat level) and only steps toward more power when that rung
// can't reach the AP. This inverts the older "start at full power and descend"
// scheme: on weak-regulator ESP32-C3 SuperMini clones full power browns out the
// radio's regulator and fails the handshake, so a strategy that begins high and
// only descends can strand those boards on a browning rung when the AP is faint.
// Climbing from a cool default never touches the browning high rungs unless a
// rung below them has already failed to connect — so a nearby board settles cool
// and a board that browns out at full power is a genuinely-unreachable edge case.
// See .planning/2026-07-09-tx-power-climb-from-default-design.md.
//
// Runs as a non-blocking state machine: begin() kicks off the first attempt
// and returns immediately; update() must be called every loop() iteration to
// advance it. The initial connect, retrying a timed-out rung, climbing to more
// power, and reconnecting after a later WiFi loss all go through the same
// update() path — there is no separate blocking call anywhere. This matters
// because the node's own automations must keep running even on a board that
// never sees a known AP; see .planning/2026-07-09-wifi-status-led-design.md.
//
// Once connected it holds that rung — no while-connected power adjustment. The
// start rung is already cool, so a node near its AP connects there and stays.
// Every (re)connect and forceReprobe() restarts the climb from kStartTxPower;
// nothing about TX power persists across boots — each session re-derives it.
class WifiConnector {
 public:
  // kAuthFailed is latched once we see an AUTH_FAIL against a present AP: a
  // wrong compiled-in password can't self-heal, so it stays until a genuine
  // connect. Callers surface it distinctly (an SOS blink — "needs a human").
  // kConnectingExhausted means the climb reached full power without connecting
  // (AP present but unreachable) and is re-probing from the cool default;
  // callers surface it distinctly (a fast double-blink — "trying hard, no luck").
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

  // Restart the climb from the cool default. For a manual "I changed this node's
  // situation, re-probe now" trigger (e.g. a BOOT-button long-press).
  // Non-blocking.
  void forceReprobe();

  // Human-readable label for the TX power the radio is using right now
  // (queries the live hardware value), e.g. "rung 3/10".
  static String currentPowerLabel();

 private:
  // A failed rung means one of several things, and only one of them is fixed by
  // adding TX power. We climb the ladder (add power) ONLY when the AP is audibly
  // present (in the scan) yet association failed for a non-credential reason —
  // i.e. our transmit didn't reach it. Every other failure holds the current
  // rung instead of climbing pointlessly into the browning high rungs:
  //   - AP not in the scan  → it's off / out of range / 5GHz-only. TX power is
  //     irrelevant; poll (kWaiting) until it reappears, then retry this rung.
  //   - AP present, AUTH_FAIL → wrong password. A credential problem, not a
  //     power one; retry this rung (it will keep failing, harmlessly) and latch
  //     the auth-failed state for the SOS blink.
  // See .planning/2026-07-09-tx-power-climb-from-default-design.md.
  enum class Phase { kAttempting, kScanning, kWaiting };

  void startRungAttempt(size_t rung);  // (re)associate at rung; enters kAttempting
  void onConnected();
  void onScanComplete();  // decide: climb, retry-same, or wait
  void climbLadder();     // AP present but unreached → step up one rung (or re-probe from the default at the top)
  void enterWaiting();    // AP absent → schedule the next presence scan
  void beginScan();
  bool pollScan();  // true once the scan has finished; sets scanSawSsid_/scanRssi_

  const char *ssid_ = nullptr;
  const char *password_ = nullptr;
  bool connected_ = false;
  bool ladderExhausted_ = false;  // latched when the climb hit full power without connecting; re-probing from the default
  bool authFailed_ = false;  // latched on AUTH_FAIL; cleared only on a real connect
  Phase phase_ = Phase::kAttempting;
  bool scanToDiagnose_ = false;  // scan follows a failed attempt (may climb) vs. polling for an absent AP
  bool scanSawSsid_ = false;
  int32_t scanRssi_ = -127;
  size_t rung_ = 0;
  size_t startRung_ = 0;  // ladder index of kStartTxPower; the climb's starting/rest rung (set in begin())
  uint32_t rungAttemptStart_ = 0;
  uint32_t nextScanAt_ = 0;

  static const wifi_power_t kLadder[10];  // exactly 10 rungs (19.5..-1 dBm), index 0 = most power — the
                                           // bound itself is the compile-time guard;
                                           // wifi_connector.cpp's initializer must match it.
  static const size_t kLadderSize;
  // The cool default the climb starts (and rests) at. Lower = cooler but reaches
  // less far before it has to climb; the sole tuning knob for rest power. Must be
  // one of the kLadder values.
  static const wifi_power_t kStartTxPower = WIFI_POWER_8_5dBm;
  static const uint32_t kPerLevelTimeoutMs = 6000;
  static const uint32_t kApPollIntervalMs = 3000;   // re-scan cadence while waiting for an absent AP
};
