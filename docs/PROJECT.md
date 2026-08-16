# Oleander

A Raspberry Pi 4-based multi-effects pedal for guitar and synthesizers, inspired by [GuitarEffects](https://github.com/Quinny/GuitarEffects) but redesigned for a compact, headless, web-controlled stompbox form factor.

## Overview

Oleander runs a chain of real-time audio effects loaded from a web browser on any device on the same network. Physical controls (5 latching microswitches and an OLED display) provide on-device interaction, while the full effects chain is managed through `http://oleander.local` in a browser.

## Key Features

- **Real-time audio effects chain** — delay, reverb, chorus, lo-fi, distortion, compression, filters, and more
- **Web-based control** — add, remove, and configure pedals from any browser via `oleander.local`
- **5 presets** — save the whole effects chain (which pedals, all knob values, on/off state) into any of 5 slots, and recall one instantly from a footswitch or the browser
- **Physical controls** — 5 latching microswitches for instant preset recall
- **OLED display** — SSD1306 (128x64) shows the active preset name, falls back to per-pedal knob values when freely editing, and a splash screen when idle
- **Headless operation** — runs as two systemd services (audio/web server + hardware), no monitor or keyboard needed
- **Works offline** — the web control page is fully self-contained (no CDN dependency), so it loads even with no internet access on the local network
- **USB audio interface** — uses a standard USB audio device (e.g., Behringer XENYX 302USB) for clean, low-noise I/O
- **One-click setup** — clone and run a single install script

## Architecture

```
┌─────────────┐     ┌──────────────────┐     ┌─────────────────┐
│  Web Browser │────▶│  Oleander Server │────▶│  USB Audio I/O  │
│  (oleander.  │     │  (C++ / Crow)    │     │  (Behringer)    │
│   local)     │◀────│  + Python HW Svc │◀────└─────────────────┘
└─────────────┘     │                  │
                    │  SSD1306 OLED    │
                    │  5x Latching Sw  │
                    │  (preset recall) │
                    └──────────────────┘
```

The audio/web server and the hardware service (GPIO switches + OLED) run as
two independent systemd units (`oleander` and `oleander-hardware`) that
talk to each other over localhost HTTP/WebSocket, rather than one process
launching the other -- see SOFTWARE.md.

## Hardware

- **Compute:** Raspberry Pi 4 (any model)
- **Audio I/O:** USB audio interface (Behringer XENYX 302USB or similar)
- **Display:** SSD1306 128x64 OLED via I2C
- **Controls:** 5x latching keyboard microswitches on GPIO, one per preset slot
- **Enclosure:** 3D printed (user-designed)

See [HARDWARE.md](./HARDWARE.md) for full wiring diagrams and component details.

## Software

- **Audio engine:** C++ with RtAudio for real-time I/O
- **Effects:** Modular pedal architecture using cycfi::q DSP library
- **Web server:** Crow C++ REST framework
- **Presets:** JSON-persisted (`presets.json`) snapshots of the whole pedal board
- **Hardware service:** Python script for GPIO switches and OLED display
- **OS:** Raspberry Pi OS (64-bit)
- **Service management:** systemd (`oleander.service` + `oleander-hardware.service`)

See [SOFTWARE.md](./SOFTWARE.md) for architecture details and implementation plan.

## Getting Started

```bash
# On a fresh Raspberry Pi 4 (needs internet access for this step):
git clone <repo-url> oleander
cd oleander
chmod +x setup/install.sh
sudo ./setup/install.sh
```

Then open `http://oleander.local` in a browser to configure effects and save presets.

## License

GPL-3.0 (based on GuitarEffects)
