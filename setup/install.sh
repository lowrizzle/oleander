#!/bin/bash
# Oleander - One-Click Setup Script
# Run on a fresh Raspberry Pi 4 with Raspberry Pi OS (64-bit recommended)

set -e

echo "=========================================="
echo "  Oleander Multi-Effects Pedal Setup"
echo "=========================================="
echo ""

# Check if running as root
if [ "$EUID" -ne 0 ]; then
  echo "Error: This script must be run as root (use sudo)"
  exit 1
fi

# Check if running on Raspberry Pi
if ! grep -q "Raspberry" /proc/cpuinfo 2>/dev/null; then
  echo "Warning: This does not appear to be a Raspberry Pi."
  echo "Continuing anyway..."
fi

# Determine which user (and install directory) the systemd services should
# run as. Raspberry Pi OS no longer guarantees a "pi" user -- since the
# Bullseye release, Raspberry Pi Imager prompts for a custom username
# during flashing, and raspi-config's own user-creation flow allows the
# same -- so hardcoding User=pi / /home/pi/... in the unit files breaks on
# any system that didn't use that legacy default (the service fails to
# start with "User pi does not exist", or starts as the wrong user if a
# "pi" account happens to exist for an unrelated reason).
#
# When this script is run the normal way (`sudo ./setup/install.sh` from
# your own login shell), $SUDO_USER is that real user. If it's missing
# (e.g. invoked from an actual root shell), fall back to whoever owns this
# checkout, and refuse to install services that would run as root.
if [ -n "$SUDO_USER" ] && [ "$SUDO_USER" != "root" ]; then
  OLEANDER_USER="$SUDO_USER"
else
  OLEANDER_USER="$(stat -c '%U' "$(dirname "$(readlink -f "$0")")/..")"
  if [ -z "$OLEANDER_USER" ] || [ "$OLEANDER_USER" = "root" ]; then
    echo "Error: could not determine a non-root user to run Oleander as."
    echo "Run this script as 'sudo ./setup/install.sh' from your normal login"
    echo "user's shell (not from a root shell/login), so the services get"
    echo "installed to run as that user rather than as root."
    exit 1
  fi
fi
OLEANDER_DIR="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)"
echo "Installing Oleander to run as user '$OLEANDER_USER' from $OLEANDER_DIR"

# Update system
echo "[1/11] Updating system packages..."
apt update && apt upgrade -y

# Install build dependencies
#
# python3-dev: several of the pip packages installed into the venv below
# (e.g. spidev) ship as source and compile a small C extension against
# Python.h, which needs the Python headers this package provides.
#
# python3-lgpio: current Raspberry Pi OS kernels (Bookworm and newer)
# dropped the old /sys/class/gpio sysfs interface in favor of the gpiochip
# character device API; gpiozero needs the `lgpio` Python module to talk
# to it (see below). `pip install lgpio` looked like the obvious way to
# get that, but PyPI's lgpio package only ships SWIG source and expects
# a system `liblgpio` already present to link against (`cannot find
# -llgpio`) -- it's not meant to be built standalone. Raspberry Pi OS's
# own apt repo ships a prebuilt, version-matched python3-lgpio (C library
# included) instead, which is what's actually installed below.
echo "[2/11] Installing build dependencies..."
apt install -y \
  g++ \
  libasound2-dev \
  libboost-system-dev \
  libsdl2-dev \
  python3 \
  python3-pip \
  python3-venv \
  python3-dev \
  python3-lgpio \
  avahi-daemon \
  avahi-utils \
  git \
  curl

# Install Python dependencies for hardware service
#
# --system-site-packages: lets the venv see python3-lgpio (installed via
# apt just above) alongside whatever's pip-installed here -- pip-installed
# packages in the venv still take priority, this only adds the system
# packages as a fallback for anything not already provided by the venv
# itself, which is exactly what's needed for `import lgpio` to work.
#
# Without lgpio available, gpiozero silently falls back through
# RPi.GPIO/pigpio to its legacy "Native" factory, which still assumes the
# sysfs interface above and fails with a confusing OSError the first time
# a switch GPIO is set up. oleander-hardware.service pins
# GPIOZERO_PIN_FACTORY=lgpio so this fails loudly instead of silently
# degrading if it's ever missing.
echo "[3/11] Installing Python dependencies..."
# Recreate from scratch rather than reusing whatever's already at this path
# -- this script may be run more than once while getting things working
# (as happened here), and a stale venv from an earlier attempt could be
# missing --system-site-packages, or have a partial/failed package install
# left over from a previous run's build error.
rm -rf /opt/oleander-venv
python3 -m venv --system-site-packages /opt/oleander-venv
source /opt/oleander-venv/bin/activate
pip install gpiozero luma.oled pillow requests spidev websocket-client
deactivate

# Enable I2C (required for SSD1306 OLED display)
echo "[4/11] Enabling I2C interface..."
if grep -q "^dtparam=i2c_arm=on" /boot/config.txt 2>/dev/null || \
   grep -q "^dtparam=i2c_arm=on" /boot/firmware/config.txt 2>/dev/null; then
  echo "I2C already enabled"
else
  echo "dtparam=i2c_arm=on" >> /boot/config.txt
  echo "I2C enabled. Reboot required."
fi

# Fetch vendored dependencies (RtAudio, eurorack, Crow, cycfi/Q, etc.).
# These are git submodules, not part of the top-level repo tree -- a plain
# `git clone` (without --recurse-submodules) leaves their directories empty,
# which fails the build below with "No such file or directory" on headers
# like rtaudio/RtAudio.h that very much do exist, just not yet fetched here.
#
# Deliberately NOT `--recursive`: cycfi/Q has its own nested submodule,
# q_io/external/portaudio, pointing at a private git.assembla.com repo that
# prompts for credentials nobody running this script has -- --recursive
# hangs the whole install on that prompt. q_io is Q's own live-audio-I/O
# layer; Oleander doesn't use it (only Q's header-only q_lib DSP library,
# via -I cycfi/Q/q_lib/include in the Makefile), so it's skipped entirely.
# Only the nested submodules actually referenced by the build are fetched
# below: eurorack/stmlib (used directly in the Makefile's build command)
# and cycfi/infra's own small filesystem-shim dependency.
echo "[5/11] Fetching vendored submodules (RtAudio, eurorack, Crow, ...)..."
cd "$(dirname "$0")/.."
if [ -d .git ]; then
  git submodule update --init
  # These two are submodules *of* eurorack/cycfi-infra, not of this repo --
  # `git submodule update --init <nested-path>` from here doesn't see them
  # as a known pathspec, so it has to run inside the nested repo instead.
  git -C eurorack submodule update --init stmlib
  git -C cycfi/infra submodule update --init external/filesystem
else
  echo "Warning: not a git checkout (no .git directory) -- skipping submodule"
  echo "fetch. If the build below fails with missing headers under rtaudio/"
  echo "or eurorack/, re-clone with 'git clone --recurse-submodules' instead"
  echo "(then answer 'skip'/interrupt if prompted for git.assembla.com creds"
  echo "-- see the comment above this block)."
fi

# Build the server
echo "[6/11] Building Oleander server..."
make server
echo "Build complete: bin/server"

# Download the web UI's third-party JS/CSS so it works with no internet
# access once deployed (see setup/vendor_assets.sh for why).
echo "[7/11] Downloading web UI assets for offline use..."
bash setup/vendor_assets.sh

# Install systemd services: the audio/web server and the hardware service
# (GPIO footswitches + OLED) are separate units so each can be restarted
# independently and neither can accidentally end up running twice.
echo "[8/11] Installing systemd services (user: $OLEANDER_USER, dir: $OLEANDER_DIR)..."
sed -e "s|__OLEANDER_USER__|$OLEANDER_USER|g" -e "s|__OLEANDER_DIR__|$OLEANDER_DIR|g" \
  setup/oleander.service > /etc/systemd/system/oleander.service
sed -e "s|__OLEANDER_USER__|$OLEANDER_USER|g" -e "s|__OLEANDER_DIR__|$OLEANDER_DIR|g" \
  setup/oleander-hardware.service > /etc/systemd/system/oleander-hardware.service
systemctl daemon-reload
systemctl enable oleander
systemctl enable oleander-hardware
echo "Systemd services installed and enabled"

# Belt-and-suspenders: the primary account created via raspi-config/Imager
# is normally already in these groups (needed for GPIO, I2C, and audio
# device access without root), but a custom-flow install (e.g. useradd by
# hand) might not be. Adding again is a harmless no-op if already a member.
usermod -aG gpio,i2c,spi,dialout,audio,video "$OLEANDER_USER" 2>/dev/null || true

# Install Avahi for mDNS (oleander.local)
echo "[9/11] Configuring mDNS (oleander.local)..."
systemctl enable avahi-daemon
systemctl start avahi-daemon
echo "mDNS enabled"

# Create a user-friendly startup note
echo "[10/11] Setup complete!"
echo ""
echo "=========================================="
echo "  Installation Complete!"
echo "=========================================="
echo ""
echo "To start Oleander:"
echo "  sudo systemctl start oleander oleander-hardware"
echo ""
echo "To check status:"
echo "  sudo systemctl status oleander oleander-hardware"
echo ""
echo "To view logs:"
echo "  sudo journalctl -u oleander -u oleander-hardware -f"
echo ""
echo "Open http://oleander.local in a browser"
echo "  (or http://<pi-ip-address> if mDNS isn't working)"
echo ""
echo "Hardware wiring:"
echo "  SSD1306 OLED: I2C on GPIO 2/3"
echo "  Switches (presets 1-5): GPIO 21, 20, 16, 12, 6"
echo "  Audio: USB interface (select on first run)"
echo ""
echo "[11/11] Reboot to apply all changes:"
echo "  sudo reboot"
