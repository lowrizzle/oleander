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
								-I cycfi/infra/include

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

all: makedir record server

plot: makedir
	${COMPILER} plot.cpp ${COMPILE_FLAGS} ${MATPLOT_FLAGS} -o ./bin/plot

server: makedir
	${COMPILER} web/main.cpp ${RTAUDIO_SRC} ${COMPILE_FLAGS} -o ./bin/server

record: makedir
	${COMPILER} record.cpp ${RTAUDIO_SRC} ${COMPILE_FLAGS} -o ./bin/record

run: all
	./bin/pedalboard
