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

Layout (spec v1.1): the window has exactly two working surfaces — the pitch
ruler and the correction knob strip. Everything else is a header line, a
footer line, or lives inside a popover: the header carries the status dot +
latency (click for the ⚙ engine popover), Bypass, A/B slots + Copy, the
Preset menu (presets, Save, Scala import/export) and the Perform toggle; a
single tuning line (`31-EDO ▾ · C ▾ +0¢ · A4 440`) sits above the ruler with
the compact live readout right-aligned on the same line. **Perform mode**
strips the window to the ruler + a stage-readable readout.

- **Tuning line** — one EDO dropdown-button whose menu holds the quick-chips
  (12 · 17 · 19 · 22 · 24 · 31 · 53 · 72), a numeric field (10–72) and the
  octave-stretch slider; root note picker with a fine cents offset (degree 0
  sits on the root); A4 reference.
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
  (`deg 41 · 683.3¢ · G −16.7¢ · ≈3/2 −18.6¢`). The live readout is drawn
  on the ruler itself: an amber needle at the detected pitch drifting
  against the glowing target lug. The footer line has All / None / Invert
  plus a Select ▾ menu (Near-12 / Near-JI filters with a ±¢ threshold, mask
  rotation) and the status text `n/N · step · gap` (⚠ red past ~240¢).
  Lane visibility (12-EDO keys, JI lane) lives in the ruler's right-click
  menu and the ⚙ popover; the how-to line is a dismissible first-run hint
  and a `?` hover thereafter. Computed scale presets (major, minor,
  chain-of-fifths diatonic & pentatonic — correct in any EDO by
  construction), user preset save slots and Scala `.scl` import/export are
  in the header's Preset menu.
- **Correction knob strip** — six mini-knobs in one row (drag vertically,
  double-click to type): Speed (within-note retune), Amount (partial
  correction), Tol (dead zone that preserves vibrato), Stick (hysteresis
  before re-snapping — auto-raised above 41 EDO with a toast saying so),
  Trans (glide between *different* degrees), Human (relaxes retune on
  sustained notes).
- **⚙ engine popover** — input/output device (may be different devices at
  different rates; the app resamples and compensates drift), buffer size,
  detection-range preset (Bass … Soprano / Instrument / Wide; changing it
  restarts the engine — announced by a toast — and latency follows the low
  limit), output gain, lane toggles, restart, and any engine error.
- **Header** — Bypass, A/B config slots with Copy A→B, Preset menu,
  Perform toggle.
- **Harmony strip** (Xentar smart-harmonizer emulation) — a third surface,
  collapsed to one line: master ⏻, a three-way **Lock** (`Off` = raw
  parallel interval · `Mask` = snap each ghost to the lit-degree mask via
  the walk-outward-up-first algorithm · `JI` = snap to the JI landmark
  set), and colored chips showing the active voices. Expanded: five
  independent voice pods (V1 pink · V2 violet · V3 cyan · V4 lime · V5
  blue), each with the full interval pool (±1 … ±equave, labeled in
  steps + cents), octave-extension dots (0–2, stacking in the voice's
  direction), gain and pan mini-knobs, and mute/solo. Voices ride the
  *corrected* pitch, dedupe when they land on one pitch, and render as
  colored ghost ticks on the ruler (gold when a voice lands on the root).
  **Learn-from-ruler**: select a pod, then shift-click a lug to set its
  interval. Harmony state persists as `hm`/`hx`/`hg`/`hp` fields.
- **Scale catalog** — the Preset menu carries the full Xentar scale dump
  (31 EDO packs, 702 named scales incl. the world-music sets, served at
  `/api/scales`) filtered to the current EDO, plus the familiar 12-EDO
  reference scales auto-quantized into any EDO (deduped against native
  scales). Loading a scale sets the mask and remembers the name until the
  mask is edited by hand. Multi-octave scales load octave 0's steps and
  carry a ⧉ badge (both display and engine flatten to octave 0).

All settings apply live and persist to `~/.autoedo.json`. Not in this build
(future work): the pitch-trace lane, MIDI target/out, per-degree gravity
weights, and ruler zoom/minimap. Formant preservation needs no toggle —
TD-PSOLA preserves formants inherently.

The UI is plain HTML/JS embedded into the binary at build time
(`web/index.html` → `build/web_index.h`); the JSON API underneath is:

| Endpoint | Method | Purpose |
|---|---|---|
| `/ws`          | WS   | status pushed ~10×/s — the UI renders from this stream and polls nothing |
| `/api/status`  | GET  | same status JSON (serialized once per tick, shared by all consumers) |
| `/api/devices` | GET  | audio device list |
| `/api/config`  | POST | partial config update (restarts engine if devices/buffer changed) |
| `/api/restart` | POST | force an engine restart |

The server binds to `127.0.0.1` only. If the stream dies the UI dims and
says so (no animating a dead engine), keeps reconnecting once a second,
and falls back to slow polling until the socket is back.

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
