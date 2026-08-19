#ifndef PITCH_SHIFTER_PEDAL_H
#define PITCH_SHIFTER_PEDAL_H

#include "pedal.h"
#include "pedal_registry.h"
#include "signal_type.h"

#include "clouds/dsp/frame.h"
#include "clouds/dsp/fx/pitch_shifter.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

// A 3-voice chord/harmony effect: 3 independent instances of Mutable
// Instruments' Clouds granular pitch shifter, each tuned to its own
// interval and mixed with the dry signal -- see docs/ROADMAP.md item 3
// and docs/THIRD_PARTY.md. clouds::PitchShifter itself is a single-voice
// class (one `ratio` knob); there's no multi-voice/chord primitive to
// port from upstream, so running 3 of them at independently-set
// intervals is original composition on top of the vendored single-voice
// engine, not a port of any one upstream feature.
//
// Each voice is mono-duplicated into a clouds::FloatFrame and averaged
// back down, the same approach pedals/chorus_pedal.h and
// pedals/reverb_pedal.h use for the same reason (this pipeline is mono;
// the underlying engine is stereo).
class PitchShifterPedal : public Pedal {
 public:
  PitchShifterPedal() {
    for (int i = 0; i < kNumVoices; i++) {
      buffers_[i].resize(kBufferSize);
      shifters_[i].Init(buffers_[i].data());
      shifters_[i].set_size(grain_size_);
    }
    UpdateRatios();
  }

  SignalType Transform(SignalType signal) override {
    float wet = 0.0f;
    for (int i = 0; i < kNumVoices; i++) {
      clouds::FloatFrame frame{signal, signal};
      shifters_[i].Process(&frame, 1);
      wet += (frame.l + frame.r) * 0.5f;
    }
    wet /= kNumVoices;
    return signal + (wet - signal) * mix_;
  }

  PedalInfo Describe() override {
    PedalInfo info;
    info.name = "Pitch Shifter";
    info.knobs = {
        PedalKnob{.name = "voice_1_semitones",
                  .value = semitones_[0],
                  .tweak_amount = 1,
                  .min = -24,
                  .max = 24},
        PedalKnob{.name = "voice_2_semitones",
                  .value = semitones_[1],
                  .tweak_amount = 1,
                  .min = -24,
                  .max = 24},
        PedalKnob{.name = "voice_3_semitones",
                  .value = semitones_[2],
                  .tweak_amount = 1,
                  .min = -24,
                  .max = 24},
        PedalKnob{.name = "grain_size",
                  .value = grain_size_,
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
    if (knob.name == "voice_1_semitones") {
      semitones_[0] = ClampSemitones(knob.value);
      UpdateRatios();
    } else if (knob.name == "voice_2_semitones") {
      semitones_[1] = ClampSemitones(knob.value);
      UpdateRatios();
    } else if (knob.name == "voice_3_semitones") {
      semitones_[2] = ClampSemitones(knob.value);
      UpdateRatios();
    } else if (knob.name == "grain_size") {
      grain_size_ = Clamp01(knob.value);
      for (int i = 0; i < kNumVoices; i++) {
        shifters_[i].set_size(grain_size_);
      }
    } else if (knob.name == "mix") {
      mix_ = Clamp01(knob.value);
    }
  }

 private:
  static constexpr int kNumVoices = 3;
  static constexpr size_t kBufferSize = 4096;

  static float Clamp01(double v) {
    return static_cast<float>(std::max(0.0, std::min(v, 1.0)));
  }

  static float ClampSemitones(double v) {
    return static_cast<float>(std::max(-24.0, std::min(v, 24.0)));
  }

  void UpdateRatios() {
    for (int i = 0; i < kNumVoices; i++) {
      shifters_[i].set_ratio(std::pow(2.0f, semitones_[i] / 12.0f));
    }
  }

  // A triad stacked on the dry note: major third, perfect fifth, octave.
  std::array<float, kNumVoices> semitones_ = {4.0f, 7.0f, 12.0f};
  float grain_size_ = 0.5f;
  float mix_ = 0.5f;

  std::array<clouds::PitchShifter, kNumVoices> shifters_;
  std::array<std::vector<uint16_t>, kNumVoices> buffers_;
};

REGISTER_PEDAL("Pitch Shifter", []() {
  return std::unique_ptr<Pedal>(new PitchShifterPedal());
});

#endif /* PITCH_SHIFTER_PEDAL_H */
