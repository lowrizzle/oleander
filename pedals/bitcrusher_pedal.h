#ifndef BITCRUSHER_PEDAL_H
#define BITCRUSHER_PEDAL_H

#include "pedal.h"
#include "pedal_registry.h"
#include "signal_type.h"

#include "plaits/dsp/fx/sample_rate_reducer.h"

#include <algorithm>

// Alias-free downsampling (BLEP-based, not a naive sample-and-hold) --
// a genuine bitcrusher/digital degradation effect. Ported from Mutable
// Instruments' open-source Plaits firmware -- see docs/ROADMAP.md item 3
// and docs/THIRD_PARTY.md.
//
// Uses the non-optimized Process<false>() path: the optimized/fast path
// requires `size` to be a multiple of 4 and glitches under frequency
// modulation, per the class's own comment -- this pedal calls it
// per-sample (size=1), same as every other simple pedal here.
class BitcrusherPedal : public Pedal {
 public:
  BitcrusherPedal() { reducer_.Init(); }

  SignalType Transform(SignalType signal) override {
    float sample = signal;
    reducer_.Process<false>(rate_, &sample, 1);
    return sample;
  }

  PedalInfo Describe() override {
    PedalInfo info;
    info.name = "Bitcrusher";
    info.knobs = {
        // 1.0 = original rate (no crushing), lower = more aggressive
        // downsampling. Matches plaits::SampleRateReducer's own
        // normalized-frequency units directly.
        PedalKnob{.name = "rate",
                  .value = rate_,
                  .tweak_amount = 0.05,
                  .min = 0.01,
                  .max = 1},
    };
    return info;
  }

  void AdjustKnob(const PedalKnob& knob) override {
    if (knob.name == "rate") {
      rate_ = static_cast<float>(std::max(0.01, std::min(knob.value, 1.0)));
    }
  }

 private:
  float rate_ = 0.3f;
  plaits::SampleRateReducer reducer_;
};

REGISTER_PEDAL("Bitcrusher", []() {
  return std::unique_ptr<Pedal>(new BitcrusherPedal());
});

#endif /* BITCRUSHER_PEDAL_H */
