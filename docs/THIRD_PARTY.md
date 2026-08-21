# Third-party code

## Sky Chive pedal (`pedals/clouds_pedal.h`)

The granular synthesis engine behind the "Sky Chive" pedal is a direct port
of Mutable Instruments' open-source **Clouds** Eurorack module firmware.

- Source: [pichenettes/eurorack](https://github.com/pichenettes/eurorack)
  (`clouds/dsp/`, `clouds/resources.{h,cc}`), plus its `stmlib` dependency
  (also by the same author), vendored here as git submodules at `eurorack/`
  and `eurorack/stmlib/`.
- License: MIT.
- Copyright: `Copyright 2014 Emilie Gillet.` (`stmlib` files: `Copyright
  2012 Emilie Gillet.`) Author: Emilie Gillet (emilie.o.gillet@gmail.com).
- Only the hardware-independent DSP core (`clouds/dsp/`) is used --
  `clouds/drivers/`, `clouds/ui.cc`, `clouds/cv_scaler.*`, and the rest of
  `eurorack`'s STM32-specific firmware are not part of this project.

Per Mutable Instruments' own request that derivative works not use the
"Mutable Instruments" or "Clouds" names, this port is named "Sky Chive"
everywhere user-facing (the web UI, OLED, presets); code comments and this
file note the lineage for attribution, as the MIT license requires.

**Modified from upstream** (everywhere else in this project, vendored MI
code is used unmodified, only ever called through its public API): a
small patch, `patches/clouds_granular_processor_init_zero_buffers.patch`,
changes `clouds/dsp/granular_processor.cc`'s `GranularProcessor::Init()`
to explicitly zero several of the class's own internal buffers (`fb_`,
`in_`, `in_downsampled_`, `out_`, `out_downsampled_`, `tail_buffer_`)
that upstream's constructor/`Init()` never clear. On the original
embedded target this class is a single instance constructed once at
boot, so the MCU's BSS zero-init already guaranteed these started
silent; that guarantee doesn't hold here, where `pedals/clouds_pedal.h`
constructs a fresh `GranularProcessor` on every preset recall and a new
instance can inherit another just-freed instance's real leftover audio
data at the same heap address. Confirmed via a standalone scratchpad
test (deliberately reusing just-freed, real-audio-filled heap memory for
a fresh `GranularProcessor`) that this was reachable through `fb_`,
feeding straight into the feedback path on the very first `Process()`
call. See `pedals/clouds_pedal.h`'s own comment for the full incident
(reported on real hardware 2026-08-20).

`eurorack/` is a third-party submodule (`pichenettes/eurorack`) this
project has no push access to, so the fix can't be shipped as a bumped
submodule commit the normal way -- `patches/` holds it instead, and the
Makefile's `vendor-patches` target (a dependency of both `plot` and
`server`) applies it automatically on every build, idempotently, without
needing any extra manual step beyond the usual `git pull && make
server`. `eurorack/`'s own git state stays untouched/pristine as tracked
by this repo; the patched file only exists in the actual build output on
disk.

The MIT license text (reproduced from `eurorack`'s `LICENSE`):

```
Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

## Reverb pedal (`pedals/reverb_pedal.h`)

The reverb engine is a direct port of Mutable Instruments' open-source
**Clouds** Eurorack module firmware (the same project Sky Chive above is
ported from) -- specifically its Dattorro/Griesinger-topology reverb, not
used by Sky Chive itself.

- Source: [pichenettes/eurorack](https://github.com/pichenettes/eurorack)
  (`clouds/dsp/fx/reverb.h`, `clouds/dsp/fx/fx_engine.h`), vendored here
  as a git submodule at `eurorack/`.
- License: MIT. Copyright: `Copyright 2014 Emilie Gillet.` Author: Emilie
  Gillet (emilie.o.gillet@gmail.com). Same license text as the Sky Chive
  entry above.

## Chorus, Overdrive, Bitcrusher, and LP Gate pedals (`pedals/chorus_pedal.h`, `pedals/overdrive_pedal.h`, `pedals/bitcrusher_pedal.h`, `pedals/low_pass_gate_pedal.h`)

All four are direct ports of effects from Mutable Instruments' open-source
**Plaits** Eurorack module firmware.

- Source: [pichenettes/eurorack](https://github.com/pichenettes/eurorack)
  (`plaits/dsp/fx/ensemble.h`, `plaits/dsp/fx/overdrive.h`,
  `plaits/dsp/fx/sample_rate_reducer.h`, `plaits/dsp/fx/low_pass_gate.h`,
  `plaits/dsp/fx/fx_engine.h`, `plaits/resources.{h,cc}`), vendored here
  as a git submodule at `eurorack/`.
- License: MIT. Copyright: `Copyright 2014 Emilie Gillet.` Author: Emilie
  Gillet (emilie.o.gillet@gmail.com). Same license text as the Sky Chive
  entry above.

## Pitch Shifter pedal (`pedals/pitch_shifter_pedal.h`)

A 3-voice chord/harmony effect built from 3 independent instances of
Mutable Instruments' open-source **Clouds** Eurorack module firmware's
granular pitch shifter (the same project Sky Chive is ported from).
`clouds::PitchShifter` is a single-voice class -- running 3 of them at
independently-tunable intervals to get a chord is original composition
on top of the vendored single-voice engine, not itself a port of any one
upstream multi-voice feature.

- Source: [pichenettes/eurorack](https://github.com/pichenettes/eurorack)
  (`clouds/dsp/fx/pitch_shifter.h`, `clouds/dsp/fx/fx_engine.h`),
  vendored here as a git submodule at `eurorack/`.
- License: MIT. Copyright: `Copyright 2014 Emilie Gillet.` Author: Emilie
  Gillet (emilie.o.gillet@gmail.com). Same license text as the Sky Chive
  entry above.

## Daisy Chains pedal (`pedals/daisy_chains_pedal.h`)

The resonant filter engine is a direct port of Mutable Instruments'
open-source **Rings** Eurorack module firmware's modal resonator bank
(`rings::Resonator`) -- not the larger `rings::Part` class, which adds
polyphony, note/strum triggering, chord tables, and a bundled reverb and
limiter built for Rings' synth-voice mode; `Resonator` alone is the
continuous-audio-in/continuous-audio-out piece that fits how every pedal
in this codebase works. Per the same request from Mutable Instruments
that derivative works not reuse their module names (the same reasoning
behind "Sky Chive" for Clouds), this port is named "Daisy Chains"
everywhere user-facing.

- Source: [pichenettes/eurorack](https://github.com/pichenettes/eurorack)
  (`rings/dsp/resonator.{h,cc}`, `rings/resources.{h,cc}`, plus its
  `stmlib` dependency, also by the same author), vendored here as git
  submodules at `eurorack/` and `eurorack/stmlib/`.
- License: MIT. Copyright: `Copyright 2015 Emilie Gillet.` (`stmlib`
  files: `Copyright 2012 Emilie Gillet.`) Author: Emilie Gillet
  (emilie.o.gillet@gmail.com). Same license text as the Sky Chive entry
  above.

## Ring Modulator pedal (`pedals/ring_modulator_pedal.h`)

The ring-modulation math (a diode-based analog ring modulator
approximation) is hand-ported from Mutable Instruments' open-source
**Warps** Eurorack module firmware -- specifically `Modulator::Diode()`
and `Modulator::Xmod<ALGORITHM_ANALOG_RING_MODULATION>()`, not the
surrounding `Modulator` class as a whole (that class expects two audio
inputs and pulls in an oscillator, quadrature transform, oversampling,
and a vocoder path this pedal doesn't use -- see the comment in
`pedals/ring_modulator_pedal.h` for why only the formula, not the class,
was ported). The carrier oscillator driving it is original code, not
derived from Warps.

- Source: [pichenettes/eurorack](https://github.com/pichenettes/eurorack)
  (`warps/dsp/modulator.cc`, `warps/dsp/modulator.h`), vendored here as a
  git submodule at `eurorack/`.
- License: MIT. Copyright: `Copyright 2014 Emilie Gillet.` Author: Emilie
  Gillet (emilie.o.gillet@gmail.com). Same license text as the Sky Chive
  entry above.
