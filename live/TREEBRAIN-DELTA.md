# AutoEDO → Treebrain: handoff spec for engine `main@cf43a03`

Companion to Treebrain's `docs/AUTOEDO-CONTROL.md`, and a digest of
`live/CONTROL.md` (the authoritative reference — this file is the *delta*,
written for the controller author, and it says why as well as what).

Everything below is live on the engine's `main`. The engine has no version
key and doesn't need one: **feature-detect by key presence in the
`/api/status` config echo**, exactly as Treebrain already does. Probe keys,
in the order the batches landed:

`harmGainDb` → `harmHold` → `leadShiftSteps` → `attackSound` →
`sampleInstrument` → **`expression`**

A rig that sees `expression` echoed has everything in this document.
`status.engineBuild` carries the git short hash of the running binary —
put it in the diagnostics panel and in every future report, so "which
build" is never a question anyone has to ask.

Sections are ordered by how much they change what the user hears.

---

## 1. `expression` — correct the note, not the playing

**The field report that started it:** pitch-shifting pedals keep tracking
your bends and vibrato once they've found the note, and it transfers
audibly to the output. Ours didn't.

**It couldn't, by construction.** At `amount: 1` the correction law is

```
shift = target_cents − detected_cents        (target = a fixed degree)
```

When the player bends, `detected` rises and `target` does not, so the
shift cancels the bend exactly as it happens — the harder you bend, the
harder the engine bends back. Every scrap of expression was removed on
purpose, and the ghosts inherited it because they anchor to the corrected
lead.

**The fix.** A played pitch is a note plus what the player is doing to it.
Track the note's **centre** with a follower slower than any bend or
vibrato but faster than a phrase; the difference between the centre and the
instantaneous pitch *is* the bend, the vibrato, the scoop. Correct the
centre alone and add the deviation back:

```
centre_cents += (detected_cents − centre_cents) · (1 − exp(−dt / 0.180))
expr_cents    = detected_cents − centre_cents
degree        = nearest_enabled_degree(centre_cents)     // NOT detected_cents
out_cents     = degree_cents + expr_cents · expression
shift         = out_cents − detected_cents
```

### The key

| Key | Type | Applies | Default |
|---|---|---|---|
| `expression` | float 0–1 | live | **1** |

`1` — bends and vibrato reach the output, and the harmony, while the note
still lands on its degree. `0` — the old behaviour, output pinned to the
degree.

**Default-on is safe.** A steady note is *identical* either way: the
follower has converged, `expr_cents ≈ 0`, and nothing changes. Only motion
faster than the follower is at stake, which is exactly the expression. So
the default changes what the engine does to *playing* without changing
what it does to a *held note*. Surface it as one slider — "Expression" or
"Bend / vibrato transfer" — and let the robot-voice users dial it to 0.

**Measured:** whole tone bent over 400 ms, sampled mid-bend — **0 ¢** from
the degree at `expression: 0`, **37.6 ¢ / 43.5 ¢** at `expression: 1`.

### Apply the same law to TENDRIL capture snap and `fxSnapSteps`

This is the part worth Treebrain's own engineering time. **Any** place
Treebrain measures a pitch, snaps it to a grid, and drives a shifter with
the difference has this identical bug — capture snap is eating the
performance's expression the way the engine was. 180 ms is a good starting
constant: comfortably under vibrato (5–7 Hz) and comfortably over a
deliberate slide.

Two consequences fall out of snapping the centre rather than the
instantaneous pitch, both wanted, and both of which Treebrain inherits if
it ports the law:

- **Degree stability.** A vibrato wider than half a step used to flip the
  target back and forth. In 22-EDO a step is **54.5 cents** — a guitarist
  crosses that without trying, so the flipping is not an edge case here the
  way it is in 12. Stickiness was papering over it; centre-based selection
  removes it.
- **The octave re-vote guard has to change with it.** It used to require
  `detected` and `target` to jump an equave *together* — which can no
  longer happen, because the target now follows the centre. It must test
  the **detected jump alone** and take the whole jump into the centre. No
  player moves an octave in 5 ms, and a real octave leap wants exactly that
  response, so the rule is right either way. Port the law without the
  guard and a detector octave doubling will *glide* instead of snapping,
  audibly.

### Don't double-apply

A Treebrain-side split upstream of the engine composes fine — the engine's
law runs on what it detects at its own input. But if Treebrain ever
pre-corrects pitch before handing audio to the engine, write `expression`
explicitly rather than leaning on the default.

---

## 2. Sample voices (the Xentar pitched library)

`harmSource`, `leadSource` and `hSrc[]` all take **`"sample"`**. Bind the
library with `samplePath` / `sampleInstrument` (+ optional
`sampleManifest`, and `sampleHash` as the change token — the bank is
re-read only when the binding actually moves).

### 2.1 Keys

| Key | Type | Applies | Meaning |
|---|---|---|---|
| `samplePath` | string | live (reloads) | absolute root of the per-rate cache; the engine reads `<samplePath>/<instrument>/*.wav` |
| `sampleManifest` | string | live (reloads) | optional file list. **Any JSON shape works** — the engine harvests the `".wav"` strings and derives instrument, zone, layer and variant from the filename, which carries all four. No schema to agree on; a manifest change cannot break the engine. Absent → the directory is scanned |
| `sampleHash` | string | live (reloads) | the librarian's "library changed" token. 120 WAVs is not a per-POST operation |
| `sampleInstrument` | string | live (reloads) | which folder to load. **No whitelist in the engine** — see §2.2 |
| `sampleMix` | float 0–1 | live | layer blend against the shifted rendering of the same ghost. `0` = shifted alone, `1` = sample alone, **`0.5` = both at unity** — see §2.7 |
| `sampleVelocity` | float −1…1 | live | fixed strike level; **`-1` (default) = measure it** from the lead's attack |
| `sampleOctave` | `"auto"` \| int −24…+24 | live (reloads) | filename→**sounding** pitch offset — see §2.4 |

Status: `sampleInstruments`, `sampleNormDb`, `sampleOctaveApplied`,
`sampleClipped`, `sampleVelLast`, `sampleZones`, `sampleFiles`,
`sampleError`. A failed load **leaves the running bank playing** — a bad
path mid-set is a message, not a hole.

### 2.2 Build the SOUND picker from `sampleInstruments`, not a hardcoded list

The engine scans `<samplePath>/` at load and reports every subdirectory
holding at least one file the zone parser recognises, sorted, as
`status.sampleInstruments`. **Build the picker from that, exactly as the
synth picker is built from `synthPatches`.** Adding an instrument is then
dropping a folder in the cache — no engine change, no controller change.

Adding pizzicato needed no engine change at all for this reason;
what it *did* need was killing the hardcoded list of eight in the engine's
own built-in UI, which made a ninth invisible. Treebrain will have the same
list in the same shape.

A configured instrument the cache no longer holds should stay in the
picker **marked missing**, rather than silently vanishing and leaving the
control disagreeing with the engine.

Shipped: `piano` `electric` `acoustic` `bass` `vibraphone` `choir`
`harpsichord` `oboe` `pizzicato`.

### 2.3 Per-set mastering spread is ~21 dB — measure, don't use a constant

The sampler-dump delta's correction was that `levelConst` (1.25 / 0.56) is
the *entire* runtime override system and the real inter-instrument balance
is baked into the **file mastering**, with sets peak-normalised to targets
~16 dB apart. Measured here over the first 300 ms of each main-layer
recording the spread is wider still: **piano −10.7 dBFS against acoustic
−31.9 — 21 dB.** Ship "no per-instrument constant" and switching from
piano to acoustic drops the ghosts by that much.

**Spec:** each bank **measures itself at load** — median RMS over the first
300 ms of the **MAIN-layer** recordings — and is normalised to a common
**−22 dBFS** reference. Use the same target and the two rigs match.
Reported as `sampleNormDb`.

Two details that matter:

- **Median, not mean.** See §2.5 — a mean over a partly-corrupt bank gets
  dragged badly; the median held the error to 1.4 dB with 23 of 78 files
  full-scale.
- **Exclude the soft layer, and normalise per BANK not per file.** Soft
  files are peak-matched to their main layer *on purpose*; normalising per
  file destroys exactly the relationship that makes the soft/main swap a
  timbre change rather than a level change.

Rather than copying Xentar's constants — which the delta itself warns
produces a different mix on differently-mastered files, and which needs a
new entry for every future set — measuring keeps the promise §2.2 makes: a
set dropped into the cache at *any* mastering arrives usable.

Not adopted, and Treebrain shouldn't either: `_isLoudSynth` / `_CHIP_GAIN`
(browser oscillator gains — this engine has its own measured patch table),
and pizzicato's GM program number (no MIDI export here).

### 2.4 Filename octave ≠ sounding octave on two sets

The delta states explicitly what was already true:

- **bass** filenames sit one octave **above** sounding pitch → **−12**
- **harpsichord** filenames sit one octave **below** → **+12**

The other seven sets are correct as named. Xentar's zone map and rate math
stay self-consistent through this because it plays the *recorded* pitch —
but if the contract is "a ghost asked for E4 SOUNDS E4", both sets are a
clean octave off. **If Treebrain's player maps by filename, bass is
currently an octave high and harpsichord an octave low.** Bass sounding an
octave high is very likely audible as "that doesn't sound like a bass".

The offset is folded in as files are indexed, so everything downstream is
in sounding pitch: **bass indexes B0–A3, harpsichord A1–F6** — the two
instruments' actual ranges. `sampleOctave` (`"auto"` by default) covers any
future set not in the built-in table; `sampleOctaveApplied` reports what
took effect. Keep the override — it's how a future mis-mastered set gets
fixed without a code change.

### 2.5 Count full-scale recordings and report them

The first pizzicato drop shipped **32 corrupted files**: the VSCO-2 CE
cello and viola pizz sources are 24-bit WAV and the transcode decoded them
as 16-bit. They loaded cleanly, parsed to the right zones, and played
perfectly happily — **as full-scale noise.** Nothing anywhere said so.

A properly mastered set peaks *below* full scale, so a recording sitting
exactly at it is the signature of a decode fault, not a mix decision.

**Spec:** count samples at |s| ≥ 1.0 − ε per file, sum per bank, surface
it. The engine reports `status.sampleClipped` — **23 on the original set,
0 on the re-export.** Anything above 0 means re-transcode that set, and
the count says how much of it. Put it on the panel: this has now happened
once, so it will happen again, and the failure mode is silent.

Second reason to report it: it skews the level measurement in §2.3.

### 2.6 Two live candidates for "samples are silent in Treebrain"

Still open on your side. In order of cheapness to rule out:

1. **The `#` URI-encoding trap.** Sharp filenames (`C#4`) break when a path
   is used as a URI without encoding — `#` starts a fragment, so the
   request silently fetches a truncated path or 404s. The pizzicato README
   warns about exactly this. **If Treebrain fetches over HTTP anywhere in
   its sample path, `encodeURIComponent` each segment.** (The engine's
   parser accepts both spellings — `F#1` and `Fs1` are the same zone — so a
   set using `#` and one using `s` sit side by side in one cache.)
2. **Wrong-rate cache files.** The engine **refuses** any file whose header
   rate isn't the engine's, and names it in `sampleError`, rather than
   resampling or playing garbage — a cache at the wrong rate plays at the
   wrong pitch *and* the wrong speed. Recommend the same: a loud refusal
   beats a silent zero.
3. The `isSampleWave()` / `GUITAR_SAMPLE_FILES` gate on your side.

### 2.7 Where a straight port of a browser sampler goes wrong

A ghost is continuous; a sample is **struck**. The engine strikes a sample
voice at the lead's **onset** — the same energy edge the attack sound fires
on, several detection hops *before the pitch is known* — and then
**re-pitches, never re-strikes**, for as long as that note lasts. A
retrigger starts a fresh slot across a 6 ms fade rather than cutting a
sounding one (Xentar's node-swap discipline, ported to a language with no
nodes).

- **Velocity is a measurement.** Peak over the first 30 ms from the foot of
  the attack, across 40 dB (−40 dBFS → 0, 0 dBFS → 1). The strike cannot
  *wait* for that window without giving back the latency the feature exists
  to hide, so a voice is struck at the fast follower's reading and the
  window's verdict refines it over a ~15 ms ramp — a level correction that
  slides reads as the note settling; a step reads as a second event. Level
  only: the soft/main layer is a different recording, committed at the
  strike, and is the one thing measuring cannot fix without delaying it.
  Echoed as `sampleVelLast`. `sampleVelocity ≥ 0` pins it instead.
- **The soft layer is TIMBRE, not level.** `<Note>_soft` files are
  softer-*played* recordings peak-matched to the main layer; loudness comes
  only from the velocity gain. Below velocity 0.6 the soft pool is
  preferred **where one exists** — harpsichord has none by design, and
  individual zones can lack one (pizzicato's D2); both fall back to the
  main pool rather than being special-cased.
- **Round robin is per (instrument, zone, layer) and never the immediately
  previous pick.** A repeated note that reuses its recording machine-guns
  at once. Selftest: 200 picks, zero immediate repeats.
- **Zone then RATE.** Nearest recording by pitch, then a fractional,
  **unquantised** playback rate — that is what lands a 22-EDO degree.
- **`sampleMix` is a layer, not a swap.** Below 1 the voice's shifter keeps
  running. `0.5` = both at unity: a **plateau at centre with an equal-power
  taper either side**, `g = sin(min(1, 2·d)·π/2)` per side (`d` the
  distance from the far end) — *not* the textbook crossfade that would drop
  both to 0.707 in the middle.

  **↳ Open question for you.** If Treebrain's layer law differs in the
  taper, the same velocity produces a different timbre in the two rigs.
  The plateau-at-0.5 constraint you gave determines the law only up to the
  curve. **Tell me yours and I'll match it** — I'd rather converge than
  have you chase mine.

One characteristic worth knowing rather than fixing: the onset detector is
a **ratio** test, so a note much quieter than the last one, soon after it,
is inside the previous note's decay and is deliberately *not* an edge.
That governs the attack sound too.

### 2.8 `harmLock: "mask"` breaks a tie away from the lead

No key added. On a tie the walk now goes **away** from the lead — up for a
ghost above, down for one below — so a third stops collapsing onto a second
or a unison at those degrees. A unison ghost keeps up-first (no apartness
to preserve), and so does the plain lead-correction walk, which matches
your TENDRIL carve-out exactly: a detected note being corrected onto the
mask has no second voice to stay away from. An existing mask patch just
stops collapsing.

---

## 3. The record send

1. **The send exists.** `sendChannel` (int 0–32, **restart** — it lives in
   the device channel map, same scope as `outputChannel`; batch the two),
   `sendContent` (`"wet"` default | `"full"` | `"lead"` | `"harm"`, live),
   `sendGainDb` (−60…+12, live, never passes through `outputGainDb`),
   `sendOn` (live, click-free mute, **default off, never asserted on at
   launch** — assert it at song load, drop it on panic, alongside the
   `harmOn` guards). `"wet"` is a true stem: the engine's contribution with
   the dry blend removed — near-silent while unvoiced rather than passing
   the dry through, so it mixes against the dry rows without
   double-counting. `"lead"` is pre-lead-IR; noted so the recorded stem's
   space differs from the PA's if a lead IR is loaded.
2. **Latency is reported.** `processLatencyMs` in status (alias of the
   long-standing `latencyMs`, added under the name you probe for):
   input-to-output at current settings, algorithmic plus buffering. It
   moves with `quality`, devices and `bufferFrames` — re-read from the echo
   after any restart-scoped write. Feed it to `captureOffsetMs`
   automatically; keep the by-ear trim as an override.
3. **No cable**: option (a) as you preferred — aim `sendChannel` at a spare
   output pair and let a loopback-capable interface close it internally.
   The UDP/shared-memory tap was considered and deliberately not built
   while the send covers the need; say the word if it stops covering.
4. **Nothing else changes, and collisions are refused, not summed** — from
   both directions: a `sendChannel` write landing on the live output OR an
   `outputChannel` write landing on the send keeps the old value and puts
   the reason in `status.sendError` (`""` after any accepted send write).
   Surface it like `irError`.

Also landed: **`formantSemitones`** (float −12…+12, live) — the formant
offset flagged as nice-if-cheap. It was: the library call always took a
semitones argument and the engine always passed zero. `+` toward a smaller
instrument, `−` larger; composes with `formantHold` either way (hold-then-
offset, or follow-pitch-then-offset), and engages the stage even on a
`formantHold: false` guitar channel. Applies to the lead and every shifted
ghost.

**M4 recipe** replacing SETUP.md §5's full-mix return: voice engine
`{"outputChannel":3, "sendChannel":5, "sendContent":"wet", "sendOn":true}`
(guitar likewise on its own pair), record the loopback/return of the send
channel, align by `processLatencyMs`. The return is now a stem,
re-balanceable after the show, and `sendGainDb` sets record level without
touching the stage.

---

## 4. Field-fix batch — read this first if the guitar was bassy

- **`midiOctaves`** — `"nearest"` (new default) | `"held"`, live. MIDI
  Harmony used to snap to the nearest held note in its **absolute octave**:
  chord voicings octaves below the lead line became a standing transpose,
  retuning the guitar down into the chord's register — a real "incredibly
  bassy" mechanism, present even with perfect detection. `nearest`: the
  held note names the pitch **class**, the player names the register.
  `held` restores absolute for charts that deliberately place the voice's
  octave. **If the guitar channel runs MIDI mode, this was almost certainly
  the bassiness** — no Treebrain change needed beyond re-reading the echo,
  since `nearest` is the default.
- **`formantHold`** — bool, live, default true. **Set `false` on the guitar
  channel**: formant preservation exists for vocal tracts; off removes the
  stage from the path entirely (and off the suspect list).
- **Release guards** (no keys, always on): a mute/lift bends the string
  while the level collapses; harmony retargeting now freezes during the
  collapse and ghost pitches rewind ~40 ms at note end — "the pitch that
  was stable while held no longer changes at release" (measured 44.9 ¢ of
  release drift before, < 15 after). HOLD exempt.
- **Attack Sound double-fires fixed** — the trigger is a Schmitt now (one
  hit per onset edge); no controller change.

---

## 5. The rest of the config keys

All `POST /api/config`, all echoed.

### `leadShiftSteps` — the SHIFT control
int, −72…+72, live. Static transpose of the corrected lead in EDO steps,
applied **after** the snap: the detector, tolerance, stickiness and retune
all run against the real note; the shift only moves what comes out. `±edo`
is an exact equave and keeps the pitch class — the degree mask never
notices. **Locked ghosts stack their intervals on the shifted lead degree**
(mask walk included), so harmony stays a scale interval from the note the
audience hears. `targetHz` and the pitch trace echo the **shifted** target —
draw them as-is.

One rail to surface: the audio path safety-clamps the *total* shift
(correction + transpose) at ±36 semitones. In steps that is
`±floor(3600 · edo / periodCents)` — ±36 in 12-EDO, **±66 in 22-EDO** — so
show a soft limit there rather than letting the fader imply ±72 always
lands.

The pairing worth a macro on the guitar channel: `detectMinHz` up off the
low strings + `leadShiftSteps: +edo` = octave-up lead without asking the
detector to work down where the subharmonics live.

### `harmHold` — momentary HOLD (bool, live)
Freeze every ghost at its current pitch and level, ring it indefinitely,
lead keeps tracking normally.

- **Momentary, not a toggle.** CC gate: value ≥ 64 → `{"harmHold":true}`,
  < 64 → `false`. Send **edges only**, not every CC frame. A sustain-pedal
  CC (64) behaves exactly right; for a latching switch, hold the state in
  Treebrain and send the edges.
- Rising edge snapshots once (the sustain loop + the level the choir holds
  at); nothing sung afterwards retargets the choir.
- Still live while held: `harmGainDb`, `hg`, `hp`, `hMute`, `hSolo`,
  `harmTiltDb`, IR. `hm`/`hx`/`hd` are inert until release.
- **Forced false at launch** (like `harmOn`/`droneOn`). Never a saved state.

### `harmSustain` — bool, live, **default ON** (behaviour change)
Shifted ghosts ring through `synthReleaseMs` after the input stops: the
engine loops a period-aligned, crossfaded slice of the end of the note into
the harmony shifters. Bounded entirely by the release — short release, no
audible change. Expose as a toggle; a rig that wants the old hard cut sets
it false.

### `harmGlideMs` — float 0–5000, live (semantics changed if you shipped early)
Ghost portamento, **constant-time**: linear in cents, arrives in this many
ms, then locks on and tracks the lead's vibrato exactly. It is *not* a
one-pole time constant — an earlier build's exponential never truly landed,
so if you calibrated against that, **recalibrate**: the control now means
what it says. `0` = jump. One number for both ghost sources. A voice
entering from silence starts on pitch (no swoop); consonants and
note-change frames do not restart a glide in progress; and turning harmony
off and back on clears the tracked portamento note.

### `hd` — float[5], −100…+100, live: per-voice detune, cents
Applied after the `harmLock` quantize (the lock can't snap it away) and
before the dedupe (two voices on one interval, detuned apart, both sound).
The headline: **`hm[v]=0` with nonzero `hd[v]` is a UNISON ghost** — e.g.
`{"hm":[0,…],"hd":[-4,…]}` is the −4-cent thickener. `hm=0, hd=0` stays
"voice off" (an exact unison is a comb filter, not a voice).

### `refNote` / `refNoteHz` — state the pitch standard as C
`refNote` int 0–11 (0 = C, the default); `refNoteHz` float = that note's
frequency in octave 4. Parsed after `refA4`, so a POST carrying both lands
on these. Recommended link-up assertion for the C-standard rig:

```json
{"rootNote": 0, "refNote": 0, "refNoteHz": 261.6256}
```

Background, worth encoding in Treebrain's guardrails: the EDO grid hangs
off degree 0 (`rootNote`). With root C, **C is the same frequency in every
EDO** and the degree you'd call A lands wherever the EDO puts it — 433.12
or 446.99 Hz in 22-EDO, never 440. `refA4` is only the 12-EDO arithmetic
that derives the anchor. If any preset or operator has nudged `refA4`
because "A isn't 440", that moved C and the whole rig — and past ~27 cents
of error in 22-EDO it flips which degree notes snap to (wrong notes, not
detune). **Treebrain should treat `refA4 ≠ 440` on this rig as a lint
warning.**

### `attackSound` / `attackGainDb` — onset transients
`attackSound`: `"off" | "noise" | "pick" | "click"`, live, default off.
`attackGainDb`: float −60…+12, live, default −26.

A transient fired the moment a note's **energy** appears — several
detection hops before the pitch is known. Purpose: cover the ghosts' attack
latency; the intended pairing is a *long* `synthAttackMs` (the envelope
hides the note arriving) plus the attack sound (covers the pick itself).
Outside every envelope, own gain. Fires for ghosts of **either** source and
for a synth lead; a rig with nothing on the bus gets no hits. ~80 ms
refractory = one hit per pick.

- `"pick"` is the Xentar pick-noise set (Build 2753), **embedded in the
  engine**: 3 ranges × 2 directions, range from last known pitch, direction
  from the economy-picking state machine, ±8% rate / ±26% level jitter per
  hit, each hit volume-matched to the note's own onset level. No files to
  deploy.
- `attackGainDb` is **relative to the note's own onset** (soft note, soft
  pick). −26 is Xentar's shipped "felt more than heard"; near 0 for the
  attack-cover job.
- Suggested macro: "Pad + pick" = `{"synthAttackMs": 400,
  "attackSound": "pick", "attackGainDb": -6}`.

### `detectMinHz` / `detectMaxHz` and `range: "guitar"`
Explicit detection window (restart-scoped, like `range`); 0 = use the
preset. `"guitar"` preset = 78–1400 Hz. On the guitar channel set
`detectMinHz` to the real bottom of the instrument — **a subharmonic
outside the window can't be voted for at all.**

---

## 6. Changed semantics (no new keys)

- **`hg` means "relative to the lead" now.** 0 dB = the ghost sits at the
  lead's level (pan law normalised to unity-at-centre, and the shifter is
  level-matched across its formant stage, which used to cost 4–6 dB on
  upward shifts). Ceiling raised **+6 → +12**; floor unchanged at −60.
  Re-scale any knob that mapped to the old range.
- **`harmOn`, `droneOn`, `harmHold` are forced off at engine launch.**
  Still written to the settings file, still echoed — only the launch value
  is forced. Treebrain's link-up `{"harmOn":false}` assertion and the
  song-top 4-second guard are now redundant; keeping them is harmless.
- **`synthEnvActive` is gone.** It existed for part of one day between
  builds; the envelope now applies to both ghost sources. Grey nothing on
  its account; don't read it.
- **`synthAttackMs` / `synthReleaseMs` scope**: both sources now. On a
  shifted ghost the release is real (see `harmSustain`); floors at an
  internal 5 ms click-guard.
- **`targetHz` and `trace`** reflect `leadShiftSteps` (shifted target).

---

## 7. Engine fixes with no wiring — but field behaviour changes

- **The super-bassy corrected guitar lead is fixed, and the cause was not
  what it looked like.** Two mechanisms, found in this order:
  1. The detector re-voting the octave of a sustained note (dominant 2nd
     harmonic → octave-high vote at the pluck, re-vote mid-note), swinging
     the correction ratio through an octave for ~`transitionMs` after every
     re-vote — measured 10.8-semitone spikes, now < 0.6.
  2. **The real one:** the shifter's formant-stage makeup gain compared
     input-*now* against output-*now*, while the output is the input from
     `latency` ago. On plucks that error pumps the gain, the pumped lead
     drove the output soft clip, and sustained saturation reads to the ear
     as thick and dark. Steady tones cancel the error exactly, which is why
     every synthetic test passed while the guitar rig pumped. Fixed with a
     delay-aligned input tracker.

  The Lead-button diagnostic (bassy with lead, clean with unison ghost) is
  explained and obsolete. If the rig carries a "guitar goes bassy"
  mitigation, retire it.
- **Ghost parity and anchoring:** at `hg` 0 a ghost sits at the lead's
  level, and every ghost stacks its (snapped) interval on the pitch
  actually being heard — the corrected lead when `leadOn`, the played note
  when the lead is muted. Relevant to the harmony-only guitar
  configuration: with `leadOn: false` the ghosts follow the amp'd guitar
  itself.

---

## 8. Diagnostics worth putting on the panel

All already in `/api/status`. These exist so a field fault arrives as data
instead of a description.

| Key | Why it earns panel space |
|---|---|
| `engineBuild` | git short hash of the running binary. First question on any report, answered without asking |
| `shiftSt`, `shiftStMin`, `shiftStMax` | instantaneous and windowed lead shift, semitones. **The single most diagnostic number in the block** — a lead that sounds wrong with a `shiftStMax` of 10.8 is an octave re-vote, not a tone problem. Graph `shiftSt` next to the pitch trace |
| `leadMakeupDb` | the shifter's formant-stage level match. If this pumps, the lead pumps — see §7 |
| `outPeakDb` | output peak. Sustained near 0 means the soft clip is saturating, which reads as "thick" or "bassy" |
| `leadGainDb` | needed for step 3 of the bisection |
| `processLatencyMs` | drives `captureOffsetMs` |
| `sendError`, `irError`, `sampleError` | non-empty = a write was refused, with the reason |
| `sampleClipped`, `sampleNormDb`, `sampleOctaveApplied` | §2.3–2.5 |

**Port the 5-step wrong-lead bisection** (`live/CONTROL.md` §4, "Diagnosing
a wrong-sounding lead") into whatever runbook Treebrain shows the user. It
resolved the bassy-lead hunt in a single round of field testing:

1. Read `shiftSt` / `shiftStMax` — rules the pitch path in or out at once.
2. `amount: 0` — rules out the tuning maths.
3. Ghost-as-control: `leadGainDb: -60`, `leadOn: true`, unison ghost —
   isolates the lead path from the shared output path.
4. Four lead-only stages (a–d): formant, makeup, clip, pan.
5. Bare restart vs `quality`.

---

## 9. CC / posting guidance

- `harmHold`: **edges only** (§5).
- `harmGainDb`, `attackGainDb`, `hg`, `hp`, `expression`: safe to sweep —
  the engine smooths (~5 ms) — but coalesce CC floods; one POST per UI tick
  (≤ ~30 Hz) is plenty. Every POST returns the full fresh echo.
- `leadShiftSteps`, `hm`, `hx`: step-valued; post on change only.
- Restart-scoped keys (`detectMinHz`, `detectMaxHz`, `range`, `quality`,
  devices, `bufferFrames`, `sendChannel`, `outputChannel`): batch into one
  POST — each restart-key POST restarts the engine once.
- Sample binding keys reload the bank; move `sampleHash` only when the
  library actually changed.

---

## 10. Update checklist

**Do first — audible:**

1. Add the **`expression`** slider (0–1, default 1). §1.
2. Apply the note-centre / expression split to **TENDRIL capture snap and
   `fxSnapSteps`**, with the octave-re-vote guard restructured to test the
   detected jump alone. §1.
3. Fix the **bass −12 / harpsichord +12** sounding octave in Treebrain's
   own player. §2.4.

**Do next — the silent-samples hunt:**

4. `encodeURIComponent` every path segment in the sample fetch path. §2.6.
5. Refuse and report wrong-rate cache files instead of failing silently.
   §2.6.

**Correctness and staleness:**

6. Measure each bank at load (median RMS, first 300 ms, main layer only)
   and normalise to −22 dBFS; drop `levelConst`. §2.3.
7. Count full-scale samples per bank and surface it. §2.5.
8. Build the SOUND picker from `status.sampleInstruments`; mark a
   configured-but-missing instrument rather than dropping it. §2.2.

**Carried forward from the earlier batches:**

9. Light SHIFT from the `leadShiftSteps` echo; soft-limit at
   `±floor(3600·edo/periodCents)` steps.
10. Map HOLD to a momentary CC, **edges only**; drop any latching
    assumption.
11. Add the `harmSustain` toggle (default-on behaviour change).
12. Recalibrate any portamento presets made against the exponential glide.
13. Add per-voice detune (`hd`) and the unison-ghost affordance
    (`hm:0` + `hd≠0`).
14. Replace `refA4` handling with the `refNote`/`refNoteHz` link-up
    assertion; lint `refA4 ≠ 440`.
15. Add Attack Sound controls; consider the "Pad + pick" macro.
16. Rescale `hg` controls to −60…+12 with 0 = lead parity.
17. Add the record send controls and feed `processLatencyMs` into
    `captureOffsetMs`.
18. Retire the bassy-guitar mitigation and (optionally) the `harmOn`
    link-up assertion.
19. Delete any reference to `synthEnvActive`.
20. Put the §8 diagnostics and the 5-step bisection on the panel.

---

## 11. Open on my side

- **Your layer-blend taper.** §2.7 — tell me the curve and I'll match it.
- **Samples silent in Treebrain's own player.** Three candidates in §2.6;
  I have no visibility into which. If none of them is it, send me the
  failing path and I'll look.
