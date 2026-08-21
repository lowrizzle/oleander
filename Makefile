COMPILER = g++

COMPILE_FLAGS = -lpthread \
								-lboost_system \
								-I crow/include \
								-std=c++1y \
								-w \
								-Wno-enum-constexpr-conversion \
							 	-I . \
								-D CROW_DISABLE_STATIC_DIR \
								-I cycfi/Q/q_lib/include \
								-I cycfi/infra/include \
								-I eurorack \
								-D TEST

MATPLOT_FLAGS = -I/usr/include/python2.7 \
								-I/System/Library/Frameworks/Python.framework/Versions/2.7/Extras/lib/python/numpy/core/include \
								-lpython2.7

# RtAudio is vendored as a git submodule (rtaudio/) rather than expected to
# be a preinstalled system package: Raspberry Pi OS doesn't ship a
# prebuilt RtAudio at all (linking bare `-lrtaudio` fails with "cannot
# find -lrtaudio"), and even on a system that does have one, its API can
# differ from the specific version vendored here -- this vendored 5.1.0
# has a void-returning openStream() that throws on failure, which isn't
# true of every RtAudio release (see codefix.md Round 3 #1). So instead of
# linking against some external copy, RtAudio.cpp is compiled directly
# alongside whichever target actually uses it.
RTAUDIO_SRC = rtaudio/RtAudio.cpp

# The "Sky Chive" pedal's granular engine is a port of Mutable Instruments'
# open-source Clouds firmware (eurorack/, vendored as a git submodule --
# see pedals/clouds_pedal.h and docs/THIRD_PARTY.md). clouds/dsp/ is
# header-only except for these files, which its own upstream desktop test
# build (clouds/test/makefile) also compiles directly rather than linking a
# prebuilt library. -D TEST is the same macro that build uses to strip a
# `#ifndef TEST` branch that would otherwise pull in STM32-only headers
# (clouds/drivers/debug_pin.h) -- safe to define globally since nothing
# else in this codebase defines or checks a TEST macro.
CLOUDS_SRC = eurorack/clouds/dsp/granular_processor.cc \
						 eurorack/clouds/dsp/correlator.cc \
						 eurorack/clouds/dsp/mu_law.cc \
						 eurorack/clouds/resources.cc \
						 eurorack/clouds/dsp/pvoc/frame_transformation.cc \
						 eurorack/clouds/dsp/pvoc/phase_vocoder.cc \
						 eurorack/clouds/dsp/pvoc/stft.cc \
						 eurorack/stmlib/dsp/atan.cc \
						 eurorack/stmlib/dsp/units.cc \
						 eurorack/stmlib/utils/random.cc

# The Chorus, Overdrive, Bitcrusher, and LP Gate pedals wrap Mutable
# Instruments' open-source Plaits firmware DSP (eurorack/plaits/dsp/fx/
# ensemble.h, overdrive.h, sample_rate_reducer.h, low_pass_gate.h -- see
# pedals/chorus_pedal.h, pedals/overdrive_pedal.h,
# pedals/bitcrusher_pedal.h, pedals/low_pass_gate_pedal.h, and
# docs/THIRD_PARTY.md). All header-only except for the LUT data in
# resources.cc, same pattern as CLOUDS_SRC above.
PLAITS_SRC = eurorack/plaits/resources.cc

# The Daisy Chains pedal wraps Mutable Instruments' open-source Rings
# firmware DSP (eurorack/rings/dsp/resonator.h -- see
# pedals/daisy_chains_pedal.h and docs/THIRD_PARTY.md). Unlike the Plaits/
# Clouds fx headers above, Resonator's implementation lives in a .cc file,
# not the header -- resources.cc is its LUT data, same pattern as
# CLOUDS_SRC/PLAITS_SRC. Shared stmlib sources are already linked via
# CLOUDS_SRC above; not duplicated here.
RINGS_SRC = eurorack/rings/dsp/resonator.cc \
						eurorack/rings/resources.cc

UNAME := $(shell uname)
ifeq ($(UNAME), Linux)
	# GCC complains about some ABI differences in one of the libraries, but it
	# doesn't appear to cause issues.
	COMPILE_FLAGS += -Wno-psabi
	# ALSA is RtAudio's standard Linux/Raspberry Pi OS backend.
	# libasound2-dev (installed by setup/install.sh) provides both the
	# headers RtAudio.cpp needs under this define and -lasound to link.
	COMPILE_FLAGS += -D__LINUX_ALSA__ -lasound
	# Linux and OSX link in SDL2 differently
	COMPILE_FLAGS += `sdl2-config --cflags --libs`
endif
ifeq ($(UNAME), Darwin)
	# Unlike Linux, a Homebrew-installed RtAudio is assumed to already be
	# present as a system library on OSX, so it's linked directly instead
	# of compiling the vendored source (which needs -D__MACOSX_CORE__ and
	# the CoreAudio/CoreFoundation frameworks to build from scratch --
	# untested here since this project targets a Raspberry Pi).
	RTAUDIO_SRC =
	COMPILE_FLAGS += -lrtAudio
	COMPILE_FLAGS += -lsdl2
endif


makedir:
	mkdir -p bin

# eurorack/ is a third-party submodule (pichenettes/eurorack) we don't
# control and can't push a modified commit to, so the one local fix this
# project needs (GranularProcessor::Init() not clearing several of its
# own buffers -- see docs/THIRD_PARTY.md's "Modified from upstream" note
# and pedals/clouds_pedal.h) ships as a patch file instead and gets
# applied here. Checked via grep rather than a stamp file so this stays
# correct even after a `git submodule update` resets eurorack/ back to
# its tracked (unpatched) commit, which would silently discard a
# previously-applied patch -- a stamp file would go stale and lie about
# that; grepping the actual file content can't.
.PHONY: vendor-patches
vendor-patches:
	@grep -q "memset(fb_, 0" eurorack/clouds/dsp/granular_processor.cc || \
		(cd eurorack && git apply ../patches/clouds_granular_processor_init_zero_buffers.patch)

all: makedir record server

plot: makedir vendor-patches
	${COMPILER} plot.cpp ${CLOUDS_SRC} ${PLAITS_SRC} ${RINGS_SRC} ${COMPILE_FLAGS} ${MATPLOT_FLAGS} -o ./bin/plot

server: makedir vendor-patches
	${COMPILER} web/main.cpp ${RTAUDIO_SRC} ${CLOUDS_SRC} ${PLAITS_SRC} ${RINGS_SRC} ${COMPILE_FLAGS} -o ./bin/server

record: makedir
	${COMPILER} record.cpp ${RTAUDIO_SRC} ${COMPILE_FLAGS} -o ./bin/record

run: all
	./bin/pedalboard
