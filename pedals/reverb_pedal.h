#ifndef REVERB_PEDAL_H
#define REVERB_PEDAL_H

#include "pedal.h"
#include "pedal_registry.h"
#include "signal_type.h"

#include "clouds/dsp/frame.h"
#include "clouds/dsp/fx/reverb.h"

#include <algorithm>
#include <vector>

// Reverb, using the Dattorro/Griesinger topology (4 allpass diffusers on
// the input, then a feedback loop of 2x(2 allpass + 1 delay)) from
// Mutable Instruments' open-source Clouds firmware. Replaces the previous
// engine here (4 parallel delay lines + 2 series allpasses, no real
// diffusion network) -- see docs/ROADMAP.md item 3 and
// docs/THIRD_PARTY.md. Kept registered under the same "Reverb" name so
// existing presets/chain-library entries referencing it still resolve;
// only the DSP inside changed.
//
// clouds::Reverb::Process() operates on FloatFrame (stereo) blocks of any
// size, including 1 -- unlike the granular engine behind Sky Chive, it
// isn't fixed to a specific internal sample rate via lookup-table
// indices, so no block buffering or resampling is required here. It is,
// however, tuned (LFO rates in Init(), and the delay-line lengths chosen
// for a ~32kHz reference) assuming roughly Clouds hardware's own 32kHz
// processing rate. Running it at this pipeline's 44.1kHz without
// resampling (consistent with every other pedal in this codebase, none
// of which thread the real device rate down to construction) makes the
// shimmer LFOs run somewhat faster and shortens the effective decay
// versus authentic Clouds hardware -- a cosmetic coloration, not a
// functional defect.
class ReverbPedal : public Pedal {
 public:
  ReverbPedal(double amount, double time)
      : amount_(Clamp01(amount)), time_(Clamp01(time)) {
    buffer_.resize(kBufferSize);
    reverb_.Init(buffer_.data());
    reverb_.set_amount(amount_);
    reverb_.set_input_gain(input_gain_);
    reverb_.set_time(time_);
    reverb_.set_diffusion(diffusion_);
    reverb_.set_lp(damping_);
  }

  SignalType Transform(SignalType signal) override {
    clouds::FloatFrame frame{signal, signal};
    reverb_.Process(&frame, 1);
    return (frame.l + frame.r) * 0.5f;
  }

  PedalInfo Describe() override {
    PedalInfo info;
    info.name = "Reverb";

    info.knobs = {
        PedalKnob{.name = "amount",
                  .value = amount_,
                  .tweak_amount = 0.1,
                  .min = 0,
                  .max = 1},
        PedalKnob{.name = "time",
                  .value = time_,
                  .tweak_amount = 0.05,
                  .min = 0,
                  .max = 0.95},
        PedalKnob{.name = "diffusion",
                  .value = diffusion_,
                  .tweak_amount = 0.1,
                  .min = 0,
                  .max = 1},
        PedalKnob{.name = "damping",
                  .value = damping_,
                  .tweak_amount = 0.1,
                  .min = 0,
                  .max = 1},
    };

    return info;
  }

  void AdjustKnob(const PedalKnob& knob) override {
    if (knob.name == "amount") {
      amount_ = Clamp01(knob.value);
      reverb_.set_amount(amount_);
    } else if (knob.name == "time") {
      // Kept below 1.0 -- clouds::Reverb's feedback loop is unstable
      // (unbounded growth) at time >= 1.
      time_ = static_cast<float>(std::max(0.0, std::min(knob.value, 0.95)));
      reverb_.set_time(time_);
    } else if (knob.name == "diffusion") {
      diffusion_ = Clamp01(knob.value);
      reverb_.set_diffusion(diffusion_);
    } else if (knob.name == "damping") {
      damping_ = Clamp01(knob.value);
      reverb_.set_lp(damping_);
    }
  }

 private:
  static constexpr size_t kBufferSize = 16384;

  static float Clamp01(double v) {
    return static_cast<float>(std::max(0.0, std::min(v, 1.0)));
  }

  float amount_;
  float time_;
  float diffusion_ = 0.625f;
  float damping_ = 0.7f;
  const float input_gain_ = 0.5f;

  clouds::Reverb reverb_;
  std::vector<uint16_t> buffer_;
};

REGISTER_PEDAL("Reverb", []() {
  return std::unique_ptr<Pedal>(
      new ReverbPedal(/* amount= */ 0.5, /* time= */ 0.6));
});

#endif /* REVERB_PEDAL_H */
