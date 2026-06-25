# light-control

LED control system for lighting up LEGO models, built on the ESP32-C3.

- **Controller:** ESP32-C3 PRO MINI (ESP32-C3FH4 — RISC-V, 4 MB flash, native USB)
- **LED driver:** PCA9685 (16-channel PWM over I²C)
- **Inputs (planned):** switches, potentiometers, rotary encoders
- **Connectivity:** two-way WiFi to a central computer; multiple nodes; runs
  automations autonomously when offline

See `.planning/` for design notes.

## Layout

This is a monorepo with one sub-project per component:

```
light-control/
├── firmware/                 # ESP32-C3 firmware (PlatformIO + Arduino)
├── server/                   # server node — mDNS peer + roster API + serves the PWA (Node + TS)
├── app/                      # PWA — shows the roster of peers (React + Vite)
├── .planning/                # design notes
└── light-control.code-workspace
```

## v1: discovery walking skeleton

The current version is discovery-only: every participant announces itself on the
LAN via mDNS (`_lightctrl._tcp`) and can show a roster of the others. No LEDs,
config, or persistence yet. See
[`.planning/2026-06-23-v1-discovery-design.md`](.planning/2026-06-23-v1-discovery-design.md).

Three participant kinds:

- **Firmware node** (ESP32) — announces, browses, prints its roster over serial,
  and serves `GET /api/peers`.
- **Server node** (`server/`) — announces, browses, serves the PWA, and exposes
  `GET /api/peers`. Run one or more.
- **PWA** (`app/`) — served by a server node; polls `/api/peers` every 2s and
  lists the peers. (Browsers can't speak mDNS, so the PWA asks a node.)

### Run the server + PWA

```sh
cd app && npm install && npm run build      # build the PWA into app/dist
cd ../server && npm install
npm test                                    # roster unit tests
npm start                                   # serves PWA + API on :8080
```

Open <http://localhost:8080> to see the roster. Run a second peer with
`NODE_NAME=beta PORT=8081 npm start` and the two will discover each other. For
live PWA development use `npm run dev` in `app/` (proxies `/api` to `:8080`).

Open `light-control.code-workspace` in VS Code to load every sub-project in one
window (PlatformIO activates for `firmware/`).

## Toolchain

VS Code + PlatformIO (Arduino framework). The toolchain installs automatically
the first time you build. All `pio` commands below run from the `firmware/`
folder (or pass `-d firmware`).

## When the board arrives

1. Plug the board into USB.
2. Find the serial port:
   ```sh
   pio device list
   ```
   The C3 enumerates as a native USB device (look for `/dev/cu.usbmodem*`).
   You usually do **not** need a driver.
3. Copy WiFi credentials into a git-ignored secrets file:
   ```sh
   cp include/secrets.example.h include/secrets.h   # then edit SSID/password
   ```
4. Build, upload, and open the serial monitor in one step:
   ```sh
   pio run -t upload -t monitor
   ```
   You should see it connect to WiFi, announce itself as `esp32-XXXXXX.local`,
   and print a roster of discovered peers every 5 seconds. With a server node
   running on the same network, each should appear in the other's roster, and
   `curl esp32-XXXXXX.local/api/peers` should return the roster JSON.

If upload fails to start, hold **BOOT**, tap **RESET**, release **BOOT** to
force the bootloader, then upload again.

## Useful commands

| Command | What it does |
|---|---|
| `pio run` | Compile only (no board needed) |
| `pio run -t upload` | Compile + flash |
| `pio device monitor` | Open serial monitor (115200 baud) |
| `pio run -t clean` | Clean build artifacts |
