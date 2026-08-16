# Code Review Fixes

This document describes all changes made to the Oleander multi-effects pedal codebase following the comprehensive code review. Issues are organized by severity from the original review.

## P0 - Build-Breaking

No build-breaking issues were found in the final codebase. The issues originally flagged (duplicate member declarations, missing semicolon, wrong variable name) were either already resolved or were false positives from the initial analysis.

## P1 - Runtime Safety

### 1. Flanger Buffer Overflow (`pedals/flanger_pedal.h`)

**Problem:** `read_offset` was implicitly cast from `double` to `int` without bounds checking. Large `delay_end_seconds` values could cause `delayed_read` to underflow past `INT_MIN`, causing undefined behavior.

**Fix:** Added proper clamping and modulo arithmetic:

```cpp
// Before
int delayed_read = delay_index_ - read_offset;
if (delayed_read < 0)
    delayed_read += delay_buffer_.size();

// After
size_t buffer_size = delay_buffer_.size();
size_t clamped_offset = static_cast<size_t>(
    std::min(static_cast<double>(buffer_size), std::max(0.0, read_offset)));
int delayed_read = static_cast<int>(
    (delay_index_ - static_cast<int>(clamped_offset) + buffer_size) % buffer_size);
```

Also added an empty buffer check before access.

### 2. WaveShaper Division by Zero (`fx/wave_shaper.h`)

**Problem:** If `points_to_generate` was 0 (possible via unvalidated web API input), `distance / (points_to_generate * 1.0)` produces infinity/NaN, corrupting the entire curve.

**Fix:** Validate at constructor entry:

```cpp
if (points_to_generate <= 0) {
    points_to_generate = 1;
}
```

### 3. Integer Overflow in Delay Buffers (`pedals/delay_pedal.h`, `echo_pedal.h`, `flanger_pedal.h`, `reverse_delay_pedal.h`, `reverb_pedal.h`)

**Problem:** `delay_seconds_ * 44100` could overflow an `int` when users set extreme values via the web API. No bounds checking was performed on knob values.

**Fix:** All delay pedals now clamp `delay_seconds_` to `[0.001, 10.0]` seconds:

```cpp
delay_seconds_ = std::max(0.001, std::min(knob.value, 10.0));
```

Buffer allocation uses `size_t` instead of implicit `int`:

```cpp
delay_buffer_ = std::vector<SignalType>(
    static_cast<size_t>(delay_seconds_ * 44100), 0);
```

The ReverbPedal also guards against negative delay times when applying its offset offsets.

### 4. Input Validation in AdjustKnobHandler (`web/handlers.h`)

**Problem:** `std::stod()` could throw `std::invalid_argument` or `std::out_of_range` on invalid input, crashing the web server thread. No bounds checking on `pedal_index`.

**Fix:** Full validation with try/catch and post-validation:

```cpp
double value;
try {
    size_t pos = 0;
    value = std::stod(value_param, &pos);
    if (pos != strlen(value_param)) {
        return crow::response(400);
    }
} catch (const std::exception&) {
    return crow::response(400);
}

if (std::isinf(value) || std::isnan(value)) {
    return crow::response(400);
}
```

### 5. Use-After-Free in UpdatesHandler (`web/handlers.h`)

**Problem:** `UpdatesHandler` stored raw pointers to WebSocket connections. If a client disconnected during `OnUpdate()`, `send_text()` could dereference a freed pointer. Concurrent `OnUpdate()` and `RemoveConnection()` could cause iterator invalidation.

**Fix:** Copy the connection list under the lock before iterating, and catch exceptions from failed sends:

```cpp
void OnUpdate() {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<crow::websocket::connection*> connections_copy;
    for (auto* connection : connections_) {
        connections_copy.push_back(connection);
    }
    for (auto* connection : connections_copy) {
        try {
            connection->send_text("ping");
        } catch (...) {
            RemoveConnection(connection);
        }
    }
}
```

### 6. Bounds Checking in PedalBoard (`web/pedal_board.h`)

**Problem:** `AdjustKnob()` and `RemovePedal()` accessed `pedals_[pedal_index]` without bounds checking, allowing out-of-bounds access from the web API.

**Fix:** Added index validation:

```cpp
void AdjustKnob(int pedal_index, const PedalKnob& knob) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pedal_index < 0 || static_cast<size_t>(pedal_index) >= pedals_.size()) {
        return;
    }
    pedals_[pedal_index]->AdjustKnob(knob);
}
```

Same pattern applied to `RemovePedal()`.

(Superseded in Round 2 below: these are no longer index lookups at all -- pedals are now addressed by a stable id, which sidesteps the bounds question by making an invalid/stale id simply "not found" rather than a raw index that has to be range-checked.)

## P2 - Undefined Behavior

### 1. Playback Empty Vector (`playback.h`)

**Problem:** `frames_[next_frame_]` on an empty vector is undefined behavior. `% 0` is also division by zero.

**Fix:** Return 0 when the buffer is empty:

```cpp
SignalType next() {
    if (frames_.empty()) {
        return SignalType(0);
    }
    // ... existing code
}
```

### 2. LooperPedal Empty Buffer (`pedals/looper_pedal.h`)

**Problem:** In `REPLAY` mode, if `loop_buffer_` is empty, `loop_buffer_[0]` is UB and `% 0` is division by zero.

**Fix:** Return raw signal when buffer is empty:

```cpp
case Mode::REPLAY:
    if (loop_buffer_.empty()) {
        return signal;
    }
    // ... existing code
```

### 3. WavLooper Crash (`pedals/wav_looper.h`)

**Problem:** Used `assert()` for error handling. If the WAV directory was empty or unreadable, the process would abort. `assert(!files.empty())` crashes in release builds.

**Fix:** Replaced assertions with graceful empty returns:

```cpp
std::vector<std::string> GetAllFiles(const std::string& directory_path) {
    DIR* directory = opendir(directory_path.c_str());
    if (directory == nullptr) {
        return {};
    }
    // ... existing code
    closedir(directory);
    return files;  // May be empty
}
```

Added empty checks in `Transform()`, `AdjustKnob()`, and the constructor.

### 4. AudioTransformer Error Checking (`audio_transformer.h`)

**Problem:** No bounds checking on device indices, no check on `openStream()` return value.

**Fix:** Added validation and exception throwing:

```cpp
unsigned int device_count = audio_interface_.getDeviceCount();
if (static_cast<unsigned int>(input_device_index) >= device_count ||
    static_cast<unsigned int>(output_device_index) >= device_count) {
    throw std::out_of_range("Audio device index out of range");
}

const auto input_device = audio_interface_.getDeviceInfo(input_device_index);
const auto output_device = audio_interface_.getDeviceInfo(output_device_index);
if (!input_device.probed || !output_device.probed) {
    throw std::runtime_error("Audio device could not be probed");
}

int rc = audio_interface_.openStream(...);
if (rc != 0) {
    throw std::runtime_error("Failed to open audio stream");
}
```

### 5. AutoWah Filter Bug (`pedals/auto_wah_pedal.h`)

**Problem:** Both `lowpass_` and `bandpass_` were set to `max_frequency_` in `AdjustKnob()`. The bandpass should use `min_frequency_` to create the wah sweep effect.

**Fix:**

```cpp
// Before
lowpass_ = {max_frequency_, 44100, q_};
bandpass_ = {max_frequency_, 44100, q_};

// After
lowpass_ = {max_frequency_, 44100, q_};
bandpass_ = {min_frequency_, 44100, q_};
```

## P3 - Stylistic and Quality

### 1. Unused Includes

Removed `#include <iostream>` from:
- `pedals/delay_pedal.h`
- `pedals/echo_pedal.h`
- `pedals/reverb_pedal.h`
- `pedals/reverse_delay_pedal.h`
- `pedals/sawtooth.h`

Added `#include <algorithm>` and `#include <cmath>` to `pedals/sawtooth.h` for `std::abs`.

### 2. Wrong Include Path (`fx/effects_pipeline.h`)

**Problem:** Included `"signal.h"` instead of `"signal_type.h"`.

**Fix:**

```cpp
// Before
#include "signal.h"

// After
#include "signal_type.h"
```

### 3. Typo: `frequency_multipler` (`pedals/fuzz_pedal.h`)

**Problem:** Member and knob name misspelled as `frequency_multipler` instead of `frequency_multiplier`.

**Fix:** Renamed all occurrences (member variable, knob name string, AdjustKnob condition).

### 4. Approximate Pi (`pedals/flanger_pedal.h`)

**Problem:** Used hardcoded approximation `3.14145` instead of proper `M_PI`.

**Fix:**

```cpp
// Before
auto previous_phase = std::sin(2 * 3.14145 * phase_);

// After
auto previous_phase = std::sin(2.0 * M_PI * phase_);
```

### 5. Singleton Memory Leak (`pedal_registry.h`)

**Problem:** Used raw `new` for the Meyers singleton, which never frees memory at program exit.

**Fix:**

```cpp
// Before
static auto* instance = new PedalRegistry();
return *instance;

// After
static PedalRegistry instance;
return instance;
```

### 6. BoostPedal Clipping (`pedals/boost_pedal.h`)

**Problem:** No output clipping. If `boost_ > 1.0`, the signal exceeds `[-1, 1]` range, causing DAC artifacts.

**Fix:** Added clipping:

```cpp
SignalType Transform(SignalType signal) override {
    SignalType boosted = signal * boost_;
    if (boosted > 1) return 1;
    if (boosted < -1) return -1;
    return boosted;
}
```

### 7. Python Virtual Environment (`setup/install.sh`, `setup/oleander.service`)

**Problem:** Used `pip3 install --break-system-packages` which bypasses Python's venv protection and can break system packages.

**Fix:** Create and use a virtualenv at `/opt/oleander-venv`:

```bash
python3 -m venv /opt/oleander-venv
source /opt/oleander-venv/bin/activate
pip install gpiozero luma.oled pillow requests spidev
deactivate
```

Updated `oleander.service` to activate the venv before running:

```ini
Environment=VIRTUAL_ENV=/opt/oleander-venv
Environment=PATH=/opt/oleander-venv/bin:%{env:PATH}
```

## Round 2 - Control Surface & Web Interface (Presets)

A follow-up review focused specifically on the physical control surface (the 5 footswitches + OLED) and the web interface, with the goal of turning them into a 5-preset switcher for live use. That surfaced its own set of bugs, plus the fact that presets didn't exist as a concept anywhere in the codebase. Both are addressed here.

### 1. Hardware service started twice (`web/main.cpp`, `setup/oleander.service`)

**Problem:** `main.cpp` unconditionally ran `std::system("python3 hardware/hardware_service.py &")` on every server startup, while `oleander.service`'s `ExecStart` *also* launched `hardware_service.py` in the background before starting `bin/server`. Under the real (systemd) deployment path this meant two independent processes both binding the same 5 GPIO pins and the same I2C OLED address, and both reacting to switch presses -- a single footswitch press could be read as two rapid, independent HTTP calls.

**Fix:** `hardware_service.py` is no longer started from `main.cpp` at all. It now has its own dedicated systemd unit, `setup/oleander-hardware.service`, so it runs as exactly one instance and can be restarted independently of the audio/web server.

### 2. Pedals (and the footswitches) were addressed by chain position, not identity (`pedal.h`, `web/pedal_board.h`, `web/handlers.h`, `web/static/app.jsx`)

**Problem:** Every mutating endpoint (`/adjust_knob`, `/remove_pedal`, `/push_button`) and the 5 GPIO switches addressed a pedal by its position in the live chain. Adding or removing a pedal shifted every position after it, so a switch (or an in-flight browser request) could silently end up hitting a different pedal than the one it was pressed for a moment earlier.

**Fix:** `PedalInfo` now carries a stable `id`, assigned once when a pedal is added (or when a preset/snapshot is loaded) and unchanged for that pedal's lifetime. `PedalBoard::AdjustKnob/RemovePedal/Push` and the corresponding endpoints now look pedals up by `id`, not position; the React UI does the same (and uses `id` as the React `key` for correct list reconciliation, which it wasn't using at all before). This also made the "which switch does what" question moot for the footswitches specifically, since they no longer address individual pedals at all -- see Presets below.

### 3. `PedalBoard::Describe()` was a stub (`web/pedal_board.h`)

**Problem:** Returned an empty `PedalInfo` unconditionally (`// TODO: this.`).

**Fix:** Implemented: reports the board's own pedal count as its "state" (e.g. `"3 pedal(s)"`).

### 4. No persistence -- a restart lost the entire chain (`web/main.cpp`, `web/preset_store.h`)

**Problem:** Nothing ever wrote the pedal chain to disk. A power cycle came back to an empty board.

**Fix:** New `PresetStore` (`web/preset_store.h`) persists to `presets.json`: the 5 named preset slots, plus a "current" snapshot of the live board that's updated (and written to disk) after every mutation. `main.cpp` loads the "current" snapshot back into the board at startup, so a restart resumes with whatever was last dialed in rather than an empty board.

### 5. `hardware_service.py` polled instead of subscribing (`hardware/hardware_service.py`)

**Problem:** The server already broadcasts a WebSocket "ping" to every client on every change, but `hardware_service.py`'s comment claimed "we can't listen to WebSocket from Python directly" and instead polled `/active_pedals` on a 150 ms timer -- adding up to 150 ms of pure latency between a footswitch press and the OLED reflecting it, on top of the request round-trip.

**Fix:** `hardware_service.py` now joins the same `/updates` WebSocket the browser uses (via the `websocket-client` package) and refreshes immediately on each push, with a slow (2 s) polling fallback only for while the socket is disconnected, so the display doesn't go stale indefinitely if the connection drops.

### 6. No debounce against duplicate preset-recall requests (`web/handlers.h`)

**Problem:** Nothing protected against the same logical action firing twice in a short window (e.g. bug #1 above, or a switch's mechanical contact bouncing past the 2 ms electrical debounce already handled in `gpiozero`).

**Fix:** `LoadPresetHandler` ignores a repeat request for the same preset slot arriving within 150 ms of the last one.

### 7. Switch-press failures were silently swallowed (`hardware/hardware_service.py`)

**Problem:** `on_switch_pressed` fired an HTTP request and ignored the result entirely; the old polling loop swallowed every exception with a bare `pass`. A dropped request during a momentary hiccup meant the performer thought they'd changed presets and hadn't, with no indication anything went wrong.

**Fix:** Switch-triggered requests now retry once on failure and log a clear warning if both attempts fail.

### 8. Web UI depended entirely on public CDNs (`web/static/index.html`)

**Problem:** jQuery, Bootstrap, Tether, React, ReactDOM, and Babel-standalone all loaded from public CDNs at runtime. Oleander is designed to run headless on the local network via mDNS (`oleander.local`), which commonly has no internet uplink at all (e.g. a venue or rehearsal space's Wi-Fi) -- in which case every one of those requests fails and the control page never renders.

**Fix:** All of the above are now vendored locally into `web/static/` (see `setup/vendor_assets.sh`, run automatically by `setup/install.sh`), so the control page loads with zero internet access. `index.html` now references the local copies instead of CDN URLs.

### 9. No WebSocket reconnect logic in the browser (`web/static/app.jsx`)

**Problem:** The socket was opened once in `PedalBoard.componentDidMount` with no `onclose`/reconnect handling. A Pi reboot or a Wi-Fi blip left the page silently frozen on stale state until someone manually refreshed the tab.

**Fix:** The WebSocket connection now lives in `App` (shared by both the preset bar and the pedal board) and reconnects with exponential backoff (capped at 15s) on disconnect, re-syncing immediately on reconnect.

### 10. Dark-theme contrast bug (`web/static/style.css`)

**Problem:** The page uses Bootswatch's dark "darkly" theme, but `.app-title` and `.splash-title` hardcoded `#212529` (near-black, meant for light backgrounds), making them nearly unreadable against the dark page background outside of the white pedal cards.

**Fix:** Changed both to a light color (`#f8f9fa`) consistent with the rest of the dark theme.

### 11. Presets, end to end

New feature, addressing the goal of quickly switching between 5 saved configurations while playing:

- **Backend:** `web/preset_store.h` (new) persists 5 named presets to `presets.json`. `PedalBoard::LoadSnapshot()` (new, in `web/pedal_board.h`) rebuilds the chain from a saved preset via the existing `PedalRegistry` factories. New endpoints: `GET /presets` (list), `GET /preset/<n>` (recall, debounced), `POST /preset/<n>/save[?name=]` (snapshot current board into slot n), `POST /preset/<n>/name?name=` (rename).
- **Hardware:** the 5 footswitches now call `GET /preset/<n>` instead of `/push_button/<n>` -- pressing one recalls a whole saved chain rather than toggling a single pedal. The OLED shows a large "PRESET n / name" screen whenever a preset is active, falling back to the previous per-pedal knob-cycling view when the board has been freely edited outside of any preset, and the idle splash screen when it's empty.
- **Web UI:** a new preset bar (`PresetBar`/`PresetTile` in `app.jsx`) with 5 tiles -- tap to recall (same endpoint the footswitches use), a "Save here" button to snapshot the current board into that slot (with an optional rename prompt), and the active slot highlighted in green, kept live via the same WebSocket the pedal board already uses.

## Round 3 - Build Fixes & Portability

1. **`audio_transformer.h` wouldn't compile** -- the vendored RtAudio (5.1.0) declares `openStream()` as returning `void` and signaling failure by throwing `RtAudioError` (a `std::runtime_error` subclass), but the code captured its return value as `int rc` and checked `rc != 0`, which doesn't compile against a `void`-returning function. This was a pre-existing bug, only surfaced once actually built against the real vendored header. Fixed by dropping the return-value capture/check and letting the thrown exception propagate, which is both what compiles and what the constructor's callers already handle.

2. **`LoadPresetHandler` broke Crow's route registration** -- introduced in Round 2's debounce logic: the handler held a `std::mutex` directly as a member, which made the whole class non-movable/non-copyable, and Crow moves handler objects into an internal dispatch closure when you register a route (`CROW_ROUTE(...)(handler)`), so this failed to compile (`use of deleted function 'LoadPresetHandler::LoadPresetHandler(LoadPresetHandler&&)'`). Fixed by moving the debounce state (mutex, last index, last load time) into a small heap-allocated struct held via `shared_ptr`, so the handler stays movable/copyable while every copy still shares the same debounce state.

3. **`setup/oleander.service` / `setup/oleander-hardware.service` hardcoded `User=pi` and `/home/pi/oleander-effects`** -- Raspberry Pi OS has not guaranteed a `pi` user since the Bullseye release (Raspberry Pi Imager, and raspi-config's own account creation, both prompt for a custom username), so a plain `User=pi` unit fails to start on any system that used a different name, or worse, silently runs as an unrelated existing `pi` account. Fixed by turning both unit files into templates (`__OLEANDER_USER__` / `__OLEANDER_DIR__`), with `install.sh` resolving the real invoking user (via `$SUDO_USER`, since the script itself must run under `sudo`) and the actual checkout path, then `sed`-substituting them in before copying to `/etc/systemd/system/`. Also added a defensive `usermod -aG gpio,i2c,spi,dialout,audio,video` on the resolved user, since that group membership -- not the username -- is what actually grants GPIO/I2C/audio access without root.

4. **`oleander-hardware.service`'s `Environment=PATH=...%{env:PATH}` was not valid systemd syntax** -- found while fixing #3 above. `%{env:...}` isn't a systemd unit-file specifier (specifiers are single `%`-letters, e.g. `%h`/`%u`); it would have been passed through as a literal string rather than expanding to the caller's actual `PATH`. Removed both `Environment=` lines entirely: `ExecStart` already invokes the venv's `python3` by absolute path, and CPython resolves its own `site-packages` from the interpreter's location (via the venv's `pyvenv.cfg`) independent of `PATH`/`VIRTUAL_ENV`, so neither was actually needed.

5. **`ld: cannot find -lrtaudio`** -- the `Makefile` linked `-lrtaudio` as if a prebuilt RtAudio library was installed on the system, but nothing ever built or installed one: RtAudio is vendored as a git submodule (`rtaudio/`) containing only source (`RtAudio.cpp`/`RtAudio.h`), never compiled into a library, and Raspberry Pi OS doesn't ship a `librtaudio` package to link against instead. Fixed by compiling `rtaudio/RtAudio.cpp` directly alongside `web/main.cpp` (and `record.cpp`) rather than linking an external library, with `-D__LINUX_ALSA__` defined (RtAudio's standard Linux backend, selected the same way its own `configure.ac` does for `--with-alsa`) and `-lasound` linked (provided by `libasound2-dev`, already in `setup/install.sh`'s apt list). This also sidesteps any future API mismatch between a distro-provided RtAudio and the specific 5.1.0 vendored here (see #1 above) -- the vendored source is always what actually gets built.

## Round 4 - First-Boot Runtime Crashes

The build now succeeds, but two separate crash-loops showed up the first time both services actually ran on real hardware (via a journalctl capture): `oleander.service` crashing with `std::bad_alloc` on every startup, and `oleander-hardware.service` failing every switch-pin setup with a deep `gpiozero`/`OSError` traceback. Both were first-boot-only problems -- neither had ever been exercised end to end before.

### 1. `oleander.service` crash-looped with `std::bad_alloc` on first boot (`web/main.cpp`)

**Problem:** `SelectDevices()` only had two paths: read `devices.txt` (works after the first successful run), or fall back to an interactive `std::cin >>` prompt asking the user to pick an input/output device by number. That prompt was written assuming someone is at a real terminal. Under systemd (`Type=simple`, no TTY attached), stdin is closed, so the first `std::cin >> selected_input_device` hits EOF immediately and (per the standard, since the stream was still good going in) writes `0`. The *second* `std::cin >> selected_output_device` then runs on a stream whose failbit is already set from the first failure -- and per the standard, when the stream is already failed, extraction returns without touching the destination at all, leaving `selected_output_device` **uninitialized stack garbage** rather than `0`. `all_devices[selected_output_device]` then indexes the vector with that garbage value: undefined behavior, which in practice manifested as copying a garbage `std::string` (bogus length) a moment later, triggering a `std::bad_alloc` and aborting (`SIGABRT`) every single time the service started -- an unrecoverable crash-loop, since `devices.txt` never got written to break the cycle.

**Fix:** `SelectDevices()` now checks `isatty(STDIN_FILENO)` before falling into the interactive prompt. With no terminal attached, it instead calls a new `AutoSelectDevices()`: picks the first input-capable device (in practice almost always the one real USB interface, since the Pi's own onboard audio has no input channels at all), and for output prefers *that same device* if it also does output -- the normal one-interface-does-both pedal setup -- rather than whichever output-capable device happens to enumerate first, which on a Pi is often its own onboard `bcm2835` headphone jack. The auto-selected choice is written to `devices.txt` immediately, so this only ever runs once; every later restart goes straight through the existing `ReadSelectionsFromFile` path. If no input device is found at all (e.g. the USB interface isn't plugged in yet), it now fails loudly with a clear message and exits cleanly instead of hitting undefined behavior. The original interactive prompt is unchanged and still runs when a real terminal *is* attached (e.g. `./bin/server debug` over SSH).

### 2. `oleander-hardware.service` failed every switch setup with a `gpiozero` `OSError` (`setup/install.sh`, `setup/oleander-hardware.service`)

**Problem:** The journalctl capture showed `gpiozero` falling back through its default pin-factory chain -- `lgpio` (not installed) -> `RPi.GPIO` (not installed) -> `pigpio` (not installed) -> its legacy `NativeFactory`, which talks to the old `/sys/class/gpio` sysfs interface. Current Raspberry Pi OS kernels (Bookworm and newer) dropped that sysfs interface in favor of the `gpiochip` character-device API, so `NativeFactory` failed immediately on the very first switch: `FileNotFoundError: /sys/class/gpio/gpio21/value`, then `OSError: [Errno 22] Invalid argument` trying to export it, crashing the whole service before any of the 5 switches were even set up.

**Fix:** Added `lgpio` to the pip packages `setup/install.sh` installs into the venv (`setup/oleander-hardware.service`'s `usermod -aG gpio,...` from Round 3 already grants the access `/dev/gpiochip0` needs). Also pinned `Environment=GPIOZERO_PIN_FACTORY=lgpio` in `oleander-hardware.service`, so instead of gpiozero silently falling through several backends and eventually landing on the broken legacy one, a missing or broken `lgpio` install now fails immediately with an unambiguous "can't load pin factory" error rather than the confusing multi-layer traceback seen here.

    `pip install lgpio` turned out to be the wrong approach entirely, in two stages. First it failed needing build tooling that wasn't installed: the PyPI package ships source only, and building it runs SWIG (`lgpio.i` -> `lgpio_wrap.c`) then compiles a C extension against Python.h -- `swig` and `python3-dev` were added to fix that (`error: command 'swig' failed: No such file or directory`). With those installed, the build got further but then failed at the link step: `/usr/bin/ld: cannot find -llgpio` -- the PyPI `lgpio` package's SWIG wrapper links against a *system* `liblgpio` it expects to already be installed; it doesn't bundle or build the C library itself, only Python bindings for one that already exists.

    Rather than also hunting down and installing the matching `liblgpio`/`liblgpio-dev` packages by hand, `setup/install.sh` now installs `python3-lgpio` via apt instead of `pip install lgpio` -- Raspberry Pi OS's own repo ships this prebuilt and version-matched to the running kernel/distro, C library included, no compilation needed. Since that lands in the system Python's site-packages rather than the venv, the venv is now created with `python3 -m venv --system-site-packages`, which makes system packages visible as a fallback without shadowing anything actually pip-installed inside the venv. `swig` was dropped from the apt list again since nothing builds `lgpio` from source anymore; `python3-dev` stayed, since other pip packages in the same install line (e.g. `spidev`) still compile small C extensions of their own. The venv is also now `rm -rf`'d and recreated from scratch on every `install.sh` run, rather than reused, so a stale venv from an earlier failed attempt (as happened here, twice) can't leave behind a broken or half-installed environment.

## Round 5 - OLED Failure Was Taking Down the Switches

A fresh journalctl capture (after the Round 4 fixes) showed real progress: `oleander.service` now fails cleanly with "Fatal: No audio input device found" instead of crashing (expected -- no USB audio interface was plugged in yet), and `oleander-hardware.service` successfully initialized all 5 GPIO switches via `lgpio` (`[SWITCH] Initialized GPIO 21 -> preset 0` through `GPIO 6 -> preset 4`). But the service then crashed a moment later anyway.

### 1. A failed OLED/I2C init took the already-working switches down with it (`hardware/hardware_service.py`)

**Problem:** `main()` constructed `OleanderDisplay()` unconditionally, right after `init_switches()`. `OleanderDisplay.__init__` opens the I2C bus immediately (`i2c(port=I2C_BUS, address=I2C_ADDRESS)`), which raises `luma.core.error.DeviceNotFoundError: I2C device not found: /dev/i2c-1` if I2C isn't enabled yet -- `/boot/config.txt`'s `dtparam=i2c_arm=on` (added by `setup/install.sh`) only takes effect after a reboot, and this device apparently hadn't been rebooted since the first `install.sh` run. That exception was never caught, so it propagated straight out of `main()` and killed the entire `oleander-hardware.service` process -- including the 5 footswitches, which had already initialized successfully just before and have nothing to do with the display. The one feature this project is actually for (recall a preset instantly while playing) was taken out by an unrelated, non-essential status readout.

**Fix:** Added a `NullDisplay` class (a no-op stand-in implementing the same `draw_splash()`/`update()`/`start()`/`stop()` interface as `OleanderDisplay`) and wrapped the `OleanderDisplay()` construction in `main()` in a `try/except`: on failure it prints a clear warning explaining the likely cause (I2C not enabled yet -- needs `sudo raspi-config` or editing `/boot/firmware/config.txt` plus a reboot -- or the OLED not wired to GPIO 2/3) and falls back to `display = NullDisplay()` instead of letting the exception escape `main()`. The switches, `ServerSync`, and the rest of the service now keep running normally with no visible display, and will pick up a real display automatically on the next service restart once I2C/wiring is actually fixed -- no code change or manual intervention needed at that point, since `oleander-hardware.service`'s `Restart=on-failure` will no longer even be triggered by this particular failure.

## Round 6 - Web Server Couldn't Bind Port 80 As A Non-Root User

A fresh journalctl capture showed real progress on the audio side -- RtAudio successfully enumerated devices (the harmless-looking `snd_pcm_open`/`Unknown error 524` and `capture slave is not defined` lines are just probe failures on devices RtAudio checks and discards while enumerating, not the real interface) and logged `[MAIN] Audio engine started. Web server on port 80`. But `oleander.service` crashed immediately afterward anyway, every restart, `status=6/ABRT`.

### 1. `bind: Permission denied` starting the web server (`setup/oleander.service`)

**Problem:** `web/main.cpp` binds the web server to port 80 in production (`app.port(in_debug_mode ? 8080 : 80).run()`), but `setup/oleander.service` runs the process as the unprivileged `__OLEANDER_USER__` (templated in Round 3, replacing the old, effectively-root-ish default). Linux reserves ports below 1024 for root by default, so the very first `bind()` call on the listening socket failed with `EACCES`. Boost.Asio surfaces that as a thrown `boost::system::system_error`, which was never caught, so it propagated out and `std::terminate()` aborted the whole process (`SIGABRT`) -- a crash-loop, since nothing about a restart changes the outcome. This was invisible before Round 3's user-templating fix, because whatever ran the service previously had enough privilege (root, or a `pi` account with some other implicit grant) to bind low ports without needing to ask for the capability explicitly; moving to a real unprivileged user surfaced the gap.

**Fix:** Added `AmbientCapabilities=CAP_NET_BIND_SERVICE` and `CapabilityBoundingSet=CAP_NET_BIND_SERVICE` to `oleander.service`. This grants the process exactly the one Linux capability needed to bind privileged ports, without running as root or making `bin/server` setuid (which would need re-applying, e.g. via `setcap`, after every rebuild -- the systemd-level grant survives rebuilds automatically, since `install.sh` reinstalls the unit file, not the capability, on every run). systemd handles preserving the ambient capability across the `User=` switch itself, so no other unit changes were needed.

## Round 7 - 404 On Every Page (Static Files Served From The Wrong Directory)

With Round 6's capability fix, `oleander.service` finally stayed up and bound port 80 successfully -- but visiting `http://oleander.local` returned `404 Not Found` for the page itself and every asset.

### 1. Static files served relative to the wrong working directory (`web/main.cpp`)

**Problem:** The `/` route and the catch-all `/<string>` route (which serves every static asset -- `index.html`, `app.jsx`, `style.css`, and the vendored `vendor-*.js`/`.css` files) both constructed `StaticFileHandler(/* directory = */ "static")`. `StaticFileHandler` (`web/handlers.h`) opens files with a plain relative `std::ifstream(directory + "/" + filename)`, resolved against the *process's* current working directory -- not the source file's location, not the binary's location. `setup/oleander.service` sets `WorkingDirectory=__OLEANDER_DIR__`, the repository root (where `Makefile`, `web/`, `hardware/`, `setup/` live), which has no top-level `static/` directory at all -- the real one is `web/static/`. So every request, starting with `/` itself, opened a file at `<repo root>/static/...` that never existed, and `StaticFileHandler` correctly (if unhelpfully) returned `404` every time. The server was otherwise completely healthy: process up, port bound, routes registered -- it just couldn't find any of its own files. (This bug predates Round 6's fix and was simply unreachable before it, since the service never got past starting up long enough to serve a request.)

**Fix:** Changed both `StaticFileHandler` directory arguments in `web/main.cpp` from `"static"` to `"web/static"`, matching the actual on-disk layout relative to the repo-root `WorkingDirectory` systemd launches the process with. `devices.txt` and `presets.json` (the only other runtime-relative paths in the C++ server) are unaffected by this -- they're written fresh wherever the process happens to run and don't depend on any specific existing directory, so changing the static-file path in place (rather than changing `WorkingDirectory` itself) was the narrower, lower-risk fix.

## Round 8 - No Feedback When The OLED Isn't Working (Switch Latch State On The Web UI)

With Rounds 6-7 fixed, the web server is up and reachable, but the OLED still isn't confirmed working (I2C enablement/wiring is a separate hardware/config step -- see codefix.md Round 5 #1 and HARDWARE.md's troubleshooting section) and there was no other way to tell whether a footswitch press was actually being registered by the hardware service and reaching the server at all.

### 1. Add a live on/off indicator per footswitch to the web UI

**Problem:** The only place a footswitch's physical state was ever visible was the OLED -- which may not be working yet for unrelated hardware/config reasons -- or indirectly, by noticing the loaded preset change. There was no direct, always-available confirmation that a press was even detected and reported to the server, making it hard to tell "is this switch not working" apart from "the OLED just isn't up yet" or "that preset slot happens to look identical to what's already loaded."

**Fix:** Added an in-memory `SwitchStates` store (`web/handlers.h`) tracking the live latch state of all 5 physical footswitches, with two new endpoints: `GET /switches` (list all 5 states) and `POST /switch/<n>/state?pressed=<0|1>` (report a state change; broadcasts a WebSocket ping like the other mutating endpoints). `hardware/hardware_service.py` now calls the latter on every press, every release, and once at startup for each switch's actual current position (previously the startup state was hardcoded to "off" regardless of the switch's real position) -- separately from, and in addition to, the `/preset/<n>` call a press already makes to recall the saved chain. The web UI (`app.jsx`) fetches `/switches` alongside `/presets` on every refresh and renders a small dot on each preset tile (dim grey = latched off, lit amber = latched on) reflecting that switch's live position, independent of whether its preset is the one currently active. This is deliberately decoupled from preset recall: a switch's physical position and "which preset is loaded" are related but distinct, and the indicator should keep tracking the switch either way.

## Round 9 - Audio Interface Drops Left The Service Silently Deaf, With No Way To Fix It

`journalctl -u oleander` showed a burst of `RtApiAlsa::callbackEvent: audio read error, No such device.` lines -- the USB audio interface had disappeared mid-stream -- and the user asked where to set the audio device by hand, since the docs mentioned this was possible somewhere but didn't actually show where.

### 1. A dropped audio device left the process running with a permanently dead audio path (`audio_transformer.h`)

**Problem:** `AudioTransformer`'s constructor calls `RtAudio::openStream()` without its optional 9th argument, an `RtAudioErrorCallback` -- so it defaults to `NULL`. Reading `rtaudio/RtAudio.cpp`'s ALSA backend directly: on a read error, `RtApiAlsa::callbackEvent` logs a `WARNING` (which is what showed up in the journal) and jumps straight back into the same callback -- it never stops the stream, never retries opening the device, and gives the rest of the process no signal that anything is wrong beyond that one stderr line. With no error callback registered and nothing in `main()` ever polling stream health after `at.Start()`, the practical effect was: once the USB interface actually disappeared, every single audio callback failed forever, silently, while the web server and everything else kept running completely normally -- so `systemctl status oleander` showed "active (running)" the whole time, with no audio actually flowing and no way to tell short of reading the journal.

**Fix:** Registered `internal::OnStreamError` as the stream's error callback. It tracks consecutive stream errors (any gap over 500ms starts a new streak) and, once 20 have arrived back-to-back -- which happens almost instantly once the device is truly gone, since the audio callback fires far faster than that -- calls `std::exit(1)`. `setup/oleander.service`'s existing `Restart=on-failure` then restarts the process, which runs `SelectDevices()` fresh: if the interface has reappeared under the same ALSA-reported name (the common case for a brief unplug or power cycle), it picks it back up automatically; otherwise it keeps retrying every 5 seconds until it does. A single isolated warning (an occasional buffer under/overrun, which is normal) does not trigger this, since the streak resets after any 500ms gap between errors.

### 2. There was no discoverable, non-interactive way to see or set the audio device (`audio_transformer.h`, `web/main.cpp`, `HARDWARE.md`)

**Problem:** `devices.txt` (a plain 2-line text file: exact input device name, then exact output device name, matched verbatim against whatever RtAudio currently enumerates) is genuinely hand-editable, but nothing told the user that. `HARDWARE.md` only said "Select the device by name on first run (saved to devices.txt)" -- true, but it doesn't explain the file is editable by hand, what format it needs, or how to find the exact device name string in the first place. The only way to actually see a list of device names was the full interactive `std::cin` picker in `SelectDevices()`, which requires (a) SSH/terminal access, (b) deleting or renaming a working `devices.txt` first, since it only runs when one doesn't already exist, and (c) `isatty(STDIN_FILENO)` being true, i.e. running the binary by hand rather than via systemd. There was also a `AudioTransformer::DumpDeviceInfo()` method already written to print full device info non-interactively -- but it was never called from anywhere in the codebase.

**Fix:** Added a `list-devices` argv subcommand, checked first thing in `main()`: `./bin/server list-devices` calls `DumpDeviceInfo()` (rewritten to print clearer input/output channel counts and a one-line reminder of the `devices.txt` format) and exits immediately -- no audio stream opened, no server started, no prompt, safe to run any time including while the real service is already running. Also rewrote `HARDWARE.md`'s "USB Audio Interface" and "Audio Not Working" sections to actually document the hand-editable `devices.txt` format, how to get device names via `list-devices`, how to force auto-reselection (delete `devices.txt` and restart), and how Round 9 #1's new crash-and-retry behavior shows up in the journal.

## Round 10 - Hardware Service's WebSocket Connection Never Stayed Up Long Enough To Deliver An Update

The user reported the audio-side errors from Round 9 were gone and both services were staying up, but pressing a footswitch still didn't visibly do anything, the OLED still showed nothing, and now also: clicking a pedal's ON/OFF button in the web UI didn't visibly arm it either (it stayed darkened out). `journalctl -u oleander-hardware` was full of a repeating pair of lines: `[SYNC] WebSocket connected` immediately followed by `[SYNC] WebSocket disconnected (Connection timed out); retrying in 1.0s`, over and over.

### 1. `hardware_service.py`'s WebSocket client dropped its own connection every few seconds (`hardware/hardware_service.py`)

**Problem:** `ServerSync._run_websocket()` opens the `/updates` socket with `websocket.create_connection(WEBSOCKET_URL, timeout=5)`, then loops on a blocking `ws.recv()` that's supposed to wait for the next `"ping"` broadcast from the server -- which only fires when something actually changes (a pedal pushed, a preset saved/loaded, a switch state reported), so in the idle case it could legitimately be anywhere from milliseconds to minutes before the next one arrives. The `timeout=5` argument looks like it only bounds the initial connect, but the `websocket-client` library applies it via a plain `sock.settimeout(5)` on the underlying socket, which stays in effect for every operation on that socket afterward -- including `recv()`. So five seconds into any idle period, `recv()` raised a timeout, the `except` block logged it as a disconnect and tore the whole connection down, and the loop reconnected a second later just to repeat the same cycle indefinitely. In practice this meant the hardware service was almost never actually listening for updates -- it spent most of its time either mid-reconnect or about to time out again -- so a `"ping"` broadcast sent while the socket happened to be down was simply missed, and the OLED (and the switch-latch state it reports back) could sit stale for an essentially unbounded amount of time depending on how the ~6-second churn cycle happened to line up with when a real update was sent. (The 2-second fallback poll softened this somewhat but only runs while `self.connected` is clear, and was fighting a connection that reported itself "connected" for a few seconds at a time before flipping back.)

This is a client-side bug specific to `hardware_service.py`'s use of the low-level `websocket-client` API -- it does not affect the browser's own `/updates` connection (a native browser `WebSocket`, which has no equivalent recv-timeout footgun), so it does not by itself explain the web UI's ON/OFF button not visibly arming. That symptom is still open; see Remaining Issues below.

**Fix:** Added `ws.sock.settimeout(None)` immediately after `create_connection()` succeeds, so the 5-second timeout only ever applies to the initial connect/handshake, and the subsequent `recv()` loop goes back to blocking indefinitely as the code already intended (the comment above it already said "blocks until the server sends 'ping'" -- the timeout parameter just silently broke that promise). No other behavior changes: a genuinely dead connection (server restarted, network actually down) still raises on `recv()` or `send()` in the normal way and gets picked up by the existing reconnect loop.

## Round 11 - OLED Showed Nothing, Even With I2C Enabled And Wiring Confirmed Correct

The user had double- and triple-checked wiring, and tried a second physical OLED module, with no display output at all. `i2cdetect -y 1` confirmed the display was actually present on the bus at `0x3c`, ruling out wiring or a bad module as the cause.

### 1. `OleanderDisplay`'s draw methods referenced a `luma.core` attribute that doesn't exist in the installed version (`hardware/hardware_service.py`)

**Problem:** `draw_splash()`, `draw_preset()`, and `draw_pedal_info()` all called `draw.rectangle(self.device.bbox, ...)`. The installed `luma.core` version's `mixin.capabilities.capabilities()` only ever sets `self.bounding_box` -- confirmed directly from `luma/core/mixin.py` -- `bbox` isn't an alias, a deprecated name, or present at all in this version. Every one of those three draw calls raised `AttributeError: 'ssd1306' object has no attribute 'bbox'`, and since `draw_splash()` is called unwrapped from `main()`, that exception propagated out and crashed `oleander-hardware.service` on every startup, before the "Display update thread started" log line ever printed -- confirmed from the traceback in `journalctl -u oleander-hardware`.

**Fix:** Changed all three `self.device.bbox` references to `self.device.bounding_box`, matching both the installed library's actual attribute name and `luma.oled`'s own bundled usage examples.

## Round 12 - Added A Granular Texture Effect ("Sky Chive"), Ported From Mutable Instruments' Clouds

Vendored the DSP core of Mutable Instruments' open-source Clouds Eurorack module firmware (`pichenettes/eurorack`, MIT license) as a new pedal, rather than writing a from-scratch granular effect -- the Pi's compute headroom made a faithful port practical where it wouldn't have been on the original module's STM32F4. Only `clouds/dsp/` (the hardware-independent DSP core) and its `stmlib` dependency are used; `eurorack`'s STM32-specific drivers/UI are not part of the build. Named "Sky Chive" rather than "Clouds" per Mutable Instruments' own stated preference that derivative works not use their names; full MIT attribution is in `docs/THIRD_PARTY.md`. See `docs/SOFTWARE.md` Component 11 for the full technical writeup.

### 1. `/add_pedal/<string>` silently failed for any pedal name containing a space (`web/handlers.h`, `web/static/app.jsx`)

**Problem:** Sky Chive is the first pedal name in the project with a space in it. `AvailablePedal.add()` built the URL as `"/add_pedal/" + this.props.name` with no encoding, and even where the browser did percent-encode the space, Crow's `<string>` route parameter is never URL-decoded by the router (unlike a query-string value, e.g. `/preset/<n>/save?name=`, which Crow does decode) -- so the server was looking up the literal string `"Sky%20Chive"` in the pedal registry instead of `"Sky Chive"`, missing every time and returning a 404 the client never checked. The pedal silently never got added; every other pedal name being a single word meant this had never been exercised before.

**Fix:** `AddPedalHandler` now decodes the incoming name with `crow::qs_decode` (the same decoder Crow already uses for query-string values) before the registry lookup; `AvailablePedal.add()` now wraps the name in `encodeURIComponent(...)`, matching the convention already used for preset names.

### 2. The granular engine could get permanently stuck outputting silence after a mode change -- but only on the Pi, never in testing (`pedals/clouds_pedal.h`)

**Problem:** `GranularProcessor::Process()` outputs silence whenever it detects `playback_mode_` doesn't match `previous_playback_mode_` (normal and brief by design -- it's how the engine masks a re-carve). The audio thread writes `playback_mode_` (via `set_playback_mode()`) and calls `Process()`; a background thread reads it and writes `previous_playback_mode_` back into sync inside `Prepare()`. Both are plain, non-atomic members of `GranularProcessor`, shared across two real OS threads with no synchronization between them -- reasoned at the time to be safe because upstream's own firmware runs `Process` and `Prepare` "concurrently" too, but that's on a single CPU core with interrupt-driven preemption, where a context switch is itself a full memory barrier for free. A real `std::thread` port across the Pi's actual multiple ARM cores has no such guarantee, and ARM's memory model is weak enough that a write on one core isn't guaranteed to become visible on another in bounded time. Every test during development ran on an x86_64 machine, whose much stronger memory model happened to paper over the missing synchronization every time -- so it looked entirely correct until run on the actual hardware it was written for, where changing the `mode` knob (or removing and re-adding the pedal, or toggling it enough times) could leave it stuck outputting silence indefinitely, immune to any further knob or enable/disable change.

**Fix:** Added a `std::mutex` guarding every access to the shared `GranularProcessor` instance from both threads, restoring the synchronization the single-core design got for free. Confirmed fixed via the user's own repro (repeated add/remove/enable/disable/mode-change cycling) on the real Pi -- the one thing that couldn't be checked from x86 alone, and shouldn't be assumed fixed from x86 testing again in the future for the same reason it wasn't caught the first time.

## Round 13 - Vintage-Synth Fader/LED Control Surface, Replacing The +/- Knob Stepper

Purely a UI/control-surface change -- no pedal's `AdjustKnob()` value-handling logic changed. Direction (a warm walnut-and-cream panel, vertical faders, no color fill on faders since real hardware sliders don't glow) was settled by mocking up candidate styles before writing any real code. See `docs/SOFTWARE.md` Component 12 for the full technical writeup.

`PedalKnob` (`pedal.h`) gained `min`/`max`/`labels` (every pedal's `Describe()` now declares real ranges -- previously only 5 pedals' `seconds`-style knobs and Sky Chive's knobs were clamped at all in `AdjustKnob()`, so most ranges were authored by inference from each knob's default and its use in the DSP math rather than read out of an existing clamp). One incidental correctness improvement fell out of that exercise: Echo's `decay_factor` is used as a divisor in `Transform()`; giving its fader a floor above zero closes off a divide-by-zero the old text box could already reach. `web/static/app.jsx`'s `Knob` component is replaced by `Fader` (drag-to-adjust) and `LedButtonGroup` (for booleans/enums); `ActivePedal`'s card is now a walnut control panel in place of the plain white Bootstrap card it was before.

## Remaining Issues (Not Fixed)

The following issues from the original review were noted but not addressed in this round of fixes:

1. **Audio thread race condition** -- The RtAudio callback runs on a separate thread and modifies pedal parameters concurrently with the main thread. Fixing this requires a double-buffering approach for pedal parameters, which is a significant architectural change.

2. **No authentication** -- The web server has no authentication. Anyone on the network can reconfigure the effects chain (or now, recall/overwrite a preset). This should be addressed before production deployment.

3. **No unit tests** -- The codebase has zero automated tests. Adding unit tests for pedal transforms, edge cases, the pedal board, and the new `PresetStore` (de)serialization would significantly improve confidence in the code.

4. **Hardcoded paths** -- ~~`setup/oleander.service`, `setup/oleander-hardware.service`~~ fixed in Round 3 (see below): those two now install as templates, with the real user and checkout path filled in by `install.sh`. `pedals/wav_looper.h`'s `../pedals/wav_files/` (a relative path that only resolves correctly if the server is launched from a specific working directory) is still outstanding.

5. **update.sh destructive reset** -- `git reset --hard HEAD` discards local changes unconditionally.

6. **Vendored web assets require a one-time, internet-connected step** -- `setup/vendor_assets.sh` downloads jQuery/React/Bootstrap/etc. into `web/static/` and needs real internet access to do so (normally satisfied by running it, or all of `install.sh`, on the Pi during initial setup, which already requires internet for `apt`). If you're upgrading an existing install rather than doing a fresh `install.sh` run, re-run `setup/vendor_assets.sh` manually once before relying on the web UI working offline.

7. **Web UI's per-pedal ON/OFF button not visibly updating after a click (open, under investigation)** -- reported alongside Round 10: clicking a pedal's ON/OFF button in the browser does not visibly arm it. The button's click handler (`ActivePedal.push()`, `web/static/app.jsx`) is fire-and-forget by design -- it calls `GET /push_button/<id>` and relies on the server's WebSocket `"ping"` broadcast to trigger a re-fetch of `/active_pedals`, the same mechanism the preset bar and OLED both depend on. `PushButtonHandler` (`web/handlers.h`) does correctly toggle the pedal and call `notifier_.NotifyBoardChanged()` (which broadcasts the ping) -- that part of the server is unmodified and was already verified correct. Round 10 #1 fixed a confirmed instance of this exact failure mode (a dropped/never-delivered update) on the hardware service's *own* WebSocket client specifically -- but that fix is Python-`websocket-client`-specific and does not touch the browser's native `WebSocket`, which doesn't share the same bug. Whether the browser is missing pings for a related reason (e.g. it was also timing out/reconnecting around the same window) or for an unrelated one needs a browser-side check (dev tools Network tab, WS frames, on the `/updates` connection) to confirm before making a further change here.
