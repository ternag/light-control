# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

LED control system for lighting up LEGO models. ESP32-C3 nodes drive LEDs via a
PCA9685 (16-channel PWM over I²C) and talk two-way over WiFi to a central
computer. Each node must keep running its automations (light patterns, input
responses) autonomously when offline. Multiple nodes, one central computer.

Current state: the **discovery + OTA walking skeleton** works end to end.
Firmware nodes join WiFi, announce/browse `_lightctrl._tcp` over mDNS, keep a
peer roster, serve a small HTTP API, and accept signed OTA updates
(Ed25519-verified, confirm-healthy-or-rollback). The server discovers peers,
probes liveness, watches GitHub releases (`fw-v*` tags) for signed firmware,
caches the binary for LAN serving, and triggers node updates. The PWA shows the
roster with an update button. No LED driving yet — that's next.

## Monorepo layout

One sub-project per component: `firmware/` (ESP32-C3, PlatformIO/Arduino),
`server/` (central computer, Node/TypeScript, run via `./server.sh`), and
`app/` (PWA, React + Vite, served by the server from `app/dist`). `.planning/`
holds design notes — read it for intent that isn't yet in code. Artifacts for
AI sessions go in `~/Claude/`, never in this repo.

## Build / flash / monitor

`./flash.sh` wraps the common firmware flows (build / upload / monitor / diag);
or run `pio` from `firmware/` (or pass `-d firmware`).

| Command | What it does |
|---|---|
| `pio run` | Compile only — works with no board attached; use this to verify changes |
| `pio run -t upload` | Compile + flash |
| `pio run -t upload -t monitor` | Flash + open serial monitor (115200 baud) |
| `pio device list` | Find the serial port (native USB enumerates as `/dev/cu.usbmodem*`) |
| `pio run -t clean` | Clean build artifacts |

Server tests: `cd server && npx vitest run`. Firmware has no on-target tests;
`pio run` (compile-only) is the pre-hardware verification step.

If upload won't start: hold **BOOT**, tap **RESET**, release **BOOT** to force
the bootloader, then upload again.

## Hardware / toolchain specifics

- Board: ESP32-C3 PRO MINI (ESP32-C3FH4 — RISC-V, 4 MB flash). PlatformIO board
  id is `esp32-c3-devkitm-1`, framework `arduino`.
- **Native USB:** the C3FH4 has no separate USB-UART chip. `build_flags` set
  `ARDUINO_USB_MODE=1` and `ARDUINO_USB_CDC_ON_BOOT=1` so `Serial.print()` routes
  over USB-CDC on the same cable used to flash. Don't remove these or serial
  output disappears.
- `lib_ldf_mode = deep+` is required so transitive framework deps (Wire.h, SPI.h
  pulled in by Adafruit BusIO) resolve against the Arduino core.
- Onboard LED pin is uncertain across C3 "mini" boards: code assumes GPIO8,
  active-low. If the LED misbehaves on real hardware, flip `LED_ACTIVE_LOW` or
  try another pin (`main.cpp`).
- LED driver library: `adafruit/Adafruit PWM Servo Driver Library` (for the
  PCA9685).
