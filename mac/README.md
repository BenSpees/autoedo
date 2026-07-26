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

Two ways to launch — one brain (`tools/autoedo.command`), two faces:

**Dock button.** Drag `mac/tools/AutoEDO.app` into your Dock. One click
stops the running copy, rebuilds only if sources changed, relaunches, and
*focuses your existing browser tab* instead of opening a duplicate. No
Terminal window appears; a real rebuild shows a notification banner, and a
failure shows a dialog with a *Show Log* button.

**Terminal.** Double-click `mac/tools/autoedo.command` (or run it / the
`./run.sh` wrapper) for the full watch-it-scroll experience:

```bash
cd mac
./run.sh                      # build if needed + relaunch + open/focus the UI
./run.sh --stop               # stop the service
./run.sh --no-ui              # don't touch the browser
AUTOEDO_PORT=9000 ./run.sh    # serve the UI on another port
tail -f logs/run-*.log        # watch the service log
```

The Dock bundle is three files (`Info.plist`, a 4-line shim that `exec`s
`autoedo.command --headless`, and `AppIcon.icns`); it lives inside the repo
and finds it relative to its own path, so it keeps working wherever the
clone sits. Regenerate the icon anywhere with
`python3 tools/make_launcher_icon.py` (pure stdlib). Note: the first
tab-focus triggers a one-time macOS Automation permission prompt, and a
*downloaded zip* (unlike a `git clone`) is quarantined by Gatekeeper —
right-click → Open the bundle once.

> **Microphone permission:** the first time the engine starts, macOS will ask
> for microphone access for your terminal (or whatever launched `run.sh`).
> If you decline, the engine reports a start failure in the UI — grant access
> under *System Settings → Privacy & Security → Microphone* and press
> *Restart engine*.

> **Feedback warning:** with the built-in mic and speakers you will get a
> feedback loop. Use headphones, or route input/output to separate devices.

## Web UI

- **Tuning** — EDO quick-chips (12 · 17 · 19 · 22 · 24 · 31 · 53 · 72) plus a
  numeric field (10–72); root note picker with a fine cents offset (degree 0
  sits on the root); A4 reference; octave stretch (±¢/oct, period ≠ 1200).
- **Pitch ruler** — the degree picker is a cents-proportional octave strip
  with stacked lanes: a slim 12-TET piano lane (true fractional key
  boundaries, never quantized to the EDO grid; clicking a key toggles the
  nearest degree; hideable), an optional teal JI-landmark lane (3/2, 5/4,
  7/4, …), and the degree lane itself — click toggles a lug, click-drag
  paints a range, alt-click auditions the pitch. Lit degrees are the quiet
  bone default; unlit degrees get a darkened full-column wash; the root is
  gold; amber means exactly one thing: the degree being corrected to right
  now. Tolerance renders as a halo around each lug. Labels are adaptive
  (every lug ≤ 24 EDO, every 5th above), with a full-identity tooltip
  (`deg 41 · 683.3¢ · G −16.7¢ · ≈3/2 −18.6¢`). Selection tools: All /
  None / Invert / rotate mask / Near-12 / Near-JI filters, computed scale
  presets (major, minor, chain-of-fifths diatonic & pentatonic — correct in
  any EDO by construction), user preset save slots, and Scala `.scl`
  import/export. The status line shows `n/N degrees · step · largest gap`
  (red when a sparse mask makes jumps > ~240¢).
- **Correction** — retune speed (within-note), amount (partial correction),
  tolerance (dead zone that preserves vibrato), stickiness (hysteresis
  before re-snapping — auto-raised above 41 EDO, where the step is smaller
  than vibrato), transition (glide between *different* degrees), humanize
  (relaxes retune on sustained notes), and a detection-range preset
  (Bass … Soprano / Instrument / Wide; changing it restarts the engine and
  changes latency, which follows the low limit).
- **Audio device** — input/output device (may be different devices at
  different rates; the app resamples and compensates drift) and buffer size.
- **Output** — bypass and output gain. **Live pitch** — detected input pitch
  vs. the amber target degree, with a cents meter.
- **A/B** — two config slots with Copy A→B.

All settings apply live and persist to `~/.autoedo.json`. Not in this build
(future work): the pitch-trace lane, MIDI target/out, per-degree gravity
weights, and ruler zoom/minimap. Formant preservation needs no toggle —
TD-PSOLA preserves formants inherently.

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
├── run.sh              thin wrapper → tools/autoedo.command
├── tools/
│   ├── autoedo.command        the launcher (terminal + --headless modes)
│   ├── AutoEDO.app/           dockable 3-file bundle (shim → the .command)
│   └── make_launcher_icon.py  regenerates AppIcon.icns (pure stdlib)
├── Makefile
├── scripts/embed.sh    embeds web/index.html into the binary (od-based, no deps)
├── web/index.html      the control UI (single self-contained page)
├── src/
│   ├── tuning.h        EDO tuning math (root anchor + octave stretch)
│   ├── yin.[ch]        YIN pitch detector (port of YinPitchDetector)
│   ├── psola.[ch]      TD-PSOLA corrector (mono) + tolerance/stickiness/
│   │                   transition/amount/humanize correction behavior
│   ├── ring.h          lock-free SPSC ring buffer with resampling reader
│   ├── audio.h         backend interface
│   ├── audio_params.h  lock-free live-parameter mirror shared by backends
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
