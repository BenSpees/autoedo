# AutoEDO → Treebrain: handoff spec for engine `main@HEAD`

Companion to Treebrain's `docs/AUTOEDO-CONTROL.md`, and a digest of
`live/CONTROL.md` (the authoritative reference — this file is the *delta*,
written for the controller author, and it says why as well as what).

Everything below is live on the engine's `main`. The engine has no version
key and doesn't need one: **feature-detect by key presence in the
`/api/status` config echo**, exactly as Treebrain already does. Probe keys,
in the order the batches landed:

`harmGainDb` → `harmHold` → `leadShiftSteps` → `attackSound` →
`sendChannel` → `sampleInstrument` → `expression` → `sampleRing` →
`sampleVelRefDb` → `followEnv` → `polyMode` → **`polyNotes`**

This is the one authoritative ordering — it is the order the batches
actually landed in, and `sendChannel` sits where it does because the record
send shipped before the sample work. A rig that sees `polyNotes` in
the **config echo** has everything in this document, including the chord
sampler (§3c-ii); `polyMode` without `polyNotes` is poly shifting only. Probing any key later
in the chain implies every key before it. (`sampleRing` and the three keys
beside it landed one build earlier, so a rig that has `sampleRing` but not
`sampleVelRefDb` has everything except §2.9's supplied reference.)

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
| `sampleVelocity` | float **−1…1** | live | fixed strike level. **Any negative value = measure it**; `-1` is canonical and the default. `0…1` pins and bypasses the map (a pinned `0` is silence, not the floor). The range is −1…1, not 0…1 — that is the answer to "pick one": the sentinel has to live in the same key, and it is echoed as `-1` |
| `sampleRing` | bool | live | **let-ring**, default `true` — see §2.8 |
| `sampleVelRefDb` | `"auto"` \| float −60…0 | live | **supply the strike map's reference** instead of letting the engine observe one — see §2.9 |
| `sampleOctave` | `"auto"` \| int −24…+24 | live (reloads) | filename→**sounding** pitch offset — see §2.4 |

Status: `sampleInstruments`, `sampleNormDb`, `sampleOctaveApplied`,
`sampleClipped`, `sampleVelLast`, `sampleVelRefDb`, `sampleZones`,
`sampleFiles`, `sampleError`. A failed load **leaves the running bank playing** — a bad
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

- **Velocity is a measurement, and it is RELATIVE — your map, adopted
  verbatim.** See §2.9; the absolute window this document previously
  specified was the defect you had already fixed, and re-shipping it was my
  error. The strike cannot *wait* for the 30 ms window without giving back
  the latency the feature exists to hide, so a voice is struck at the fast
  follower's reading and the window's verdict refines it over a ~15 ms ramp
  — a level correction that slides reads as the note settling; a step reads
  as a second event. Level only: the soft/main layer is a different
  recording, committed at the strike, and is the one thing measuring cannot
  fix without delaying it. Echoed as `sampleVelLast`, with the reference it
  is relative to as `sampleVelRefDb`.
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

### 2.8 `sampleRing` — let-ring

bool, **default `true`**. A struck sample voice plays to its natural end
*through* the next strike. `false` is the previous behaviour:
damp-on-repitch legato, the sounding voice retired across a 6 ms fade as
the new one is struck.

Default-on because it is what the instruments in this library actually do
— pizzicato, piano and vibraphone all ring past the note that follows them,
and damping every one of them at the next onset is a choice nobody made
deliberately. A fast figure now stacks into the chord a real instrument
would leave ringing, because the next note is a **new string**, not this
one being re-fretted.

Implementation notes that matter to a controller:

- **Four playback slots per voice**, not two. Two is enough to crossfade a
  retrigger past a dying note; let-ring needs a ring, because every slot is
  a note still sounding on its own decay. A fifth strike steals the slot
  furthest through its recording — the oldest, and by then the quietest, so
  it is the least audible steal available.
- **Only the most recent strike is re-pitched.** The others keep their own
  pitch, which is what makes the ring a chord rather than a glissando.
- **The release is the ceiling, and it starts at supersession.** From the
  moment the next strike lands, the superseded note decays under
  `synthReleaseMs` (ghosts) / `leadReleaseMs` (lead), natural end if
  sooner. Let-ring does not mean unbounded. *(Regression note: the first
  build of this feature documented the ceiling and did not implement it —
  superseded notes rang to the recording's natural end, up to four
  many-second recordings deep, default ON. In the field that was reported
  as "the bassy corrected note is back": a sustained low-mid wash under
  everything, same ear-description as the old soft-clip saturation,
  entirely different mechanism. Fixed and now under test; the discriminating
  test stages three notes under a 120 ms ceiling and asserts the first is
  gone by the third.)*
- **Turning it off damps what is already ringing** rather than stranding
  it, so the switch is heard as the damper coming down instead of as
  nothing until the next note.

### 2.9 The strike velocity map is RELATIVE — your map, adopted verbatim

**You were right and I re-shipped a defect you had already fixed.** The
previous version of this document specified velocity from an absolute
window (−40 dBFS → 0, full scale → 1). That is what the engine did, and it
was wrong for exactly the reason your 2026-08-05 addendum gives: a real
interface leaves 12–20 dB of headroom, so hard playing peaks at −12…−20
dBFS and never approaches full scale. The map was scoring the gain staging
rather than the playing — every velocity on the rig sat in the bottom half
of its range, and quiet notes fell off the bottom and vanished.

Adopted verbatim, constants included:

```
ref   = max(refPeak, peak)            // never asks for more than unity
below = 20·log10(peak / ref)          // <= 0 dB
t     = clamp(1 + below / 24, 0, 1)   // a 24 dB window below the reference
vel   = 0.2 + 0.8·t                   // floor 0.2
```

**24 dB window, 0.2 floor.** Treat these as a contract between the two
rigs: changed in one place and not the other, the same playing strikes
different velocities on each. They are named constants in
`live/src/corrector.h` with a comment saying so.

Everything else you specified is unchanged: the fast-follower strike with
the ~15 ms refinement stays, the soft/main threshold is still 0.6, and the
layer is still committed at the strike.

**The reference is the caller's, and here the engine is the caller**, so it
keeps its own — a rolling peak of measured onsets decaying over ~20 s, the
same shape as your FX layer's. Three details worth matching:

- Raised **only by measured onsets**, never by sustain, so a long held note
  cannot talk itself into being a hard strike.
- Never decays below −40 dBFS, so a silent rig maps its own noise floor to
  the velocity floor rather than to fortissimo.
- **Starts at that floor, not at a plausible seed.** Seeding it with a
  likely "playing hard" level sounds reasonable and is not: the seed then
  outranks the player until it decays, and every note until then is scored
  against a number nobody played. Starting low costs exactly one note — the
  first of a set reads as the loudest so far, because it is — and the first
  genuinely hard note fixes it for good. (I tried the seed first. It put
  the hard note at 0.85 instead of 1.0 and the 12-dB-down note at 0.49
  instead of 0.60, which is the same bug in a smaller size.)

Reported as **`sampleVelRefDb`** in status. A relative map is unreadable
from outside without it — `sampleVelLast: 0.55` says nothing until you know
what it is 0.55 *of*.

#### Supplying the reference: `sampleVelRefDb` (the key you asked for)

`"auto"` (default) | float −60…0 dBFS, live.

The reference is the caller's by definition — "how hard this player plays
when playing hard" is a fact about the performance, not about this engine —
so TENDRIL's *loudest onset of the capture* is the same quantity from a
better-informed source. Write it and the engine stops guessing:

```json
{"sampleVelRefDb": -14.2}
```

- **Held exactly.** A supplied reference is neither decayed nor raised by a
  louder note. The `max(refPeak, peak)` inside the map still keeps a
  louder-than-reference note at unity rather than past it, but the
  reference itself does not move — you asserted it, so the engine does not
  quietly disagree.
- **Round-trips symmetrically.** The config echo carries it in the units
  you wrote (`"auto"` or dBFS); `status.sampleVelRefDb` is always the
  number actually in force. Pinned, the two agree — which is the cheapest
  possible confirmation that the write landed.
- **`"auto"` hands it back from where it was**, not from a reset. Resetting
  would drop the reference to the floor and make the next note read as the
  loudest so far — the exact failure the observed reference already avoids
  at cold start. Measured on release from −3 dBFS: −4.6, −6.2, −7.8 over
  the next twelve seconds, which is the 20 s tau.
- Live-scoped and cheap; re-assert it per phrase if that suits you. The
  reference scales velocities, it is not in the audio path, so a write
  cannot click.

If you go this route, note the division of labour: **you own the
reference, the engine still owns the measurement.** Peak over the first
30 ms from the foot of the attack, the fast-follower strike, the ~15 ms
refinement, the 0.6 soft/main threshold — all unchanged and all still on
this side.

**Measured**, playing at a realistic −16.5 dBFS peak: hard note strikes
**1.00** (the absolute map scored the same playing 0.577), and 12 dB below
the reference lands at **0.60** — mid-window, as the formula says.

### 2.10 Acoustic Instruments v2.1.0 — 46 instruments, collisions, and findings

**⚠ Do this first if any cache was built from Plucked Acoustics v1.x: ten
instruments were labelled one octave BELOW the pitch they sound, and
v2.0.0 moves their note names up an octave.** Audio bytes are unchanged —
only labels. Affected: `dantranh` `folkharp` `strumstick` `psaltery`
`kalimba` `mbira` `handchimes` `mbiramavembe` `chamberorgan` `ocarina`.
Not affected: `banjo`, `concertharp`, the six SoundFont sets. **Re-extract
and re-read pack.json — do not patch names in a cache.** If Treebrain
hardcoded any name→MIDI map for those ten, add 12. The audit trail is
`OCTAVE_AUDIT.md` in the zip; its general lesson is worth keeping: *a
pitch checker that searches ±120 cents around the claimed pitch cannot
see an octave error, by construction* — the v1.1 QA (and this side's own
first spot-check) both passed a pack that was an octave off, for exactly
that reason. Verified here with an explicit three-candidate harmonic
model over all 653 files: the corrected labels win on every instrument.

**v2.1.0 adds 22 instruments** — bowed strings (`violin` `viola` `cello`
`contrabass`, each with a pizzicato set), `harpsichord` (densest set in the
pack, 28 notes), `vibraphone`, `vibraphonebowed`, `recordertenor`,
`recorderbass`, `pipeorgan`, `tubularbells`, `ocarinalarge`, `balafon`,
`wineglasses`, `harmonica`, `saxello`, `marimba`, `kalimbakenya` — plus a
`category` field in pack.json (9 groups, by how a voice SOUNDS) that is
the right axis for the SOUND picker. All 22 are CC0; the six CC BY sets
and their attribution line are unchanged.

**⚠ Two pack ids collide with factory cache folders: `harpsichord` and
`vibraphone`.** The harpsichord one is octave-hazardous: the engine's
`sampleOctave:"auto"` table applies **+12 by name** (the factory
harpsichord's filenames sit an octave below sounding; the pack's are AT
sounding). Demonstrated live: the pack's set in a folder named
`harpsichord` loads with `sampleOctaveApplied: +12` — an octave high.
Either cache the pack's two under distinct names (`harpsichord2`,
`vibraphone2` — discovery takes any name) or replace the factory folders
and pin `sampleOctave: 0` on that instrument. `vibraphone` is benign
(both sets sounding-pitch), purely which set wins the name.

**Engine fixes this pack forced, both landed with discriminating tests:**

- **Soft-only zones sound at every velocity.** `contrabasspizz` ships
  eight notes with soft takes and no main take (`rr: 0` — upstream
  velocity coverage is uneven, exactly as the handoff warned). The pick
  fell back main-ward only, so a hard strike on those zones returned
  silence — more than half the instrument dead above the 0.6 threshold.
  Both directions fall back now: the wrong timbre is a recording, silence
  is a hole.
- **Bank normalisation is peak-aware.** The level measurement reads the
  first 300 ms; a bowed swell has nearly nothing there, so
  `vibraphonebowed` measured a "+20 dB correction" whose peaks would have
  landed +14 dBFS. The boost now caps where the bank's loudest sample
  reaches −3 dBFS: that set gets +3.0 dB instead, every other set is
  untouched. A swell being quiet early is the instrument, not a mastering
  fault.

**Pack findings from this side's octave-proof audit (1185 files), for the
next pack rev** — none block use:

- `violinpizz/C7` sounds ≈ **C5** — two octaves below its label, both by
  harmonic model and by period measurement. One file; pull or relabel it.
- `pipeorgan`'s bottom notes (`C1` `Ds1` `Fs1` `A1` `C2`, arguably `A2`)
  contain **no fundamental at all** (−54…−68 dB relative; the spectrum
  starts at the 2nd or 4th partial) — the source's bottom octave borrows a
  higher rank, so asking for those notes SOUNDS an octave up. `C3` and
  above are clean. Either trim the set's floor or note it; the engine
  plays what the file contains.
- `cellopizz/F3` (all four takes) resists both measurement methods —
  flagged, not convicted; worth an ear at the rig. The two kalimba
  marginals from v2.0 are unchanged and remain the instrument's acoustics.

**New in v2.0.0 — six winds and brass, all CC0**, chosen by measured
attack rather than articulation name:

| id | Attack | Range | Notes | Max gap | Files |
|---|---|---|---|---|---|
| `trumpetharmon` | 606 ms | A♯3–A5 | 8 | 5 st | 16 |
| `trombone` | 379 ms | A♯1–F4 | 11 | 7 st | 22 |
| `tenorsaxvib` | 190 ms | A♯2–D6 | 19 | 4 st | 19 |
| `clarinet` | 180 ms | D3–F6 | 11 | 5 st | 22 |
| `tenorsax` | 102 ms | G♯2–E6 | 23 | 2 st | 46 |
| `frenchhorn` | 95 ms | A♯2–F5 | 9 | 7 st | 16 |

Caveats that matter at the rig: `trombone` and `frenchhorn` stretch up to
7 semitones per sample, so artifacts arrive sooner than on the strings
(`tenorsax` is the dense one — whole tones throughout); `frenchhorn`'s
soft layer exists on some notes and genuinely not others (pack.json's
`perNote` is the only truth); and the winds are 6–7 s **one-shots with no
loop points** — a swell is the engine envelope's job (`synthAttackMs` on
ghosts, `leadAttackMs` on a sample lead), which is the right place for it
anyway.

**Also present on this rig, NOT in the pack: `lapsteel`** — 22 notes
C3–F♯6 at whole steps, 2 round-robins (down/up strikes), 11.7 s sustains,
thinned from Indiginus THE STEEL. It is **licence-restricted** (commercial
Kontakt library; format conversion changes nothing about the licence) and
lives only in this private repo under `live/samples/lap-steel-v1.0.0/` —
keep it out of anything public or downloadable, and out of the CC pack.
Its labels are sounding pitch, verified. Steel wants the mellow attack
done by envelope, per its own README.

**Engine side (done):** `AE_SMP_MAX_INSTRUMENTS` raised 32 → 64 — the
factory nine plus this pack plus the steel is already 34 folders, and
discovery truncated past the cap, which in an echo-built picker means
instruments that exist but cannot be selected. (Same silent-cap shape as
the round-robin fix, one commit apart.)

The pack drops into the existing cache layout: `s`-spelled sharps,
`<Note>[_soft][_rrN]` naming, filenames already **sounding pitch**
(`sampleOctave` stays `"auto"`; the banjo's octave-high upstream labels are
corrected in the pack — **never re-import an instrument from its original
source**, the tuning fixes live only in the pack). Verified here: all 18
load through the real loader, zone and file counts match `pack.json`
exactly (1185 files in v2.1), zero full-scale recordings, and labels
verified octave-proof (harmonic model, all files) after the v1.x relabel.

**Engine side (done):** the round-robin cap was 4 per (zone, layer) with a
silent skip past it — the banjo ships **11 takes on one note**, so seven
of its recordings loaded nowhere while every count in status looked
complete. Cap is now 12, with a test staged six deep that fails at the old
cap.

**Treebrain side:**

- **Index from `pack.json`, not a glob or a flat count.** Round-robin
  counts vary per note (`perNote[<Note>].rr` / `.softRr` are the truth);
  a fetcher that assumes `_rr2` exists everywhere will 404. This is the
  same class of silent cap the engine just fixed on its side.
- **`gainHint`: use it or measure, but don't do both.** The engine ignores
  `gainHint` deliberately — it measures each bank at load and normalises
  to −22 dBFS (measured spread across this pack: −6.3…+7.6 dB of
  correction, consistent with the hints). If Treebrain's own player uses
  `gainHint` as a static trim, fine — but a player that measures AND
  applies the hint corrects twice.
- **The attribution obligation is real and it is yours too.** Six
  instruments — `celesta`, `musicbox`, `dulcimer`, `koto`, `shamisen`,
  `sitar` — are CC BY 3.0 (rendered from the Fluid R3 GM SoundFont; the
  other twelve are CC0). One user-reachable line satisfies it, verbatim:

  > Celesta, music box, hammered dulcimer, koto, shamisen and sitar
  > samples derived from the Fluid (R3) General MIDI SoundFont by Frank
  > Wen, licensed CC BY 3.0.

  The engine's built-in UI now shows this line whenever one of the six is
  the selected instrument; Treebrain ships the same files and needs its
  own copy (an about panel or the SOUND picker's footer both qualify).
  Dropping the six makes the whole pack CC0.
- **Character notes worth surfacing in the picker** (from the pack README):
  the six CC BY sets are one-recording-per-zone GM renders — usable, not
  sampled-instrument quality; `dantranh` is the better koto and `psaltery`
  the better dulcimer if substitution is acceptable. `banjo` decays in
  under a second (long releases just hold silence); `ocarina` spans only
  As4–B5 because the instrument does; the sustained sets (`chamberorgan`,
  `handchimes`, `ocarina`) are one-shots with **no loop points** — they
  stop when the recording does, so a held note past ~6 s needs the synth
  source instead.

### 2.11 `harmLock: "mask"` breaks a tie away from the lead

No key added. On a tie the walk now goes **away** from the lead — up for a
ghost above, down for one below — so a third stops collapsing onto a second
or a unison at those degrees. A unison ghost keeps up-first (no apartness
to preserve), and so does the plain lead-correction walk, which matches
your TENDRIL carve-out exactly: a detected note being corrected onto the
mask has no second voice to stay away from. An existing mask patch just
stops collapsing.

---

## 3. FOLLOW — voice follows guitar, and the Treebrain UI spec

**What it is.** The voice engine's lead retunes to the guitar engine's
corrected pitch class — sing anything, sound the guitar's note — and
optionally rides the guitar's envelope, **including cutting off when the
guitar's notes stop**. Built engine-to-engine: the guitar POSTs
`{"note", "level"}` to the voice's `/api/follow` at note changes + ~20 ms
level motion + a 150 ms keepalive; the voice receives the note through
stock MIDI Harmony (`midiMode` + `midiOctaves:"nearest"` — guitar names
the pitch CLASS, singer names the register) and applies the level through
`followEnv`. Verified live: two engines, guitar at 220 Hz → voice holds
the encoded note, level rides, and killing the link returns the voice to
autonomous within the 400 ms TTL.

Design decisions that answer questions you'd otherwise ask:

- **Degrees, not frequencies, on the wire** — exact in any EDO
  (`n = 60 + (j − 4·edo)`, equave-folded; class survives, which is all a
  "nearest" receiver reads). Both instances must share the tuning grid.
- **Envelope is ABSOLUTE, not reference-normalised** — a rolling reference
  maps the same played level to different follower gains across a set,
  i.e. gain inconsistency by construction. Gated to a clean 0 below the
  sender's voicing gate, so "note stopped" transmits as silence.
- **A dead sender cannot wedge the voice** — 400 ms receiver TTL decays
  the link to neutral; a clean sender shutdown also clears it explicitly.
- **The follow note never touches the virtual chord set** — your
  `/api/midi` chords and a live link compose; the follow bit rides in
  like a hardware note.

### The keys

| Key | Instance | Type | Applies | Default |
|---|---|---|---|---|
| `followUrl` | **guitar** (sender) | `host:port` or `""` | live | `""` |
| `followHold` | guitar | bool | live | `true` |
| `followEnv` | **voice** (receiver) | float 0–1 | live | `0` |

Status: guitar `followNote` (−1 = none) + `followError`; voice
`followLevelIn` (1.0 = neutral) and the followed note in `midiNotes`.

### Treebrain UI spec

**Placement:** a FOLLOW block on the *voice* channel strip — that is where
the musician thinks the feature lives — but note the write direction:
enabling it writes to BOTH instances.

**Enable toggle** ("Follow guitar"):
- ON → guitar: `{"followUrl": "127.0.0.1:<voice-port>"}` · voice:
  `{"midiMode": true}` (assert `midiOctaves:"nearest"` — it is the
  default, but a rig that switched to `"held"` for charts would take the
  guitar's arbitrary post-fold register; lint this).
- OFF → guitar: `{"followUrl": ""}` · voice: restore `midiMode` to what it
  was before the link (Treebrain owns that memory — a rig using hardware
  MIDI charts on the voice channel had it on for its own reasons).

**ENV depth fader** (voice, `followEnv`, 0–1, live): 0 = pitch-only,
1 = guitar's envelope is the voice's envelope — silence cuts. Unipolar
with neutral at the left edge, so a plain fader is correct here (§12).

**HOLD toggle** (guitar, `followHold`): ON (default) = the voice keeps its
pitch target between guitar phrases; OFF = guitar silence releases the
voice to its own correction. Independent of ENV: at depth 1 the voice is
silent during guitar silence either way — HOLD decides what pitch it
comes back on.

**Indicators:**
- Followed-note lamp on the voice strip: decode voice `midiNotes[0]` via
  `j = 4·edo + (n − 60)` (mod edo for the class) and show the degree/note
  name. Lit = link active.
- Level meter: voice `followLevelIn` (1.0 with no link = neutral).
- Fault lamp: guitar `followError`, surfaced exactly like `sendError`.

**Lints:**
- Both instances must agree on `edo`, `rootNote`, `refNote`/`refNoteHz` —
  refuse to enable the link across mismatched grids (the degree transport
  assumes one grid; a mismatch is wrong NOTES, not detune).
- Refuse a `followUrl` that points at the instance itself.
- Warn if the voice channel's `expression` is 0 *and* `followEnv` is 1 —
  legal, but the combination is a hard-quantized fully-gated robot, which
  is usually a mistake rather than a sound.

**Latency expectations to set in the UI copy:** guitar detection lock
(~31 ms) + loopback POST (~1 ms) + voice retune (`retuneMs` /
`transitionMs`). A fast legato follow — the voice lands on the guitar's
note a few centiseconds behind the pick, which reads as ensemble, not as
a vocoder.

**Feature-detect:** `followEnv` in the config echo.

## 3b. The record send

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
   long-standing `latencyMs`, added under the name you probe for): the
   **engine's own path** at current settings, algorithmic plus the
   engine's buffering. It moves with `quality`, devices and
   `bufferFrames` — re-read from the echo after any restart-scoped
   write. **Shown, not applied** on this rig, per your call: one trim
   per device would drag the dry rows, so it is a readout beside the
   by-ear trim rather than an input to it.

   **New (latency-honesty batch): `totalLatencyMs` + `deviceLatencyMs`.**
   The old figure never counted hardware overhead — the input side's
   collection cycle plus both sides' device/stream/safety-offset
   latencies (converters included), typically 10–20 ms on a USB
   interface — which is why the rig *felt* later than the display. The
   engine now queries the hardware (and reads back the buffer size the
   device actually **granted** rather than trusting the request) and
   reports `deviceLatencyMs` (the hardware part, best-effort:
   honest-or-low, never invented) and `totalLatencyMs`
   (= `latencyMs` + `deviceLatencyMs`, the honest mic-to-speaker
   number). **UI rule: display `totalLatencyMs` on the panel; keep
   aligning recordings by `processLatencyMs`** — the send taps inside
   the engine, so hardware overhead genuinely does not apply to the
   stem, and moving `processLatencyMs` would have silently skewed your
   §3b alignment. Feature-detect: `totalLatencyMs` in status.
3. **No cable**: option (a) as you preferred — aim `sendChannel` at a spare
   output pair and let a loopback-capable interface close it internally.
   The UDP/shared-memory tap was considered and deliberately not built
   while the send covers the need; say the word if it stops covering.
4. **Nothing else changes, and collisions are refused, not summed** — from
   both directions: a `sendChannel` write landing on the live output OR an
   `outputChannel` write landing on the send keeps the old value and puts
   the reason in `status.sendError` (`""` after any accepted send write).
   Surface it like `irError`.

Also landed: **`formantSemitones`** (float −12…+12, live — **bipolar;
see §12 before putting it on a fader**) — the formant
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

## 3c. POLY mode — chords, at the pedal price

`polyMode: true` (bool, **restart-scoped**, lives with the detection-range
controls): detection is bypassed and the whole chordal input runs through
fixed-ratio spectral shifting — the same Signalsmith Stretch instances as
mono, at a **doubled analysis block**. That is the same trade the pedals
make: balanced quality goes 56 → 112 ms at 48 kHz, echoed in
`processLatencyMs`, so re-read the echo and re-feed any latency
compensation on toggle.

What the guitarist gets: `leadShiftSteps` transposes the chord (±edo is a
clean octave — the drop-tune/12-string tricks), and the five harmony
voices become their intervals applied to the whole chord — a fixed-
interval poly harmonizer with per-voice gain/pan/detune intact. Envelope
FOLLOW still transmits (energy needs no pitch), so poly guitar can still
gate the voice.

**UI spec:** a MONO/POLY toggle next to the RANGE control (the user's
suggested placement is the right one — both describe what the detector is
allowed to assume about the input). On switching to POLY, grey out
everything detection-dependent: AMOUNT and the correction cluster, SCALE
mask, `harmLock`, MIDI mode, HOLD, synth/sample sources, Attack Sound,
`expression`, and the pitch-follow half of FOLLOW (envelope half stays).
Show the latency change prominently — it is the cost the player is
choosing. `detectedHz`/`targetHz` read 0 in poly; don't draw the pitch
trace as a fault.

Verified: a C3–E3–G3 chord through poly `leadShiftSteps:+12` arrives with
all three tones intact an octave up (each shifted tone ≥3× its unshifted
residue), the same chord through mono is the negative control (it does not
survive), and the latency doubling is asserted in the suite and visible
live in the echo.

### 3c-ii. The chord sampler — POLY + `leadSource:"sample"` (MEL9 mode)

One exception to "sources are inert in poly", and it is the headline:
with `polyMode: true`, `leadSource: "sample"` and a bank loaded, the
engine becomes the EHX Mellotron/9-series move done properly — **play a
chord, hear the loaded sample library play that chord**, EDO-snapped. A
multi-f0 tracker (harmonic-sum salience + iterative spectral subtraction,
with note birth/death hysteresis) follows up to 6 simultaneous notes,
snaps each to the enabled-degree grid, applies `leadShiftSteps`, and
strikes/repitches/releases sample voices per note. Unlike the pedal, the
library is anything under `samplePath` and the tuning is whatever the
scale says — a 22-EDO choir chord is exactly as available as a 12-EDO
flute one.

Keys (all existing semantics, one new):
- **`polyNotes`** (int 1–6, live, default 6) — polyphony cap. NEW key,
  appended to the feature-detect chain (§0).
- `sampleMix` — 1 = the library **replaces** the playing; 0.5 = layers it
  under the shifted chord. Layering is the recommended default preset:
  the shifted chord carries the attack while the library blooms in under
  it, hiding the tracker's onset time.
- `sampleVelocity` / `sampleVelRefDb` / `sampleRing` — as in mono.
- `leadReleaseMs` — a lifted chord's sample tails decay at this pace
  ("cutting off when notes stop", at the player's chosen speed).

Status: **`polyNotesActive`** (int) — notes the tracker currently holds
alive; 0 whenever the chord sampler is not engaged. Drive a small "notes
held" meter next to the POLY toggle; it is also the honest diagnostic for
"the chord isn't all sounding".

**UI spec:** when POLY is on, UNGREY the sample-source cluster
(instrument picker, `sampleMix`, velocity, ring) — in 3c's original spec
it was listed with the greyed-out detection-dependent controls, and that
list is now wrong for the sample source specifically (synth stays
greyed). The **SCALE mask un-greys too**: the chord sampler snaps every
tracked note to the enabled-degree grid (and `polyDetected.cents` in
§3c-iii is measured against it), so mask edits are live and meaningful
in poly — they just don't affect the fixed-ratio *shifted* audio, only
the tracker-driven paths. Add a `polyNotes` stepper (1–6) beside the
instrument picker, and the `polyNotesActive` meter. Latency note stays
as in 3c.

Honest limits, for the tooltip: octave doublings merge (a power chord's
octave reads as one note — the fifth still sounds); dense voicings
beyond ~4 distinct pitch classes may drop the weakest note; close
intervals (thirds) are honest from ~C3 up, below that only wide
voicings resolve (a window-resolution fact — port doc §11d); and a
fresh chord's sample onset trails the strings, which the `sampleMix`
layering hides well.

**Onset behaviour (research batch 2026-08, automatic — no new keys):**
an onset doubles the tracker rate for 150 ms, so a fresh chord's first
sample lands ~21 ms after the pluck (was ~32 ms — both measured,
asserted in the suite); **re-strums retrigger** — an onset on a
still-ringing chord strikes fresh samples for the notes whose own level
jumped, with old rings decaying under `leadReleaseMs` (fast strumming
under a still-loud ring deliberately does NOT retrigger — the pad
sustains, the pedal behaviour); and an early tracker guess corrected
within ~120 ms crossfades out in 6 ms instead of ringing at a wrong
degree. Nothing to build UI for; worth a line in the POLY tooltip.

Verified end-to-end in the suite: a C3–E3–G3 chord with strong 2nd/3rd
harmonics against a pure-sine test bank comes back as all three tones
**harmonic-free** (i.e. the recordings, not shifted input — pitch alone
could not tell those apart), `polyNotes: 1` collapses it to exactly one
sounding tone, `polyNotesActive` reports 3/1 accordingly, and the whole
test fails against the reverted integration. Tracker-only test: the
triad is exactly three notes within 20 cents, no ghosts, and silence
kills all of them. Live-verified against the shipped v2.1 packs.

### 3c-iii. The poly detection, exported — build your own things on it

The multi-f0 tracker is not the chord sampler's private property. In
poly mode it runs **whether or not a bank is striking**, and status
exports its live chord as `polyDetected` — so Treebrain can use the
detection for anything:

```json
"polyDetected": [
  {"note": 48, "hz": 130.9, "cents": -3.2, "level": 1.0,  "id": 17},
  {"note": 52, "hz": 165.1, "cents":  2.8, "level": 0.84, "id": 18},
  {"note": 55, "hz": 196.4, "cents":  3.5, "level": 0.71, "id": 19}
]
```

Field contract:
- **`note`** — the FOLLOW link's MIDI-style encoding (§3: 60 = engine
  degree `4×edo`, one per degree, equave-folded to 0–127). The decoder
  you already wrote for `followNote` decodes this.
- **`hz`** — raw tracked pitch, before EDO snapping.
- **`cents`** — offset from the snapped enabled-degree pitch (the scale
  mask is honoured, so this is "how far from the note the chart allows").
- **`level`** — relative 0..1, loudest note ≈ 1. Deliberately **not**
  absolute velocity (the FOLLOW decision about gain consistency applies
  here too); treat it as balance-within-the-chord.
- **`id`** — stable for the note's life. Same `id` across two polls =
  the same sounding note; a new `id` at the same pitch = re-struck.

Timing contract: the tracker updates every ~21 ms at 48 kHz; status
ticks at ~10 Hz, so sub-100 ms notes can slip between polls, and note
birth trails the string by ~60–90 ms (window + confirmation — the same
honesty as §3c-ii). This is a **display/logic feed, not an audio-rate
event stream**; anything that must be sample-accurate belongs engine-side
(as the chord sampler is).

Feature-detect: `polyDetected` is a **status** key, present (possibly
`[]`) on every build that has it — probe status once, not the config
chain. It landed after `polyNotes`; the config chain (§0) is unchanged.

**UI spec — things this feeds, in the order they're worth building:**
1. **Chord display**: decode `note` per entry, name the chord in the
   current EDO's terms; sort by `level` for voicing order. Put it beside
   the POLY toggle where `polyNotesActive` already sits (the count is
   now just `polyDetected.length` for the uncapped view).
2. **Polyphonic tuner**: one needle/bar per entry driven by `cents` —
   the guitarist tunes all six strings in one strum, against the actual
   22-EDO grid rather than 12-TET. Colour by |cents| like the mono tuner.
3. **Chord trace**: a piano-roll strip of `note` over time, keyed by
   `id` so held notes draw as bars, not dots. `traceSeq` stitching is
   not needed — `id` does that job here.
4. **Triggering Treebrain's own instruments/visuals**: births = ids that
   appeared since the last poll, deaths = ids that vanished. Debounce by
   one poll if driving anything audible, and remember the ~10 Hz floor —
   for note-onset-critical uses, raise the status poll rate on the POLY
   panel only.

Verified: the export is asserted in the suite without any bank loaded
(three notes, exact degrees, distinct ids, FOLLOW encoding round-trip;
the test fails against the reverted "tracker only runs for the chord
sampler" gating), and live over HTTP: the stub's A3 tone reads
`{"note":57,"hz":221.2,"cents":9.7,"level":1.0,"id":1}` in poly and
`[]` in mono.

**Running the same algorithm Treebrain-side:** if polling `polyDetected`
is not enough (faster-than-poll onsets, audio the engine never sees,
detection with no engine running), the tracker itself is portable —
`TREEBRAIN-POLYF0-PORT.md` is the implementation-grade handoff: every
constant in execution order, the six structural rules that were each a
measured failure during development, Web Audio integration notes, the
acceptance fixtures both implementations are held to, and the free
cross-check against `polyDetected`. First-party code, no licence
strings attached.

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

### `bypassOutput` — what `bypass: true` puts on the output
`"dry"` (default) | `"mute"`, live.

`dry` is the historical behaviour: the input passes through uncorrected.
`mute` puts silence there instead — for a rig whose dry already reaches the
desk on its own row, where a passthrough is a second copy of a signal the
mix already has. **This retires the stateful `outputGainDb: -60` dance**
(drop the fader, remember what it was, restore it on un-bypass, get it
wrong once and the channel is dead for a song) with one switch that has no
level to put back.

Scope is exactly what it says: it decides what *bypass* does and nothing
else. With `bypass` false the live path is untouched either way, so it is
safe to assert once at link-up and leave.

### `leadAttackMs` / `leadReleaseMs` — the LEAD's own envelope
`leadAttackMs` float 0–5000, default **5** (the click guard: no shaping).
`leadReleaseMs` float 5–10000, default **500**. Both live.
`synthAttackMs` / `synthReleaseMs` remain the HARMONY's and are unchanged.

Separate keys because the two envelopes shape different things: the harmony
envelope hides the ghosts' arrival latency, this one shapes the corrected
lead itself.

What the release buys depends on the lead source, and the difference is not
a wart — it is what each source *is*:

- **Synth lead** — an oscillator, so this is a real tail at its last pitch.
  Previously the lead had no release at all and was cut by the voicing
  gate; it now leaves the way the ghosts do.
- **Sample lead** — a recording, so this is a **ceiling** over its natural
  decay. Under `sampleRing` that is what stops a let-ringing figure from
  ringing past the end of the phrase, which is the pairing the two keys
  exist for.
- **Shifted lead** — made *of* the input, so once the input stops there is
  nothing left to sustain and the release is **inert by construction**. It
  has to be: a shifted lead crossfades to the dry path when the voicing
  drops, and a lingering wet would sound every consonant twice. The attack
  still shapes its arrival.

Surface both, but expect the release control to read as doing nothing on a
`leadSource: "voice"` channel — that is correct, not a bug, and worth a
tooltip rather than a hidden control.

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
- **Detection got faster; no keys moved.** The analysis frame is sized at
  the textbook YIN minimum (two periods of `detectMinHz`) instead of being
  padded to a power of two, and an energy onset clears the detector's
  octave-continuity hysteresis (that raised bar was a claim about the note
  that just ended). Measured at the rig's guitar settings: first lock on a
  fresh pluck 37.8 → 30.7 ms average (worst case 52 → 31), vibrato
  tracking lag 17.2 → 8.8 ms at 99% amplitude transfer, detector-hostile
  fixture unchanged. Halving the detection hop was measured and rejected
  (+55% detector CPU for ~1.5 ms). Practical knob on your side, now with
  more leverage: `detectMinHz` set to the instrument's true bottom
  directly shortens the window — it is the cheapest lock-time improvement
  available from config.
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
| `sampleVelLast`, `sampleVelRefDb` | the last strike level **and what it is relative to**. The second is not optional decoration: with a relative map, a velocity of 0.55 is unreadable without its reference. §2.9 |

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
- `harmGainDb`, `attackGainDb`, `hg`, `hp`, `expression`, `leadAttackMs`,
  `leadReleaseMs`: safe to sweep —
  the engine smooths (~5 ms) — but coalesce CC floods; one POST per UI tick
  (≤ ~30 Hz) is plenty. Every POST returns the full fresh echo.
- `leadShiftSteps`, `hm`, `hx`: step-valued; post on change only.
- `bypassOutput`, `sampleRing`: assert once at link-up; they are mode
  switches, not performance controls.
- Restart-scoped keys (`detectMinHz`, `detectMaxHz`, `range`, `quality`,
  devices, `bufferFrames`, **`sendChannel`**, `outputChannel`): batch into
  one POST — each restart-key POST restarts the engine once. `sendChannel`
  is restart-scoped; see answer (a) in §11.
- Sample binding keys reload the bank; move `sampleHash` only when the
  library actually changed.

---

## 10. Update checklist

**Do first — audible:**

1. **Nothing.** The four keys you asked for — `bypassOutput`, `sampleRing`,
   `leadAttackMs`, `leadReleaseMs` — are live and echoed, so the code you
   already feature-detected against lights up on the next engine build.
   Confirm with `sampleRing` in the config echo. §2.8, §5.
2. Add the **`expression`** slider (0–1, default 1). §1.
3. Apply the note-centre / expression split to **TENDRIL capture snap and
   `fxSnapSteps`**, with the octave-re-vote guard restructured to test the
   detected jump alone. §1.
4. Fix the **bass −12 / harpsichord +12** sounding octave in Treebrain's
   own player. §2.4.

**Do next — the silent-samples hunt:**

5. ~~`encodeURIComponent` every path segment in the sample fetch path.~~
   **Closed** — the cause was `samplePath` never actually being sent, fixed
   rig-side. Both traps in §2.6 stay documented; neither was it.
6. Refuse and report wrong-rate cache files instead of failing silently.
   Still worth doing on its own merit. §2.6.

**Correctness and staleness:**

7. Measure each bank at load (median RMS, first 300 ms, main layer only)
   and normalise to −22 dBFS; drop `levelConst`. §2.3.
8. Count full-scale samples per bank and surface it. §2.5.
9. Build the SOUND picker from `status.sampleInstruments`; mark a
   configured-but-missing instrument rather than dropping it. §2.2.
10. Check the **24 dB / 0.2** velocity constants still match on your side —
    they are now a two-rig contract. §2.9.
11. Optionally drive the strike reference from TENDRIL's loudest capture
    onset with `sampleVelRefDb` instead of letting the engine observe its
    own. §2.9.

**Carried forward from the earlier batches:**

12. Light SHIFT from the `leadShiftSteps` echo; soft-limit at
    `±floor(3600·edo/periodCents)` steps.
13. Map HOLD to a momentary CC, **edges only**; drop any latching
    assumption.
14. Add the `harmSustain` toggle (default-on behaviour change).
15. Recalibrate any portamento presets made against the exponential glide.
16. Add per-voice detune (`hd`) and the unison-ghost affordance
    (`hm:0` + `hd≠0`).
17. Replace `refA4` handling with the `refNote`/`refNoteHz` link-up
    assertion; lint `refA4 ≠ 440`.
18. Add Attack Sound controls; consider the "Pad + pick" macro.
19. Rescale `hg` controls to −60…+12 with 0 = lead parity.
20. Add the record send controls. **`processLatencyMs` stays shown, not
    applied** — one trim per device would drag the dry rows on this rig, so
    it is a readout next to the by-ear trim, not an input to it.
21. Retire the bassy-guitar mitigation and (optionally) the `harmOn`
    link-up assertion.
22. Delete any reference to `synthEnvActive`.
23. Put the §8 diagnostics and the 5-step bisection on the panel.

---

## 11. Answers, and what's still open

**(a) `sendChannel` scope: RESTART.** Verified in code — the write sets the
restart flag (`live/src/main.c`, the `sendChannel` branch) because the
channel lives in the device channel map, same scope as `outputChannel`.
`CONTROL.md` says restart too, in the send table; if a copy on your side
says live, it is a third source and it is wrong. Batch it with
`outputChannel` — each restart-key POST restarts the engine once.

**(b) `sampleClipped` unit: FILES.** One increment per recording with 8 or
more full-scale samples, not a sum of samples. The 32-vs-23 gap is real and
measured, not a counting bug: **32 files were re-exported, 23 of them
tripped the test.** Nine of the mis-decoded files never reached full scale
— a quiet enough recording survives the wrong bit depth without pinning.
So read the counter as *"at least this many files are corrupt"*: it detects
the loud consequence of the fault, not the fault, and zero is meaningful
while a small number is a floor. (Re-measured both sets from the caches to
answer this: ORIG 78 files / 23 clipped, FIXED 78 / 0, 32 files differing.)

**(c) Feature-detect chain — one authoritative ordering**, now at the top
of this document and nowhere else:

```
harmGainDb → harmHold → leadShiftSteps → attackSound → sendChannel
           → sampleInstrument → expression → sampleRing
```

`sendChannel` sits between `attackSound` and `sampleInstrument` because the
record send shipped between those two batches. Probing any key implies
every key before it.

**(d) `processLatencyMs` → `captureOffsetMs`: dropped from the checklist**
as asked. It now reads "shown, not applied", with your reason recorded (one
trim per device would drag the dry rows). It stays in the status table as a
readout next to the by-ear trim.

**(e) Samples silent in Treebrain: CLOSED.** `samplePath` was never
actually sent; fixed rig-side. Both traps in §2.6 stay documented since
each is a real way to lose a sample path silently, but neither was this.

**(f) Layer-blend taper: MATCHED.** Both sides use
`g = sin(min(1, 2d)·π/2)` with the unity plateau at 0.5. Nothing to do.

## 12. UI correction: bipolar controls (field incident, 2026-08-08)

**What happened.** `formantSemitones` was surfaced as an unmarked slider.
The player read the left edge as "0, no correction" — the natural reading
of an unlabelled fader — but the control is **bipolar**: the left edge is
**−12**, neutral is the **centre**. The slider sat left of centre, which
put a permanent formant shift *down* on the corrected lead and every
shifted ghost, and was reported as "the bassy corrected note is back."
Three engine-side mechanisms were measured and exonerated before the
slider was found. Total cost: a day of diagnosis for a missing tick mark.

**The correction, for any bipolar key:**

1. **Render bipolar as bipolar.** Fill the track **from the centre to the
   thumb**, not from the left edge. A centre-filled fader cannot be
   misread as "left = off".
2. **Mark neutral.** A tick and a `0` label at centre; endpoints labelled
   with signed values (`−12` / `+12 st`).
3. **Default and snap to centre.** The control's resting position IS the
   neutral centre — matching the engine default — and the thumb snaps to
   it within ~2% of travel; double-click (or long-press) also resets to 0.
   A bipolar control that defaults anywhere else is wrong before anyone
   touches it.
4. **Signed readout.** Show the value with its sign next to the control
   (`−5.0 st`), sourced from the **config echo**, never from thumb
   position — the echo is the truth, the thumb is a request.
5. **Show non-neutral as ACTIVE.** Any nonzero `formantSemitones` engages
   the formant stage even on a `formantHold:false` guitar channel, so a
   stray −2 is not a tone tweak, it is a processing stage re-entering a
   path that was deliberately cleared. Light a badge when ≠ 0, exactly as
   for an engaged IR.
6. **Never initialize from the control.** Defaults come from the engine's
   echo at link-up; a fader must not POST its resting position on mount.
   (This is how an unmarked slider becomes a permanent −12.)

**Audit these keys — every one is bipolar or has a non-edge neutral, and
each is one unmarked fader away from the same incident:**

| Key | Range | Neutral | Trap if drawn left-to-right |
|---|---|---|---|
| `formantSemitones` | −12…+12 | **0 (centre)** | left edge = −12: the incident above |
| `hg` (per-voice gain) | −60…+12 | **0 dB = lead parity**, not an edge | left = −60 (off); "full right" is +12 dB OVER the lead |
| `harmGainDb` | −24…+12 | 0 dB | same |
| `harmTiltDb` | −12…+12 | 0 (centre) | left = maximally dark, reads as "no tilt" |
| `hd` (detune) | −100…+100 ¢ | 0 (centre) | left = −100 ¢, a quarter-tone flat |
| `hp` (pan) | −1…+1 | 0 (centre) | left-as-zero pins every voice hard left |
| `leadShiftSteps` | −72…+72 | 0 (centre) | left = six equaves down |
| `attackGainDb`, `sendGainDb`, `outputGainDb`, `leadGainDb` | −60…+12 | 0 dB, near right | left-as-zero mutes; also fine, but label it |
| `sampleVelocity` | −1…1 | **−1 = "measure" sentinel** | the left HALF is a mode, not a level — this should be an auto/manual toggle plus a 0…1 fader, never a bare −1…1 slider |
| `sampleVelRefDb` | auto \| −60…0 | "auto" | same shape: mode + fader, not one axis |
| `expression` | 0…1 | 1 (right edge!) | left-as-default reads as "expression off" — default is FULL |

**One structural rule covers all of it:** a control whose neutral is not
an endpoint **defaults to that neutral, snaps to it**, and gets a
centre-fill and a signed label; a key whose
range encodes a *mode* in part of its axis (`sampleVelocity`,
`sampleVelRefDb`) gets a mode switch plus a unipolar fader, with the
sentinel written by the switch. Both rules are checkable in a component
library once, instead of per-control forever.

### Still open

- **The velocity constants are now a two-rig contract.** 24 dB window, 0.2
  floor, named in `live/src/corrector.h`. If either side ever moves them,
  the same playing strikes different velocities on each rig — so move them
  together or not at all.
- **Nothing.** `sampleVelRefDb` closed the last one.

## 13. Answers to Treebrain, 2026-08-16 — and two contract pins

Replies to the four items in Treebrain's 2026-08-16 report. Two of them
are now PINNED GUARANTEES: engine changes that would break them get a
major version and a loud line at the top of this file, never a silent
drift.

### 13.1 FOLLOW re-assert loop — safe, and the echo is pinned

Your ~3 s re-assert against the guitar's echo is the right shape, and it
terminates on every build that exists: `followUrl` lives in
`config_json`, which is BOTH the save file and the config echo — one
serializer, so the key cannot fall out of one without falling out of the
other — and every accepted config POST rebuilds the status echo
synchronously before the response returns ("the POST echo must reflect
the change now" is a comment in the code with a job). You will never
read a stale echo of your own write.

**PINNED: `followUrl` stays in the config echo.** Same guarantee as the
feature-detect chain itself (§0) — these are load-bearing keys, and
removal is a breaking change by definition.

Your down-order (sender first, then receiver restore) matches the
engine's own assumptions. No notes on the lint set; the 409-with-text
shape is exactly how `sampleError`-class failures are meant to surface.

### 13.2 `processLatencyMs` — engine-path only, forever. PINNED

Confirmed, and pin it in your contract doc too:
**`processLatencyMs` (and its alias `latencyMs`) will never absorb
`deviceLatencyMs`.** The reasoning is structural, not preferential: the
record send taps INSIDE the engine, so hardware overhead is genuinely
absent from the stem's path — folding device numbers in wouldn't be a
different convention, it would be a wrong number, and every corrected
take would land early with nothing flagging it (your words; agreed).
`totalLatencyMs` is the display figure; the split is permanent.

One operational note from CONTROL.md worth mirroring in the panel:
`deviceLatencyMs` can change WITHOUT a config write (another app
re-sizing the shared device buffer), so read it per status tick rather
than caching it on connect.

### 13.3 `outputGainDb` connect-time write — safe; the rig owns it

Yes, the ordering is safe, and yes, own it. The mechanics on this side:

- The engine persists every accepted config write (one save file, same
  serializer as the echo) and restores it at launch **before** the HTTP
  server accepts anything — there is no window where your connect-time
  write can race the restore; yours simply lands after and wins.
- Because your writes ARE what gets persisted, the engine's restore is
  already the rig's last number in the common case — your reconnect
  write is idempotent re-assertion, not a fight.
- The engine-side persistence is the STANDALONE fallback (engine run
  without Treebrain keeps its last stage level), not a competing owner.
  Rig as owner, engine as fallback is the intended shape.
- Skipping bypassed engines is correct and composes with the engine's
  own `bypassOutput` semantics; nothing engine-side will un-park a −60.

### 13.4 Request B — already shipped; your delta copy is stale

Everything you asked for exists on current `main`, and more precisely
than the request's shape:

- **`polyMode`** (bool, restart-scoped, §3c) — the poly analysis mode.
- **`polyNotes`** (int 1–6, LIVE) — polyphony cap for the chord sampler.
- **`polyDetected`** (status, §3c-iii) — the tracker's live chord, one
  object per note: `note` (the FOLLOW link's own MIDI-style encoding —
  the decoder you already run reads it), `hz` (raw), `cents` (offset
  from the snapped enabled-degree pitch: a poly tuner for free),
  `level` (relative 0..1), `id` (stable per note-life). Runs whether or
  not a bank is striking — it is the detection export you're asking
  for, answering exactly "what chord is the guitar engine hearing".
- **`polyNotesActive`** (status) — what the chord sampler is sounding.

Feature-detect: `polyNotes` in the config echo (config chain, §0);
`polyDetected` by presence in status. §3c-iii carries the consumer UI
spec (chord display, poly tuner, chord trace by `id`, host-side
triggering) and the timing honesty (~21 ms tracker cadence behind a
~10 Hz status poll; sub-100 ms notes can slip between polls).

Your §4 has no such keys because your copy of this file predates the
POLY sections — re-pull the delta (and the port doc grew §11: research
verdicts, the onset-first staging now in the engine, and the resolution
honesty line for consumer UIs). Bonus, since you run the line-for-line
port: engine + port on the same audio is the §8 cross-check from the
port doc — diff your port's notes against `polyDetected` and badge
disagreements as a diagnostic. Two independent implementations agreeing
is the strongest lint either of us can have.

### 13.5 Lint suggestion from the field (2026-08-17): build skew between instances

First live symptom of the two-instance rig: "guitar ignored polyMode",
voice fine. The engine binary was current on disk, but the GUITAR
PROCESS predated it -- config-triggered restarts are in-process
re-prepares and never pick up a new binary, so a long-running engine
keeps its launch-time build forever. The voice had been relaunched
since; the guitar had not.

**Lint: when both engines are enabled and their `engineBuild` values
differ, badge it** ("engines on different builds -- relaunch the older
one") before any per-feature "engine too old" message. Build skew
explains the confusing half-working state in one line, and the fix
(quit + relaunch the stale process; its config persists and FOLLOW
re-asserts) is always the same. Your existing "may be older than this
rig expects" message stays as the fallback for a genuinely old build
on both.

## 14. Field-fix batch 2026-08-17 — attack behaviour, triggering, release

Four field reports, one build. No UI beyond one new key.

- **Attack hold** (no key, always on): note identification leaned ~20
  cents sharp and repeated plucks kept triggering the next 22-EDO step
  up. Cause: a plucked string physically STARTS sharp (+20..40 cents,
  settling ~50 ms) and the boundary is 27.3 cents away. The quantizer
  now holds the note that was already sounding through its own attack
  transient (re-plucks are recognised by energy onset, pitch jump, or
  re-voicing after the pick's brief unvoiced dip); a genuinely new note
  commits immediately. **If the rig compensated by moving the reference
  (C=265 was reported dead-on), move it BACK to the true value on this
  build** — the workaround was centring the grid on the attack instead
  of the sustain, and it now makes the sustain read ~22 cents flat.
- **`gateDb`** (float, live, 0 or −80..−30; 0 = default −56): the one
  noise/trigger floor, in dBFS RMS. Quiet playing losing notes = lower
  it. Belongs beside the detection-range controls; echo shows the value
  in force. Feature-detect: `gateDb` in the config echo (chain: after
  `polyNotes`).
- **Sampled-lead triggering** (no key): the energy onset was the ONLY
  strike trigger, so re-plucks under a still-ringing string and legato
  note changes never struck — "loses repeated/successive notes". The
  lead sampler now also strikes on a degree change while voiced and on
  a detected re-attack of the same note, with a 40 ms de-dupe guard.
- **Release fixes**: a superseded/finished let-ring slot now decays to
  −80 dB before it is freed (−60 was an audible cut on dense sustained
  textures — "fades out, then cuts off at a quiet volume"), and the
  suite pins that `leadReleaseMs: 5` really silences a sampled lead
  within 60 ms of the note ending. Note for the panel copy: a sampled
  choir on the HARMONY voices releases under `synthReleaseMs`, not
  `leadReleaseMs` — the two knobs rule their own rows.

### 14b. Grid-residue diagnostics (2026-08-17, follow-up)

Field state: samples read ~22 cents off against an outside reference at
the correct C, "sounds right" only with the reference pushed to 265, and
high notes fold an octave down. Both smell like SAVED-CONFIG residues,
so status now carries the readout that settles it in one glance:

- **`anchorHz`** (status): the REALIZED root-degree frequency in octave
  4, after `refA4`/`refNote`, `rootNote`, `rootCents` and `stretchCents`
  have all had their say. A rig that believes C = 261.626 but reads
  258.3 here has a residue displacing every degree (and every sample).
  Put it on the tuning card next to the reference field, and badge when
  it disagrees with `refNoteHz` by more than a couple of cents -- that
  disagreement IS the "everything is consistently off" bug, found
  before a string is played.
- **Octave-low folding**: any note above `detectMaxHz` (or above a low
  `range` preset's ceiling) cannot be represented and detects an octave
  down BY CONSTRUCTION. Badge when `detectMaxHz` (or the preset
  ceiling) sits below ~1400 on a guitar channel.
- **Show the Hz**: the note-debug display should print `detectedHz` /
  `targetHz` (status) and `polyDetected[].hz` next to note names -- the
  user asked for exactly this while diagnosing, and cents-only displays
  hide reference-level mistakes that raw Hz makes obvious.
