# Oleander - Multi-Effects Pedal

A Raspberry Pi 4-based multi-effects pedal system built for synth players and dawless jammers, forked from [GuitarEffects](https://github.com/Quinny/GuitarEffects).

![Oleander's web control interface, showing the preset bar, available pedals, and the Sky Chive granular effect on a vintage-synth-style fader panel](docs/images/web-ui.jpg)

## Features

- Real-time audio effects chain (delay, reverb, granular texture, distortion, compression, filters, and more)
- Vintage-synth-style control surface — vertical faders and LED pushbuttons, not a generic web form
- Web-based control via `http://oleander.local` in any browser
- 5 latching microswitches for physical pedal on/off control
- SSD1306 OLED display showing active pedal name, knob values, and status
- Headless operation via systemd service
- USB audio interface support (Behringer XENYX 302USB or similar)
- One-click setup script

## Quick Start

```bash
# Clone the repo
git clone <repo-url> oleander-effects
cd oleander-effects

# Run the setup script (requires sudo)
sudo ./setup/install.sh

# Reboot to apply I2C changes
sudo reboot

# Start the service
sudo systemctl start oleander

# Open http://oleander.local in your browser
```

## Hardware

![Oleander's finished hardware enclosure](case/hardware_v1.jpg)

- **Compute:** Raspberry Pi 4
- **Audio:** USB audio interface (Behringer XENYX 302USB)
- **Display:** SSD1306 128x64 OLED (I2C)
- **Controls:** 5x latching microswitches on GPIO

See [docs/HARDWARE.md](docs/HARDWARE.md) for wiring diagrams and component details. 3D-printable enclosure files (`case/oly_bod.stl`, `case/oly_lid.stl`) are in `case/`.

## Architecture

```
Web Browser ──▶ Oleander Server (C++/Crow) ──▶ USB Audio I/O
                ▲                              │
                │         hardware_service.py  │
                └──────────────────────────────┘
                            │
                    SSD1306 OLED
                    5x Latching Switches
```

## Documentation

- [docs/PROJECT.md](docs/PROJECT.md) — project overview and architecture
- [docs/HARDWARE.md](docs/HARDWARE.md) — wiring diagrams, BOM, troubleshooting
- [docs/SOFTWARE.md](docs/SOFTWARE.md) — software architecture and implementation plan
- [docs/codefix.md](docs/codefix.md) — history of bugs found and fixed
- [docs/THIRD_PARTY.md](docs/THIRD_PARTY.md) — third-party code and licenses

## License

GPL-3.0 (based on GuitarEffects by Quinny)
