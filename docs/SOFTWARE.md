# Software Changes

This document describes the software architecture and changes made to the GuitarEffects codebase for the Oleander project.

## Base Repository

Oleander is a fork of [Quinny/GuitarEffects](https://github.com/Quinny/GuitarEffects). All original pedal implementations and the core audio pipeline are retained from upstream. The web server architecture, pedal addressing, and control surface described below have since diverged significantly from upstream to support presets.

## Directory Structure

```
oleander-effects/
├── audio_transformer.h          # RtAudio audio I/O (unchanged from upstream)
├── pedal.h                      # Modified: PedalInfo.id, Pedal::SetEnabled()
├── pedal_registry.h             # Unchanged from upstream
├── signal_type.h                # Unchanged from upstream
├── Makefile                     # Modified: Pi 4 build config
├── README.md                    # Modified: project description
│
├── web/
│   ├── main.cpp                 # Modified: preset routes, no longer launches hardware_service.py
│   ├── handlers.h               # Modified: id-based routes, preset endpoints
│   ├── pedal_board.h            # Modified: id-based addressing, Describe(), LoadSnapshot()
│   ├── preset_store.h           # NEW: 5 preset slots + current-state persistence
│   ├── serializers.h            # Modified: serializes PedalInfo.id
│   └── static/
│       ├── index.html           # Modified: vendored assets instead of CDN links
│       ├── app.jsx              # Modified: preset bar, id-based actions, WS reconnect
│       └── style.css            # Modified: preset bar styles, dark-theme contrast fix
│
├── pedals/
│   ├── all_pedals.h             # Unchanged from upstream
│   └── [existing pedals...]    # Unchanged from upstream
│
├── hardware/
│   └── hardware_service.py      # Modified: presets over WebSocket instead of pedal-toggle polling
│
├── setup/
│   ├── install.sh               # Modified: installs 2 services, vendors web assets
│   ├── oleander.service         # Modified: only runs bin/server now
│   ├── oleander-hardware.service # NEW: dedicated unit for hardware_service.py
│   └── vendor_assets.sh         # NEW: downloads jQuery/React/Bootstrap/etc. locally
│
└── [submodules...]              # (unchanged)
```

## Component Details

### 1. hardware/hardware_service.py

**Purpose:** Reads the physical controls and drives the OLED display. Communicates with the C++ server over localhost HTTP + WebSocket. Runs as its own systemd unit (`oleander-hardware.service`), independent of the audio/web server -- the two used to be coupled (the server launched this script itself as well as systemd doing so), which meant it could end up running twice and fighting over the same GPIO pins and I2C display; they're now decoupled so each can restart independently and only one copy of this script is ever running.

**Responsibilities:**

- **Latching switch handling (preset recall):**
  - Monitors 5 GPIO pins for switch state changes
  - Internal pull-up resistors enabled via gpiozero
  - On press: sends `GET /preset/<slot>` to recall the whole saved chain in that slot (retries once on failure, logs a warning if both attempts fail)
  - On release: no action for preset recall (latching state is maintained by the server)
  - Electrical bounce time: 2ms; the server additionally debounces duplicate preset-recall requests for the same slot within 150ms
  - **Latch-state reporting:** every press, release, and once at startup for each switch's actual current position, also does a best-effort `POST /switch/<slot>/state?pressed=<0|1>` (no retry, unlike the preset-recall call above -- a dropped report just leaves the web UI's indicator stale, not a functional problem). This is what drives the on/off dot on each preset tile in the web UI, independent of preset recall -- useful on its own, and especially so while the OLED isn't working, since it's otherwise the only visible confirmation a footswitch press was registered at all. See codefix.md Round 8 #1.

- **Pin factory:** explicitly pinned to `lgpio` via `GPIOZERO_PIN_FACTORY=lgpio` (set in `oleander-hardware.service`), rather than letting `gpiozero` fall through its default chain -- current Raspberry Pi OS kernels dropped the legacy `/sys/class/gpio` sysfs interface `gpiozero`'s fallback-of-last-resort still depends on, so silently landing there instead of `lgpio` used to fail hard on the very first switch (see codefix.md Round 4 #2). The `lgpio` Python module itself comes from the `python3-lgpio` apt package (prebuilt, version-matched to the OS/kernel) rather than `pip install lgpio`, which expects a system `liblgpio` to already exist and isn't meant to be built standalone (see codefix.md Round 4 #2). The venv is created with `--system-site-packages` so it can see it.

- **SSD1306 OLED display:**
  - I2C interface (GPIO 2/SDA1, GPIO 3/SCL1)
  - 128x64 resolution, monochrome
  - Display modes:
    - **Idle:** "Oleander" splash screen with simple graphic
    - **Preset active:** large "PRESET n / name" screen with a slot-position indicator
    - **Freely edited (no preset active), multiple pedals:** cycles through active pedals' knobs (pauses 2s per pedal) -- the previous default view, now shown only while you're off of any saved preset
  - Update rate: repainted at ~10Hz locally, but driven by the server's WebSocket push rather than a fixed poll (see below)
  - Library: `ssd1306`/`luma.oled` Python package
  - **Graceful degradation:** `OleanderDisplay()` construction (which opens the I2C bus immediately) is wrapped in a `try/except` in `main()`. If it fails -- most commonly because I2C isn't enabled yet (`dtparam=i2c_arm=on` needs a reboot to take effect) or the OLED isn't wired to GPIO 2/3 -- the service logs a warning explaining the likely cause and falls back to a `NullDisplay` (a no-op stand-in implementing the same `draw_splash`/`update`/`start`/`stop` interface) instead of letting the exception kill the whole process. Without this, a missing/not-yet-enabled display took the already-initialized switches down with it too, even though they have nothing to do with the display (see codefix.md Round 5 #1).

**Communication with server:**

```
Switch press  → GET /preset/<slot>
State changes → server broadcasts a WebSocket "ping" on /updates to every
                connected client (this script included); on receiving one,
                the service re-fetches /active_pedals and /presets and
                updates the OLED. If the socket is down, it falls back to
                polling those endpoints every 2s until reconnected.
```

(The server does not expose `/hardware/button/...` or `/hardware/config` endpoints -- earlier drafts of this document described some, but they were never implemented; the hardware service talks to the same `/preset/<n>`, `/active_pedals`, `/presets`, and `/updates` endpoints the browser uses.)

- **WebSocket connection lifetime:** `create_connection(WEBSOCKET_URL, timeout=5)`'s `timeout` only bounds the initial connect -- `ws.sock.settimeout(None)` is called immediately afterward so the following `recv()` loop blocks indefinitely for the next `"ping"`, as intended, instead of the socket-level timeout also applying to `recv()` and tearing the connection down every 5 idle seconds. See codefix.md Round 10 #1.

### 2. web/main.cpp Changes

**Startup sequence:**

```cpp
int main() {
    // 1. Construct the PedalBoard and PresetStore, and load whatever
    //    board state was last persisted (so a restart doesn't come back
    //    to an empty chain).
    // 2. Load device selections from devices.txt.
    // 3. Create AudioTransformer with the pedal board.
    // 4. Start the audio stream.
    // 5. Register routes (pedal + preset endpoints) and start the Crow
    //    web server.
    // 6. Run the SDL2 visualizer (optional).
}
```

`hardware_service.py` is **not** started from here -- it's launched by its own systemd unit. (An earlier version of this file did start it via `std::system(...)`, which combined with systemd also starting it meant it ran twice; see codefix.md Round 2 #1.)

**Audio device selection:** on first boot (no `devices.txt` yet), device selection auto-picks the first input-capable device and, for output, the same physical device if it also does output -- rather than prompting on `std::cin`, which used to crash-loop with `std::bad_alloc` under systemd (no terminal attached to prompt on). The interactive prompt still runs when a real terminal is attached (`./bin/server debug` over SSH). See codefix.md Round 4 #1.

**`./bin/server list-devices`:** a non-interactive argv subcommand, checked before anything else in `main()`, that prints every device RtAudio detects (via `AudioTransformer::DumpDeviceInfo()`, previously defined but never called from anywhere) and exits immediately -- no audio stream opened, no web server started. This is what makes `devices.txt` actually practical to hand-edit: previously the only way to see device names was the full interactive picker, which requires deleting/renaming a working `devices.txt` first and only triggers when a real terminal is attached with no `devices.txt` present. See codefix.md Round 9 #2.

**Static file serving:** the `/` route and the catch-all `/<string>` route (which together serve `index.html`, `app.jsx`, `style.css`, and the vendored `vendor-*` assets) construct a hand-rolled `StaticFileHandler("web/static")` (see `web/handlers.h`) -- Crow's own automatic static-dir mount is compiled out (`-D CROW_DISABLE_STATIC_DIR` in the `Makefile`). The directory argument is relative to the process's working directory, which under systemd is the repo root (`WorkingDirectory=__OLEANDER_DIR__` in `oleander.service`), not `web/` -- it used to say just `"static"`, which 404'd on every request once the service actually ran under systemd instead of being launched by hand from inside `web/`. See codefix.md Round 7 #1.

### 3. web/handlers.h Changes

**Pedal endpoints** (all now address a pedal by its stable `PedalInfo.id`, not its position in the chain):

| Endpoint | Method | Purpose |
|----------|--------|---------|
| `/active_pedals` | GET | List the live board (name, knobs, state, id) |
| `/available_pedals` | GET | List pedal types that can be added |
| `/add_pedal/<string>` | GET | Add a pedal of the given type |
| `/remove_pedal/<id>` | GET | Remove a pedal by id |
| `/adjust_knob/<id>?name=&value=` | GET | Adjust one knob on a pedal by id |
| `/push_button/<id>` | GET | Toggle a single pedal's enabled state by id (web UI only -- the footswitches no longer call this) |

**Preset endpoints** (new; `<n>` is a preset slot 0-4, unrelated to pedal ids):

| Endpoint | Method | Purpose |
|----------|--------|---------|
| `/presets` | GET | List all 5 slots (name, pedal count) + which is active |
| `/preset/<n>` | GET | Recall slot `n` into the live board (debounced 150ms; this is what the footswitches call) |
| `/preset/<n>/save?name=` | POST | Snapshot the current live board into slot `n`, optionally renaming it |
| `/preset/<n>/name?name=` | POST | Rename slot `n` without changing its saved pedals |

Every mutating endpoint above persists the live board as `PresetStore`'s "current" snapshot and broadcasts a WebSocket ping, via a small `ChangeNotifier` helper shared by all the handlers.

**Switch state endpoints** (new; `<n>` is a switch/preset slot 0-4, backed by an in-memory `SwitchStates` store -- not persisted, since it just mirrors live GPIO state that `hardware_service.py` re-reports on every restart anyway):

| Endpoint | Method | Purpose |
|----------|--------|---------|
| `/switches` | GET | List all 5 switches' current latch state (`true`/`false`) |
| `/switch/<n>/state?pressed=<0\|1>` | POST | Report switch `n`'s current physical latch state; called by `hardware_service.py`, broadcasts a WebSocket ping like the mutating pedal/preset endpoints do |

### 4. web/static/app.jsx Changes

- **Preset bar:** 5 tiles above the pedal chain, one per slot -- tap to recall (`GET /preset/<n>`, the same call a footswitch makes), a "Save here" button to snapshot the current board into that slot, and the active slot highlighted in green. Stays in sync via the same WebSocket the pedal board uses.
- **Switch status dot:** each preset tile also shows a small dot next to its slot number reflecting that footswitch's live physical latch state (dim grey = off, lit amber = on), fetched from `GET /switches` on the same refresh cycle as the preset list. This is independent of which preset is "active" -- a switch can be latched on while the board has since been freely edited away from it -- and gives visible confirmation that a footswitch press reached the server even when the OLED isn't working.
- **Stable pedal addressing:** pedal actions (adjust knob, push, remove) now use each pedal's server-assigned `id` instead of its position in the list, and `id` is used as the React `key` for list items.
- **WebSocket reconnect:** the socket connection now lives in the top-level `App` component (shared by the preset bar and pedal board) and reconnects with exponential backoff if it drops, instead of silently going stale.
- **Switch-style footswitch buttons:** unchanged from before -- large, pedal-style ON/OFF buttons per pedal, green when enabled.
- **Splash screen:** unchanged -- shown when no pedals are active.

### 5. web/static/style.css Changes

- `.preset-bar` / `.preset-tile` / `.preset-tile-active` — the new preset bar, matching the existing green "enabled" accent color for the active slot
- `.switch-status-dot` / `.switch-status-dot-on` — the small per-tile footswitch latch indicator (grey off / amber on)
- Fixed `.app-title` / `.splash-title` using near-black text that was nearly invisible against the Bootswatch "darkly" dark background
- Responsive layout adjustments extended to cover the preset tiles

### 6. index.html Changes

jQuery, Bootstrap (+ the Darkly theme), Tether, React, ReactDOM, and Babel-standalone are now vendored locally into `web/static/` (see `setup/vendor_assets.sh`) instead of loaded from public CDNs, so the control page works with no internet access -- important since Oleander is designed to run headless on the local network via mDNS, which commonly has none.

### 7. Makefile Changes

**Target changes:**

- `server` target: unchanged compilation, but updated for Pi 4 toolchain
- Removed SDL2 dependency from default build (optional for visualization)
- Added note about required packages for Pi 4
- `server` and `record` now compile the vendored `rtaudio/RtAudio.cpp` directly (with `-D__LINUX_ALSA__ -lasound`) instead of linking a system `-lrtaudio`, which nothing ever built or installed -- see codefix.md Round 3 #5.

**Build command:**

```bash
make server
```

Output: `./bin/server`

### 8. setup/install.sh

**One-click setup script:**

```bash
#!/bin/bash
# Updates system
# Installs build dependencies (g++, ALSA, Boost, SDL2, curl)
# Installs Python dependencies (gpiozero, ssd1306, websocket-client) into a venv
# Enables I2C via raspi-config
# Builds the server
# Downloads web UI assets locally (setup/vendor_assets.sh)
# Installs both systemd services (oleander, oleander-hardware)
# Installs Avahi for mDNS
```

### 9. setup/oleander.service and setup/oleander-hardware.service

Two independent systemd units instead of one that also shelled out to start the other. The checked-in files are templates (`__OLEANDER_USER__` / `__OLEANDER_DIR__`) -- `install.sh` resolves the real login user (via `$SUDO_USER`) and the actual checkout path, and `sed`s them in before copying to `/etc/systemd/system/`, rather than hardcoding `User=pi` / `/home/pi/...`. Raspberry Pi OS hasn't guaranteed a `pi` user since Bullseye (Raspberry Pi Imager now prompts for a custom username), so a hardcoded `pi` would leave the service unable to start -- or starting as the wrong account -- on any image that used a different name:

```ini
# oleander.service -- audio engine + web server only
[Service]
Type=simple
User=__OLEANDER_USER__
WorkingDirectory=__OLEANDER_DIR__
AmbientCapabilities=CAP_NET_BIND_SERVICE
CapabilityBoundingSet=CAP_NET_BIND_SERVICE
ExecStart=__OLEANDER_DIR__/bin/server
Restart=on-failure
RestartSec=5
```

```ini
# oleander-hardware.service -- GPIO footswitches + OLED only
[Service]
Type=simple
User=__OLEANDER_USER__
WorkingDirectory=__OLEANDER_DIR__
ExecStart=/opt/oleander-venv/bin/python3 __OLEANDER_DIR__/hardware/hardware_service.py
Restart=on-failure
RestartSec=5
```

`install.sh` also runs `usermod -aG gpio,i2c,spi,dialout,audio,video` on the resolved user as a safety net, since that group membership (needed for GPIO/I2C/audio access without root) is what actually matters for hardware access, not the username itself.

`oleander.service`'s `AmbientCapabilities`/`CapabilityBoundingSet` grant is the one piece the user-templating in Round 3 introduced a gap for: `web/main.cpp` binds the web server to port 80 in production, which the kernel normally reserves for root, and the unprivileged `__OLEANDER_USER__` this now runs as would otherwise fail to bind it at all (`EACCES`, uncaught, `SIGABRT` -- see codefix.md Round 6 #1). `CAP_NET_BIND_SERVICE` is the one capability needed to bind privileged ports without running as root or making the binary setuid; systemd applies it to the process before the `User=` switch, so no other change was needed.

### 10. audio_transformer.h Changes

Previously identical to the upstream repo aside from the P2 #4 bounds-checking/exception-throwing already covered in the original review; now has one more addition.

**Stream error recovery:** RtAudio's own handling of a stream-time error (e.g. the ALSA device disappearing mid-stream -- `RtApiAlsa::callbackEvent: audio read error, No such device.`) is to log a `WARNING` and keep calling the audio callback forever; it never stops the stream, never retries opening the device, and gives the rest of the process no way to notice apart from watching stderr. `AudioTransformer`'s constructor now passes a `RtAudioErrorCallback` (`internal::OnStreamError` in `audio_transformer.h`) as `openStream()`'s 9th argument, which was previously left as the default `NULL`. It tracks consecutive stream errors (any gap over 500ms starts a fresh streak) and, once 20 arrive back-to-back, treats the stream as unrecoverable and calls `std::exit(1)` -- letting systemd's `Restart=on-failure` bring the process back up and run `SelectDevices()` fresh, which picks the device back up automatically if it reappeared under the same name, or keeps retrying every 5s until it does. Before this, a dropped USB audio interface left the process running indefinitely with a silently-dead audio path: the web server and everything else kept working, so nothing outwardly indicated the pedal had gone deaf and mute except a warning buried in the journal. See codefix.md Round 9 #1.

## Data Flow

### Web UI Control (adding/adjusting/removing a pedal)

```
User clicks a pedal action in the browser
  → $.get('/adjust_knob/<id>' | '/remove_pedal/<id>' | '/push_button/<id>' | '/add_pedal/<name>')
  → Crow handler mutates PedalBoard (looked up by stable pedal id, not position)
  → ChangeNotifier persists the board as PresetStore's "current" snapshot
  → ChangeNotifier broadcasts a WebSocket "ping" to all connected clients
  → React App bumps `updateToken` on the ping
  → PedalBoard/PresetBar re-fetch /active_pedals and /presets
  → UI re-renders with updated state
  → hardware_service.py (also listening on /updates) re-fetches and updates the OLED
```

### Preset Recall (physical switch or web UI)

```
User presses a footswitch, or taps a preset tile in the browser
  → GET /preset/<slot>
  → LoadPresetHandler (debounced against duplicate requests within 150ms)
  → PedalBoard::LoadSnapshot() rebuilds the chain from the saved preset
    (via PedalRegistry factories, applying each pedal's saved knob values
    and enabled state)
  → PresetStore::SetActiveIndex(slot)
  → ChangeNotifier persists + broadcasts, same as above
  → Browser's preset bar highlights the new active slot;
    hardware_service.py's OLED shows "PRESET n / name"
```

### Preset Save

```
User taps "Save here" on a preset tile (optionally entering a new name)
  → POST /preset/<slot>/save?name=...
  → SavePresetHandler snapshots PedalBoard::GetPedals() into that slot
  → PresetStore persists to presets.json and marks the slot active
  → Broadcast, same as above
```

## Unchanged Components

The following are identical to the upstream GuitarEffects repository:

- `pedal_registry.h`, `signal_type.h` — pedal registry and signal type
- All 15+ existing pedal implementations in `pedals/`
- Submodules: RtAudio, Crow, cycfi::q, AudioFile, matplotlib-cpp
- The core audio callback and signal processing pipeline
