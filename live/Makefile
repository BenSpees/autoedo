# AutoEDO Live — self-contained realtime EDO pitch corrector with a web UI.
#
#   make            build ./build/autoedo — CoreAudio on macOS, WASAPI on
#                   Windows (MSYS2/mingw-w64), test-tone stub elsewhere
#   make lib        build ./build/libautoedo.a — the engine as a library a
#                   host mixes in-process (embed backend; public API src/embed.h)
#   make test       build and run the DSP/JSON self-tests + the embed tests
#   make clean
#
# Cross-compile the Windows build from Linux/macOS:
#   make PLATFORM=windows CC=x86_64-w64-mingw32-gcc

UNAME  := $(shell uname -s)
CC     ?= cc
CXX    ?= c++
CFLAGS ?= -O2
CXXFLAGS ?= -O2
AE_GIT   := $(shell git rev-parse --short HEAD 2>/dev/null || echo unknown)
CFLAGS   += -std=c11 -Wall -Wextra -Isrc -I$(BUILD) -Ithird_party/irconv \
            -DAE_BUILD_ID=\"$(AE_GIT)\"
# Signalsmith Stretch is C++11; -ffast-math is explicitly supported by it.
CXXFLAGS += -std=c++11 -Wall -Wextra -Isrc -isystem third_party
BUILD  := build
EXE    :=

ifeq ($(OS),Windows_NT)
  PLATFORM ?= windows
else ifeq ($(UNAME),Darwin)
  PLATFORM ?= mac
else
  PLATFORM ?= stub
endif

# The application core (everything but main() and the audio backend) is
# shared verbatim between the standalone binary and the library build.
CORESRC := src/yin.c src/corrector.c src/json.c src/httpd.c src/app.c \
           src/ir_load.c src/sampler.c src/polyf0.c
SRC    := $(CORESRC) src/main.c
CXXSRC := src/shifter.cpp

ifeq ($(PLATFORM),mac)
  SRC     += src/audio_mac.c
  LDLIBS  := -framework CoreAudio -framework AudioToolbox -framework CoreMIDI -framework CoreFoundation
else ifeq ($(PLATFORM),windows)
  SRC     += src/audio_win.c
  CFLAGS  += -D_WIN32_WINNT=0x0A00
  # -static folds winpthread/libgcc in, so the exe needs only system DLLs
  LDLIBS  := -static -lole32 -lwinmm -lws2_32 -lpthread
  EXE     := .exe
else
  SRC     += src/audio_stub.c
  CFLAGS  += -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L
  LDLIBS  := -lpthread -lm
endif

OBJ := $(patsubst src/%.c,$(BUILD)/%.o,$(SRC)) \
       $(patsubst src/%.cpp,$(BUILD)/%.opp,$(CXXSRC)) \
       $(BUILD)/irconv.opp

# The library: the same core, the embed backend instead of a device one,
# and the embed.h face. No main().
LIBSRC := $(CORESRC) src/embed.c src/audio_embed.c
LIBOBJ := $(patsubst src/%.c,$(BUILD)/%.o,$(LIBSRC)) \
          $(patsubst src/%.cpp,$(BUILD)/%.opp,$(CXXSRC)) \
          $(BUILD)/irconv.opp

all: $(BUILD)/autoedo$(EXE)

lib: $(BUILD)/libautoedo.a

$(BUILD)/libautoedo.a: $(LIBOBJ)
	$(AR) rcs $@ $(LIBOBJ)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/web_index.h: web/index.html scripts/embed.sh | $(BUILD)
	sh scripts/embed.sh web_index_html web/index.html > $@

$(BUILD)/web_scales.h: web/scales.json scripts/embed.sh | $(BUILD)
	sh scripts/embed.sh web_scales_json web/scales.json > $@

$(BUILD)/%.o: src/%.c $(BUILD)/web_index.h $(BUILD)/web_scales.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.opp: src/%.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# The shared IR convolver (vendored byte-identical with Treebrain).
$(BUILD)/irconv.opp: third_party/irconv/irconv.cpp third_party/irconv/irconv.h | $(BUILD)
	$(CXX) $(CXXFLAGS) -Ithird_party/irconv -c $< -o $@

$(BUILD)/autoedo$(EXE): $(OBJ)
	$(CXX) $(CXXFLAGS) $(OBJ) $(LDLIBS) -o $@

# Headers are few; rebuild everything when any of them changes.
$(OBJ) $(LIBOBJ): $(wildcard src/*.h)

$(BUILD)/selftest: tests/selftest.c src/yin.c src/corrector.c src/json.c src/ir_load.c \
                   src/sampler.c src/polyf0.c src/audio_stub.c $(BUILD)/shifter.opp $(BUILD)/irconv.opp | $(BUILD)
	$(CC) $(CFLAGS) -c tests/selftest.c -o $(BUILD)/selftest-main.o
	$(CC) $(CFLAGS) -c src/yin.c -o $(BUILD)/t-yin.o
	$(CC) $(CFLAGS) -c src/corrector.c -o $(BUILD)/t-corrector.o
	$(CC) $(CFLAGS) -c src/json.c -o $(BUILD)/t-json.o
	$(CC) $(CFLAGS) -c src/audio_stub.c -o $(BUILD)/t-stub.o
	$(CC) $(CFLAGS) -c src/ir_load.c -o $(BUILD)/t-irload.o
	$(CC) $(CFLAGS) -c src/sampler.c -o $(BUILD)/t-sampler.o
	$(CC) $(CFLAGS) -c src/polyf0.c -o $(BUILD)/t-polyf0.o
	$(CXX) $(CXXFLAGS) $(BUILD)/selftest-main.o $(BUILD)/t-yin.o \
	    $(BUILD)/t-corrector.o $(BUILD)/t-json.o $(BUILD)/t-stub.o $(BUILD)/t-irload.o \
	    $(BUILD)/t-sampler.o $(BUILD)/t-polyf0.o \
	    $(BUILD)/shifter.opp $(BUILD)/irconv.opp -lm -lpthread -o $@

# The embed tests drive the public library face end to end (create, config
# JSON, blocks through, status echo, destroy), linked against the real lib.
$(BUILD)/embedtest: tests/embedtest.c src/embed.h $(BUILD)/libautoedo.a
	$(CC) $(CFLAGS) -c tests/embedtest.c -o $(BUILD)/embedtest-main.o
	$(CXX) $(CXXFLAGS) $(BUILD)/embedtest-main.o $(BUILD)/libautoedo.a \
	    $(LDLIBS) -lm -lpthread -o $@

# Scored guitar input from the Xentar library through the real detector in
# 22-EDO (tests/guitartest.c). Skips itself cleanly when the wav tree is
# absent, so a bare clone still passes `make test`.
$(BUILD)/guitartest: tests/guitartest.c $(BUILD)/selftest
	$(CC) $(CFLAGS) -c tests/guitartest.c -o $(BUILD)/guitartest-main.o
	$(CXX) $(CXXFLAGS) $(BUILD)/guitartest-main.o $(BUILD)/t-yin.o \
	    $(BUILD)/t-corrector.o $(BUILD)/t-json.o $(BUILD)/t-stub.o $(BUILD)/t-irload.o \
	    $(BUILD)/t-sampler.o $(BUILD)/t-polyf0.o \
	    $(BUILD)/shifter.opp $(BUILD)/irconv.opp -lm -lpthread -o $@

test: $(BUILD)/selftest $(BUILD)/embedtest $(BUILD)/guitartest
	$(BUILD)/selftest
	$(BUILD)/embedtest
	$(BUILD)/guitartest

clean:
	rm -rf $(BUILD)

.PHONY: all lib test clean
