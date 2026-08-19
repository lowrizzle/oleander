#ifndef LOW_PASS_GATE_PEDAL_H
#define LOW_PASS_GATE_PEDAL_H

#include "pedal.h"
#include "pedal_registry.h"
#include "signal_type.h"

#include "plaits/dsp/fx/low_pass_gate.h"

#include <algorithm>

// An SVF-based low-pass filter combined with a gain stage and
// high-frequency bleed (the dry signal mixed back in at full frequency,
// so at a low cutoff it stays articulate rather than fully muffled) --
// the algorithm behind envelope filters / auto-wah, and (with `gain`
// driven dynamically) the classic Buchla-style "low pass gate." Ported
// from Mutable Instruments' open-source Plaits firmware -- see
// docs/ROADMAP.md item 3 and docs/THIRD_PARTY.md.
//
// `gain` is a plain manual knob here rather than envelope-follower-driven
// off the input signal. Real LPG hardware normally drives gain that way
// -- that dynamic response (louder pluck -> briefly brighter and louder)
// is what makes it "gate"-like rather than just a filter -- but that's
// more than this first pass needs; cycfi::q::envelope_follower (already
// used in pedals/compressor_pedal.h) would be the way to add it later if
// the static-gain version doesn't feel right on hardware.
class LowPassGatePedal : public Pedal {
 public:
  LowPassGatePedal() { gate_.Init(); }

  SignalType Transform(SignalType signal) override {
    float sample = signal;
    gate_.Process(gain_, cutoff_hz_ / kSampleRate, hf_bleed_, &sample, 1);
    return sample;
  }

  PedalInfo Describe() override {
    PedalInfo info;
    info.name = "LP Gate";
    info.knobs = {
        PedalKnob{.name = "gain",
                  .value = gain_,
                  .tweak_amount = 0.1,
                  .min = 0,
                  .max = 1},
        PedalKnob{.name = "cutoff_hz",
                  .value = cutoff_hz_,
                  .tweak_amount = 100,
                  .min = 40,
                  .max = 8000},
        PedalKnob{.name = "hf_bleed",
                  .value = hf_bleed_,
                  .tweak_amount = 0.1,
                  .min = 0,
                  .max = 1},
    };
    return info;
  }

  void AdjustKnob(const PedalKnob& knob) override {
    if (knob.name == "gain") {
      gain_ = Clamp01(knob.value);
    } else if (knob.name == "cutoff_hz") {
      cutoff_hz_ =
          static_cast<float>(std::max(40.0, std::min(knob.value, 8000.0)));
    } else if (knob.name == "hf_bleed") {
      hf_bleed_ = Clamp01(knob.value);
    }
  }

 private:
  static constexpr float kSampleRate = 44100.0f;

  static float Clamp01(double v) {
    return static_cast<float>(std::max(0.0, std::min(v, 1.0)));
  }

  float gain_ = 0.8f;
  float cutoff_hz_ = 1200.0f;
  float hf_bleed_ = 0.1f;
  plaits::LowPassGate gate_;
};

REGISTER_PEDAL("LP Gate", []() {
  return std::unique_ptr<Pedal>(new LowPassGatePedal());
});

#endif /* LOW_PASS_GATE_PEDAL_H */
