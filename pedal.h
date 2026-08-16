#ifndef PEDAL_H
#define PEDAL_H

#include <string>
#include <vector>

#include "signal_type.h"

struct PedalKnob {
  std::string name;
  double value;
  double tweak_amount;
  // Valid range for this knob's value -- lets the web UI size/position a
  // fader. Most pedals don't enforce these bounds in AdjustKnob() (the
  // range here is declarative/UI-facing, not a new validation layer), a
  // handful (e.g. Sky Chive's Clamp01, the various *_pedal.h "seconds"
  // knobs) already do.
  double min = 0.0;
  double max = 1.0;
  // Non-empty => the UI renders this knob as a row of labels.size() LED
  // pushbuttons (one per integer position from min to max) instead of a
  // fader -- e.g. {"OFF", "ON"} for a boolean, or a short enum's option
  // names. Empty (the common case) => continuous fader.
  std::vector<std::string> labels;
};

struct PedalInfo {
  // Stable identifier for this pedal instance, assigned once by
  // PedalBoard::AddPedal()/LoadSnapshot() and unchanged for the pedal's
  // lifetime. Callers (the web UI, the physical footswitches, presets)
  // should always address a pedal by this id rather than by its position
  // in the chain -- chain position shifts every time a pedal is added or
  // removed, so an index captured a moment ago may no longer point at the
  // same pedal.
  int id = 0;
  std::string name;
  std::vector<PedalKnob> knobs;
  std::string state;
};

// The base pedal class which defines some transformation on the input signal.
//
// `signal` will be a floating point value in [-1, 1]. The result produced by
// the pedal should also conform to that range, otherwise the sound card may
// produce crackling noises.
class Pedal {
public:
  virtual SignalType Transform(SignalType signal) = 0;
  virtual void AdjustKnob(const PedalKnob& /* knob */) {}
  virtual PedalInfo Describe() = 0;
  virtual void Push() { enabled_ = !enabled_; }
  // Sets the enabled/disabled state directly, as opposed to Push() which
  // toggles it. Used when restoring a pedal to an exact saved state (e.g.
  // loading a preset), where toggling relative to whatever the freshly
  // constructed pedal's default happens to be would be error-prone.
  virtual void SetEnabled(bool enabled) { enabled_ = enabled; }
  bool Enabled() const { return enabled_; }
  virtual std::string State() const {
    return enabled_ ? "Enabled" : "Disabled";
  }
  virtual ~Pedal() = default;

private:
  bool enabled_ = true;
};

#endif /* PEDAL_H */
