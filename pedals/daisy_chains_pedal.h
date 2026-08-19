#ifndef DAISY_CHAINS_PEDAL_H
#define DAISY_CHAINS_PEDAL_H

#include "pedal.h"
#include "pedal_registry.h"
#include "signal_type.h"

#include "rings/dsp/resonator.h"

#include <algorithm>
#include <deque>
#include <vector>

// "Daisy Chains" -- a resonant filter/string-modeling effect: the input
// signal excites a bank of up to 64 resonant modes (an SVF per mode),
// giving it a bell-, string-, or plate-like tonal coloration depending on
// the structure/brightness/damping/position knobs. Ported from Mutable
// Instruments' open-source Rings Eurorack module firmware -- see
// docs/ROADMAP.md items 3/5 and docs/THIRD_PARTY.md. Per the same MI
// naming request Sky Chive already follows, this uses "Daisy Chains"
// rather than "Rings" anywhere user-facing.
//
// Wraps rings::Resonator specifically, not the larger rings::Part class.
// Part adds polyphony, discrete note/strum triggering, chord tables, and
// its own bundled reverb/limiter -- built for Rings' synth-voice mode
// (excited by discrete note events), not a fit for how every pedal here
// works (continuous audio in, continuous audio out). Resonator alone is
// exactly that: continuous audio in, continuously excited, continuous
// audio out.
//
// Unlike Sky Chive's GranularProcessor, Resonator has no Prepare()/
// Process() split and needs no background thread -- but it does need
// real block buffering, for a different reason: Resonator::Process()
// calls ComputeFilters() on every invocation, which recomputes all
// (up to 64) SVF coefficients from scratch. Calling that per-sample, the
// way the simpler FxEngine-based pedals (Reverb, Chorus, Overdrive) call
// their per-sample engines, would mean redoing that work 44100
// times/second instead of once per block -- a real CPU cost on a Pi 4.
// This pedal buffers kBlockSize samples (matching upstream's own
// rings::kMaxBlockSize -- about the same cadence the real hardware
// itself updates at) before calling Process() once per block, then
// drains the result. This adds a small (~0.5ms) fixed latency; both the
// excitation (dry) sample and the resulting wet sample for a given
// instant are queued together and popped together in Transform(), so
// dry/wet mixing stays time-aligned despite that latency (rather than
// blending "current dry" against "stale wet," which would smear the
// two out of phase with each other).
//
// Resonator::set_frequency() takes a self-contained normalized
// cycles-per-sample value -- no other part of the class has a baked-in
// sample-rate assumption the way e.g. Ensemble's LFO phase increments
// do -- so computing it as frequency_hz / 44100.0f (this pipeline's real
// rate) rather than reusing the library's own kSampleRate (48000)
// constant gives correct absolute pitch with no resampling or
// compensation needed.
class DaisyChainsPedal : public Pedal {
 public:
  DaisyChainsPedal() {
    resonator_.Init();
    block_in_.resize(kBlockSize);
    block_out_.resize(kBlockSize);
    block_aux_.resize(kBlockSize);
    ApplyParameters();
  }

  SignalType Transform(SignalType signal) override {
    block_in_[block_fill_] = signal;
    dry_queue_.push_back(signal);
    block_fill_++;
    if (block_fill_ == kBlockSize) {
      ProcessBlock();
    }

    if (wet_queue_.empty()) {
      // Only during the first block's worth of startup latency.
      return 0.0f;
    }
    SignalType dry = dry_queue_.front();
    SignalType wet = wet_queue_.front();
    dry_queue_.pop_front();
    wet_queue_.pop_front();
    return dry + (wet - dry) * mix_;
  }

  PedalInfo Describe() override {
    PedalInfo info;
    info.name = "Daisy Chains";
    info.knobs = {
        PedalKnob{.name = "frequency_hz",
                  .value = frequency_hz_,
                  .tweak_amount = 20,
                  .min = 40,
                  .max = 2000},
        PedalKnob{.name = "structure",
                  .value = structure_,
                  .tweak_amount = 0.1,
                  .min = 0,
                  .max = 1},
        PedalKnob{.name = "brightness",
                  .value = brightness_,
                  .tweak_amount = 0.1,
                  .min = 0,
                  .max = 1},
        PedalKnob{.name = "damping",
                  .value = damping_,
                  .tweak_amount = 0.1,
                  .min = 0,
                  .max = 1},
        PedalKnob{.name = "position",
                  .value = position_,
                  .tweak_amount = 0.1,
                  .min = 0,
                  .max = 1},
        PedalKnob{.name = "mix",
                  .value = mix_,
                  .tweak_amount = 0.1,
                  .min = 0,
                  .max = 1},
    };
    return info;
  }

  void AdjustKnob(const PedalKnob& knob) override {
    if (knob.name == "frequency_hz") {
      frequency_hz_ =
          static_cast<float>(std::max(40.0, std::min(knob.value, 2000.0)));
    } else if (knob.name == "structure") {
      structure_ = Clamp01(knob.value);
    } else if (knob.name == "brightness") {
      brightness_ = Clamp01(knob.value);
    } else if (knob.name == "damping") {
      damping_ = Clamp01(knob.value);
    } else if (knob.name == "position") {
      position_ = Clamp01(knob.value);
    } else if (knob.name == "mix") {
      mix_ = Clamp01(knob.value);
    }
    // Cheap (just setters, not ComputeFilters()) -- fine to reapply all 5
    // unconditionally rather than special-casing which knob changed.
    ApplyParameters();
  }

 private:
  // Matches rings::kMaxBlockSize (eurorack/rings/dsp/dsp.h) -- the same
  // block cadence the real hardware itself runs Resonator at.
  static constexpr size_t kBlockSize = 24;
  static constexpr float kSampleRate = 44100.0f;

  static float Clamp01(double v) {
    return static_cast<float>(std::max(0.0, std::min(v, 1.0)));
  }

  void ApplyParameters() {
    resonator_.set_frequency(frequency_hz_ / kSampleRate);
    resonator_.set_structure(structure_);
    resonator_.set_brightness(brightness_);
    resonator_.set_damping(damping_);
    resonator_.set_position(position_);
  }

  void ProcessBlock() {
    resonator_.Process(block_in_.data(), block_out_.data(),
                        block_aux_.data(), kBlockSize);
    for (size_t i = 0; i < kBlockSize; i++) {
      wet_queue_.push_back(block_out_[i] + block_aux_[i]);
    }
    block_fill_ = 0;
  }

  float frequency_hz_ = 220.0f;
  float structure_ = 0.25f;
  float brightness_ = 0.5f;
  float damping_ = 0.3f;
  float position_ = 0.999f;
  float mix_ = 0.5f;

  rings::Resonator resonator_;
  std::vector<float> block_in_;
  std::vector<float> block_out_;
  std::vector<float> block_aux_;
  size_t block_fill_ = 0;

  std::deque<SignalType> dry_queue_;
  std::deque<SignalType> wet_queue_;
};

REGISTER_PEDAL("Daisy Chains", []() {
  return std::unique_ptr<Pedal>(new DaisyChainsPedal());
});

#endif /* DAISY_CHAINS_PEDAL_H */
