#!/usr/bin/env bash
#
# Flash the light-control firmware to a connected ESP32-C3 (via PlatformIO).
#
#   ./flash.sh            compile + upload to the board
#   ./flash.sh -m         compile + upload, then open the serial monitor
#   ./flash.sh build      compile only (no board needed) — quick sanity check
#   ./flash.sh monitor    just open the serial monitor (115200 baud)
#   ./flash.sh diag       flash the RSSI scanner diagnostic + open the monitor
#                         (a WiFi-receive test for spotting bad-antenna boards;
#                          re-flash the real firmware afterwards with -m)
#
# The firmware version is stamped from git tags (fw-v*) at build time, so what
# gets flashed is whatever `git describe` reports — shown before each build.
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FW_DIR="$ROOT/firmware"

# pio is usually not on PATH; prefer the PlatformIO venv, fall back to PATH.
if [ -x "$HOME/.platformio/penv/bin/pio" ]; then
  PIO="$HOME/.platformio/penv/bin/pio"
elif command -v pio >/dev/null 2>&1; then
  PIO="$(command -v pio)"
else
  echo "error: PlatformIO (pio) not found." >&2
  echo "Install it, or open this project in VS Code with the PlatformIO extension." >&2
  exit 1
fi

# First connected serial port that looks like the board (macOS or Linux).
board_port() {
  for p in /dev/cu.usbmodem* /dev/ttyUSB* /dev/ttyACM*; do
    [ -e "$p" ] && { echo "$p"; return 0; }
  done
  return 1
}

stamped_version() {
  git -C "$ROOT" describe --tags --match 'fw-v*' --always --dirty 2>/dev/null \
    | sed 's/^fw-//' || echo "unknown"
}

require_secrets() {
  if [ ! -f "$FW_DIR/include/secrets.h" ]; then
    echo "error: $FW_DIR/include/secrets.h not found (it's git-ignored)." >&2
    echo "Create it:  cp firmware/include/secrets.example.h firmware/include/secrets.h" >&2
    echo "then set your 2.4GHz WiFi SSID and password." >&2
    exit 1
  fi
}

require_board() {
  local port
  if ! port="$(board_port)"; then
    echo "No ESP32 serial port found (looked for /dev/cu.usbmodem*, /dev/ttyUSB*, /dev/ttyACM*)." >&2
    echo "Plug the board into USB and try again. ('./flash.sh build' needs no board.)" >&2
    exit 1
  fi
  echo "Board: $port"
}

# Run an upload-capable pio command; on failure, print the bootloader hint.
upload_with_hint() {
  if ! "$PIO" run -d "$FW_DIR" "$@"; then
    echo >&2
    echo "Upload failed. If the board won't enter the bootloader:" >&2
    echo "  hold BOOT, tap RESET, release BOOT, then run again." >&2
    exit 1
  fi
}

case "${1:-upload}" in
  build)
    require_secrets
    echo "Compiling firmware (version: $(stamped_version))…"
    "$PIO" run -d "$FW_DIR" -e esp32-c3
    ;;
  upload | "")
    require_secrets
    require_board
    echo "Flashing firmware (version: $(stamped_version))…"
    upload_with_hint -e esp32-c3 -t upload
    ;;
  -m | --monitor)
    require_secrets
    require_board
    echo "Flashing firmware (version: $(stamped_version)) + serial monitor…"
    upload_with_hint -e esp32-c3 -t upload -t monitor
    ;;
  monitor)
    require_board
    "$PIO" device monitor -b 115200
    ;;
  diag)
    require_secrets
    require_board
    echo "Flashing RSSI scanner diagnostic + serial monitor…"
    upload_with_hint -e diag -t upload -t monitor
    ;;
  *)
    echo "usage: $0 [build | upload | -m | monitor | diag]" >&2
    exit 2
    ;;
esac
