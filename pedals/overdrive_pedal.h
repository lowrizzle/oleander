#ifndef OVERDRIVE_PEDAL_H
#define OVERDRIVE_PEDAL_H

#include "pedal.h"
#include "pedal_registry.h"
#include "signal_type.h"

#include "plaits/dsp/fx/overdrive.h"

#include <algorithm>

// Tube-style overdrive: pre-gain into a soft clipper, with auto makeup
// gain that normalizes output level as drive increases. Ported from
// Mutable Instruments' open-source Plaits firmware -- see
// docs/ROADMAP.md item 3 and docs/THIRD_PARTY.md. Added alongside Fuzz
// (hard clip + bandpass) rather than replacing it -- a milder,
// transparent-to-breakup dirt pedal is a different flavor from a fuzz,
// not a strict upgrade to one.
class OverdrivePedal : public Pedal {
 public:
  OverdrivePedal() { overdrive_.Init(); }

  SignalType Transform(SignalType signal) override {
    float sample = signal;
    overdrive_.Process(drive_, &sample, 1);
    return sample;
  }

  PedalInfo Describe() override {
    PedalInfo info;
    info.name = "Overdrive";
    info.knobs = {
        PedalKnob{.name = "drive",
                  .value = drive_,
                  .tweak_amount = 0.1,
                  .min = 0,
                  .max = 1},
    };
    return info;
  }

  void AdjustKnob(const PedalKnob& knob) override {
    if (knob.name == "drive") {
      drive_ = static_cast<float>(std::max(0.0, std::min(knob.value, 1.0)));
    }
  }

 private:
  float drive_ = 0.5f;
  plaits::Overdrive overdrive_;
};

REGISTER_PEDAL("Overdrive",
               []() { return std::unique_ptr<Pedal>(new OverdrivePedal()); });

#endif /* OVERDRIVE_PEDAL_H */
