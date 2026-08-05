# AutoEDO Live — External Control Spec (v1)

How another application drives the AutoEDO Live engine with **full control**
through the same interface the built-in web UI uses. Everything the built-in
UI can do, an external controller can do; the built-in page is just one
client of this API.

**Single-controller model.** The engine assumes exactly one UI at a time.
There are no sessions, locks, or client identities: every write is
last-writer-wins and is echoed to every status consumer. Run your app's UI
*instead of* opening `http://127.0.0.1:<port>/` — nothing breaks if both are
open, but two controllers will fight over the same knobs.

---

## 1. Transport and lifecycle

- The engine is a single self-contained binary (`build/autoedo`,
  `build\autoedo.exe`) that serves HTTP + WebSocket on
  **`127.0.0.1:<port>`** (default **8017**, `--port N` to change). It binds
  loopback only; a controller must run on the same machine (or tunnel).
  `--config PATH` relocates the settings file (default `~/.autoedo.json`) —
  required when running more than one instance (§10).
- **Launching — two options.**

  **(a) Spawn the binary directly** and own its lifetime:
  `./build/autoedo --port 8017`. Readiness = `GET /api/status` answering
  200 (poll ~2/s, allow ~10 s). Stop with SIGTERM (POSIX) / `taskkill /PID
  <the pid you spawned>` (Windows); it shuts down cleanly. Kill by PID, not
  by image name — `taskkill /IM autoedo.exe` takes down every instance of a
  multi-instance rig (§10), not just yours.

  **(b) Run the repo launcher** — it handles rebuild-if-changed,
  stop-what's-running (including port eviction), relaunch detached, and
  the health check for you, and exits when the engine is answering:

  ```bash
  # macOS (from your app):
  AUTOEDO_PORT=8017 <repo>/live/tools/autoedo.command --headless --no-ui
  #   exit 0  = engine is up and /api/status answers
  #   exit 1  = build or startup failed; read <repo>/live/logs/launcher.log
  #             (per-run engine logs land in live/logs/run-*.log)
  AUTOEDO_PORT=8017 <repo>/live/tools/autoedo.command --headless --stop
  ```

  ```bat
  :: Windows (from your app):
  set AUTOEDO_PORT=8017
  <repo>\live\tools\autoedo.bat --no-ui     & rem exit 0 = up, 1 = failed
  <repo>\live\tools\autoedo.bat --stop
  ```

  Flags that matter for programmatic use: `--no-ui` suppresses the
  browser-open/tab-focus step (mandatory when your app provides the UI);
  `--headless` (macOS) writes progress to `logs/launcher.log` instead of a
  terminal and reports failures via a dialog; `--stop` stops the engine and
  exits. `AUTOEDO_CONFIG=<path>` makes the launcher pass `--config <path>`.
  The launcher is idempotent — calling it while the engine runs
  restarts it (rebuild included), so "launch" and "relaunch after update"
  are the same call. Stopping is **scoped to `AUTOEDO_PORT`**: only the
  instance launched on that port (or whatever else holds the port) is
  stopped, so per-channel instances (§10) never take each other down.
  `run.sh` is a thin alias for `autoedo.command` with
  the same flags. Note the Windows launcher only rebuilds when a
  `make`/`mingw32-make` is on PATH; otherwise it reuses the existing exe.
- **An engine failure is not fatal to the server.** If audio can't start
  (missing device, mic permission), the HTTP/WS server still runs, status
  reports `running:false` with a human-readable `error`, and a config POST
  that fixes the problem (or `POST /api/restart`) recovers it. Your UI must
  surface this state.
- **CORS:** every response carries `Access-Control-Allow-Origin: *` and
  `OPTIONS` preflights are answered, so a browser-based controller on any
  origin can call the API and read responses. (Consequence to be aware of:
  any local page can too — the server trusts the loopback boundary, not
  origins.) The WebSocket does not check `Origin`.
- **Persistence:** every successful `POST /api/config` writes the full
  config to `~/.autoedo.json` (`%USERPROFILE%\.autoedo.json` on Windows) —
  or to the `--config PATH` when given — and the process reloads it on
  start. `POST /api/midi` is runtime-only, never persisted. If your app
  owns all state itself, either mirror it into the engine at startup with
  one full config POST, or treat the engine's saved file as the truth and
  hydrate from `GET /api/status`.

## 2. Endpoints

| Endpoint | Method | Purpose |
|---|---|---|
| `/ws` | WebSocket | status pushed ~10×/s as JSON text frames (see §3) |
| `/api/status` | GET | the same status JSON, on demand (same cached string) |
| `/api/config` | POST | **the write path**: partial config update (see §4) |
| `/api/devices` | GET | audio devices + MIDI sources (see §5) |
| `/api/scales` | GET | the 702-scale catalog (see §6) |
| `/api/midi` | POST | set virtual held MIDI notes (see §7) |
| `/api/restart` | POST | force an engine restart; responds with fresh status |
| `/` | GET | the built-in UI (don't load it while your UI is driving) |

All request/response bodies are JSON. POST bodies need no particular
`Content-Type`. Responses are `Connection: close` (one request per
connection); the WS is the only long-lived connection.

## 3. The status stream

Connect `ws://127.0.0.1:<port>/ws`. The server pushes the complete status
object ~10×/s and after every config change; it never reads inbound frames
(send nothing; pings are unanswered — a dead client is detected by send
failure and dropped). **Render from this stream and poll nothing.** If no
frame arrives for ~1.5 s, show the engine as offline and reconnect (retry
~1/s); fall back to polling `GET /api/status` at ~2/s while the socket is
down. Both carry the identical string, serialized once per tick server-side.

```jsonc
{
  "running": true,            // audio engine alive
  "error": "",                // human-readable engine error when !running
  "inputRate": 48000,         // capture device rate, Hz
  "outputRate": 48000,        // render device rate (DSP runs at this rate)
  "latencySamples": 2700,     // total latency in output frames
  "latencyMs": 56.2,     // shifter block + input cushion + device buffer
  "detectedHz": 224.49,       // last detected input f0 (stale when !voiced)
  "targetHz": 220.0,          // current correction target
  "voiced": true,             // false = dry passthrough (show readouts dimmed)
  "traceSeq": 12345,          // detections ever made; stitch frames by this
  "trace": [[224.4,220.0], ...],
                              // pitch trace, oldest first: one [detected,
                              // target] pair per detection hop (~200/s, ~48
                              // per frame; detected 0 = unvoiced). Frames
                              // overlap — a consumer keeps the last traceSeq
                              // it drew and appends only the new tail. This
                              // is what a pitch-graph view renders from
  "harmDeg": [52, null, null, null, null],
                              // per-voice live ghost degree (signed steps re
                              // root octave 0); null = voice silent. Muted /
                              // solo-suppressed voices still report a degree
                              // so a UI can dim rather than hide them.
  "midiNotes": [60, 64, 67],  // currently held MIDI notes (hardware ∪ virtual)
  "inputName": "MacBook Pro Microphone",
  "outputName": "External Headphones",
  "shifter": "Signalsmith Stretch 1.3.2",  // the vendored pitch-shift engine
  "formantSupport": true,     // formants held still under pitch shift
  "synthPatches": ["pad","warm","glass","organ","sine","strings",
                   "choir","brass","solina bright","bass"],
                              // the engine's synth-harmony patch table, in
                              // order — build a `synthPatch` picker from this
  "synthEnvActive": false,    // is the synth envelope in the signal path at
                              // all? synthAttackMs/synthReleaseMs are live
                              // and always honoured, but only a SYNTH-sourced
                              // voice (or the drone) has an envelope — a
                              // pitch-shifted ghost just rides a ~5 ms
                              // click-free mix ramp. False means those two
                              // controls are inapplicable right now, not
                              // dead: grey them and say why
  "stepCents": 100.0,         // period / edo, stretch-adjusted
  "config": { ... }           // the FULL config object of §4 — the echo your
                              // UI syncs its controls from
}
```

Echo etiquette (what the built-in UI does, recommended): after a local edit,
ignore the config echo for ~2.5 s on the touched control (or while a drag /
open editor is active) so the round trip can't fight the user's hand — the
stream stays the truth, it just waits for the finger to lift.

## 4. Writing: `POST /api/config`

Send any subset of keys; unknown keys are ignored, present keys are clamped
into range, applied, persisted, and the response body is a fresh full status
(§3). Live keys apply within one audio block. Keys marked **restart** tear
down and rebuild the audio engine (~100 ms of silence) when their value
actually changes — batch them.

### Tuning
| Key | Type / range | Applies | Meaning |
|---|---|---|---|
| `edo` | int 10–72 | live | divisions of the equave. The degree mask and harmony intervals are *not* rescaled for you — see notes below |
| `rootNote` | int 0–11 (0 = C) | live | root pitch class; degree 0 sits on the root |
| `rootCents` | float −50…50 | live | root fine offset |
| `refA4` | float 400–480 | live | reference A4 in Hz |
| `stretchCents` | float −30…30 | live | octave stretch: period = 1200 + this |

Derived values your UI will need (identical to the engine's math):

```
period    = 1200 + stretchCents                    (cents)
step      = period / edo                           (cents)
refHz     = refA4 · 2^((rootNote − 9)/12) / 16 · 2^(rootCents/1200)
freq(j)   = refHz · 2^(j · period / (1200 · edo))  (degree j, signed, re root octave 0)
degree(f) = round(edo · 1200 · log2(f / refHz) / period)
```

**Read this before touching `refA4`.** The EDO grid is built off **degree
0**, which `rootNote` places. With `rootNote` = C, **C is the anchor and is
the same frequency in every EDO** — 12, 22, 31, all of them — and the degree
you would *call* A falls wherever that EDO puts it. In 22-EDO that is 433.12
or 446.99 Hz. Never 440.

That is correct behaviour, not drift. `refA4` is only the arithmetic that
gets there: it is the A of a *12-EDO* grid on the same root, an expression
of the one anchor, not an independent knob and not a claim about where A
sits in the tuning in use. Nudging it because "our A isn't 440" moves **C**,
and moves the whole rig off the band by the amount you nudged it. In 22-EDO
a 54.5-cent step means a reference error over ~27 cents also starts flipping
which degree a note snaps to, so the damage is a wrong note, not just a
detune.

Set `refNote` + `refNoteHz` instead and the trap does not exist. A
C-anchored rig at concert pitch is `{"rootNote": 0, "refNote": 0,
"refNoteHz": 261.6256}` — which is what a default install already computes,
so if that is your standard there is nothing to change.

| `leadShiftSteps` | int −72…+72 | live | **static transpose of the corrected lead, in EDO steps, applied after the snap.** The detector still hears and classifies the real note — tolerance, stickiness and retune all run against what was played; the shift only moves what comes out. Whole steps, so `±edo` is an exact equave and keeps the pitch class: the degree mask never notices. **Locked ghosts stack their intervals on the shifted lead** (and the mask walk happens up there), so the harmony stays a scale interval from the note the audience hears. The published `targetHz` and pitch trace show the shifted target. Effective total shift (correction + transpose) is safety-clamped at ±36 semitones in the audio path. Pairs with `detectMinHz`: on a guitar channel, raise the detection floor off the low strings to kill octave-down locks, then `+edo` here for an octave-up lead — instead of asking the detector to work down where the subharmonics live |

### Scale mask
| Key | Type | Applies | Meaning |
|---|---|---|---|
| `degrees` | array of 0/1, up to 72 entries | live | `degrees[d] = 1` lights pitch class `d` (root-relative). Entries beyond the current `edo` are stored but ignored, and survive EDO changes. **All-zero mask (within the edo) = full-chromatic fallback**, not silence |
| `scaleCat`, `scaleName` | strings ≤ 63 | live | cosmetic labels for "which catalog scale is loaded"; the mask itself is the real state. Convention: set them when applying a catalog scale, clear to `""` on any hand edit |

Correction snaps the detected pitch to the nearest lit degree; sparse masks
mean big jumps (the built-in UI warns above a ~240¢ largest gap — that's a
UI behavior you may want to replicate).

### Correction
| Key | Type / range | Applies | Meaning |
|---|---|---|---|
| `retuneMs` | float 0–400 | live | within-note retune speed (0 = hard snap) |
| `transitionMs` | float 0–200 | live | glide between *different* target degrees |
| `amount` | float 0–1 | live | partial correction (1 = full) |
| `toleranceCents` | float 0–50 | live | dead zone around the target; preserves vibrato |
| `stickiness` | float 0–1 | live | hysteresis past the midpoint before re-snapping. **Engine does not auto-raise it** — the built-in UI raises it to `min(0.7, (edo−24)/48)` when edo > 41; do the same if you care about high-EDO flicker |
| `humanize` | float 0–1 | live | relaxes retune on sustained notes |
| `bypass` | bool | live | dry input passthrough (also silences harmony) |
| `leadOn` | bool | live | the corrected lead voice in the output mix. `false` = **harmony only**: ghosts (shifted or synth) still follow the sung pitch while the singer stays out of the engine's output — for rigs that take the dry voice from another bus. `bypass` still wins (a dry passthrough with the lead muted would be silence) |
| `outputGainDb` | float −60…12 | live | master output gain. The output stage soft-clips: transparent below ≈ −2 dBFS, then a smooth saturation that never reaches full scale — so a many-voice harmony stack (worst on a mono-folded `outputChannel` bus) saturates gently instead of crackling as hard digital clipping. If a big stack sounds *compressed*, that is the clip working: pull `hg` per voice (≈ −6 dB for five voices) or `outputGainDb` down until it cleans up |
| `quality` | `"low"` \| `"balanced"` \| `"high"` | **restart** | pitch-shifter analysis block: 25 / 45 / 120 ms **at 48 kHz**, giving 31 / 56 / 150 ms of shifter latency. The block is a fixed *sample count* (48k-referenced), so a faster device shortens it in time — at 96 kHz the same presets are 12.5 / 22.5 / 60 ms blocks and the latency halves with them. The analysis window shortens too: frequency resolution at the bottom of the range drops, so if a bass or low guitar warbles at 96 k, step up a preset. Correction is accurate at all three; harmony intervals only stay in tune on low voices at `high` |
| `refNote` | int 0–11 | live | which note `refNoteHz` names (0 = C). Default **0**, so the standard reads as C |
| `refNoteHz` | float | live | the frequency of `refNote` in octave 4 — **state the pitch standard in the terms the rig actually uses**. Concert C is 261.6256. Parsed after `refA4`, so a POST carrying both lands on this one |
| `range` | string | **restart** | detection window preset: `bass` (55–400 Hz), `baritone` (65–450), `tenor` (80–600), `alto` (100–800), `soprano` (130–1200), `guitar` (78–1400), `instrument` (65–1600, default), `wide` (40–2000). Unknown names behave as `instrument`. Latency follows the low limit |
| `detectMinHz`, `detectMaxHz` | float, 0 or 20–500 / 100–4000 | **restart** | explicit detection window, overriding whatever `range` names. `0` (default) = use the preset. **Set these for any instrument whose real range you know.** A period longer than the lowest note the source can play is, by definition, not that source's pitch, and a window that admits one invites a subharmonic lock — the classic "the guitar suddenly went an octave down". The engine also runs an octave guard inside the detector (it prefers the shortest lag whose periodicity is within 90% of the best), but the cheapest fix is to not offer it the wrong answer. `detectMaxHz` is raised to `2 × detectMinHz` if it would otherwise be below it |

### Devices
| Key | Type | Applies | Meaning |
|---|---|---|---|
| `inputUid`, `outputUid` | string | **restart** | device UIDs from `/api/devices`; `""` = system default |
| `inputChannel` | int 0–32 | **restart** | which capture channel of the input device feeds the (mono) engine, 1-based. `0` = backend default: the device's first channel on macOS, a mix of all channels on Windows. A channel past the device's count fails engine start with `error` naming the channel. This is how a per-channel rig binds one instance to input 1 (voice) and another to input 2 (guitar) of the same interface (§10) |
| `outputChannel` | int 0–32 | **restart** | which playback channel of the output device carries the engine's output, 1-based. `0` = default: stereo on the device's first two channels (harmony panned). `N` = ALL output — corrected voice plus harmony, mono-folded — on that one channel, silence elsewhere, so an instance can own a single bus of a multi-out interface (voice → out 3, guitar → out 4). A channel past the device's count fails engine start with `error` naming the channel |
| `bufferFrames` | int 32–2048 | **restart** | preferred hardware block size |

### Instance identity
| Key | Type | Applies | Meaning |
|---|---|---|---|
| `label` | string ≤ 63 | live | cosmetic instance name ("Voice", "Guitar"). The built-in UI shows it in the header and tab title so two instances read apart; the engine itself ignores it |

### Harmony (Xentar `hm`/`hx` packing)
| Key | Type | Applies | Meaning |
|---|---|---|---|
| `harmOn` | bool | live | master harmony switch. **Deliberately not restored from the config file**: it is performance state, not a setting, so a fresh engine always comes up with the ghosts silent no matter what was on when the last instance exited. (`droneOn` is the same.) The key is still written to the file and still echoed — only the *launch value* is forced false |
| `harmGlideMs` | float 0–5000 | live | **portamento**: how long a ghost takes to reach a new target when the lead moves — **constant-time**: the slide is linear in cents at a rate fixed when the leg starts, arrives in this many ms, and locks on (not a one-pole time constant, which would be 63% of the way when the time was up and asymptotic ever after). A landed voice tracks its target exactly between legs, so ghosts still ride the lead's vibrato. `0` (default) = jump, the classic harmonizer. One number for BOTH sources — applied once, upstream — so a shifted voice and a synth voice on the same interval never disagree about where they are mid-slide. A voice arriving from silence always starts *on* pitch instead of swooping in from its last note. Momentary voicing dropouts (every consonant, and the frame at a note change) do **not** restart the glide: what counts as "from silence" is whether the voice's own envelope has actually decayed, not whether this frame was voiced |
| `harmSustain` | bool | live | **sustain the shifted ghosts through their release.** A shifted ghost is made *of* the input, so when the lead stops there is normally nothing left for its release to shape. With this on the engine lifts a slice from the end of the note — a whole number of pitch periods, crossfaded at the tail against the material preceding it so the wrap is seamless — and loops that into the harmony shifters while the release rings out. Default **on**; `synthReleaseMs` still bounds it, so a short release sustains nothing. Never reaches the lead |
| `harmHold` | bool | **live, momentary** | **HOLD.** Freeze every ghost at its current pitch and level and keep it sounding indefinitely, while the lead goes on tracking normally — sing a chord in, hold it, keep singing over your own choir. See §HOLD below for the full contract |
| `harmGainDb` | float −24…+12 | live | **harmony bus master**: one gain over every ghost — shifted, synth and drone alike — applied last on the bus, after the harmony IR and tilt, so pulling it down takes the reverb tail with it. It never touches the lead. Per-voice `hg` trims ride **on top of** it, not instead of it: `hg[v] = −6` under `harmGainDb = −12` is −18 dB on that voice. Smoothed over ~5 ms, so it is safe to sweep live |
| `harmSource` | `"voice"` \| `"synth"` | live | what the ghosts are made of: pitch-shifted copies of the live input (classic harmonizer) or the built-in synth at the same target degrees. Applies to all five voices (phase 1). Synth ghosts are volume-matched to the sung level — `hg` trims from parity — and, unlike shifted ghosts, ring past the end of the sung note by the release time, holding their last pitch and level |
| `synthPatch` | string | live | synth sound, by name from the status `synthPatches` list. Simple ranks: `pad` (detuned saws + low-pass, the backing-pad default), `warm` (rounded sine stack), `glass` (brighter octaves), `organ` (drawbar harmonics), `sine` (pure). String-machine voices, with per-partial vibrato and the shared **ensemble** (three delay taps swept in opposite senses per side — the Solina/Eminent trick that turns a rank of saws into a wide, breathing section): `strings` (saw ranks at 8'/4'/2'), `solina bright` (the same ranks with the tone control open, for sitting on top of a band), `choir` (vox-humana square/saw blend, rounded down), `brass` (bright detuned saw pair, no ensemble — a section pad that cuts), `bass` (sub-octave rank, filtered down, no ensemble — a wandering bass is a tuning problem, not an effect). Unknown names are ignored |
| `hSrc` | string[5] | live | **per-voice** source override: `"voice"`, `"synth"`, or `"default"` (follow `harmSource`, the default). Mixed rigs work — voice 1 on the shifter for a real double, voices 2–3 on the synth for a pad — because the shifted and synth passes render independently into one bus |
| `leadSource` | `"voice"` \| `"synth"` | live | what the **lead** is: the shifter's corrected vocal (default) or the synth playing that same corrected pitch. A synth lead has no dry component — unvoiced is silence, not the raw microphone — so it is a synth lead, not a passthrough with occasional synth. `leadOn` still decides whether it reaches the output at all |
| `synthAttackMs` | float 0–5000 | live | synth envelope attack (default 80) |
| `synthReleaseMs` | float 0–10000 | live | synth envelope release (default 500) |
| `ensembleDepth` | float 0–1 | live | how much of the ensemble the patches that have one get (default 1). 0 leaves the dry ranks; patches without an ensemble ignore it |
| `harmTiltDb` | float −12…+12 | live | **tilt EQ on the harmony bus**: a 700 Hz pivot with equal and opposite gains above and below it, so negative is darker and positive brighter while the pivot stays put. It reaches every harmony voice — shifted and synth alike — and never the lead, which is on its own bus |
| `synthVowel` | float 0–1 | live | **vowel transfer**: the live voice's spectral envelope is lifted onto the synth, so a sung "ah" → "oo" moves the synth with it (default 0 = off). It applies to synth harmony voices and to a synth lead. Both transfer modes *filter* the carrier — they cannot invent partials — so they need a harmonically rich patch: `strings`, `solina bright`, `pad`, `organ` and `choir` all work; `sine` barely changes. Level is held where the volume match put it |
| `vowelMode` | `"vocoder"` \| `"lpc"` | live | how the vowel transfer estimates the voice. `vocoder` (default): a 16-band channel vocoder — robust, band-quantized, the classic robot-choir color. `lpc`: **formant-corrected resynthesis** — an order-18 all-pole (LPC) fit of the vocal tract, resolved continuously rather than in bands, driven by the voice's own whitened excitation so consonants ride through. Sharper vowels, more "a person singing through the synth", slightly more sensitive to noisy input. Reflection coefficients are interpolated between frames, which keeps the filter stable while the tract moves; a peak-aware normaliser and safety saturator hold the level where the volume match put it. Ignored while `synthVowel` is 0 |
| `droneOn` | bool | live | **drone**: one synth voice pinned to an ABSOLUTE degree, sustained while on regardless of what the singer does (a root-only chart chord means "drone that root"). Uses the current `synthPatch` and attack/release, volume-matched like the ghosts (level rides the frozen input RMS, so it breathes with the set instead of blasting before the first note), centred on the harmony bus, through the ensemble and the tilt but deliberately NOT the vowel transfer (a drone has no mouth to follow — the vocoder's gating would mute it between phrases). `harmOn` still gates it |
| `droneDeg` | int 0–8·edo | live | the drone's absolute engine degree (`4·edo` = the middle-C-region root, like the MIDI-harmony mapping) |

### IR points (convolution spaces)

Two convolution points from the shared `irconv` library (vendored byte-identical with Treebrain): **lead** — the corrected voice, whose first partition is direct time-domain so the live monitored path gains ZERO samples of latency — and **harm** — the stereo harmony bus, sitting post-ensemble and **pre-tilt** (the tilt stays the performer's final tone trim over whatever space the IR imposes). The spec's `irLead {…}` / `irHarm {…}` objects are spoken as flat keys here (this parser's shape); substitute `Harm` for `Lead` throughout:

| Key | Type | Applies | Meaning |
|---|---|---|---|
| `irLeadPath` | string | live* | WAV to load (PCM 16/24 or float32, mono/stereo, **at the engine rate** — Treebrain's librarian keeps per-rate caches; a mismatch is refused, never resampled). `""` clears the point. *Applied by a ~30 ms instance crossfade — no zipper, no dropout — with the file read and FFT fills on the control thread |
| `irLeadHash` | string | live* | FNV-1a 64-bit of the file bytes, 16 lowercase hex digits (the librarian's manifest convention). Verified before a byte is trusted; a mismatch refuses the load and lands in status `irError`. `""` skips verification |
| `irLeadPredelayMs` | float 0–50 | live* | pre-delay before the space; changes ride the same crossfade |
| `irLeadMix` | float 0–1 | live | wet/dry blend, smoothed (~10 ms) |
| `irLeadGainDb` | float −24…+12 | live | wet gain, smoothed |
| `irLeadOn` | bool | live | off = dry passthrough (the tail rides out through the mix smoothing); a fully faded-off point costs a copy, not a convolution |

A stereo WAV into the mono lead folds to mid; into the harm point its channels convolve L/R independently. IRs cap at 2 s (the engine's prepare ceiling). Status echoes every key plus `irError` (the last load failure, cleared by a success), and a fresh engine reloads the persisted points on start. |
| `harmLock` | `"off"` \| `"mask"` \| `"ji"` | live | ghost quantize: raw parallel / snap to lit degrees (walk outward, up first per distance) / snap to the JI landmark set (1/1, 9/8, 7/6, 6/5, 5/4, 4/3, 11/8, 3/2, 8/5, 5/3, 7/4, 9/5, 15/8) |
| `hm` | int[5], each −72…72 | live | voice intervals in EDO steps; 0 = voice off. Keep within ±`edo` (the pool is ±equave); intervals are *steps*, so re-clamp when you lower the EDO |
| `hx` | int[5], 0–2 | live | per-voice octave extension, stacking in the voice's direction: `eff = hm + sign(hm)·hx·edo` |
| `hd` | float[5], −100…+100 | live | **per-voice detune, cents.** Applied *after* the `harmLock` quantize, so the lock cannot snap it back out, and *before* the dedupe, so two voices on the same interval detuned apart both sound instead of collapsing into one. Its other job: `hm[v] = 0` normally means "voice off", but **`hm[v] = 0` with a nonzero `hd[v]` is a UNISON ghost** — the thickener you want at, say, −4 cents. Exact unison with no detune stays "off", because a phase-coherent double of the lead is a comb filter, not a voice |
| `hg` | float[5], −60…+12 | live | per-voice gain, dB, **relative to the lead**: `0` puts that ghost at the lead's own level. Two things make that literally true rather than approximately. The pan law is normalised so dead centre is unity into *both* sides of the bus, matching the mono lead, instead of the textbook 0.707 that would sit every centred ghost a fixed 3 dB down. And the shifter is level-matched across the formant stage — formant compensation costs real energy on upward shifts (≈ −4 dB at a fifth, ≈ −6 dB at an octave), which is a tonal choice with an unwanted level side effect, so a slow (~100 ms) makeup restores it. The ceiling is +12 because the whole range is now headroom above the lead rather than half of it spent climbing back to parity |
| `hp` | float[5], −1…1 | live | per-voice pan (constant-power) |
| `hMute`, `hSolo` | 0/1[5] | live | mute / solo (solo among harmony voices only) |

**How a ghost's pitch is derived.** The *interval* comes from the snapped
degrees: the lead's post-snap degree plus `hm`/`hx`, then the `harmLock`
quantize, so the interval is exact by construction. That interval is then
stacked on the pitch the performer is actually **hearing** as the lead —
`leadOn: true` means the engine's corrected lead, `leadOn: false` means the
note that was really played, because with the lead muted the reference in
the room is the instrument itself. Shifted and synth ghosts use the same
number, so a voice on either source lands on the same pitch.

That second half matters whenever the lead is not fully corrected — `amount`
below 1, a note inside the `toleranceCents` dead zone, or a glide still in
flight. In all three the lead sits off its ideal degree, and a ghost pinned
to the *degree's* ideal frequency beats against it. With `amount: 1` and the
note settled the two definitions coincide exactly, so nothing changes for a
fully-corrected lead. If ghosts still read out of tune against the
instrument, check `refA4` and `stretchCents` against how it is actually
tuned: the whole grid hangs off those two.

Voices landing on one pitch dedupe (no gain doubling) but still report their
degree in `harmDeg`. Down-shift depth is bounded by the detection range's
longest period.

### Attack Sound

| Key | Type | Applies | Meaning |
|---|---|---|---|
| `attackSound` | `"off"` \| `"noise"` \| `"pick"` \| `"click"` | live | a transient fired at note ONSET — triggered by energy appearing (a fast follower overtaking a slow one), several detection hops **before the pitch is known**. Its purpose: cover the synth voices' attack latency. Pair it with a long `synthAttackMs`: the slow envelope hides the machinery of the note arriving, and the attack sound covers the moment of the pick itself. Fires whenever a ghost (either source) or a synth lead is in the path — shifted ghosts get the cover too, same scope as the envelope: their onset is as synthetic as a synth ghost's, the shifter's latency swallowing the real pick. Only a rig with nothing on the bus (harmony off, shifted lead) gets no hits. ~80 ms refractory, so one hit per pick. Rendered into the harmony bus before its IR/tilt/master (it sits in the ghosts' space) but through **no envelope and no vocoder** |
| `attackGainDb` | float −60…+12 | live | the attack sound's own gain, dB **relative to the note's own onset level** (each hit is volume-matched to the input's attack, so soft notes get soft picks). Default −26 — Xentar's shipped 5%, "felt more than heard". Turn it up toward 0 when it is doing the covering-the-attack job |

`"pick"` is the Xentar pick-noise set (Build 2753), embedded: 3 string ranges × 2 pick directions, range chosen from the last known pitch (low < 130 Hz ≤ mid < 220 Hz ≤ hi), direction from the economy-picking state machine — a repeated range alternates down/up; crossing to a higher range continues DOWN, to a lower one UP, the pick travelling with the hand. Each hit gets ±8% playback-rate and ±26% level jitter, so a run reads as a player, not a sampler. `"noise"` is a randomized high-passed white chiff (~60 ms); `"click"` a damped high tick. All three follow the same trigger, matching and gain law.

### HOLD (momentary)

`{"harmHold": true}` engages, `{"harmHold": false}` releases. It is a
**momentary** control: hold the CC down, not a toggle. It is written to the
settings file like every other key but is **forced false at launch** — a rig
must never come up with a frozen choir nobody pressed for.

What the rising edge does, once:

- captures the sustain loop from whatever is being sung at that instant (or
  keeps the loop from the end of the last note, if nothing is);
- latches the input level the ghosts will hold at.

While engaged:

- ghost pitches, degrees and gates are frozen — the harmony targeting does
  not run at all, so nothing sung afterwards retargets the choir;
- shifted ghosts ride the sustain loop, so they keep sounding in the
  performer's own timbre rather than transposing whatever is being sung over
  them;
- synth ghosts hold their note and their latched level;
- **the lead is untouched.** It detects, corrects and outputs exactly as
  normal. That is the point of the feature.

On release the ghosts are handed back to the live pitch and glide to it if
`harmGlideMs` is set, or release naturally if nothing is being sung.

Everything else stays live while held: `harmGainDb`, `hg`, `hp`, `hMute`,
`hSolo`, `harmTiltDb` and the IR all still respond, so the choir can be
faded, panned and shaped after it is parked. Changing `hm`/`hx`/`hd` while
held does nothing until release — the pitches are frozen by definition.

**Suggested MIDI CC mapping.** Treat it as a gate: CC value ≥ 64 →
`{"harmHold": true}`, < 64 → `{"harmHold": false}`. Send only on the
transition, not on every CC frame; the engine is idempotent either way but
the POST is a lock acquisition. A sustain-pedal CC (64) behaves exactly
right. For a latching footswitch, hold the state in Treebrain and send the
edges.

### MIDI Harmony
| Key | Type | Applies | Meaning |
|---|---|---|---|
| `midiMode` | bool | live | while held notes exist they override the mask: correction snaps to the nearest **held** note (absolute), harmony's `mask` lock quantizes within the held pitch classes; no notes held = normal behavior |
| `midiSource` | string | **restart** | a name from `/api/devices.midiSources`, or `""` = all inputs |

Note mapping: MIDI 60 (middle C) → degree `4·edo` (the root, four equaves
up ≈ octave 4); each semitone = one EDO step: `j = 4·edo + (note − 60)`.

## 5. `GET /api/devices`

```jsonc
{
  "devices": [
    { "uid": "AppleHDAEngineInput:1B,0,1,0:1", "name": "MacBook Pro Microphone",
      "inputs": 1, "outputs": 0, "rate": 48000,
      "defaultInput": true, "defaultOutput": false },
    ...
  ],
  "midiSources": ["Keystation 61", ...]
}
```

Enumerated fresh per call (cheap; refresh on demand, not on a timer). Filter
by `inputs`/`outputs` > 0 for the respective pickers. UIDs are the stable
handles to persist; names are display-only. `inputs` is the channel count
`inputChannel` selects from (on Windows it is the shared-mode mix format's
channel count — what a capture client actually receives).

## 6. `GET /api/scales`

The full Xentar scale dump: `{ reference12Cents: {name: cents[]|null},
quantized12Rule, edos: { "<pack>": { edo, defaultScale, categories:
{ cat: { name: { steps:[...], octaves?:[[...],...], description? }}}}}}`.
Steps are 0-based pitch classes within one equave, root-relative. To apply a
scale: light exactly `steps` in `degrees` (use `octaves[0]` = `steps` for
multi-octave entries — the engine is single-octave; badge it), set
`scaleCat`/`scaleName`. Use packs whose `edo` matches the current EDO;
skip the `bohlenpierce` pack (tritave equave). The `reference12Cents`
scales quantize into any EDO via
`steps = sort(dedupe(round(cents·edo/1200) mod edo))` (Chromatic `null` =
all steps), dropping results identical to a native scale.

## 7. `POST /api/midi`

```json
{ "notes": [60, 64, 67] }
```

Replaces the **virtual** held-note set (≤ 32 notes, 0–127); `[]` releases.
Virtual notes OR with hardware MIDI input and appear in `midiNotes`. This is
how a controller plays chords into MIDI Harmony without a keyboard (on-screen
keys, a sequencer, a network bridge). Not persisted; cleared by an engine
restart — re-send after any restart-triggering config change.

## 8. Client-side features you may want to replicate

These live in the built-in UI, not the engine — the API gives you the
primitives: Near-12 / Near-JI mask filters, invert/rotate, computed presets
(chain-of-fifths etc.), Scala `.scl` import/export (mask ↔ cents lines),
user preset storage, A/B snapshots (two full-config objects, swap via one
POST each), audition tones, and the stickiness auto-raise from §4.

## 9. Minimal control session

```bash
autoedo --port 8017 &
until curl -sf localhost:8017/api/status >/dev/null; do sleep 0.5; done

# devices, then bind them (one restart)
curl -s localhost:8017/api/devices
curl -s -X POST localhost:8017/api/config \
  -d '{"inputUid":"<uid>","outputUid":"<uid>","bufferFrames":256,"range":"tenor"}'

# 19-EDO, meantone major mask, transparent correction, a locked P5-ish voice
curl -s -X POST localhost:8017/api/config -d '{
  "edo":19, "rootNote":0, "refA4":440,
  "degrees":[1,0,0,1,0,0,1,0,1,0,0,1,0,0,1,0,0,1,0],
  "retuneMs":40, "amount":0.85, "toleranceCents":8, "stickiness":0.2,
  "harmOn":true, "harmLock":"mask", "hm":[11,0,0,0,0], "hg":[-3,0,0,0,0]}'

# then subscribe ws://localhost:8017/ws and render
```

**Conformance checklist for a full-control UI:** renders from the WS stream
with offline dimming + reconnect · syncs controls from the config echo with
a dirty-window guard · surfaces `running:false` + `error` with a restart
affordance · batches restart-triggering keys · handles the chromatic
fallback and largest-gap warning · re-clamps `hm` and re-labels intervals on
EDO change · re-sends virtual MIDI after restarts · greys the synth envelope
controls on `synthEnvActive:false` (inapplicable, not dead).

**Deciding whether a key is real.** The config echo is the contract: a key
that appears in `status.config` is implemented, and the value that comes
back is the value the engine is using — clamped if you sent something out of
range. A key the engine does not know is dropped silently, so it never
appears in the echo; comparing what you sent against what came back is the
whole test, and it needs no version negotiation.

## 10. Multi-instance rigs (one engine per input channel)

The engine is mono by design. To process several inputs of one interface
independently (e.g. MOTU M4: input 1 = voice, input 2 = guitar), run one
instance per channel. Each instance is the full API of this spec on its own
port; nothing about the wire contract changes.

Per instance, give it:

- **its own port** — `--port 8017` / `--port 8018` (`AUTOEDO_PORT` via the
  launcher);
- **its own settings file** — `--config ~/.autoedo-guitar.json`
  (`AUTOEDO_CONFIG` via the launcher). Without this, both instances load
  and rewrite `~/.autoedo.json` and the channels' settings cross-contaminate;
- **its channel bindings** — `POST /api/config {"inputChannel": 1}` (voice)
  vs `{"inputChannel": 2}` (guitar), same `inputUid`; add `outputChannel`
  to give each instance its own playback bus of the shared interface
  (mono-folded — e.g. voice → out 3, guitar → out 4) instead of both
  stacking on channels 1–2;
- optionally **a label** — `{"label": "Voice"}` — so the built-in UIs read
  apart.

Launcher calls are per-instance and per-port (stop included):

```bash
AUTOEDO_PORT=8017 <repo>/live/tools/autoedo.command --headless --no-ui
AUTOEDO_PORT=8018 AUTOEDO_CONFIG="$HOME/.autoedo-guitar.json" \
    <repo>/live/tools/autoedo.command --headless --no-ui
```

Shared-device fine print:

- Both instances may open the same audio device; macOS AUHAL and
  shared-mode WASAPI both allow concurrent clients.
- macOS: the preferred hardware buffer size is a device-wide property —
  instances sharing a device should use the same `bufferFrames` (last
  writer wins, best-effort, non-fatal).
- Windows: winmm grants a hardware MIDI input to **one** process; with
  `midiSource` unset both instances try to open everything and only one
  gets each source. Bind `midiSource` deliberately, or drive MIDI Harmony
  over `POST /api/midi` (virtual notes need no device).
- The processed outputs of both instances mix at the output device —
  give each its own `outputChannel` when they should stay on separate
  jacks instead.
