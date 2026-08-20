#ifndef TREMOLO_PEDAL_H
#define TREMOLO_PEDAL_H

#include "pedal.h"
#include "pedal_registry.h"
#include "signal_type.h"

#include <algorithm>
#include <cmath>

// Amplitude modulation by a sine LFO -- see docs/ROADMAP.md item 3. Not
// a Mutable Instruments port; this is a plain sine LFO, same phase-
// accumulator style already used for the carrier in
// pedals/ring_modulator_pedal.h and the sweep in pedals/flanger_pedal.h.
class TremoloPedal : public Pedal {
 public:
  SignalType Transform(SignalType signal) override {
    float lfo = std::sin(2.0f * static_cast<float>(M_PI) * phase_);
    phase_ += rate_hz_ / kSampleRate;
    if (phase_ >= 1.0f) {
      phase_ -= 1.0f;
    }
    // lfo in [-1, 1] -> gain in [1-depth, 1], so depth=0 is a no-op and
    // depth=1 modulates all the way down to silence at the LFO's trough.
    float gain = 1.0f - depth_ * 0.5f * (1.0f - lfo);
    return signal * gain;
  }

  PedalInfo Describe() override {
    PedalInfo info;
    info.name = "Tremolo";
    info.knobs = {
        PedalKnob{.name = "rate_hz",
                  .value = rate_hz_,
                  .tweak_amount = 0.5,
                  .min = 0.1,
                  .max = 20},
        PedalKnob{.name = "depth",
                  .value = depth_,
                  .tweak_amount = 0.1,
                  .min = 0,
                  .max = 1},
    };
    return info;
  }

  void AdjustKnob(const PedalKnob& knob) override {
    if (knob.name == "rate_hz") {
      rate_hz_ = static_cast<float>(std::max(0.1, std::min(knob.value, 20.0)));
    } else if (knob.name == "depth") {
      depth_ = static_cast<float>(std::max(0.0, std::min(knob.value, 1.0)));
    }
  }

 private:
  static constexpr float kSampleRate = 44100.0f;

  float rate_hz_ = 5.0f;
  float depth_ = 0.6f;
  float phase_ = 0.0f;
};

REGISTER_PEDAL("Tremolo",
               []() { return std::unique_ptr<Pedal>(new TremoloPedal()); });

#endif /* TREMOLO_PEDAL_H */
