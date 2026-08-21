#ifndef CLOUDS_PEDAL_H
#define CLOUDS_PEDAL_H

#include "pedal.h"
#include "pedal_registry.h"
#include "signal_type.h"

#include "clouds/dsp/granular_processor.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

// "Sky Chive" -- a granular texture effect. The DSP engine is a direct port
// of Mutable Instruments' open-source "Clouds" firmware
// (github.com/pichenettes/eurorack, MIT License, Copyright 2014 Emilie
// Gillet -- see docs/THIRD_PARTY.md for full attribution). Per that
// project's own stated licensing guidance, this derivative intentionally
// does not use the "Mutable Instruments" or "Clouds" names anywhere
// user-facing.
//
// clouds::GranularProcessor is block-based, stereo, 16-bit, and runs its
// DSP at a fixed internal 32 kHz (hardcoded in multiple places upstream --
// granular_processor.h's sample_rate() and, independently, LFO constants in
// dsp/fx/reverb.h -- so there's no single override point). This pedal
// resamples at its own boundary via LinearResampler rather than fight that,
// duplicates mono into both channels of the stereo engine, and averages the
// stereo output back down to mono to match this pipeline's SignalType.
//
// Faithfully ported quirk, not a bug here: at dry_wet=0 the output is NOT
// unity-gain dry passthrough. Upstream's own crossfade curve
// (resources.cc's lut_xfade_out) bottoms out at 0.7071 (-3dB, the standard
// endpoint for an equal-power crossfade) rather than 1.0, and its output
// stage (stmlib/dsp/dsp.h's SoftConvert) applies a further intentional
// 0.5x headroom scale -- so "fully dry" is really about -9dB, exactly
// matching real Clouds hardware. Confirmed empirically (measured gain
// ~0.35 at dry_wet=0, matching 0.7071*0.5) rather than assumed.
//
// Threading mirrors the original firmware's own concurrency model exactly:
// on real hardware, cv_scaler.Read(processor.mutable_parameters()) runs
// immediately before processor.Process() in the same audio ISR, while
// processor.Prepare() runs from a separate, unsynchronized main loop --
// Prepare() is documented/designed to only touch buffer regions Process()
// isn't concurrently reading, so the two are safe to run concurrently by
// construction, not by locking. Here, the audio thread (inside Transform())
// plays the "ISR" role -- it writes every Parameters field and calls
// Process() -- and a single dedicated background thread plays the "main
// loop" role, calling only Prepare(). Knob updates arrive on a third
// thread (the HTTP handler), so they land in plain std::atomic shadow
// variables and only get copied into the processor's real Parameters
// struct from the audio thread, once per block.
class SkyChivePedal : public Pedal {
 public:
  SkyChivePedal()
      : down_(kDeviceSampleRate / kCloudsSampleRate),
        up_(kCloudsSampleRate / kDeviceSampleRate),
        large_buffer_(kLargeBufferSize),
        small_buffer_(kSmallBufferSize) {
    processor_.Init(large_buffer_.data(), large_buffer_.size(),
                     small_buffer_.data(), small_buffer_.size());
    processor_.set_playback_mode(clouds::PLAYBACK_MODE_GRANULAR);
    processor_.set_quality(/*quality=*/1);  // mono, full fidelity

    clouds::Parameters* p = processor_.mutable_parameters();
    p->position = position_;
    p->size = size_;
    p->pitch = pitch_;
    p->density = density_;
    p->texture = texture_;
    p->dry_wet = dry_wet_;
    p->stereo_spread = spread_;
    p->feedback = feedback_;
    p->reverb = reverb_;
    p->freeze = freeze_;
    p->trigger = false;
    p->gate = false;

    processor_.Prepare();  // initial buffer carve, before the audio or
                            // background thread ever touches processor_

    running_ = true;
    prepare_thread_ = std::thread([this] { PrepareLoop(); });
  }

  ~SkyChivePedal() override {
    running_ = false;
    if (prepare_thread_.joinable()) {
      prepare_thread_.join();
    }
  }

  SignalType Transform(SignalType signal) override {
    down_.Push(static_cast<float>(signal), [this](float sample_32k) {
      short v = FloatToInt16(sample_32k);
      block_in_[block_fill_].l = v;
      block_in_[block_fill_].r = v;
      block_fill_++;
      if (block_fill_ == clouds::kMaxBlockSize) {
        ProcessBlock();
      }
    });

    if (output_queue_.empty()) {
      // Only expected during the ~1ms startup ramp before the first block
      // has been processed -- silence is the right fallback here, not a
      // dry pass-through, which would create an inconsistent half-wet
      // transient.
      return 0.0f;
    }
    SignalType out = output_queue_.front();
    output_queue_.pop_front();
    return out * NextFadeInGain();
  }

  PedalInfo Describe() override {
    PedalInfo info;
    info.name = "Sky Chive";
    info.knobs = {
        PedalKnob{.name = "dry_wet",
                  .value = dry_wet_.load(std::memory_order_relaxed),
                  .tweak_amount = 0.1,
                  .min = 0,
                  .max = 1},
        PedalKnob{.name = "freeze",
                  .value = freeze_.load(std::memory_order_relaxed) ? 1.0 : 0.0,
                  .tweak_amount = 1,
                  .min = 0,
                  .max = 1,
                  .labels = {"OFF", "ON"}},
        PedalKnob{.name = "mode",
                  .value = static_cast<double>(mode_.load(std::memory_order_relaxed)),
                  .tweak_amount = 1,
                  .min = 0,
                  .max = 3,
                  .labels = {"GRAN", "STR", "LOOP", "SPEC"}},
        PedalKnob{.name = "position",
                  .value = position_.load(std::memory_order_relaxed),
                  .tweak_amount = 0.1,
                  .min = 0,
                  .max = 1},
        PedalKnob{.name = "size",
                  .value = size_.load(std::memory_order_relaxed),
                  .tweak_amount = 0.1,
                  .min = 0,
                  .max = 1},
        PedalKnob{.name = "pitch",
                  .value = pitch_.load(std::memory_order_relaxed),
                  .tweak_amount = 1,
                  .min = -48,
                  .max = 48},
        PedalKnob{.name = "density",
                  .value = density_.load(std::memory_order_relaxed),
                  .tweak_amount = 0.1,
                  .min = 0,
                  .max = 1},
        PedalKnob{.name = "texture",
                  .value = texture_.load(std::memory_order_relaxed),
                  .tweak_amount = 0.1,
                  .min = 0,
                  .max = 1},
        PedalKnob{.name = "feedback",
                  .value = feedback_.load(std::memory_order_relaxed),
                  .tweak_amount = 0.1,
                  .min = 0,
                  .max = 1},
        PedalKnob{.name = "reverb",
                  .value = reverb_.load(std::memory_order_relaxed),
                  .tweak_amount = 0.1,
                  .min = 0,
                  .max = 1},
        PedalKnob{.name = "spread",
                  .value = spread_.load(std::memory_order_relaxed),
                  .tweak_amount = 0.1,
                  .min = 0,
                  .max = 1},
    };
    return info;
  }

  void AdjustKnob(const PedalKnob& knob) override {
    if (knob.name == "dry_wet") {
      dry_wet_ = Clamp01(knob.value);
    } else if (knob.name == "freeze") {
      freeze_ = knob.value != 0;
    } else if (knob.name == "mode") {
      mode_ = std::max(0, std::min(static_cast<int>(std::lround(knob.value)), 3));
    } else if (knob.name == "position") {
      position_ = Clamp01(knob.value);
    } else if (knob.name == "size") {
      size_ = Clamp01(knob.value);
    } else if (knob.name == "pitch") {
      pitch_ = static_cast<float>(std::max(-48.0, std::min(knob.value, 48.0)));
    } else if (knob.name == "density") {
      density_ = Clamp01(knob.value);
    } else if (knob.name == "texture") {
      texture_ = Clamp01(knob.value);
    } else if (knob.name == "feedback") {
      feedback_ = Clamp01(knob.value);
    } else if (knob.name == "reverb") {
      reverb_ = Clamp01(knob.value);
    } else if (knob.name == "spread") {
      spread_ = Clamp01(knob.value);
    }
  }

 private:
  // Upstream's own working-memory sizes (clouds/clouds.cc's block_mem /
  // block_ccm) -- ~180 KB total, trivial on a Pi 4.
  static constexpr size_t kLargeBufferSize = 118784;
  static constexpr size_t kSmallBufferSize = 65536 - 128;

  static constexpr double kCloudsSampleRate = 32000.0;
  // Matches every other buffered pedal's existing convention (DelayPedal,
  // EchoPedal, etc. all size their buffers off a hardcoded 44100 rather
  // than the actual negotiated device rate) -- nothing in this codebase
  // threads the real RtAudio stream rate down to pedal construction today.
  static constexpr double kDeviceSampleRate = 44100.0;

  // Every preset recall rebuilds the whole pedal chain from scratch
  // (PedalBoard::LoadSnapshot), so a newly-recalled Sky Chive is always a
  // brand new GranularProcessor whose grain scheduler, diffuser, reverb
  // tail, and feedback high-pass filter have never processed a single
  // real sample -- there's no equivalent "cold start" on real Clouds
  // hardware, which is a single always-on instance that just glides to
  // new parameter values in place. That cold engine, suddenly fed live
  // audio at whatever feedback/spread/density/reverb the new preset asks
  // for, can produce an audible transient/burst before it settles into
  // steady playback (reported on real hardware 2026-08-20, worse with
  // higher feedback and grain spread). Rather than chase the exact
  // internal cause across GranularSamplePlayer/Diffuser/Reverb, this
  // fades the pedal's own output in linearly over kFadeInSamples after
  // construction -- masks any cold-start transient regardless of which
  // internal stage produces it, at the cost of a brief (very likely
  // musically unnoticeable) fade-in on every preset that includes this
  // pedal.
  static constexpr size_t kFadeInSamples =
      static_cast<size_t>(kDeviceSampleRate * 0.25);  // 250 ms

  float NextFadeInGain() {
    if (fade_in_position_ >= kFadeInSamples) {
      return 1.0f;
    }
    float gain = static_cast<float>(fade_in_position_) / kFadeInSamples;
    fade_in_position_++;
    return gain;
  }

  static float Clamp01(double v) {
    return static_cast<float>(std::max(0.0, std::min(v, 1.0)));
  }

  static short FloatToInt16(float sample) {
    float clipped = std::max(-1.0f, std::min(sample, 1.0f));
    return static_cast<short>(clipped * 32767.0f);
  }

  // Generic linear-interpolation rate converter. Works in either direction
  // (upsampling or downsampling) depending on whether `step` (source rate /
  // destination rate) is above or below 1 -- fed one source-rate sample at
  // a time via Push(), which invokes `emit` zero or more times with
  // destination-rate samples as they become due. No existing resampler in
  // this codebase or in cycfi::q to reuse; this is intentionally minimal
  // (not a windowed-sinc/anti-aliased resampler) -- adequate for a
  // granular texture effect, not intended as a mastering-grade conversion.
  class LinearResampler {
   public:
    explicit LinearResampler(double step) : step_(step) {}

    template <typename Fn>
    void Push(float sample, Fn&& emit) {
      float prev = cur_;
      cur_ = sample;
      input_index_ += 1.0;
      while (next_pos_ <= input_index_) {
        float frac = static_cast<float>(next_pos_ - (input_index_ - 1.0));
        emit(prev + frac * (cur_ - prev));
        next_pos_ += step_;
      }
    }

   private:
    const double step_;
    double input_index_ = 0.0;
    double next_pos_ = 0.0;
    float cur_ = 0.0f;
  };

  // Runs a full 32-sample block through the granular engine: copies the
  // current knob values into processor_'s real Parameters (the one place
  // that struct is written, matching upstream's ISR-side
  // cv_scaler.Read()-then-Process() ordering), calls Process(), then feeds
  // the block's output through the up-sampler into output_queue_. Only
  // ever called from the audio thread (via Transform()).
  //
  // processor_mutex_ matters here even though upstream's own Process()/
  // Prepare() split is designed to run without one: that design assumes a
  // single CPU core with interrupt-driven preemption (the audio ISR
  // preempting the main loop), where a context switch is itself a full
  // memory barrier. A real std::thread port onto the Pi's actual multiple
  // ARM cores has no such guarantee -- ARM's memory model is weak enough
  // that a plain (non-atomic) write to GranularProcessor's internal state
  // (playback_mode_, previous_playback_mode_, reset_buffers_, Parameters)
  // on one core is not guaranteed to become visible to another core in
  // bounded time without explicit synchronization. Confirmed the hard way:
  // this looked completely fine in every test run in this dev environment
  // (x86_64, whose much stronger memory model happens to paper over the
  // missing synchronization) but left Process() permanently stuck in its
  // "previous_playback_mode_ != playback_mode_" silence branch on real Pi
  // hardware after a mode change, immune to further enable/disable or knob
  // changes -- exactly the "completely unresponsive" symptom this was
  // fixed in response to.
  void ProcessBlock() {
    std::lock_guard<std::mutex> lock(processor_mutex_);

    clouds::Parameters* p = processor_.mutable_parameters();
    p->position = position_.load(std::memory_order_relaxed);
    p->size = size_.load(std::memory_order_relaxed);
    p->pitch = pitch_.load(std::memory_order_relaxed);
    p->density = density_.load(std::memory_order_relaxed);
    p->texture = texture_.load(std::memory_order_relaxed);
    p->dry_wet = dry_wet_.load(std::memory_order_relaxed);
    p->stereo_spread = spread_.load(std::memory_order_relaxed);
    p->feedback = feedback_.load(std::memory_order_relaxed);
    p->reverb = reverb_.load(std::memory_order_relaxed);
    p->freeze = freeze_.load(std::memory_order_relaxed);
    p->trigger = false;
    p->gate = false;

    processor_.set_playback_mode(static_cast<clouds::PlaybackMode>(
        mode_.load(std::memory_order_relaxed)));

    processor_.Process(block_in_, block_out_, clouds::kMaxBlockSize);
    block_fill_ = 0;

    for (size_t i = 0; i < clouds::kMaxBlockSize; ++i) {
      float sample = (block_out_[i].l + block_out_[i].r) * (0.5f / 32768.0f);
      up_.Push(sample, [this](float device_rate_sample) {
        output_queue_.push_back(device_rate_sample);
      });
    }
  }

  // Background "main loop" analog -- see class comment. Only calls
  // Prepare(); never touches Parameters or calls Process()/set_playback_mode.
  void PrepareLoop() {
    while (running_.load(std::memory_order_relaxed)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(15));
      std::lock_guard<std::mutex> lock(processor_mutex_);
      processor_.Prepare();
    }
  }

  // Knob-controlled parameters, written by AdjustKnob() (HTTP handler
  // thread), read by ProcessBlock() (audio thread) once per block.
  std::atomic<float> position_{0.5f};
  std::atomic<float> size_{0.5f};
  std::atomic<float> pitch_{0.0f};
  std::atomic<float> density_{0.5f};
  std::atomic<float> texture_{0.5f};
  std::atomic<float> dry_wet_{0.5f};
  std::atomic<float> spread_{0.0f};
  std::atomic<float> feedback_{0.0f};
  std::atomic<float> reverb_{0.0f};
  std::atomic<bool> freeze_{false};
  std::atomic<int> mode_{0};  // clouds::PlaybackMode

  LinearResampler down_;  // device rate -> 32 kHz
  LinearResampler up_;    // 32 kHz -> device rate

  // Guards every access to processor_ from either the audio thread
  // (ProcessBlock) or the background thread (PrepareLoop) -- see
  // ProcessBlock's comment for why this is required on real multi-core
  // hardware even though upstream's own Process()/Prepare() split doesn't
  // use one.
  std::mutex processor_mutex_;
  clouds::GranularProcessor processor_;
  std::vector<uint8_t> large_buffer_;
  std::vector<uint8_t> small_buffer_;

  clouds::ShortFrame block_in_[clouds::kMaxBlockSize];
  clouds::ShortFrame block_out_[clouds::kMaxBlockSize];
  size_t block_fill_ = 0;

  std::deque<SignalType> output_queue_;
  size_t fade_in_position_ = 0;

  std::atomic<bool> running_{false};
  std::thread prepare_thread_;
};

REGISTER_PEDAL("Sky Chive",
               []() { return std::unique_ptr<Pedal>(new SkyChivePedal()); });

#endif /* CLOUDS_PEDAL_H */
