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
class WifiConnector {
 public:
  // onTick is invoked roughly every 250ms while waiting on a connection
  // attempt (e.g. to blink a status LED). WifiConnector has no knowledge of
  // LED_PIN or any other board-specific hardware — that stays in the caller.
  explicit WifiConnector(void (*onTick)() = nullptr);

  // Blocks until connected. Logs progress and diagnostics over Serial.
  void connect(const char *ssid, const char *password);

 private:
  bool tryConnectAtPower(const char *ssid, const char *password,
                          wifi_power_t power, uint32_t timeoutMs);
  void scanNetworks(const char *ssid);
  int loadStartRung();
  void saveGoodRung(int rung);

  void (*onTick_)();

  static const wifi_power_t kLadder[10];  // exactly 10 rungs (19.5..-1 dBm) — the
                                           // bound itself is the compile-time guard;
                                           // wifi_connector.cpp's initializer must match it.
  static const size_t kLadderSize;
  static const uint32_t kPerLevelTimeoutMs = 7000;
};
