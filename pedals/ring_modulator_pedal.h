#ifndef RING_MODULATOR_PEDAL_H
#define RING_MODULATOR_PEDAL_H

#include "pedal.h"
#include "pedal_registry.h"
#include "signal_type.h"

#include "stmlib/dsp/dsp.h"

#include <algorithm>
#include <cmath>

// A classic ring modulator: the input signal multiplied against an
// internal oscillator (the "carrier"), the way single-audio-input
// hardware ring mod pedals normally work (e.g. Moogerfooger MF-102,
// Electro-Harmonix Ring Thing).
//
// The actual ring-modulation math -- a diode-based analog ring
// modulator approximation -- is hand-ported from Modulator::Diode() and
// Modulator::Xmod<ALGORITHM_ANALOG_RING_MODULATION> in Mutable
// Instruments' open-source Warps firmware
// (eurorack/warps/dsp/modulator.cc, MIT licensed -- see
// docs/THIRD_PARTY.md). Only that ~15-line formula is ported, not the
// surrounding Modulator class: that class expects two audio inputs
// (carrier + modulator channels), and internally wires up an oscillator,
// quadrature transform, 6x oversampling, and a full vocoder path this
// pedal has no use for. docs/ROADMAP.md item 4.6 originally described
// Warps' ring modulator as a low-effort, single-input port; reading
// modulator.cc (not just its header) during item 3's implementation
// found that description was wrong about the class as a whole, hence
// hand-porting just the formula instead.
//
// The carrier oscillator is a plain phase accumulator, the same minimal
// style already used elsewhere in this codebase (e.g. the
// LinearResampler in pedals/clouds_pedal.h) rather than pulling in any
// oscillator machinery from Warps or cycfi::q.
class RingModulatorPedal : public Pedal {
 public:
  SignalType Transform(SignalType signal) override {
    float carrier = std::sin(2.0f * static_cast<float>(M_PI) * phase_);
    phase_ += carrier_hz_ / kSampleRate;
    if (phase_ >= 1.0f) {
      phase_ -= 1.0f;
    }

    float ring = Xmod(signal, carrier, character_);
    return signal + (ring - signal) * mix_;
  }

  PedalInfo Describe() override {
    PedalInfo info;
    info.name = "Ring Modulator";
    info.knobs = {
        PedalKnob{.name = "carrier_hz",
                  .value = carrier_hz_,
                  .tweak_amount = 10,
                  .min = 20,
                  .max = 2000},
        PedalKnob{.name = "character",
                  .value = character_,
                  .tweak_amount = 0.1,
                  .min = 0,
                  .max = 1},
        PedalKnob{.name = "mix",
                  .value = mix_,
                  .tweak_amount = 0.1,
                  .min = 0,
                  .max = 1},
    };
    return info;
  }

  void AdjustKnob(const PedalKnob& knob) override {
    if (knob.name == "carrier_hz") {
      carrier_hz_ =
          static_cast<float>(std::max(20.0, std::min(knob.value, 2000.0)));
    } else if (knob.name == "character") {
      character_ = Clamp01(knob.value);
    } else if (knob.name == "mix") {
      mix_ = Clamp01(knob.value);
    }
  }

 private:
  static constexpr float kSampleRate = 44100.0f;

  static float Clamp01(double v) {
    return static_cast<float>(std::max(0.0, std::min(v, 1.0)));
  }

  // Diode-based analog ring modulator approximation. Ported from
  // Modulator::Diode(), eurorack/warps/dsp/modulator.cc:347.
  static float Diode(float x) {
    float sign = x > 0.0f ? 1.0f : -1.0f;
    float dead_zone = std::fabs(x) - 0.667f;
    dead_zone += std::fabs(dead_zone);
    dead_zone *= dead_zone;
    return 0.04324765822726063f * dead_zone * sign;
  }

  // Ported from Modulator::Xmod<ALGORITHM_ANALOG_RING_MODULATION>(),
  // eurorack/warps/dsp/modulator.cc:382.
  static float Xmod(float modulator, float carrier, float parameter) {
    carrier *= 2.0f;
    float ring = Diode(modulator + carrier) + Diode(modulator - carrier);
    ring *= (4.0f + parameter * 24.0f);
    return stmlib::SoftLimit(ring);
  }

  float carrier_hz_ = 220.0f;
  float character_ = 0.3f;
  // 0.5 rather than a fully-wet 1.0 default -- at 1.0 this pedal is loud
  // and overdriven-sounding the moment it's added, before a player has
  // had a chance to dial anything in.
  float mix_ = 0.5f;
  float phase_ = 0.0f;
};

REGISTER_PEDAL("Ring Modulator", []() {
  return std::unique_ptr<Pedal>(new RingModulatorPedal());
});

#endif /* RING_MODULATOR_PEDAL_H */
