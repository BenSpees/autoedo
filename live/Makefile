# AutoEDO Live — self-contained realtime EDO pitch corrector with a web UI.
#
#   make            build ./build/autoedo — CoreAudio on macOS, WASAPI on
#                   Windows (MSYS2/mingw-w64), test-tone stub elsewhere
#   make test       build and run the DSP/JSON self-tests
#   make clean
#
# Cross-compile the Windows build from Linux/macOS:
#   make PLATFORM=windows CC=x86_64-w64-mingw32-gcc

UNAME  := $(shell uname -s)
CC     ?= cc
CXX    ?= c++
CFLAGS ?= -O2
CXXFLAGS ?= -O2
CFLAGS   += -std=c11 -Wall -Wextra -Isrc -Ibuild
# Signalsmith Stretch is C++11; -ffast-math is explicitly supported by it.
CXXFLAGS += -std=c++11 -Wall -Wextra -Isrc -Ithird_party
BUILD  := build
EXE    :=

ifeq ($(OS),Windows_NT)
  PLATFORM ?= windows
else ifeq ($(UNAME),Darwin)
  PLATFORM ?= mac
else
  PLATFORM ?= stub
endif

SRC    := src/yin.c src/corrector.c src/json.c src/httpd.c src/main.c
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
       $(patsubst src/%.cpp,$(BUILD)/%.opp,$(CXXSRC))

all: $(BUILD)/autoedo$(EXE)

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

$(BUILD)/autoedo$(EXE): $(OBJ)
	$(CXX) $(CXXFLAGS) $(OBJ) $(LDLIBS) -o $@

# Headers are few; rebuild everything when any of them changes.
$(OBJ): $(wildcard src/*.h)

$(BUILD)/selftest: tests/selftest.c src/yin.c src/corrector.c src/json.c \
                   $(BUILD)/shifter.opp | $(BUILD)
	$(CC) $(CFLAGS) -c tests/selftest.c -o $(BUILD)/selftest-main.o
	$(CC) $(CFLAGS) -c src/yin.c -o $(BUILD)/t-yin.o
	$(CC) $(CFLAGS) -c src/corrector.c -o $(BUILD)/t-corrector.o
	$(CC) $(CFLAGS) -c src/json.c -o $(BUILD)/t-json.o
	$(CXX) $(CXXFLAGS) $(BUILD)/selftest-main.o $(BUILD)/t-yin.o \
	    $(BUILD)/t-corrector.o $(BUILD)/t-json.o $(BUILD)/shifter.opp -lm -o $@

test: $(BUILD)/selftest
	$(BUILD)/selftest

clean:
	rm -rf $(BUILD)

.PHONY: all test clean
