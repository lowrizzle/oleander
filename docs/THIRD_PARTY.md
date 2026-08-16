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
