#ifndef NOISE_GATE_PEDAL_H
#define NOISE_GATE_PEDAL_H

#include "q/support/base.hpp"
#include "q/support/frequency.hpp"
#include "q/support/literals.hpp"

#include "pedal.h"
#include "pedal_registry.h"
#include "q/fx/envelope.hpp"
#include "signal_type.h"

#include <algorithm>
#include <cmath>

// Mutes the signal when its level drops below a threshold -- the inverse
// of pedals/compressor_pedal.h, and reusing the same
// cycfi::q::envelope_follower it uses to track input level. See
// docs/ROADMAP.md item 3. Not a Mutable Instruments port.
//
// The gate's own gain (not just the tracked envelope) is smoothed with a
// one-pole filter toward its 0/1 target rather than switched instantly:
// opening fast (a fixed, short attack) so playing isn't perceptibly
// delayed, closing over the `release_seconds` knob so silence between
// notes fades out rather than cutting off with an audible click.
class NoiseGatePedal : public Pedal {
 public:
  NoiseGatePedal() {
    UpdateReleaseCoefficient();
    attack_coefficient_ = 1.0f - std::exp(-1.0f / (kAttackSeconds * kSampleRate));
  }

  SignalType Transform(SignalType signal) override {
    float envelope = envelope_tracker_(std::abs(signal));
    float target_gain = envelope >= threshold_ ? 1.0f : 0.0f;
    float coefficient =
        target_gain > gain_ ? attack_coefficient_ : release_coefficient_;
    gain_ += (target_gain - gain_) * coefficient;
    return signal * gain_;
  }

  PedalInfo Describe() override {
    PedalInfo info;
    info.name = "Noise Gate";
    info.knobs = {
        PedalKnob{.name = "threshold",
                  .value = threshold_,
                  .tweak_amount = 0.01,
                  .min = 0,
                  .max = 0.5},
        PedalKnob{.name = "release_seconds",
                  .value = release_seconds_,
                  .tweak_amount = 0.05,
                  .min = 0.005,
                  .max = 1},
    };
    return info;
  }

  void AdjustKnob(const PedalKnob& knob) override {
    if (knob.name == "threshold") {
      threshold_ = static_cast<float>(std::max(0.0, std::min(knob.value, 0.5)));
    } else if (knob.name == "release_seconds") {
      release_seconds_ =
          static_cast<float>(std::max(0.005, std::min(knob.value, 1.0)));
      UpdateReleaseCoefficient();
    }
  }

 private:
  static constexpr float kSampleRate = 44100.0f;
  static constexpr float kAttackSeconds = 0.003f;

  void UpdateReleaseCoefficient() {
    release_coefficient_ =
        1.0f - std::exp(-1.0f / (release_seconds_ * kSampleRate));
  }

  float threshold_ = 0.02f;
  float release_seconds_ = 0.15f;
  float release_coefficient_;
  float attack_coefficient_;
  float gain_ = 1.0f;

  cycfi::q::envelope_follower envelope_tracker_{
      /* attack_seconds= */ 0.001, /* release_seconds= */ 0.05,
      /* sample_rate= */ 44100};
};

REGISTER_PEDAL("Noise Gate", []() {
  return std::unique_ptr<Pedal>(new NoiseGatePedal());
});

#endif /* NOISE_GATE_PEDAL_H */
