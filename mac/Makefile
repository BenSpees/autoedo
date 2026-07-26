# AutoEDO Live — self-contained realtime EDO pitch corrector with a web UI.
#
#   make            build ./build/autoedo (CoreAudio on macOS, stub elsewhere)
#   make test       build and run the DSP/JSON self-tests
#   make clean

UNAME  := $(shell uname -s)
CC     ?= cc
CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra -Isrc -Ibuild
BUILD  := build

SRC := src/yin.c src/psola.c src/json.c src/httpd.c src/main.c

ifeq ($(UNAME),Darwin)
  SRC     += src/audio_mac.c
  LDLIBS  := -framework CoreAudio -framework AudioToolbox -framework CoreFoundation
else
  SRC     += src/audio_stub.c
  CFLAGS  += -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L
  LDLIBS  := -lpthread -lm
endif

OBJ := $(patsubst src/%.c,$(BUILD)/%.o,$(SRC))

all: $(BUILD)/autoedo

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/web_index.h: web/index.html scripts/embed.sh | $(BUILD)
	sh scripts/embed.sh web_index_html web/index.html > $@

$(BUILD)/%.o: src/%.c $(BUILD)/web_index.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/autoedo: $(OBJ)
	$(CC) $(CFLAGS) $(OBJ) $(LDLIBS) -o $@

# Headers are few; rebuild everything when any of them changes.
$(OBJ): $(wildcard src/*.h)

$(BUILD)/selftest: tests/selftest.c src/yin.c src/psola.c src/json.c | $(BUILD)
	$(CC) $(CFLAGS) tests/selftest.c src/yin.c src/psola.c src/json.c -lm -o $@

test: $(BUILD)/selftest
	$(BUILD)/selftest

clean:
	rm -rf $(BUILD)

.PHONY: all test clean
