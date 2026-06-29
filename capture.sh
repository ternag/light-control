#!/usr/bin/env bash
#
# Capture one boot/connect session from a connected ESP32-C3, then exit.
#
#   ./capture.sh         capture ~22s from whichever board is plugged in
#   ./capture.sh 30      capture for 30 seconds instead
#
# Meant for testing boards one at a time: plug a board in, run this, read the
# verdict, unplug, plug the next one in, run it again. The board's firmware
# prints its WiFi state continuously (a roster line with its IP when connected,
# or "[disconnect reason: N]" while it keeps failing), so no reset is needed —
# we just listen for a few seconds and summarise.
#
# Requires PlatformIO's bundled Python (it has pyserial). The C3 talks over
# native USB-CDC, which only emits once DTR is asserted — the script does that.
set -euo pipefail

SECONDS_TO_CAPTURE="${1:-22}"

if [ -x "$HOME/.platformio/penv/bin/python" ]; then
  PY="$HOME/.platformio/penv/bin/python"
else
  echo "error: PlatformIO Python not found at ~/.platformio/penv/bin/python." >&2
  echo "It ships with PlatformIO and has pyserial; install PlatformIO first." >&2
  exit 1
fi

CAP_SECONDS="$SECONDS_TO_CAPTURE" "$PY" - <<'PY'
import glob, os, re, sys, time

try:
    import serial
except ImportError:
    print("error: pyserial not available in this Python.", file=sys.stderr)
    sys.exit(1)

def find_port():
    for pat in ("/dev/cu.usbmodem*", "/dev/tty.usbmodem*", "/dev/ttyACM*", "/dev/ttyUSB*"):
        hits = sorted(glob.glob(pat))
        if hits:
            return hits[0]
    return None

secs = float(os.environ.get("CAP_SECONDS", "22"))

port = None
for _ in range(20):  # wait up to ~10s for a board to enumerate
    port = find_port()
    if port:
        break
    if _ == 0:
        print("waiting for a board on USB...", flush=True)
    time.sleep(0.5)
if not port:
    print("no board found (looked for /dev/cu.usbmodem*, /dev/ttyACM*, ...).", file=sys.stderr)
    sys.exit(1)

print(f"port: {port} — capturing {secs:.0f}s\n{'-'*60}", flush=True)
ser = serial.Serial(port, 115200, timeout=1)
ser.dtr = True   # native USB-CDC stays silent until DTR is asserted
ser.rts = False

lines = []
end = time.time() + secs
while time.time() < end:
    raw = ser.readline()
    if not raw:
        continue
    text = raw.decode("utf-8", "replace").rstrip("\r\n")
    lines.append(text)
    print(text, flush=True)
ser.close()

# ---- verdict ----------------------------------------------------------------
blob = "\n".join(lines)
ip = None
m = re.search(r"WiFi connected:\s*([0-9.]+)", blob)
if not m:
    m = re.search(r"\*\s+esp32-\S+\s+\w+\s+([0-9.]+):", blob)  # roster self line
if m:
    ip = m.group(1)
node = None
mid = re.search(r"id (esp32-\w+)", blob) or re.search(r"\*\s+(esp32-\w+)\b", blob)
if mid:
    node = mid.group(1)
reasons = sorted(set(re.findall(r"disconnect reason:\s*(\d+)", blob)), key=int)
no_aps = "(no networks found" in blob or "(none found)" in blob

print("-" * 60)
tag = f" [{node}]" if node else ""
if ip:
    print(f"VERDICT: CONNECTED{tag} — IP {ip}")
elif reasons:
    extra = "; scan saw no APs" if no_aps else ""
    print(f"VERDICT: NOT CONNECTED{tag} — disconnect reasons seen: {', '.join(reasons)}{extra}")
elif not lines:
    print("VERDICT: NO OUTPUT — board silent (wrong port? not powered? try reseating USB)")
else:
    print(f"VERDICT: UNCLEAR{tag} — no 'connected' and no reason codes in this window; "
          "try a longer capture, e.g. ./capture.sh 30")
PY
