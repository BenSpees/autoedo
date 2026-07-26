# AutoEDO Live (standalone Mac C app)

A self-contained, plain-C version of AutoEDO that runs **live**: it captures
the selected audio input, pitch-corrects it to an *N*-EDO scale in real time,
and plays the result on the selected audio output. Every device and
processing setting is controlled from a **web UI** served by the app itself
at `http://127.0.0.1:8017/` — no plugin host, no JUCE, no dependencies
beyond the macOS system frameworks.

```
mic ─► CoreAudio AUHAL (input) ─► ring buffer ─► YIN ─► EDO snap ─► glide ─► TD-PSOLA ─► AUHAL (output) ─► speakers
                                                          ▲ live settings via embedded web UI ▲
```

The DSP is a direct C port of the plugin engine in `../Source/dsp/`
(YIN pitch detection, C-anchored EDO tuning math, TD-PSOLA resynthesis with
a latency-matched dry path for unvoiced passages).

## Quick start

```bash
cd mac
./run.sh
```

`run.sh` rebuilds all code, restarts the background service, waits for it to
come up, and opens the web UI in your browser. Other verbs:

```bash
./run.sh --stop          # stop the service
AUTOEDO_PORT=9000 ./run.sh   # serve the UI on another port
tail -f build/autoedo.log    # watch the service log
```

> **Microphone permission:** the first time the engine starts, macOS will ask
> for microphone access for your terminal (or whatever launched `run.sh`).
> If you decline, the engine reports a start failure in the UI — grant access
> under *System Settings → Privacy & Security → Microphone* and press
> *Restart engine*.

> **Feedback warning:** with the built-in mic and speakers you will get a
> feedback loop. Use headphones, or route input/output to separate devices.

## Web UI

- **Audio device** — pick the input and output device (they may be different
  devices at different sample rates; the app resamples and compensates for
  clock drift between them) and the hardware buffer size. Device or buffer
  changes restart the engine automatically.
- **Tuning** — EDO (10–72 divisions of the octave) and retune speed
  (0 ms = hard robotic snap, larger = natural glide).
- **Scale degrees** — one toggle per degree of the current EDO (degree 0 = C);
  correction snaps only to lit degrees. `All` / `None` for bulk edits.
  Disabling every degree falls back to full-chromatic snapping.
- **Output** — bypass (dry input) and output gain.
- **Live pitch** — detected input pitch (with nearest 12-TET name), the EDO
  target degree it is being corrected to, and the cents offset between them.

All settings apply live and persist to `~/.autoedo.json`.

The UI is plain HTML/JS embedded into the binary at build time
(`web/index.html` → `build/web_index.h`); the JSON API underneath is:

| Endpoint | Method | Purpose |
|---|---|---|
| `/api/status`  | GET  | engine state, live pitch read-out, current config |
| `/api/devices` | GET  | audio device list |
| `/api/config`  | POST | partial config update (restarts engine if devices/buffer changed) |
| `/api/restart` | POST | force an engine restart |

The server binds to `127.0.0.1` only.

## Building manually

```bash
make          # -> build/autoedo  (CoreAudio backend on macOS)
make test     # DSP + JSON self-tests (no audio hardware needed)
./build/autoedo --port 8017
```

On non-macOS hosts `make` builds the same app against a stub audio backend
(`src/audio_stub.c`) that feeds the corrector a wobbling test tone — useful
for developing the UI/API and running the self-tests anywhere.

## Layout

```
mac/
├── run.sh              launcher: rebuild + relaunch service + open web UI
├── Makefile
├── scripts/embed.sh    embeds web/index.html into the binary (od-based, no deps)
├── web/index.html      the control UI (single self-contained page)
├── src/
│   ├── tuning.h        EDO tuning math (C port of ../Source/dsp/Tuning.h)
│   ├── yin.[ch]        YIN pitch detector (port of YinPitchDetector)
│   ├── psola.[ch]      TD-PSOLA corrector (port of PsolaPitchCorrector, mono)
│   ├── ring.h          lock-free SPSC ring buffer with resampling reader
│   ├── audio.h         backend interface
│   ├── audio_mac.c     CoreAudio backend (two AUHAL units, device selection)
│   ├── audio_stub.c    test-tone backend for non-macOS builds
│   ├── httpd.[ch]      tiny embedded HTTP server (127.0.0.1 only)
│   ├── json.[ch]       minimal JSON helpers
│   └── main.c          config persistence + JSON API + lifecycle
└── tests/selftest.c    tuning/YIN/PSOLA/JSON self-tests (`make test`)
```

## Latency

Total latency ≈ PSOLA correction latency (3 × the longest detectable period,
~46 ms — required so every output sample is fully covered by synthesis
grains) + a ~20 ms input cushion (absorbs clock jitter between independent
input/output devices) + the hardware buffer. The UI status line shows the
live figure.
