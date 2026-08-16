#ifndef BOOST_PEDAL_H
#define BOOST_PEDAL_H

#include "pedal.h"
#include "pedal_registry.h"
#include "signal_type.h"

// A pedal which "boosts" the input signal by a given factor.
// Output is clipped to [-1, 1] to prevent DAC clipping artifacts.
class BoostPedal : public Pedal {
public:
  SignalType Transform(SignalType signal) override {
    SignalType boosted = signal * boost_;
    if (boosted > 1) return 1;
    if (boosted < -1) return -1;
    return boosted;
  }

  PedalInfo Describe() override {
    PedalInfo info;
    info.name = "Boost";
    info.knobs = {
        PedalKnob{.name = "boost", .value = boost_, .tweak_amount = 0.1},
    };
    return info;
  }

  void AdjustKnob(const PedalKnob& pedal_knob) override {
    if (pedal_knob.name == "boost") {
      boost_ = pedal_knob.value;
    }
  }

private:
  double boost_ = 1.0;
};

REGISTER_PEDAL("Boost",
               []() { return std::unique_ptr<Pedal>(new BoostPedal()); });

#endif /* BOOST_PEDAL_H */
