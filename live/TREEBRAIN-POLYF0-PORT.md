# Porting the poly detection to Treebrain — algorithm handoff

This is the implementation-grade spec of the engine's multi-f0 tracker
(`live/src/polyf0.c`, ~370 lines of self-contained C11 — stdlib + math.h
only, first-party code, no third-party licence attached: port it freely).
Everything Treebrain needs to run the *same algorithm* natively —
constants, order of operations, the traps that were measured and fixed,
and the acceptance fixtures that hold both implementations to the same
behaviour.

**First decide you actually need the port.** The engine already exports
the detection live as `status.polyDetected` (delta §3c-iii) at the
tracker's own update rate behind a ~10 Hz poll. Port only if Treebrain
needs: faster-than-poll onsets, analysis of audio the engine never sees
(recordings, other inputs), or detection with no engine running. If a
port happens, keep reading `polyDetected` anyway — it is the free
cross-check (§8).

The simplest correct port is a line-by-line translation of `polyf0.c`;
this document is then the review checklist. Sections 2–5 restate the file
exactly, in execution order.

---

## 1. Shape of the algorithm

Harmonic-sum salience with **iterative spectral subtraction**, plus a
note tracker with birth/death hysteresis:

```
window → zero-padded FFT → magnitude (keep a pristine copy)
repeat up to 6×:
    scan a log grid of candidate f0s, score each by harmonic salience
    take the best; stop when it stops holding its own
    refine it against the PRISTINE spectrum (log-parabolic, multi-partial)
    subtract its harmonic comb from the residual
tracker: match candidates to living notes; 2 sightings = birth,
         4 misses = death; ids stable across a note's life
```

Frames estimate; the tracker decides. A chord consumer must act on
NOTES, not frames — one glitchy frame must neither strike a note nor
kill one.

Honest scope (same as the engine's): 2–5 distinct pitch classes track
well; octave doublings merge into one note (a power chord reads as root
+ fifth); very dense voicings may drop the weakest tone. These are
properties of the algorithm, not bugs of the port.

## 2. Prepare-time constants

| Thing | Value | Why it is this and not something else |
|---|---|---|
| Window `win` | `2·fs/min_hz` rounded UP to a power of two, **floor 4096, cap 8192** | Resolution rules, not just reach: two chord tones a minor third apart at the range bottom must land in separate bins before interpolation. 4096 @ 48 k = 11.7 Hz bins — barely enough at C3. **2048 was measured hopeless** (23 Hz bins read a triad as soup). |
| Transform `fft` | `2 · win`, zero-padded | Doubles bin density latency-free — the window (and the lag) stays the same; only the spectrum sampling gets finer. The low-register refinement starves without it. |
| Window function | Hann, `0.5 − 0.5·cos(2πi/(win−1))` | The main-lobe width below (±2 unpadded bins) is Hann's; change the window and §4's subtraction width is wrong. |
| Range | `min_hz` (floor 55), `max_hz` (default 1200, and `max(2·min)`) | Engine ties these to the user's detection-range controls; do the same. |
| Polyphony | 6 slots | |
| Precision | double (Float64) throughout the spectra | The log-parabolic refinement works on ratios of nearby magnitudes; Float32 rounding there is audible as cents. Time-domain input can stay Float32. |

## 3. Per-frame, step by step

Input: the freshest `win` samples (most recent audio, contiguous).

1. **Window + zero-pad + FFT** (radix-2; `polyf0.c` lines 9–45 is a
   complete 40-line implementation to lift). Compute magnitude
   `mag[b] = |X[b]|` for `b < fft/2`. Copy to `mag0` — the **pristine**
   spectrum. Also accumulate windowed time-domain power.
2. **Energy gate**: if `Σv² ≤ 1e-4 · win`, emit zero candidates (silence
   must yield no notes, not noise-floor ghosts) and skip to the tracker.
3. **Pick loop**, up to 6 iterations:
   - Scan candidate f0s over a log grid: `hz = min_hz · 2^(n/48)` up to
     `max_hz` (≈4 candidates per semitone). Score each with
     `salience(mag, hz)` (§4) **against the residual** `mag`.
   - Best scorer wins the iteration. Remember the first winner's score
     as `first_sal`; a later winner with `score < 0.20 · first_sal`
     ends the loop — residual notes must hold their own.
   - **Refine** the winner with `refine_f0(mag0, hz)` (§5) — against
     the pristine spectrum, *never* the residual.
   - **Deduplicate**: if the refined pitch is within **70 cents** of an
     already-kept candidate, drop it (but still subtract below).
   - **Subtract the comb** from the residual `mag`: partials
     `k = 1..10` at `k·hz` (stop above `0.45·fs`); at each, multiply
     `mag[b−w .. b+w] *= 0.12` where `b = round(k·hz/bin_hz)` and
     **`w = 2·fft/win`** — that is ±2 *unpadded* bins scaled to the
     transform, the Hann main lobe. ×0.12 rather than zeroing: chord
     tones share partials, and the second tone needs its share to
     survive scoring.
4. **Normalise levels**: divide all kept candidates' scores by the
   frame's top score → `level ∈ (0..1]`, loudest ≈ 1. This is
   balance-within-the-chord, deliberately not absolute velocity.
5. **Tracker** (§6).

## 4. `salience(mag, hz)` — exactly

```
fund = mag_near(hz)                     # magnitude at the candidate's own bin
s = Σ_{k=1..8}  mag_near(k·hz) / k      # stop when k·hz > 0.45·fs
strongest = max over those partials
if fund < 0.15 · strongest: return 0    # missing-fundamental guard
s -= 0.7 · Σ_{k=1..3} mag_near((k−0.5)·hz) / k   # half-harmonic penalty
return s
```

- `mag_near(f)`: the max of the three bins around `round(f/bin_hz)`
  (a cheap peak-tolerant read; out-of-range → 0).
- `1/k` weighting keeps a strong low partial from being outvoted by a
  comb of overtones an octave up.
- The **half-harmonic penalty** is the octave guard: a candidate one
  octave HIGH leaves energy stranded at 0.5f, 1.5f, … that a true
  fundamental would own.
- The **missing-fundamental guard** is load-bearing: an unguarded
  harmonic sum scores a major triad as its chord root an octave below
  every played note — the virtual pitch the ear also hears — and a note
  consumer must act on what was *played*. Without this line the C3-E3-G3
  fixture detects as C2.

## 5. `refine_f0(mag0, hz)` — exactly

For partials `k = 1..4` (skip any whose bin is within 2 of the spectrum
edges):

1. Find the local peak bin `pb` among `b−1, b, b+1` where
   `b = round(k·hz/bin_hz)`.
2. **Log**-magnitude parabola through `pb−1, pb, pb+1`:
   `off = 0.5·(y0−y2)/(y0−2y1+y2)` with `y = log(mag0[...])`. Skip the
   partial if any of the three bins is ≤ 0 or `|off| > 0.6` (not a clean
   peak).
3. Implied fundamental `f0k = (pb+off)·bin_hz / k`. Skip if it disagrees
   with the coarse `hz` by **> 50 cents** — that peak is another note's
   partial and gets no vote.
4. Accumulate weighted by **`mag0[pb] / k²`** — the fundamental's own
   vote dominates, because higher partials are the ones a subset tone
   (an octave or twelfth above) contaminates.

Return the weighted mean (or the coarse `hz` if nothing voted).

Two structural rules here, both measured:
- **Log** magnitude, not linear: a linear-magnitude parabola on a Hann
  lobe is biased at half-bin scale — downstream grids (54.5-cent 22-EDO
  steps) cannot absorb that.
- Refine against **`mag0`**, never the residual: subtraction dents peak
  shapes, and a dented peak refines flat — measured **29 cents flat** on
  a triad's E3, more than half a 22-EDO step.

## 6. The tracker — exactly

State per slot (6 slots): `hz, level, id, active`, plus `miss[]`,
`seen[]`, `cand_hz[]/cand_lv[]` (pending-birth memory), `next_id`.

Per frame, in this order:

1. **Match** each *active* note to the nearest unused candidate within
   **80 cents**. Matched: `hz += (cand − hz) · 0.5` (smoothing — bends
   still track), `level = cand level`, `miss = 0`, candidate marked
   used. Unmatched: `++miss`; at **4** misses the note deactivates and
   its `id` returns to −1.
2. **Births**: each unused candidate looks for a pending slot already
   watching a pitch within 80 cents (`seen > 0`); failing that, a fresh
   idle slot (`seen == 0`). It records itself there; when a slot's
   `seen` reaches **2** (consecutive frames), the note activates with a
   fresh `id = next_id++`.
3. **Pending reset**: any inactive slot whose watched pitch was not
   refreshed this frame drops back to `seen = 0`.

Consequences the consumer can rely on: an `id` is stable for the note's
whole life; a re-struck same pitch gets a new `id`; birth latency is
2 tracker frames, death is 4.

## 7. Cadence and integration

- Run the tracker every **`win/4`** samples on the freshest full window
  (75 % overlap): ≈ every 21 ms at 48 k/4096. Multi-f0 wants overlap,
  and note edges should be known no later than honesty allows.
- Total birth-to-report: window fill (85 ms) is already behind you in
  steady state; a *fresh* chord is seen once the window mostly contains
  it (~40–60 ms) plus 2 frames confirmation (~43 ms) — ≈ 60–90 ms
  behind the strings. Same honesty as the engine's chord sampler.
- **Web Audio notes**: `AnalyserNode` is not usable here — wrong window,
  Float32, its own smoothing, and no access to the exact samples. Feed
  raw samples from an `AudioWorklet` into a ring buffer and run this
  analysis OFF the audio thread (a Worker); at ~46 runs/s an 8192-point
  double FFT plus a ~350-point grid scan × 6 picks is small change for a
  Worker but a stutter risk inside the audio callback.
- The FFT in `polyf0.c` (lines 9–45) is dependency-free and ports to JS
  as-is; `Float64Array` throughout the spectra.

## 8. Acceptance fixtures — hold the port to these

All at 48 kHz. Synthesis: each tone `sin(ph) + 0.30·sin(2ph) +
0.15·sin(3ph)`, summed, × 0.1; feed win-sized frames continuously and
run ≥ 10 tracker frames. (These are the engine's own selftests —
`tests/selftest.c`, `test_polyf0_tracker` / `test_poly_detect_export`.)

| Input | Must hold |
|---|---|
| C3+E3+G3 (130.81 / 164.81 / 196.0) | exactly **3** active notes, each within **20 cents**; no 4th note (no ghosts) |
| same, then silence frames | all notes dead within 6 frames; **zero** candidates during silence (energy gate) |
| A2+E3+A3 power chord | 2 notes (A-octaves merge — documented, don't fight it); the fifth survives |
| G2+B2+D3+G3 | ≥ 3 of the 4 (G3 may merge into G2) |
| bare triad, guard check | with the missing-fundamental guard removed, C3-E3-G3 must (wrongly) detect C2 — if your port *doesn't* fail this way with the guard off, the guard isn't wired where you think it is |
| levels | loudest note ≈ 1.0; all > 0 |
| ids | distinct across simultaneous notes; stable across frames while held |

Reference behaviour of this implementation on the triad: 196.0 / 164.7 /
130.9 Hz — all within 6 cents.

**Cross-check harness (free):** run the engine in `polyMode` on the same
audio and diff your port's notes against `status.polyDetected` — `note`,
`cents`, and `id` lifetimes should agree within a poll tick.

## 9. What is tunable vs what is structural

Tunable (taste, safe to adjust per use): the 0.20 keep threshold, 70 ¢
dedup, 80 ¢ match, 2/4 birth/death counts, 0.5 pitch smoothing, grid
density 2^(1/48), energy gate.

**Structural — change these and it stops working** (each was a measured
failure during development):
1. Subtraction width in **unpadded** bins (`w = 2·fft/win`). Counted in
   transform bins it silently halves when zero-padding doubles the FFT —
   lobe skirts survive and come back as ghost notes.
2. Refinement reads **`mag0`**, the pristine spectrum (§5).
3. **Log**-magnitude parabola (§5).
4. The missing-fundamental guard at `0.15 · strongest` (§4).
5. Pick-and-subtract, not joint selection: scoring all maxima of the
   salience curve without subtraction reads lumpy pseudo-peaks — six
   "notes" for a triad.
6. Floor 4096 on the window (§2).

## 10. UI needs — the whole poly surface in Treebrain, one checklist

Full specs live in `TREEBRAIN-DELTA.md` (§3c the POLY toggle, §3c-ii the
chord sampler, §3c-iii the `polyDetected` consumers); this checklist is
here so the port doc does not travel without the UI story.

**Controls (config keys):**
- MONO/POLY toggle beside the RANGE control — restart-scoped; show the
  latency swing prominently (re-read `processLatencyMs` after toggling
  and re-feed any latency compensation). [§3c]
- `polyNotes` stepper (1–6) beside the instrument picker. [§3c-ii]
- Greyed in POLY: AMOUNT and the correction cluster (retune, tolerance,
  stickiness), `harmLock`, MIDI mode, HOLD, sustain, the **synth**
  source, Attack Sound, `expression`, and the pitch half of FOLLOW
  (envelope half stays live). [§3c]
- Live in POLY — two corrections to §3c's original grey list:
  the **sample** source cluster (instrument picker, `sampleMix`,
  `sampleVelocity`, `sampleVelRefDb`, `sampleRing`) [§3c-ii], and the
  **SCALE mask** — it governs the chord sampler's note snapping and
  `polyDetected`'s `cents`, so editing it in poly is meaningful (it does
  not affect the fixed-ratio *shifted* audio, only the tracker-driven
  paths). [§3c-iii]
- Also live, unchanged: `leadShiftSteps`, formant controls, IR points,
  tilt, harmony voice cluster, the record send, `leadReleaseMs`.

**Readouts:**
- `polyNotesActive` meter (what the sampler is sounding, capped by
  `polyNotes`).
- `polyDetected` consumers, in build order: chord display, polyphonic
  tuner (`cents` per note), chord trace keyed by `id`, host-side
  triggering (births/deaths = id set changes). [§3c-iii]
- `detectedHz`/`targetHz` read 0 in poly and the mono pitch trace goes
  quiet — render as "poly mode", not as a fault. [§3c]

**Feature-detect:** `polyNotes` in the config echo (config chain, §0 of
the delta); `polyDetected` by presence in status.

**If Treebrain runs its own port** (this document's subject): drive the
latency-sensitive widgets — tuner needles, onset flashes — from the
native tracker, and keep the engine's `polyDetected` as the agreement
check; where the two disagree beyond a poll tick, badge it as a
diagnostic rather than silently preferring either.

## 11. Appendix — the mono detector's portable rules

From the earlier detection exchange, restated with exact constants so
this file is self-contained:

- **Analysis window = 2 periods of `min_hz`, no power-of-two padding**
  (the FFT pads internally). Measured: first lock 37.8 → 17.9 ms,
  vibrato lag 17.2 → 8.6 ms at the rig's range.
- **Octave guard with continuity**: prefer the shortest lag whose
  periodicity is within **0.90** of the best; but once locked, a
  different octave must beat **0.95** to take over (hysteresis kills
  frame-alternating octave flips).
- **Onsets clear the continuity memory** — a new note must not inherit
  the last note's octave preference.
- Hop 5 ms; halving it was measured (−1.8 ms lock, −1.3 ms lag,
  +55 % detector CPU) and rejected.
