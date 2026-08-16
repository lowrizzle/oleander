# Hardware

This document describes the hardware components, wiring, and mechanical design for the Oleander multi-effects pedal.

## Bill of Materials

| Item | Quantity | Notes |
|------|----------|-------|
| Raspberry Pi 4 (any model) | 1 | 2GB, 4GB, or 8GB |
| USB audio interface | 1 | Behringer XENYX 302USB or similar |
| SSD1306 OLED display | 1 | 128x64, I2C interface |
| Latching microswitches | 5 | Keyboard-style, SPST |
| 2.54mm header pins | 1x16 | For SSD1306 connection |
| Jumper wires | several | Female-female, female-male |
| USB micro-SD card | 1 | 16GB+ for Raspberry Pi OS |
| USB cable | 1 | For audio interface |
| 3D printed enclosure | 1 | User-designed |

## Wiring Diagram

### SSD1306 OLED Display (I2C)

```
SSD1306 Pin    →    Pi 4 Pin    →    GPIO / Function
─────────────────────────────────────────────────────────
VCC (Pin 16)   →    Pin 1        →    3.3V power
GND (Pin 14)   →    Pin 6        →    Ground
SDA  (Pin 5)   →    Pin 3        →    GPIO 2 (SDA1)
SCL  (Pin 3)   →    Pin 5        →    GPIO 3 (SCL1)
```

**Notes:**
- I2C is enabled by default on Pi 4, but verify with `i2cdetect -y 1`
- Add 4.7k pull-up resistors on SDA and SCL if the display board does not include them
- Operating voltage: 3.3V (do not connect to 5V)

### Latching Microswitches

Each switch recalls one of the 5 saved presets (the whole effects chain --
which pedals are active, every knob value, on/off state) -- pressing SW1
always recalls preset 1, regardless of what's currently loaded or how many
pedals are in the chain.

```
Switch    →    Pi 4 Pin    →    GPIO    →    Preset Slot
─────────────────────────────────────────────────────────────
SW1       →    Pin 40       →    GPIO 21 →    1
SW2       →    Pin 38       →    GPIO 20 →    2
SW3       →    Pin 36       →    GPIO 16 →    3
SW4       →    Pin 32       →    GPIO 12 →    4
SW5       →    Pin 22       →    GPIO  6 →    5

All switches: other terminal → GND (Pi Pin 9, 14, 20, 25, 30, 34, or 39)
```

**Notes:**
- Internal pull-up resistors enabled in software via gpiozero (`pull_up=True`)
- No external resistors needed
- Bounce time: 2ms (configured in software); the server additionally
  ignores a repeat preset-recall request for the same slot within 150ms,
  in case of switch contact chatter beyond that
- Switches are latching: software tracks state, physical switch maintains position
- To save the *current* settings into a slot (rather than just recalling
  it), use the "Save here" button on that preset's tile in the web UI --
  the switches themselves only recall, they don't overwrite

### USB Audio Interface

```
Behringer XENYX 302USB  →    Pi 4 USB-A port (via USB-A to USB-B cable)
```

**Notes:**
- Standard USB Audio Class device — no drivers needed
- Appears as an ALSA device enumerated by RtAudio
- On first boot the server auto-selects it (and writes the choice to `devices.txt`) rather than prompting, since there's no terminal attached when it's launched by systemd; to pick a specific device by hand instead (e.g. if more than one USB audio device is plugged in), run `./bin/server debug` over SSH once before starting the service
- Instrument/line input and headphone/line output both pass through the interface

**Setting/changing the device yourself:** `devices.txt` (in the repo root) is a plain text file, two lines: the input device's exact name on line 1, the output device's exact name on line 2 -- there's no web UI or HTTP endpoint for this (the server picks a device once, at startup, before it even starts listening). To find the exact name string to use:

```bash
cd oleander-effects   # wherever the repo is checked out
./bin/server list-devices
```

This prints every device RtAudio currently detects (id, name, input/output channel counts) and exits immediately -- no prompt, no audio stream opened, safe to run any time, including while `oleander.service` is already running. Copy the exact `name:` line for the device you want (case- and whitespace-sensitive) into `devices.txt`, then `sudo systemctl restart oleander`. To go back to auto-selection, just delete `devices.txt` and restart -- the server will pick one on its own and write a fresh file.

If the device you want doesn't show up in `list-devices` at all, it's not something oleander can fix in software: check the physical USB connection first (see "Audio Not Working" below).

## Pinout Reference

### Raspberry Pi 4 GPIO Header

```
Pin  1:  3.3V  ──┬── SSD1306 VCC
Pin  3:  SDA1  ──┴── SSD1306 SDA
Pin  5:  SCL1  ──┴── SSD1306 SCL
Pin  6:  GND   ──┬── SSD1306 GND
                 │     Switch GND (all 5)
Pin  9:  GND   ──┘
Pin 14:  GND
Pin 20:  GND
Pin 22:  GPIO 6   ── SW5
Pin 25:  GND
Pin 30:  GND
Pin 32:  GPIO 12  ── SW4
Pin 36:  GPIO 16  ── SW3
Pin 38:  GPIO 20  ── SW2
Pin 39:  GND
Pin 40:  GPIO 21  ── SW1
```

## Power

All components draw power from the Pi 4:

- **Pi 4:** USB-C power supply (5V/3A recommended)
- **SSD1306:** ~20mA from Pi 3.3V rail
- **Microswitches:** negligible (pull-up current only)
- **USB audio interface:** powered independently via its own adapter

**Total Pi 3.3V draw:** well within the Pi 4's capability (~1A+ available)

## Enclosure

The enclosure is user-designed (3D printed). Design considerations:

### Recommended Dimensions

| Dimension | Min Size | Notes |
|-----------|----------|-------|
| Width | 120mm | Pi 4 is 85x56mm, allow clearance |
| Depth | 80mm | USB ports and GPIO need clearance |
| Height | 40mm | OLED + switches + wiring clearance |

### Cutout Requirements

| Cutout | Size | Quantity | Position |
|--------|------|----------|----------|
| OLED window | 32x16mm | 1 | Top front |
| Switch holes | 6mm diameter | 5 | Top surface, spaced evenly |
| USB port | 12x4mm | 1 | Side or rear |
| Power switch | 8x8mm | 1 | Optional, front or side |
| Ventilation | N/A | N/A | Optional, small slots |

### Mounting

- Pi 4 mounts with standard M2.5 or 3-6 standoff hardware
- OLED sits flush in top cutout, secured with adhesive or clips
- Switches mount through top surface, secured with nuts
- USB cable exits through side or rear cutout
- Consider adding a foot-print adhesive pad on the bottom

### Assembly Steps

1. 3D print enclosure body and any caps/lids
2. Install Pi 4 into enclosure with standoffs
3. Mount OLED display in top cutout
4. Install 5 microswitches through top surface
5. Wire SSD1306 to GPIO (I2C)
6. Wire microswitches to GPIO pins
7. Route USB cable to enclosure cutout
8. Connect USB audio interface
9. Install micro-SD card with Raspberry Pi OS
10. Run setup script

## Component Sourcing

| Component | Where to Buy | Approx. Cost |
|-----------|-------------|-------------|
| Raspberry Pi 4 | Any electronics retailer | $35-75 |
| SSD1306 OLED (I2C) | Adafruit, SparkFun, AliExpress | $5-8 |
| Latching microswitches | Digi-Key, Mouser, AliExpress | $1-2 each |
| Header pins, jumper wires | Any electronics retailer | $3-5 |
| USB audio interface | Already owned (Behringer) | $0 |
| 3D printing | Print yourself or service | $10-20 |

## Troubleshooting

### OLED Not Displaying

A missing or not-yet-working OLED no longer takes the footswitches down with it -- `oleander-hardware.service` catches display init failures and falls back to a no-op display, logging a warning to the journal (`sudo journalctl -u oleander-hardware -f`) and continuing to serve preset recalls normally. So if switches work but the screen stays blank, it's just the display that needs attention below, not the whole service.

```bash
# Check the I2C device node actually exists
ls /dev/i2c-1

# If it doesn't: I2C isn't enabled yet, or was enabled but the Pi hasn't
# been rebooted since (dtparam=i2c_arm=on in /boot/config.txt or
# /boot/firmware/config.txt only takes effect after a reboot -- setup/
# install.sh adds this line but does not reboot for you)
sudo raspi-config   # Interface Options -> I2C -> Enable
sudo reboot

# Once /dev/i2c-1 exists, check the OLED is actually detected on the bus
i2cdetect -y 1

# Should show address 0x3c or 0x3d

# Test display
python3 -c "import ssd1306; print('OLED OK')"

# Restart the hardware service to pick up a display that's now working --
# it only tries once, at startup
sudo systemctl restart oleander-hardware
```

### Switches Not Responding

```bash
# Check GPIO state (current Raspberry Pi OS kernels use the gpiochip
# character-device API -- the old /sys/class/gpio/gpioN/value files are
# gone, so don't bother checking those)
gpioinfo gpiochip0 | grep -A1 "line *21:"

# Test with gpiozero (should print True/False, not raise an exception)
python3 -c "from gpiozero import Button; print(Button(21).is_pressed)"

# If that raises an OSError/FileNotFoundError instead, gpiozero probably
# fell back to its legacy sysfs-based factory instead of using lgpio --
# confirm lgpio is actually installed in the venv:
/opt/oleander-venv/bin/python3 -c "import lgpio; print('lgpio OK')"

# Check the hardware service is running (and not, say, crash-looping
# because a second copy is already holding the GPIO pins)
sudo systemctl status oleander-hardware
sudo journalctl -u oleander-hardware -f
```

### Audio Not Working

```bash
# List audio devices the OS sees at all
aplay -l

# List audio devices RtAudio (oleander's audio library) sees, and their
# exact name strings -- see "Setting/changing the device yourself" above
cd oleander-effects && ./bin/server list-devices

# Check what oleander is actually currently using
cat devices.txt

# Test audio output outside of oleander entirely
speaker-test -t wav -c 2
```

If the interface was working and then stopped (e.g. `journalctl -u oleander` shows a burst of `RtApiAlsa::callbackEvent: audio read error, No such device.` lines): the USB connection was very likely dropped -- power management on the port, a marginal cable, or the interface itself losing power. `oleander.service` now detects this automatically (20+ stream errors in rapid succession) and exits so systemd restarts it and re-runs device selection from scratch, rather than continuing to run with an already-dead, unrecoverable audio path -- see codefix.md Round 9 #1. Check the journal for `[AUDIO] Fatal: ...` to confirm this happened. If the interface has genuinely re-appeared (reseat the USB cable, or power-cycle it) but the restart loop is still failing, its device name may have changed on re-enumeration -- run `./bin/server list-devices` again, and if the name in `devices.txt` no longer matches, either delete `devices.txt` to auto-select again or paste in the new exact name by hand.

### High Latency

- Adjust buffer size in `audio_transformer.h` (default: 32 frames)
- Larger buffer = more latency but more stable
- Smaller buffer = less latency but may crackle under load
- Target: 32-64 frames for ~3-6ms latency at 48kHz
