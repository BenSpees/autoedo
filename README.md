# AutoEDO

A real-time, AutoTune-style **pitch corrector that tunes to arbitrary equal
divisions of the octave (EDO)** — not just the usual 12. Sing or play a
monophonic source and AutoEDO snaps it to the nearest pitch of an *N*-tone
equal-tempered scale, where you choose *N* from 10 to 72.

Built with [JUCE](https://juce.com) so a single codebase targets **AAX**
(Pro Tools), **VST3** (Windows / macOS / Linux), **AU** (macOS) and a
**Standalone** app.

> **Phase 1 scope.** C is the reference note, fixed at its standard-tuning
> frequency, and every other pitch is derived relative to it. At `EDO = 12`
> the plugin reproduces standard 12-TET exactly (A4 = 440 Hz); other values
> subdivide the octave differently while keeping every C in place.

## Controls

| Control | Range | Description |
|---|---|---|
| **EDO** (Divisions / Octave) | 10 – 72 | Number of equal steps the octave is divided into. `12` = standard tuning. |
| **Retune Speed** | 0 – 500 ms | How quickly the output glides to the target pitch. `0` = hard/instant snap (robotic); higher = natural slide. |
| **Scale degrees** | per-degree toggles | One on/off button per degree of the current EDO (degree `0` = C, the reference). Correction only snaps to *enabled* degrees, so you build any scale directly — no preset keys. `All` / `None` toggle them in bulk. (Disabling every degree falls back to full-chromatic snapping.) |

A **live read-out** shows the detected input pitch (with its nearest 12-TET name
for orientation) and the output target pitch (with its EDO degree), updating as
you play.

## How the tuning works

The set of allowed pitches for an *N*-EDO scale is

```
f(j) = C_anchor · 2^(j / N)      for every integer j
```

where `C_anchor` is C0 at its standard-tuning frequency
(`16.3515978… Hz`, derived from A4 = 440 Hz). Because every octave of C is an
exact power-of-two multiple of the anchor, **all C's stay at their
standard-tuning frequencies for any N**. A detected fundamental is converted to
a continuous position on this grid, rounded to the nearest *enabled* degree
(see **Scale degrees** above), and the audio is resynthesised at that pitch.

See [`Source/dsp/Tuning.h`](Source/dsp/Tuning.h) for the (tiny, well-commented)
core math.

## Signal flow

```
input ─► YIN pitch detection ─► nearest EDO degree (re: C) ─► retune-speed glide ─► TD-PSOLA resynthesis ─► output
```

- **Detection** — [YIN](https://en.wikipedia.org/wiki/Pitch_detection_algorithm)
  (`Source/dsp/YinPitchDetector.*`), a robust monophonic f0 estimator. Its
  squared-difference function is evaluated by FFT cross-correlation rather than
  the textbook double loop — same answer, but O(N log N) instead of
  O(tauMax × window), which is what keeps a 5 ms detection hop affordable at
  high sample rates.
- **Quantisation** — nearest EDO degree relative to C (`Source/dsp/Tuning.h`).
- **Correction** — time-domain PSOLA (`Source/dsp/PsolaPitchCorrector.*`),
  which shifts pitch while preserving duration and formants. Unvoiced / silent
  passages pass through a latency-matched dry path with a smooth crossfade.

PSOLA quality lives or dies on where each grain is read from. Analysis marks
form a *continuous chain* — each one a local period on from the last — and the
mark nearest a given output position is the one that grain reads, at fractional
sample resolution. Consecutive grains are therefore always a whole number of
local periods apart in the source, which is what lets them overlap-add in phase.
Pitch state (period, correction ratio, voicing) is stamped onto the timeline
position it describes rather than read from the analysis frontier, so a grain
uses the estimate belonging to its own moment and the dry/wet crossfade opens on
the note it belongs to. `tests/DspTests.cpp` measures the result: a note already
on the grid comes back with its harmonic-to-artefact ratio intact to within a
fraction of a dB.

Detection and timing are shared across channels (the source is assumed
monophonic), so a stereo signal stays phase-coherent. The plugin reports a
constant latency (`3 × the longest analysed period`, ≈ 46 ms — roughly constant
across sample rates) so the host can compensate. The analysis frame scales with
sample rate, so the lowest detectable pitch (~65 Hz) is the same at 44.1 k as at
96/192 k.

The DSP is plain C++ with **no JUCE dependency**, which is why it can be
unit-tested on its own (see below).

## Building

Requires CMake ≥ 3.22 and a C++17 compiler. JUCE is fetched automatically.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

This builds VST3 + Standalone (and AU on macOS). Build artefacts land under
`build/AutoEDO_artefacts/`.

Linux additionally needs the usual JUCE dev packages, e.g.:

```bash
sudo apt install libasound2-dev libx11-dev libxext-dev libxinerama-dev \
  libxrandr-dev libxcursor-dev libfreetype-dev libcurl4-openssl-dev
```

### Just the tests (no JUCE, no network)

```bash
cmake -B build-tests -DAUTOEDO_BUILD_PLUGIN=OFF
cmake --build build-tests
ctest --test-dir build-tests --output-on-failure
```

## AAX (Pro Tools)

AAX requires Avid's **AAX SDK**, which is NDA-gated and cannot be redistributed,
so it is *not* bundled here. Once you have it locally, enable the AAX target:

```bash
cmake -B build -DAUTOEDO_AAX_SDK_PATH=/path/to/aax-sdk
cmake --build build
```

### Getting AAX SDK / NDA access

1. Create a free developer account at **https://developer.avid.com** and sign in.
2. Go to the **AAX SDK** page and accept the click-through **AAX SDK License
   Agreement** (this is the NDA covering the SDK); then download the SDK.
3. To *distribute* AAX plug-ins (as opposed to running unsigned builds in
   Pro Tools Developer mode), you must additionally:
   - apply for Avid's commercial developer / partner program, and
   - obtain a **PACE/iLok** developer account and code-signing certificate —
     AAX binaries have to be signed with PACE's `wraptool` before Pro Tools
     will load them outside developer mode.

For local development you only need steps 1–2; the signing/partner steps
(step 3) are needed for release distribution.

## Status & roadmap

Phase 1 (this version): fixed C reference, EDO 10–72, retune speed, direct
per-degree scale selection, a live pitch/target read-out, and working
detection + correction — all verified by unit tests.

Possible next steps:
- Glottal-epoch-synchronous PSOLA marks for higher vocal quality. Marks are
  currently epoch-*free*: a continuous chain spaced by the local period, which
  keeps consecutive grains phase-coherent but does not align them to the glottal
  pulse. Anchoring to real epochs is the remaining route to lower phasiness on
  voice. (A hard peak-snap was tried and caused period-doubling on harmonically
  rich tones; it would need a proper epoch detector, nudged rather than snapped.)
- Reduce the residual that a *shifted* note carries — a trace of the original
  pitch, measurable at roughly −65 dB as sidebands on multiples of the input f0.
  It is inherent to grain repetition/omission; less of it needs either
  epoch-aligned marks or a spectral method.
- Selectable reference note / root, and custom non-equal (e.g. just-intonation)
  scales.
- Saving/recalling named degree presets.

## Licensing note

JUCE is dual-licensed (GPLv3 / commercial). Distributing binaries built with
JUCE must comply with its licence; see https://juce.com/get-juce for details.
