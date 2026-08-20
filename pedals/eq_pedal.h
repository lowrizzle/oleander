#ifndef EQ_PEDAL_H
#define EQ_PEDAL_H

#include "q/support/base.hpp"
#include "q/support/frequency.hpp"
#include "q/support/literals.hpp"

#include "pedal.h"
#include "pedal_registry.h"
#include "q/fx/biquad.hpp"
#include "signal_type.h"

#include <algorithm>

// A 3-band parametric EQ: fixed-corner low and high shelves for overall
// bass/treble tilt, plus a fully parametric mid band (sweepable
// frequency, gain, and Q) for scooping/boosting a specific range -- the
// same 3-band-plus-sweepable-mid layout found on most guitar preamps and
// EQ pedals. See docs/ROADMAP.md item 3. Not a Mutable Instruments port;
// built from cycfi::q's existing shelf/peaking biquads (`q::lowshelf`,
// `q::peaking`, `q::highshelf`), applied in series -- the standard EQ
// topology (each band adjusts its own frequency range independently of
// the others' current gain).
class EqPedal : public Pedal {
 public:
  EqPedal() { Retune(); }

  SignalType Transform(SignalType signal) override {
    signal = low_shelf_(signal);
    signal = mid_peak_(signal);
    signal = high_shelf_(signal);
    return signal;
  }

  PedalInfo Describe() override {
    PedalInfo info;
    info.name = "EQ";
    info.knobs = {
        PedalKnob{.name = "bass_gain_db",
                  .value = bass_gain_db_,
                  .tweak_amount = 1,
                  .min = -15,
                  .max = 15},
        PedalKnob{.name = "mid_hz",
                  .value = mid_hz_,
                  .tweak_amount = 50,
                  .min = 200,
                  .max = 5000},
        PedalKnob{.name = "mid_gain_db",
                  .value = mid_gain_db_,
                  .tweak_amount = 1,
                  .min = -15,
                  .max = 15},
        PedalKnob{.name = "mid_q",
                  .value = mid_q_,
                  .tweak_amount = 0.1,
                  .min = 0.3,
                  .max = 3},
        PedalKnob{.name = "treble_gain_db",
                  .value = treble_gain_db_,
                  .tweak_amount = 1,
                  .min = -15,
                  .max = 15},
    };
    return info;
  }

  void AdjustKnob(const PedalKnob& knob) override {
    if (knob.name == "bass_gain_db") {
      bass_gain_db_ = static_cast<float>(std::max(-15.0, std::min(knob.value, 15.0)));
    } else if (knob.name == "mid_hz") {
      mid_hz_ = static_cast<float>(std::max(200.0, std::min(knob.value, 5000.0)));
    } else if (knob.name == "mid_gain_db") {
      mid_gain_db_ = static_cast<float>(std::max(-15.0, std::min(knob.value, 15.0)));
    } else if (knob.name == "mid_q") {
      mid_q_ = static_cast<float>(std::max(0.3, std::min(knob.value, 3.0)));
    } else if (knob.name == "treble_gain_db") {
      treble_gain_db_ =
          static_cast<float>(std::max(-15.0, std::min(knob.value, 15.0)));
    }
    Retune();
  }

 private:
  static constexpr uint32_t kSampleRate = 44100;
  static constexpr float kBassHz = 150.0f;
  static constexpr float kTrebleHz = 3000.0f;

  void Retune() {
    low_shelf_.config(bass_gain_db_, kBassHz, kSampleRate, 0.707);
    mid_peak_.config(mid_gain_db_, mid_hz_, kSampleRate, mid_q_);
    high_shelf_.config(treble_gain_db_, kTrebleHz, kSampleRate, 0.707);
  }

  float bass_gain_db_ = 0.0f;
  float mid_hz_ = 800.0f;
  float mid_gain_db_ = 0.0f;
  float mid_q_ = 0.8f;
  float treble_gain_db_ = 0.0f;

  cycfi::q::lowshelf low_shelf_{bass_gain_db_, kBassHz, kSampleRate, 0.707};
  cycfi::q::peaking mid_peak_{mid_gain_db_, mid_hz_, kSampleRate, mid_q_};
  cycfi::q::highshelf high_shelf_{treble_gain_db_, kTrebleHz, kSampleRate, 0.707};
};

REGISTER_PEDAL("EQ", []() { return std::unique_ptr<Pedal>(new EqPedal()); });

#endif /* EQ_PEDAL_H */
