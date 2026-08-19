#ifndef PHASER_PEDAL_H
#define PHASER_PEDAL_H

#include "q/support/base.hpp"
#include "q/support/frequency.hpp"
#include "q/support/literals.hpp"

#include "pedal.h"
#include "pedal_registry.h"
#include "q/fx/biquad.hpp"
#include "signal_type.h"

#include <algorithm>
#include <array>
#include <cmath>

// A classic 4-stage phaser: the input runs through a cascade of allpass
// filters whose shared center frequency is swept by a sine LFO, mixed
// back with the dry signal -- the moving notches this creates in the
// combined signal are the "swoosh" a phaser is known for. A portion of
// the output is fed back into the input for a stronger, more resonant
// sweep. See docs/ROADMAP.md item 3. Not a Mutable Instruments port;
// built from cycfi::q's existing allpass biquad (`q::allpass`, already
// vendored and used e.g. in pedals/reverb_pedal.h's predecessor) and the
// same phase-accumulator LFO style used in pedals/tremolo_pedal.h and
// pedals/flanger_pedal.h.
class PhaserPedal : public Pedal {
 public:
  PhaserPedal() { RetuneStages(center_hz_); }

  SignalType Transform(SignalType signal) override {
    float lfo = std::sin(2.0f * static_cast<float>(M_PI) * phase_);
    phase_ += rate_hz_ / kSampleRate;
    if (phase_ >= 1.0f) {
      phase_ -= 1.0f;
    }
    float swept_hz = center_hz_ + lfo * depth_ * center_hz_ * 0.9f;
    RetuneStages(swept_hz);

    float wet = signal + feedback_sample_ * feedback_;
    for (auto& stage : stages_) {
      wet = stage(wet);
    }
    feedback_sample_ = wet;

    return signal + (wet - signal) * mix_;
  }

  PedalInfo Describe() override {
    PedalInfo info;
    info.name = "Phaser";
    info.knobs = {
        PedalKnob{.name = "rate_hz",
                  .value = rate_hz_,
                  .tweak_amount = 0.1,
                  .min = 0.05,
                  .max = 10},
        PedalKnob{.name = "depth",
                  .value = depth_,
                  .tweak_amount = 0.1,
                  .min = 0,
                  .max = 1},
        PedalKnob{.name = "feedback",
                  .value = feedback_,
                  .tweak_amount = 0.05,
                  .min = 0,
                  .max = 0.95},
        PedalKnob{.name = "mix",
                  .value = mix_,
                  .tweak_amount = 0.1,
                  .min = 0,
                  .max = 1},
    };
    return info;
  }

  void AdjustKnob(const PedalKnob& knob) override {
    if (knob.name == "rate_hz") {
      rate_hz_ = static_cast<float>(std::max(0.05, std::min(knob.value, 10.0)));
    } else if (knob.name == "depth") {
      depth_ = Clamp01(knob.value);
    } else if (knob.name == "feedback") {
      feedback_ = static_cast<float>(std::max(0.0, std::min(knob.value, 0.95)));
    } else if (knob.name == "mix") {
      mix_ = Clamp01(knob.value);
    }
  }

 private:
  static constexpr float kSampleRate = 44100.0f;
  static constexpr int kNumStages = 4;

  static float Clamp01(double v) {
    return static_cast<float>(std::max(0.0, std::min(v, 1.0)));
  }

  void RetuneStages(float hz) {
    float clamped_hz = std::max(20.0f, std::min(hz, 15000.0f));
    for (auto& stage : stages_) {
      stage.config(clamped_hz, static_cast<uint32_t>(kSampleRate), 0.7);
    }
  }

  float rate_hz_ = 0.4f;
  float depth_ = 0.7f;
  float feedback_ = 0.3f;
  float mix_ = 0.5f;
  float center_hz_ = 800.0f;
  float phase_ = 0.0f;
  float feedback_sample_ = 0.0f;

  std::array<cycfi::q::allpass, kNumStages> stages_{
      cycfi::q::allpass{center_hz_, static_cast<uint32_t>(kSampleRate), 0.7},
      cycfi::q::allpass{center_hz_, static_cast<uint32_t>(kSampleRate), 0.7},
      cycfi::q::allpass{center_hz_, static_cast<uint32_t>(kSampleRate), 0.7},
      cycfi::q::allpass{center_hz_, static_cast<uint32_t>(kSampleRate), 0.7}};
};

REGISTER_PEDAL("Phaser",
               []() { return std::unique_ptr<Pedal>(new PhaserPedal()); });

#endif /* PHASER_PEDAL_H */
