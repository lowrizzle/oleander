#!/usr/bin/env python3
"""
Oleander hardware service.

Handles the 5 latching microswitches (preset recall) and drives the
SSD1306 OLED display. Communicates with the C++ server over localhost
HTTP + WebSocket.

This process is started by its own systemd unit
(setup/oleander-hardware.service) and is meant to be the *only* instance
running -- previously it was also started as a background subprocess from
web/main.cpp, so two copies would end up racing over the same GPIO pins and
I2C display. If you're running this by hand for development, don't also
run the compiled `server` binary's old system()-launch path (it no longer
does that -- see web/main.cpp).
"""

import threading
import time

import requests
import websocket  # pip package: websocket-client

from gpiozero import Button
from signal import pause
from luma.oled.device import ssd1306
from luma.core.interface.serial import i2c
from luma.core.render import canvas
from PIL import ImageFont

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

# GPIO -> preset slot (0-4). Pressing a switch recalls that preset -- it no
# longer toggles an individual pedal on/off (that's still possible, but only
# from the web UI now, since "switch N always controls pedal N" broke the
# moment pedals were added/removed and the chain shifted underneath it).
PRESET_GPIO_MAP = {
    21: 0,
    20: 1,
    16: 2,
    12: 3,
    6:  4,
}

SERVER_URL = "http://localhost"
WEBSOCKET_URL = "ws://localhost/updates"

# HTTP requests to the local server should never hang the switch handler.
REQUEST_TIMEOUT_SECONDS = 1.0
REQUEST_RETRY_DELAY_SECONDS = 0.15

# OLED display
I2C_BUS = 1
I2C_ADDRESS = 0x3c

# Display update timing
DISPLAY_REFRESH_INTERVAL = 0.1  # 10 Hz repaint of whatever is already known
DISPLAY_PEDAL_PAUSE = 2.0       # seconds per pedal when cycling (edit view)

# If the WebSocket connection is down, fall back to polling the server at
# this (much slower) interval so the display still eventually catches up,
# instead of the previous constant ~6.7 Hz poll.
FALLBACK_POLL_INTERVAL_SECONDS = 2.0
WEBSOCKET_RECONNECT_DELAY_SECONDS = 1.0

# ---------------------------------------------------------------------------
# Switch handling
# ---------------------------------------------------------------------------

switch_buttons = {}
switch_states = {}  # gpio -> True/False (pressed / released)


def request_with_retry(url):
    """GET `url`, retrying once on failure. Returns True on eventual success."""
    for attempt in range(2):
        try:
            response = requests.get(url, timeout=REQUEST_TIMEOUT_SECONDS)
            if response.status_code == 200:
                return True
            print(f"[SWITCH] {url} -> HTTP {response.status_code}")
            return False
        except requests.exceptions.RequestException as exc:
            if attempt == 0:
                time.sleep(REQUEST_RETRY_DELAY_SECONDS)
                continue
            print(f"[SWITCH] {url} failed after retry: {exc}")
    return False


def report_switch_state(preset_index, pressed):
    """
    Report a switch's current physical latch state to the server, for the
    on/off indicator dot on that preset's tile in the web UI (see
    codefix.md Round 8 #1). This is separate from, and in addition to, the
    preset recall a press also triggers via request_with_retry above.

    Best-effort only, unlike request_with_retry: a dropped report just
    leaves the web UI's indicator stale until the next state change (press
    the switch again, or restart this service), not a functional problem
    for the pedal itself -- so this doesn't retry or print a warning on
    failure the way the preset recall does.
    """
    try:
        requests.post(
            f"{SERVER_URL}/switch/{preset_index}/state",
            params={"pressed": "1" if pressed else "0"},
            timeout=REQUEST_TIMEOUT_SECONDS,
        )
    except requests.exceptions.RequestException:
        pass


def on_switch_pressed(gpio, preset_index):
    """Physical switch pressed -> recall the corresponding preset."""
    switch_states[gpio] = True
    print(f"[SWITCH] GPIO {gpio} pressed -> preset {preset_index}")
    report_switch_state(preset_index, True)
    if not request_with_retry(f"{SERVER_URL}/preset/{preset_index}"):
        print(f"[SWITCH] WARNING: preset {preset_index} recall did not "
              "reach the server -- press again once it responds.")


def on_switch_released(gpio):
    """Physical switch released -> no-op for preset recall (latching), but
    still reported so the web UI's indicator reflects the new position."""
    switch_states[gpio] = False
    report_switch_state(PRESET_GPIO_MAP[gpio], False)


def init_switches():
    """Initialize all latching microswitches."""
    for gpio, preset_index in PRESET_GPIO_MAP.items():
        btn = Button(gpio, pull_up=True, bounce_time=0.002)
        btn.when_pressed = lambda g=gpio, p=preset_index: on_switch_pressed(g, p)
        btn.when_released = lambda g=gpio: on_switch_released(g)
        switch_buttons[gpio] = btn
        # Read the switch's actual current position (rather than assuming
        # "off") so the web UI's indicator is correct from the very first
        # load, not just from the next press onward.
        switch_states[gpio] = btn.is_pressed
        report_switch_state(preset_index, btn.is_pressed)
        state_label = "latched on" if btn.is_pressed else "latched off"
        print(f"[SWITCH] Initialized GPIO {gpio} -> preset {preset_index} "
              f"(currently {state_label})")


# ---------------------------------------------------------------------------
# OLED Display
# ---------------------------------------------------------------------------

class OleanderDisplay:
    """SSD1306 OLED display driver for Oleander."""

    def __init__(self):
        self.serial = i2c(port=I2C_BUS, address=I2C_ADDRESS)
        self.device = ssd1306(self.serial)
        self.cycle_thread = None
        self.running = False
        self.pedals = []
        self.active_preset = None  # {"index": int, "name": str} or None
        self.splash_shown = True

        # Try to load a TrueType font; fall back to default
        try:
            self.font_large = ImageFont.truetype(
                "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 16)
            self.font_normal = ImageFont.truetype(
                "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 10)
            self.font_small = ImageFont.truetype(
                "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 8)
        except IOError:
            self.font_large = ImageFont.load_default()
            self.font_normal = ImageFont.load_default()
            self.font_small = ImageFont.load_default()

    def draw_splash(self):
        """Draw the Oleander splash screen."""
        with canvas(self.device) as draw:
            draw.rectangle(self.device.bounding_box, outline="black", fill="black")
            draw.text((18, 12), "OLEANDER", fill="white", font=self.font_large)
            draw.text((22, 34), "Multi-Effects", fill="white", font=self.font_normal)
            draw.text((38, 48), "Pedal", fill="white", font=self.font_small)
        self.splash_shown = True

    def draw_preset(self, index, name):
        """Draw a large, at-a-glance 'which preset is this' screen.

        This is the primary in-performance view: a footswitch press should
        be confirmable at a glance, not by reading a knob-by-knob readout.
        """
        with canvas(self.device) as draw:
            draw.rectangle(self.device.bounding_box, outline="black", fill="black")
            draw.text((2, 4), f"PRESET {index + 1}", fill="white", font=self.font_large)
            draw.line((2, 24, 126, 24), fill="white")
            display_name = (name or "")[:18]
            draw.text((2, 32), display_name, fill="white", font=self.font_normal)
            # Slot indicator dots along the bottom, current slot filled in.
            for i in range(5):
                x = 4 + i * 14
                if i == index:
                    draw.ellipse((x, 52, x + 10, 62), fill="white")
                else:
                    draw.ellipse((x, 52, x + 10, 62), outline="white", fill="black")

    def draw_state_dot(self, draw, enabled):
        """Draw a small state indicator dot in the top-right corner."""
        if enabled:
            draw.ellipse((116, 4, 124, 12), fill="white")
        else:
            draw.ellipse((116, 4, 124, 12), outline="white", fill="black")

    def draw_pedal_info(self, pedal_info):
        """Draw a single pedal's info on the display (manual-edit view)."""
        name = pedal_info.get("name", "Unknown")[:14]
        state = pedal_info.get("state", "")
        enabled = "Enabled" in state
        knobs = pedal_info.get("knobs", [])

        with canvas(self.device) as draw:
            draw.rectangle(self.device.bounding_box, outline="black", fill="black")

            # Pedal name
            draw.text((2, 2), name, fill="white", font=self.font_large)
            self.draw_state_dot(draw, enabled)

            # Knobs (up to 3)
            for i, knob in enumerate(knobs[:3]):
                y = 22 + i * 16
                label = knob.get("name", "?")[:8]
                value = f"{knob.get('value', 0):.1f}"
                draw.text((2, y), f"{label}: {value}", fill="white", font=self.font_normal)

    def update(self, pedals, active_preset):
        """
        Update display with current board + preset state.

        - A preset is active -> show a big "PRESET n / name" screen.
        - No preset active but pedals exist (freely edited board) -> cycle
          through each pedal's knobs, as before.
        - Nothing active -> idle splash screen.
        """
        self.pedals = pedals
        self.active_preset = active_preset

        if active_preset is not None:
            self.splash_shown = False
            self.draw_preset(active_preset["index"], active_preset["name"])
            return

        if not pedals:
            if not self.splash_shown:
                self.draw_splash()
            return

        self.splash_shown = False
        if len(pedals) == 1:
            self.draw_pedal_info(pedals[0])
        else:
            index = int(
                (time.time() % (DISPLAY_PEDAL_PAUSE * len(pedals)))
                // DISPLAY_PEDAL_PAUSE
            )
            self.draw_pedal_info(pedals[index])

    def start(self):
        """Start the display repaint thread (keeps pedal-cycling animating)."""
        self.running = True

        def _cycle():
            while self.running:
                self.update(self.pedals, self.active_preset)
                time.sleep(DISPLAY_REFRESH_INTERVAL)

        self.cycle_thread = threading.Thread(target=_cycle, daemon=True)
        self.cycle_thread.start()
        print("[OLED] Display update thread started")

    def stop(self):
        """Stop the display and clear the screen."""
        self.running = False
        if self.cycle_thread:
            self.cycle_thread.join(timeout=1.0)
        with canvas(self.device) as draw:
            draw.rectangle(self.device.bounding_box, outline="black", fill="black")
        print("[OLED] Display stopped")


class NullDisplay:
    """
    Stand-in for OleanderDisplay when the real one can't be constructed --
    e.g. I2C isn't enabled yet (needs a reboot after setup/install.sh's
    `dtparam=i2c_arm=on`), the OLED isn't wired up yet, or it's a loose
    connection during a gig. Implements the same interface as
    OleanderDisplay (draw_splash/update/start/stop) as no-ops, so
    ServerSync and main() don't need to know or care whether a real
    display is attached.

    The alternative -- letting the OleanderDisplay() construction failure
    propagate out of main() -- used to take the whole hardware service
    down, including the 5 footswitches, even though switch handling has
    nothing to do with the display and had already initialized
    successfully by that point (see codefix.md Round 5 #1). The switches
    are the actual "recall a preset instantly while playing" feature this
    project is for; the OLED is a nice-to-have status readout that
    shouldn't be able to take them out.
    """

    def draw_splash(self):
        pass

    def update(self, pedals, active_preset):
        pass

    def start(self):
        pass

    def stop(self):
        pass


# ---------------------------------------------------------------------------
# Server sync: WebSocket push (primary) + slow polling fallback
# ---------------------------------------------------------------------------

class ServerSync:
    """
    Keeps the display in sync with the server.

    The C++ server already broadcasts a WebSocket "ping" to every connected
    client (browsers included) on every state change -- we join that same
    /updates socket instead of polling on a fixed timer, so the OLED
    updates as soon as a footswitch (or the web UI) changes something,
    rather than up to FALLBACK_POLL_INTERVAL_SECONDS/2 later on average.

    If the socket is down (server not started yet, restarting, network
    hiccup), this falls back to polling at a slow interval so the display
    still eventually catches up instead of going stale indefinitely.
    """

    def __init__(self, display):
        self.display = display
        self.running = False
        self.ws_thread = None
        self.fallback_thread = None
        self.connected = threading.Event()
        self.last_pedals = None
        self.last_active_preset = None

    def fetch_and_apply(self):
        try:
            pedals_resp = requests.get(f"{SERVER_URL}/active_pedals",
                                        timeout=REQUEST_TIMEOUT_SECONDS)
            presets_resp = requests.get(f"{SERVER_URL}/presets",
                                         timeout=REQUEST_TIMEOUT_SECONDS)
            if pedals_resp.status_code != 200 or presets_resp.status_code != 200:
                return
            pedals = pedals_resp.json().get("pedals", [])
            presets_data = presets_resp.json()
            active_index = presets_data.get("active_index", -1)
            active_preset = None
            if active_index is not None and active_index >= 0:
                for preset in presets_data.get("presets", []):
                    if preset.get("index") == active_index:
                        active_preset = {"index": active_index,
                                          "name": preset.get("name", "")}
                        break

            if pedals != self.last_pedals or active_preset != self.last_active_preset:
                self.last_pedals = pedals
                self.last_active_preset = active_preset
                self.display.update(pedals, active_preset)
        except requests.exceptions.RequestException:
            pass  # Server not reachable right now; try again next time.

    def _run_websocket(self):
        while self.running:
            try:
                # `timeout` here only bounds the initial TCP connect/
                # handshake -- but websocket-client applies it via
                # sock.settimeout(), which stays in effect for every
                # subsequent socket operation unless explicitly cleared.
                # Left as-is, that made the blocking `ws.recv()` below
                # (which legitimately waits for the next "ping" broadcast --
                # only sent on an actual board/preset/switch change, so it
                # could be anywhere from milliseconds to minutes away) raise
                # a socket timeout and tear the whole connection down every
                # 5 seconds even when nothing was wrong -- exactly the
                # "WebSocket connected" / "disconnected (Connection timed
                # out)" cycle seen in the journal. See codefix.md Round 10 #1.
                ws = websocket.create_connection(WEBSOCKET_URL, timeout=5)
                ws.sock.settimeout(None)  # only the connect above should time out
                self.connected.set()
                print("[SYNC] WebSocket connected")
                self.fetch_and_apply()  # catch up immediately on (re)connect
                while self.running:
                    ws.recv()  # blocks until the server sends "ping"
                    self.fetch_and_apply()
            except Exception as exc:
                self.connected.clear()
                if self.running:
                    print(f"[SYNC] WebSocket disconnected ({exc}); "
                          f"retrying in {WEBSOCKET_RECONNECT_DELAY_SECONDS}s")
                    time.sleep(WEBSOCKET_RECONNECT_DELAY_SECONDS)

    def _run_fallback_poll(self):
        while self.running:
            time.sleep(FALLBACK_POLL_INTERVAL_SECONDS)
            if not self.connected.is_set():
                self.fetch_and_apply()

    def start(self):
        self.running = True
        self.ws_thread = threading.Thread(target=self._run_websocket, daemon=True)
        self.ws_thread.start()
        self.fallback_thread = threading.Thread(target=self._run_fallback_poll, daemon=True)
        self.fallback_thread.start()
        print("[SYNC] Server sync started (WebSocket + fallback poll)")

    def stop(self):
        self.running = False


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    print("=" * 50)
    print("Oleander Hardware Service")
    print("=" * 50)

    # Initialize switches
    init_switches()

    # Initialize display. Its failure must not take the switches (already
    # initialized above) down with it -- fall back to a no-op display and
    # keep going. Whatever's wrong (I2C not enabled yet, OLED not wired up,
    # a loose connection) will resolve on the next normal service restart
    # without needing this process itself to stay up and keep retrying.
    try:
        display = OleanderDisplay()
    except Exception as exc:
        print(f"[OLED] WARNING: display init failed ({exc}); continuing "
              "without it -- switches still work. Check I2C is enabled "
              "(`sudo raspi-config` or /boot/firmware/config.txt, needs a "
              "reboot to take effect) and the OLED is wired to GPIO 2/3.")
        display = NullDisplay()
    display.draw_splash()
    display.start()

    # Start syncing with the server
    sync = ServerSync(display)
    sync.start()

    print("\nHardware service ready.")
    print("Switches: GPIO 21, 20, 16, 12, 6 -> presets 1-5")
    print("Display:  SSD1306 I2C (0x3c)")
    print("Press Ctrl+C to stop.\n")

    try:
        pause()
    except KeyboardInterrupt:
        print("\nShutting down...")
        sync.stop()
        display.stop()
        print("Done.")


if __name__ == "__main__":
    main()
