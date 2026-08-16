#ifndef FLANGER_PEDAL_H
#define FLANGER_PEDAL_H

#include "pedal.h"
#include "pedal_registry.h"
#include "signal_type.h"

#include <algorithm>
#include <cmath>
#include <vector>

// A pedal which modulates the delay amount using an LFO.
class FlangerPedal : public Pedal {
public:
  FlangerPedal(double delay_seconds, double delay_blend)
      : delay_seconds_(std::max(0.001, std::min(delay_seconds, 10.0))),
        delay_blend_(delay_blend),
        delay_buffer_(static_cast<size_t>(delay_seconds_ * 44100), 0) {

    slope_ = (delay_end_seconds_ - delay_start_seconds_) / 2;
  }

  SignalType Transform(SignalType signal) override {
    delay_buffer_[delay_index_] = signal;
    delay_index_ = (delay_index_ + 1) % delay_buffer_.size();
    auto read_offset = MapPhaseToOffset() * 44100;
    if (delay_buffer_.empty()) {
      return signal;
    }
    size_t buffer_size = delay_buffer_.size();
    size_t clamped_offset = static_cast<size_t>(
        std::min(static_cast<double>(buffer_size), std::max(0.0, read_offset)));
    int delayed_read = static_cast<int>(
        (delay_index_ - static_cast<int>(clamped_offset) + buffer_size) % buffer_size);

    return signal + (delay_buffer_[delayed_read] * delay_blend_);
  }

  PedalInfo Describe() override {
    PedalInfo info;
    info.name = "Flanger";

    info.knobs = {
        PedalKnob{
            .name = "seconds", .value = delay_seconds_, .tweak_amount = 0.1},
        PedalKnob{
            .name = "delay_blend", .value = delay_blend_, .tweak_amount = 0.1},
        PedalKnob{.name = "rate", .value = rate_, .tweak_amount = 0.5},
        PedalKnob{.name = "delay_start_seconds",
                  .value = delay_start_seconds_,
                  .tweak_amount = 0.001},
        PedalKnob{.name = "delay_end_seconds",
                  .value = delay_end_seconds_,
                  .tweak_amount = 0.001},
    };

    return info;
  }

  // Map the phase to the amount of delay offset in seconds.
  double MapPhaseToOffset() {
    return delay_start_seconds_ + slope_ * (GetNextPhase() + 1);
  }

  // Advance through the LFO curve and return the Y value.
  SignalType GetNextPhase() {
    auto previous_phase = std::sin(2.0 * M_PI * phase_);
    phase_ += rate_ / 44100.0;
    if (phase_ >= 1)
      phase_ -= 1;
    return previous_phase;
  }

  void AdjustKnob(const PedalKnob& knob) override {
    if (knob.name == "seconds") {
      delay_seconds_ = std::max(0.001, std::min(knob.value, 10.0));
    } else if (knob.name == "delay_blend") {
      delay_blend_ = knob.value;
    } else if (knob.name == "rate") {
      rate_ = knob.value;
    } else if (knob.name == "delay_start_seconds") {
      delay_start_seconds_ = std::max(0.0001, std::min(knob.value, delay_end_seconds_));
    } else if (knob.name == "delay_end_seconds") {
      delay_end_seconds_ = std::max(
          delay_start_seconds_ + 0.0001, std::min(knob.value, 10.0));
    }

    // Reset the buffer always so that the new settings take effect right away.
    delay_index_ = 0;
    delay_buffer_ = std::vector<SignalType>(delay_seconds_ * 44100, 0);

    // Recompute the slope incase the delay range changed.
    slope_ = (delay_end_seconds_ - delay_start_seconds_) / 2;
  }

private:
  double delay_seconds_;
  double delay_blend_;
  int delay_index_ = 0;
  // Controls the speed at which we move through the LFO curve.
  double rate_ = 4.5;
  // Current x value in the LFO curve.
  SignalType phase_ = 0.0;
  std::vector<SignalType> delay_buffer_;
  // Range of the delay modulation in seconds.
  double delay_start_seconds_ = 0.001;
  double delay_end_seconds_ = 0.005;
  // Precomputed the slope for mapping between the LFO output to the delay
  // range.
  double slope_;
};

REGISTER_PEDAL("Flanger", []() {
  return std::unique_ptr<Pedal>(
      new FlangerPedal(/* delay_seconds =*/1, /* delay_blend= */ 1));
});

#endif /* FLANGER_PEDAL_H */
