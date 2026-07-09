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
class WifiConnector {
 public:
  enum class State { kConnecting, kConnectingExhausted, kConnected };

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

  // Human-readable label for the TX power the radio is using right now
  // (queries the live hardware value, not Preferences), e.g. "rung 3/10".
  static String currentPowerLabel();

 private:
  void startRungAttempt(size_t rung);
  void advanceAfterFailure();
  void beginScan();
  bool pollScan();  // returns true once the scan has finished (results printed)
  int loadStartRung();
  void saveGoodRung(int rung);

  const char *ssid_ = nullptr;
  const char *password_ = nullptr;
  bool connected_ = false;
  bool ladderExhausted_ = false;
  bool waitingForScan_ = false;
  size_t rung_ = 0;
  uint32_t rungAttemptStart_ = 0;

  static const wifi_power_t kLadder[10];  // exactly 10 rungs (19.5..-1 dBm) — the
                                           // bound itself is the compile-time guard;
                                           // wifi_connector.cpp's initializer must match it.
  static const size_t kLadderSize;
  static const uint32_t kPerLevelTimeoutMs = 7000;
};
