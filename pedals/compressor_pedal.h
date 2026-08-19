#ifndef COMPRESSOR_PEDAL_H
#define COMPRESSOR_PEDAL_H

// dynamic.hpp needs these to be included.
#include "q/support/base.hpp"
#include "q/support/frequency.hpp"
#include "q/support/literals.hpp"

#include "pedal.h"
#include "pedal_registry.h"
#include "q/fx/dynamic.hpp"
#include "q/fx/envelope.hpp"
#include "signal_type.h"

#include <algorithm>
#include <cmath>

// Compresses the input signal. Uses cycfi::q's soft_knee_compressor
// (rather than its plain hard-knee compressor) for a more gradual gain
// transition around the threshold -- see docs/ROADMAP.md item 3.
//
// Applies auto makeup gain: without it, compression is easy to mistake
// for "not doing anything" -- it only ever reduces gain, so all it does
// on its own is make loud passages quieter, which doesn't read as an
// effect the way an EQ or modulation change does, especially through a
// small speaker. The makeup gain compensates by measuring how much the
// compressor would attenuate a full-scale (0dBFS) signal at the current
// threshold/ratio, and boosting the output by that much (clamped to a
// sane maximum) so a hot input comes back out near unity instead of just
// quieter, while quieter passages -- attenuated less to begin with --
// come out louder than they went in. That relative loudening of quieter
// material against louder material is what actually reads as
// "compressed" by ear.
class CompressorPedal : public Pedal {
 public:
  CompressorPedal(double attack_seconds, double release_seconds)
      : attack_seconds_(attack_seconds),
        release_seconds_(release_seconds),
        envelope_tracker_(attack_seconds_, release_seconds_,
                          /* sample_rate= */ 44100) {
    UpdateMakeupGain();
  }

  SignalType Transform(SignalType signal) override {
    // Take the absolute value of the signal here because we only care about its
    // amplitude, not its sign, when performing compression.
    SignalType compression_gain =
        SignalType(compressor_(envelope_tracker_(std::abs(signal))));

    SignalType out = signal * compression_gain * makeup_gain_;
    return std::max<SignalType>(-1, std::min<SignalType>(out, 1));
  }

  PedalInfo Describe() override {
    PedalInfo info;
    info.name = "Compressor";

    info.knobs = {
        PedalKnob{.name = "attack",
                  .value = attack_seconds_,
                  .tweak_amount = 0.1,
                  .min = 0.001,
                  .max = 2},
        PedalKnob{.name = "release",
                  .value = release_seconds_,
                  .tweak_amount = 0.1,
                  .min = 0.001,
                  .max = 2},
        PedalKnob{.name = "threshold",
                  .value = threshold_,
                  .tweak_amount = 0.1,
                  .min = 0,
                  .max = 1},
        PedalKnob{.name = "ratio",
                  .value = ratio_,
                  .tweak_amount = 0.1,
                  .min = 0,
                  .max = 1},
        PedalKnob{.name = "knee_width",
                  .value = knee_width_db_,
                  .tweak_amount = 1,
                  .min = 0,
                  .max = 24},
    };
    return info;
  }

  void AdjustKnob(const PedalKnob& pedal_knob) override {
    if (pedal_knob.name == "attack") {
      attack_seconds_ = pedal_knob.value;
    } else if (pedal_knob.name == "release") {
      release_seconds_ = pedal_knob.value;
    } else if (pedal_knob.name == "threshold") {
      threshold_ = pedal_knob.value;
    } else if (pedal_knob.name == "ratio") {
      ratio_ = pedal_knob.value;
    } else if (pedal_knob.name == "knee_width") {
      knee_width_db_ = pedal_knob.value;
    }

    envelope_tracker_ = {attack_seconds_, release_seconds_,
                         /* sample_rate= */ 44100};
    compressor_ = {threshold_, cycfi::q::decibel(knee_width_db_), ratio_};
    UpdateMakeupGain();
  }

 private:
  void UpdateMakeupGain() {
    double reduction_db = -compressor_(cycfi::q::decibel(1.0)).val;
    double makeup_db = std::max(0.0, std::min(reduction_db, 12.0));
    makeup_gain_ = static_cast<float>(std::pow(10.0, makeup_db / 20.0));
  }

  double attack_seconds_;
  double release_seconds_;
  float threshold_ = 0.1;
  float ratio_ = 0.1;
  float knee_width_db_ = 6;
  float makeup_gain_ = 1.0f;

  cycfi::q::soft_knee_compressor compressor_{
      threshold_, cycfi::q::decibel(knee_width_db_), ratio_};
  cycfi::q::envelope_follower envelope_tracker_{attack_seconds_,
                                                release_seconds_,
                                                /* sample_rate= */ 44100};
};

REGISTER_PEDAL("Compressor", []() {
  return std::unique_ptr<Pedal>(new CompressorPedal(
      /* attack_seconds= */ 0.4, /* release_seconds= */ 0.4));
});

#endif /* COMPRESSOR_PEDAL_H */
