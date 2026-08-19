#ifndef CHORUS_PEDAL_H
#define CHORUS_PEDAL_H

#include "pedal.h"
#include "pedal_registry.h"
#include "signal_type.h"

#include "plaits/dsp/fx/ensemble.h"

#include <algorithm>
#include <vector>

// A 3-tap chorus/ensemble effect with 2x3 LFOs (a slow ~0.75Hz set and a
// fast ~6.57Hz set, each with 120-degree phase offsets between the 3
// taps) -- the shimmering, widening effect behind things like the Roland
// Dimension D. Ported from Mutable Instruments' open-source Plaits
// firmware -- see docs/ROADMAP.md item 3 and docs/THIRD_PARTY.md.
//
// What this algorithm actually does: 3 continuously, independently
// modulated delay-line taps (each reading the input at a slightly
// different, constantly drifting delay time) are summed and blended with
// the dry signal. The drifting delay times Doppler-shift each tap's
// pitch up and down in a slow, irregular sweep, and because the 3 taps'
// LFOs are 120 degrees out of phase with each other, they drift apart
// and back together continuously rather than all moving in lockstep.
// That's the "shimmering ensemble of voices" character a chorus is going
// for -- but it's an evolving blend/wash, not 3 discretely distinguishable
// simultaneous pitches the way a polyphonic pitch-shifter/harmonizer
// would produce (a genuinely different, unrelated effect). Confirmed via
// a standalone numeric test (feeding a steady tone through this exact
// ensemble_.Process() call and comparing output 2 seconds apart) that the
// filtering is in fact continuously time-varying, not a static/frozen
// coloration -- at low `amount`, though, the dry signal dominates the mix
// heavily enough (see dry_amount below) that this modulation can be easy
// to miss. Defaults below favor `amount`/`depth` high enough to make it
// unmistakable.
//
// This pipeline is mono; Ensemble::Process() is stereo. Duplicates the
// input into both channels and averages the output back to mono, the
// same approach the Sky Chive pedal (pedals/clouds_pedal.h) uses for the
// same reason.
//
// Ensemble's LFO rates are hardcoded (as phase increments per Process()
// call) assuming roughly Plaits hardware's own 48kHz processing rate;
// running it at this pipeline's 44.1kHz without resampling (same
// simplification made in pedals/reverb_pedal.h, for the same class of
// upstream engine) shifts them ~8% slower than authentic. Negligible for
// a chorus effect.
class ChorusPedal : public Pedal {
 public:
  ChorusPedal(double amount, double depth)
      : amount_(Clamp01(amount)), depth_(Clamp01(depth)) {
    buffer_.resize(kBufferSize);
    ensemble_.Init(buffer_.data());
    // Init() doesn't clear the underlying FxEngine's write_ptr_ (only
    // Reset() does) -- without this it's whatever the freshly-allocated
    // vector's memory happened to contain, which every access still
    // masks into bounds, but is unnecessary undefined behavior to leave
    // in place when Reset() is right here for it.
    ensemble_.Reset();
    ensemble_.set_amount(amount_);
    ensemble_.set_depth(depth_);
  }

  SignalType Transform(SignalType signal) override {
    float left = signal;
    float right = signal;
    ensemble_.Process(&left, &right, 1);
    return (left + right) * 0.5f;
  }

  PedalInfo Describe() override {
    PedalInfo info;
    info.name = "Chorus";
    info.knobs = {
        PedalKnob{.name = "amount",
                  .value = amount_,
                  .tweak_amount = 0.1,
                  .min = 0,
                  .max = 1},
        PedalKnob{.name = "depth",
                  .value = depth_,
                  .tweak_amount = 0.1,
                  .min = 0,
                  .max = 1},
    };
    return info;
  }

  void AdjustKnob(const PedalKnob& knob) override {
    if (knob.name == "amount") {
      amount_ = Clamp01(knob.value);
      ensemble_.set_amount(amount_);
    } else if (knob.name == "depth") {
      depth_ = Clamp01(knob.value);
      ensemble_.set_depth(depth_);
    }
  }

 private:
  using Engine = plaits::Ensemble::E;
  static constexpr size_t kBufferSize = 1024;

  static float Clamp01(double v) {
    return static_cast<float>(std::max(0.0, std::min(v, 1.0)));
  }

  float amount_;
  float depth_;

  plaits::Ensemble ensemble_;
  std::vector<Engine::T> buffer_;
};

REGISTER_PEDAL("Chorus", []() {
  return std::unique_ptr<Pedal>(
      new ChorusPedal(/* amount= */ 0.9, /* depth= */ 1.0));
});

#endif /* CHORUS_PEDAL_H */
