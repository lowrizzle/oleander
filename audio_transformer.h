#ifndef AUDIO_TRANSFORMER_H
#define AUDIO_TRANSFORMER_H

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <stdexcept>

#include "rtaudio/RtAudio.h"
#include "signal_type.h"

namespace internal {

// Settings required for reading and writing to the stream via the RtAudio
// callback.
struct StreamSettings {
  SignalTransformFn transform_fn;
  int num_output_channels;
};

SignalType clip(SignalType input) {
  if (input > 1)
    return 1;
  if (input < -1)
    return -1;
  return input;
}

// Callback on each buffer recieved from the RTAudio library. Applies the
// transformation function to the signal and writes the result into the output
// buffer.
int callback(void* output_buffer, void* input_buffer,
             unsigned int buffered_frames, double /* stream_time */,
             RtAudioStreamStatus status, void* data) {
  if (status) {
    std::cerr << "Stream over/underflow detected. Status: " << status
              << std::endl;
  }

  auto* input = (SignalType*)input_buffer;
  auto* output = (SignalType*)output_buffer;
  auto* stream_settings = (StreamSettings*)data;
  const auto& transform_fn = stream_settings->transform_fn;

  for (unsigned int frame = 0; frame < buffered_frames; ++frame) {
    auto transformed_input = clip(transform_fn(*input));
    ++input;
    for (int channel = 0; channel < stream_settings->num_output_channels;
         ++channel) {
      *output = transformed_input;
      ++output;
    }
  }
  return 0;
}

// RtAudio's own handling of a stream-time error (e.g. the ALSA device
// disappearing mid-stream -- "audio read error, No such device") is to
// print a WARNING and keep calling the audio callback forever: it does
// not stop the stream, does not retry opening the device, and gives no
// way for the rest of this process to notice apart from watching stderr
// (confirmed by reading rtaudio/RtAudio.cpp's ALSA read-error path, which
// logs and `goto`s straight back into the same callback). A single benign
// warning (an occasional buffer under/overrun, say) is normal and
// shouldn't restart anything -- but if the same callback keeps erroring
// on every single invocation, the stream is not going to recover on its
// own, because the device is actually gone. So: track how many stream
// errors have arrived back-to-back (any gap over
// kErrorGapResetThreshold starts a fresh streak), and once a streak
// reaches kMaxConsecutiveErrors, treat it as unrecoverable and exit the
// whole process rather than let it spin forever with a silently-dead
// audio path. systemd's Restart=on-failure (setup/oleander.service) then
// restarts it, which runs SelectDevices() fresh -- picking the device
// back up automatically if it has reappeared under the same name (the
// common case for a USB interface that was briefly unplugged or
// power-cycled), or retrying every 5s until it does. See codefix.md
// Round 9 #1.
std::mutex stream_error_mu;
int consecutive_stream_errors = 0;
std::chrono::steady_clock::time_point last_stream_error_time;

void OnStreamError(RtAudioError::Type /* type */,
                    const std::string& error_text) {
  std::cerr << "[AUDIO] " << error_text << std::endl;

  std::lock_guard<std::mutex> lock(stream_error_mu);
  const auto now = std::chrono::steady_clock::now();
  static constexpr auto kErrorGapResetThreshold = std::chrono::milliseconds(500);
  if (consecutive_stream_errors == 0 ||
      now - last_stream_error_time > kErrorGapResetThreshold) {
    consecutive_stream_errors = 1;
  } else {
    consecutive_stream_errors++;
  }
  last_stream_error_time = now;

  static constexpr int kMaxConsecutiveErrors = 20;
  if (consecutive_stream_errors >= kMaxConsecutiveErrors) {
    std::cerr << "[AUDIO] Fatal: " << consecutive_stream_errors
              << " audio stream errors in rapid succession -- the audio "
                 "device is very likely gone (unplugged, power-cycled, or "
                 "lost its USB connection) and RtAudio will not reopen it "
                 "on its own. Exiting so systemd restarts the service and "
                 "re-selects a device; run `./bin/server list-devices` to "
                 "see what is currently detected, or see HARDWARE.md's "
                 "\"Audio Not Working\" section if this keeps happening."
              << std::endl;
    std::exit(1);
  }
}

} // namespace internal

class AudioTransformer {
public:
  // Open an audio stream that reads signal from `input_device_index`,
  // transforms it using `transform_fn` and writes the result to
  // `output_device_index`.
  AudioTransformer(SignalTransformFn transform_fn, int input_device_index,
                   int output_device_index) {
    unsigned int device_count = audio_interface_.getDeviceCount();
    if (static_cast<unsigned int>(input_device_index) >= device_count ||
        static_cast<unsigned int>(output_device_index) >= device_count) {
      throw std::out_of_range("Audio device index out of range");
    }

    const auto input_device =
        audio_interface_.getDeviceInfo(input_device_index);
    const auto output_device =
        audio_interface_.getDeviceInfo(output_device_index);
    if (!input_device.probed || !output_device.probed) {
      throw std::runtime_error("Audio device could not be probed");
    }
    const auto sample_rate = std::min(input_device.preferredSampleRate,
                                      output_device.preferredSampleRate);

    stream_settings_.transform_fn = std::move(transform_fn);
    stream_settings_.num_output_channels = output_device.outputChannels;

    input_stream_parameters_.deviceId = input_device_index;
    input_stream_parameters_.nChannels = 1;

    output_stream_parameters_.deviceId = output_device_index;
    output_stream_parameters_.nChannels = output_device.outputChannels;

    audio_interface_.showWarnings(true);

    frames_to_buffer_ = 32;

    RtAudio::StreamOptions opts;
    // NOTE: RtAudio::openStream() returns void in this vendored version
    // (rtaudio/RtAudio.h, RTAUDIO_VERSION 5.1.0) and signals failure by
    // throwing RtAudioError (a std::runtime_error subclass) instead of
    // returning an error code. An earlier version of this constructor
    // captured the return value as `int rc` and checked `rc != 0`, which
    // doesn't compile against this API (assigning a void expression to
    // int) -- letting the exception propagate is both what actually
    // compiles and preserves the original intent: callers of this
    // constructor already handle failure to open the audio device as a
    // thrown std::runtime_error.
    audio_interface_.openStream(
        &output_stream_parameters_, &input_stream_parameters_, RTAUDIO_FLOAT32,
        sample_rate, &frames_to_buffer_, &internal::callback, &stream_settings_,
        &opts, &internal::OnStreamError);
    stream_options_ = opts;
  }

  void Start() { audio_interface_.startStream(); }
  void Abort() { audio_interface_.abortStream(); }

  // Called from `./bin/server list-devices` (see main.cpp) -- a
  // non-interactive way to see exactly what RtAudio detects, and to find
  // the exact device name string devices.txt needs, without going
  // through the full interactive std::cin picker (which requires
  // deleting/renaming devices.txt first and only runs when one isn't
  // already present -- see codefix.md Round 9 #2 and HARDWARE.md's
  // "Audio Not Working" section).
  static void DumpDeviceInfo() {
    RtAudio audio_interface;
    unsigned int devices = audio_interface.getDeviceCount();
    if (devices == 0) {
      std::cout << "No audio devices found." << std::endl;
      return;
    }
    std::cout << "Detected audio devices -- to force a specific one, put "
                 "its exact name below on line 1 (input) and/or line 2 "
                 "(output) of devices.txt in the repo root, then restart "
                 "the service:\n" << std::endl;
    for (unsigned int i = 0; i < devices; i++) {
      auto info = audio_interface.getDeviceInfo(i);
      if (!info.probed) {
        continue;
      }
      std::cout << "id " << i << ": " << info.name << std::endl;
      std::cout << "  input channels:  " << info.inputChannels << std::endl;
      std::cout << "  output channels: " << info.outputChannels << std::endl;
      std::cout << "  preferred sample rate: " << info.preferredSampleRate
                << std::endl;
      std::cout << std::endl;
    }
  }

private:
  RtAudio audio_interface_;
  RtAudio::StreamParameters input_stream_parameters_;
  RtAudio::StreamParameters output_stream_parameters_;
  RtAudio::StreamOptions stream_options_;
  internal::StreamSettings stream_settings_;
  // Zero means let RtAudio pick the smallest allowed buffer size. Smaller
  // buffers means lower latency as the library waits less for frames.
  unsigned int frames_to_buffer_ = 0;
};

#endif /* AUDIO_TRANSFORMER_H */
