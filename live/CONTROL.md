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
| `rootNote` | int 0–11 (0 = C) | live | **the SCALE's tonic — display and scale semantics only. It never moves the grid.** Varies per song; the tuning does not. (It used to anchor the grid — see the pin below) |
| `rootCents` | float −50…50 | live | root fine annotation, echoed for the rig's scale logic; **inert in the engine's tuning math**. Grid-level trims belong to `refNoteHz` |
| `refA4` | float 400–480 | live | reference A4 in Hz |
| `stretchCents` | float −30…30 | live | octave stretch: period = 1200 + this |

Derived values your UI will need (identical to the engine's math):

```
period    = 1200 + stretchCents                    (cents)
step      = period / edo                           (cents)
refHz     = refA4 · 2^((refNote − 9)/12) / 16
freq(j)   = refHz · 2^(j · period / (1200 · edo))  (degree j, signed, re reference octave 0)
degree(f) = round(edo · 1200 · log2(f / refHz) / period)
```

**THE PIN (2026-08-17): root is scale, reference is tuning.** Degree 0
sits on the **reference note** (`refNote`, default C) — never on the
root. `rootNote` names the song's tonic and changes per chart;
`refNote`/`refNoteHz` name the pitch standard and never move mid-set. The
engine used to anchor degree 0 on the root, which silently transposed the
whole grid — samples, targets, everything — with the song's key: in
22-EDO a root of D displaced C by −18.6 cents, and the field damage was a
rig that "only sounded right" with a false reference dialled in. Check
`status.anchorHz`: it now equals the reference in octave 4 regardless of
root.

**Read this before touching `refA4`.** The EDO grid is built off **degree
0**, which `refNote` places. With `refNote` = C, **C is the anchor and is
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
concert-pitch rig is `{"refNote": 0, "refNoteHz": 261.6256}` — what a
default install already computes — and `rootNote` is then free to follow
the song without consequence to the tuning.

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
| `expression` | float 0–1 | live | **how much of the playing survives correction.** A played pitch is a note plus what the player is doing to it; the engine tracks the note's centre with a ~180 ms follower and the difference is the bend, the vibrato, the scoop. Correction is applied to the **centre alone** and the deviation is added back on top. `1` (default): bends and vibrato reach the output — and the harmony, which anchors to the corrected lead — while the note still lands on its degree. `0`: the old behaviour, output pinned to the degree. **A steady note is identical either way**; only motion faster than the follower is at stake. Without this the law `shift = target − detected` cancels a bend exactly as it happens — the harder you bend, the harder the engine bends back — which is why expression never reached the output |
| `bypass` | bool | live | dry input passthrough (also silences harmony) |
| `bypassOutput` | `"dry"` \| `"mute"` | live | **what `bypass` PUTS on the output.** `dry` (default) is the historical behaviour: the input passes through uncorrected. `mute` puts silence there instead — for a rig whose dry already reaches the desk on its own row, where a passthrough is a second copy of a signal the mix already has. It decides what bypass does and nothing else: with `bypass` false the live path is untouched either way. This replaces the stateful `outputGainDb: -60` dance (drop the fader, remember what it was, restore it on un-bypass) with one switch that has no level to put back |
| `leadOn` | bool | live | the corrected lead voice in the output mix. `false` = **harmony only**: ghosts (shifted or synth) still follow the sung pitch while the singer stays out of the engine's output — for rigs that take the dry voice from another bus. `bypass` still wins (a dry passthrough with the lead muted would be silence) |
| `outputGainDb` | float −60…12 | live | master output gain. The output stage soft-clips: transparent below ≈ −2 dBFS, then a smooth saturation that never reaches full scale — so a many-voice harmony stack (worst on a mono-folded `outputChannel` bus) saturates gently instead of crackling as hard digital clipping. If a big stack sounds *compressed*, that is the clip working: pull `hg` per voice (≈ −6 dB for five voices) or `outputGainDb` down until it cleans up |
| `quality` | `"low"` \| `"balanced"` \| `"high"` | **restart** | pitch-shifter analysis block: 25 / 45 / 120 ms **at 48 kHz**, giving 31 / 56 / 150 ms of shifter latency. The block is a fixed *sample count* (48k-referenced), so a faster device shortens it in time — at 96 kHz the same presets are 12.5 / 22.5 / 60 ms blocks and the latency halves with them. The analysis window shortens too: frequency resolution at the bottom of the range drops, so if a bass or low guitar warbles at 96 k, step up a preset. Correction is accurate at all three; harmony intervals only stay in tune on low voices at `high` |
| `refNote` | int 0–11 | live | which note `refNoteHz` names (0 = C). Default **0**, so the standard reads as C |
| `refNoteHz` | float | live | the frequency of `refNote` in octave 4 — **state the pitch standard in the terms the rig actually uses**. Concert C is 261.6256. Parsed after `refA4`, so a POST carrying both lands on this one |
| `range` | string | **restart** | detection window preset: `bass` (55–400 Hz), `baritone` (65–450), `tenor` (80–600), `alto` (100–800), `soprano` (130–1200), `guitar` (70–1400, sized for a bottom string at D2 — drop-D and the D-based tunings; standard low E rides inside it), `instrument` (65–1600, default), `wide` (40–2000). Unknown names behave as `instrument`. Latency follows the low limit — and so does detection **responsiveness**: the analysis window is two periods of `detectMinHz`, so raising the floor to the instrument's real bottom is the single cheapest way to make lock-on faster (at the guitar preset a fresh pluck locks in ~32 ms; a 40 Hz `wide` floor nearly doubles that). The floor is a hard wall, not a preference: a note below `detectMinHz` is **invisible**, so size the window for the tuning actually in use — 78 Hz (the old guitar floor, sized for low E) silently erased a drop-D low string |
| `polyMode` | bool | **restart** | **POLY: chords, at the cost of latency** — the pedal trade. Detection is bypassed and the whole (chordal) input runs through fixed-ratio spectral shifters — the same [Signalsmith Stretch](https://github.com/Signalsmith-Audio/signalsmith-stretch) instances the mono path uses, which is the strongest permissively-licensed real-time polyphonic shifter available; poly is an analysis MODE here, not a different algorithm. The analysis block **doubles** (balanced: 56 → 112 ms latency at 48 kHz, echoed in `latencyMs`/`processLatencyMs`), which is what buys chord fidelity. What still works: `leadShiftSteps` moves the chord (±edo = clean octaves), the harmony voices become their intervals applied to the chord (`hm`/`hx`/`hd`/`hg`/`hp` all live), formant controls, IR points, tilt, master, the record send, `followEnv` (energy needs no pitch), and the envelope-gated wet/dry crossfade. What is inert, because there is no detected note: correction (`amount`, stickiness, retune), `harmLock`, MIDI Harmony, HOLD, sustain, synth sources, Attack Sound, the FOLLOW sender's note, `expression`. The **scale mask stays live**: it does not touch the fixed-ratio shifted audio, but the multi-f0 tracker snaps against it — it governs the chord sampler's note choices and `polyDetected`'s `cents`. `detectedHz`/`targetHz` read 0; `shiftSt` reads the fixed lead shift. **The exception is `leadSource:"sample"` with a bank loaded — the chord sampler** (the EHX MEL9 move): a multi-f0 tracker (harmonic-sum salience with iterative spectral subtraction, note birth/death hysteresis so one glitchy frame neither strikes nor kills a note) follows up to 6 simultaneous notes in the playing, snaps each to the enabled-degree EDO grid, applies `leadShiftSteps`, and **strikes the loaded sample library per note** — play a chord, hear the library play it, at any tuning. `sampleMix` keeps its meaning (1 = the library replaces the playing, 0.5 = layers it under the shifted chord), `sampleVelocity`/`sampleVelRefDb` set strike level, `sampleRing` governs re-strikes, and a note's end decays under `leadReleaseMs`. Onsets are staged first (automatic): a pluck doubles the tracker rate for 150 ms and lets a note birth on a single tracker sighting (the pluck itself is the second witness) — a fresh chord's first sample lands within ~20 ms — **re-strums retrigger** (an onset on a ringing chord strikes fresh samples for the re-plucked notes; an onset the per-note salience test cannot attribute — a still-loud ring hides the jump — re-strikes every sounding note ~70 ms in rather than staying silent, since re-striking validated pitches can never be a wrong note), and an early tracker guess corrected within ~120 ms crossfades out in 6 ms rather than ringing wrong. Reliability guards (research batch 2026-08): the tracker's energy gate rides `gateDb` instead of an absolute floor (quiet playing tracks), the missing-fundamental guard relaxes below ~160 Hz (a low string's weak fundamental is not erased), pick-transient smears and quiet harmonic ghosts are refused as births (a real octave/twelfth over a ring passes — it arrives at comparable level), and a chord member's strike level is its salience share compressed (sqrt), so quiet members stay present. Honest limits: detection is octave-blind on doublings (a power chord's octave merges into one note), and close intervals (thirds) resolve from ~C3 up — below that only wide voicings, a window-resolution fact. The layering (`sampleMix` ≈ 0.5) hides the onset trail well — the mix's dry side is the LATENCY-FREE input, so the hands carry the attack and the library blooms in under it. For the full Mellotron construction add the **MEL layer** (`melMix` below): a fixed-ratio octave-stack transform of the playing itself, swelled in to own the sustain — zero decisions, so tracker misses degrade as timbre, never as silence |
| `polyNotes` | int 1–6 | live | chord-sampler polyphony cap: strike at most this many simultaneous notes. Default **6**. Only meaningful with `polyMode` on and `leadSource:"sample"`. Status reports the live count in `polyNotesActive` |
| `gateDb` | float 0 or −80…−30 | live | **the noise/trigger gate**, in dBFS RMS. One knob, one meaning: the voicing gate, the onset detector's floors and the envelope gate all scale from it. `0` (default) = the built-in −56. **If quiet playing loses notes — especially sample strikes — lower this** toward the rig's real noise floor; a hot pickup rig can raise it. Echoed as the value in force (−56 when default) |
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
| `synthAttackMs` | float 0–5000 | live | the **HARMONY** envelope's attack (default 80) |
| `synthReleaseMs` | float 0–10000 | live | the **HARMONY** envelope's release (default 500) |
| `leadAttackMs` | float 0–5000 | live | the **LEAD's own** attack (default 5, i.e. the click guard and no shaping). Separate from the harmony's because the two shape different things: `synthAttackMs` hides the ghosts' arrival latency, this shapes the corrected lead itself |
| `leadReleaseMs` | float 5–10000 | live | the **LEAD's own** release (default 500). What it buys depends on the lead source, and the difference is not a wart — it is what each source is. A **synth** lead is an oscillator, so this is a real tail at its last pitch. A **sample** lead is a recording, so this is a CEILING over its natural decay: a note that would ring past where the player wanted it is closed here (see `sampleRing`). A **shifted** lead is made *of* the input, so once the input stops there is nothing left to sustain and the release is inert by construction — it crossfades to the dry path on the voicing drop, and a lingering wet would sound every consonant twice |
| `ensembleDepth` | float 0–1 | live | how much of the ensemble the patches that have one get (default 1). 0 leaves the dry ranks; patches without an ensemble ignore it |
| `harmTiltDb` | float −12…+12 | live | **tilt EQ on the harmony bus**: a 700 Hz pivot with equal and opposite gains above and below it, so negative is darker and positive brighter while the pivot stays put. It reaches every harmony voice — shifted and synth alike — and never the lead, which is on its own bus |
| `synthVowel` | float 0–1 | live | **vowel transfer**: the live voice's spectral envelope is lifted onto the synth, so a sung "ah" → "oo" moves the synth with it (default 0 = off). It applies to synth harmony voices and to a synth lead. Both transfer modes *filter* the carrier — they cannot invent partials — so they need a harmonically rich patch: `strings`, `solina bright`, `pad`, `organ` and `choir` all work; `sine` barely changes. Level is held where the volume match put it |
| `vowelMode` | `"vocoder"` \| `"lpc"` | live | how the vowel transfer estimates the voice. `vocoder` (default): a 16-band channel vocoder — robust, band-quantized, the classic robot-choir color. `lpc`: **formant-corrected resynthesis** — an order-18 all-pole (LPC) fit of the vocal tract, resolved continuously rather than in bands, driven by the voice's own whitened excitation so consonants ride through. Sharper vowels, more "a person singing through the synth", slightly more sensitive to noisy input. Reflection coefficients are interpolated between frames, which keeps the filter stable while the tract moves; a peak-aware normaliser and safety saturator hold the level where the volume match put it. Ignored while `synthVowel` is 0 |
| `droneOn` | bool | live | **drone**: one synth voice pinned to an ABSOLUTE degree, sustained while on regardless of what the singer does (a root-only chart chord means "drone that root"). Uses the current `synthPatch` and attack/release, volume-matched like the ghosts (level rides the frozen input RMS, so it breathes with the set instead of blasting before the first note), centred on the harmony bus, through the ensemble and the tilt but deliberately NOT the vowel transfer (a drone has no mouth to follow — the vocoder's gating would mute it between phrases). `harmOn` still gates it |
| `droneDeg` | int 0–8·edo | live | the drone's absolute engine degree (`4·edo` = the middle-C-region root, like the MIDI-harmony mapping) |

### Sample voices (the Xentar pitched library)

A third ghost source beside `voice` and `synth`. **Treebrain is the
librarian**: it transcodes the shipped MP3s once into mono float32 WAV at
the engine's rate and hands over a cache root. The engine never decodes an
MP3, never resamples, and refuses any file whose header rate is not the
engine's — a cache at the wrong rate would play at the wrong pitch *and*
the wrong speed.

| Key | Type | Applies | Meaning |
|---|---|---|---|
| `samplePath` | string | live (reloads) | absolute root of the per-rate cache. The engine reads `<samplePath>/<instrument>/*.wav` |
| `sampleManifest` | string | live (reloads) | optional file list. **Any JSON shape works**: the engine harvests the `".wav"` strings out of it and derives instrument, zone, layer and variant from the FILENAME, which already carries all four. There is no schema to agree on and a manifest change cannot break the engine. Absent/unreadable → the instrument directory is scanned instead |
| `sampleHash` | string | live (reloads) | the librarian's "the library changed" token. The bank is re-read only when this moves — 120 WAVs is not a per-POST operation |
| `sampleInstrument` | string | live (reloads) | which instrument folder to load. **The engine carries no instrument list** — it is whatever `<samplePath>/` actually holds, so adding one (e.g. `pizzicato`) is dropping a folder in the cache and needs no engine or controller change. Status echoes `sampleInstruments` (what was found, sorted); build the picker from that, as with `synthPatches`. The shipped sets are `piano` `electric` `acoustic` `bass` `vibraphone` `choir` `harpsichord` `oboe` `pizzicato`, plus the 46 **Acoustic Instruments v2.1.0** instruments when that pack is in the cache — 18 strings/lamellophones/keyed percussion (`banjo` `dantranh` `folkharp` `concertharp` `strumstick` `psaltery` `kalimba` `mbira` `handchimes` `mbiramavembe` `chamberorgan` `ocarina` `celesta` `musicbox` `dulcimer` `koto` `shamisen` `sitar`) 6 winds/brass (`tenorsax` `tenorsaxvib` `trumpetharmon` `trombone` `frenchhorn` `clarinet`), and v2.1's 22 additions (bowed strings `violin` `viola` `cello` `contrabass` each with a pizzicato set, `harpsichord` `vibraphone` `vibraphonebowed` `recordertenor` `recorderbass` `pipeorgan` `tubularbells` `ocarinalarge` `balafon` `wineglasses` `harmonica` `saxello` `marimba` `kalimbakenya`) — plus the private `lapsteel` set committed under `live/samples/` (licence-restricted; see its NOTICE). **⚠ The pack's `harpsichord` and `vibraphone` collide with the factory folder names, and `harpsichord` is octave-hazardous: the `"auto"` octave table applies +12 BY NAME (the factory set's filenames sit an octave low), which is wrong for the pack's sounding-pitch set — cache the pack's under a distinct name, or replace the factory folder AND pin `sampleOctave: 0`.** A zone with only soft takes plays its soft pool at every velocity (v2.1's contrabass pizzicato ships eight such notes), and bank normalisation is capped where the loudest sample would reach −3 dBFS (a bowed swell reads near-silent in the 300 ms window; the boost clamp alone allowed +20 dB). **v2.0.0 relabelled ten instruments up an octave from v1.x** (audio unchanged — v1.x names were an octave below sounding pitch); a cache built from v1.x must be re-extracted, not patched. Filenames are sounding pitch (`sampleOctave` stays `"auto"`), round-robin pools run up to **11 deep** on one banjo note, and six of the sets (`celesta` `musicbox` `dulcimer` `koto` `shamisen` `sitar`) are **CC BY 3.0** — the built-in UI shows the required Fluid (R3) attribution while one is selected; any other app shipping those files needs its own line. **Feature-detect on this key** |
| `sampleMix` | float 0…1 | live | **the sample section's one mix.** The sample voices (lead and ghosts alike) against the **DRY instrument — the latency-free input**, only the hardware buffer between the hands and the ear, never the detector's pipeline. `0.5` = **both at full level** (not a crossfade; the shared equal-power taper, −3 dB at the quarter travel), `1` = samples alone, `0` = dry alone. The dry side follows the config INTENT: a sample lead whose library failed to load still passes the instrument at the mix's dry gain — a bad path must never silence the player. The old behaviour (layering against the SHIFTED path, pipeline latency included) is gone, chord sampler included: the dry covers the onset better than a delayed shift ever did |
| `sampleVelocity` | float **−1…1** | live | fixed strike level. **Any negative value means MEASURE it** from the lead's own attack; `-1` is the canonical spelling and the default. `0…1` pins the strike and bypasses the map entirely, `0` included (a pinned 0 is silence, not the floor). The range is −1…1 rather than 0…1 precisely so "measure" is expressible in the same key — it is echoed as `-1`, so read the echo, don't assume 0 |
| `sampleMatch` | float 0…1 | live | **where the strike level comes from.** `1` (default): **SOURCE PARITY** — the sample's body lands at the level the note was actually played, measured as body RMS (120 ms past the pick) against the bank's load-time normalisation, so the library speaks at your instrument's own loudness and the faders are taste, not compensation. `0`: the relative velocity map alone (0.2…1 against `sampleVelRefDb`), the pre-parity behaviour. In between, a dB-domain crossfade. A pinned `sampleVelocity` bypasses parity entirely. Parity is clamped to −34…+12 dB so a noise floor is never renormalised into fortissimo. Strike levels are also **smoothed strike-to-strike** (all modes): a verdict moves at most 6 dB per note — one odd measurement is a nudge, a deliberate dynamic change walks the whole way in two or three notes (field: "very touchy… too many very loud or very quiet notes") |
| `sampleLevelDb` | float −60…+12 | live | **the sample section's one fader**: a master over EVERY sample voice — the lead and all five ghosts — applied at render (a move reaches notes already ringing), BEFORE the mix with the dry. Lead and harmony sample voices share the whole gain chain (strike level × this × per-instrument trim × mix), so they produce the same base volume by default |
| `sampleToneDb` | float −12…+12 | live | **the sample section's tone**: the same one-pole tilt EQ `harmTiltDb` uses (700 Hz pivot, ± half the value each way), on the sample voices only — dry and shifted paths untouched |
| `melMix` | float 0…1 | live | **the MEL layer (experimental)** — the EHX 9-series move, translation without transcription: a fixed-ratio TRANSFORM of the playing itself, layered under the chord sampler. The layer is the latency-matched dry (the 8′ footage) plus the whole chord shifted one clean octave through the lead shifter (the 4′ — idle in chord-sampler mode anyway), swelled in under its own envelope. Zero decisions live in it: when the tracker drops a note mid-chord this layer still carries it, so detection errors degrade as **timbre**, never as missing or wrong notes — and a harmonic ratio is tuning-agnostic (a 22-EDO chord at 2:1 is still 22-EDO; no grid in the path). The division of labour with the sampler is deliberate: the sampler's fast strikes (~15–20 ms) cover the attack, the swell blooms the transform in to own the sustain — which also makes the shifter's poly latency read as intent rather than lag. Formant controls and the lead IR sculpt it (the layer rides the lead bus). Default **0** (off). Only active with `polyMode` on, `leadSource:"sample"` and a bank loaded; the record-send `wet` tap stays samples-only. The layer's VOICE is `melPreset` below — `footage` is this plain stack, the instrument presets are the full spectral remodeller |
| `melPreset` | string | live | **the remodeller's instrument** — each preset is a tiny parameter set (ratios, gains, spectral shapes, artifact depths), never stored audio, which is the 9-series' actual architecture. `"footage"` (default) is the plain dry + octave stack, exactly the original layer. The rest replace the raw dry with a **unison copy through the shifter bank** — resynthesis smears the pick transient, the strongest "this is a guitar" cue — plus harmonic footages, each under its own low-pass spectral shape, the sum under shared tape wow + flutter (one capstan: every layer detunes together), soft saturation and a bandwidth rolloff, and a **per-onset swell re-trigger** (each attack blooms; without it the swell only fired after silence). `"organ"` (1:1 + 2:1 + 3:1, clean), `"flute"` (dark unison, whisper of octave, most wow), `"strings"` (unison + octave + a detuned ensemble copy), `"brass"` (the swell drives a **filter sweep** — the spectral trajectory is the attack, plus a sub-octave), `"choir"` (dark, wide detune, slow wow). Harmonic ratios are tuning-agnostic: every preset stays in tune in any EDO |
| `melOctDb` | float −60…+12 | live | the 4′ octave footage's gain against the 8′ dry inside the MEL layer — **`footage` preset only**; the instrument presets carry their own gains. Default **−6** |
| `melAttackMs` | float 0…5000 | live | the MEL layer's swell. The swell IS the instrument identity — an onset that blooms reads as bowed/brass/tape — and it is also what buries the transform's latency. Default **150** |
| `melReleaseMs` | float 5…10000 | live | the MEL layer's tail ceiling after the strings stop. Default **400** |
| `strikeTilt` | float −6…+6 | live | **the strike measurement's pitch weighting**, dB per octave around A3 (220 Hz), default **+3**: each octave below the pivot is discounted that much, each octave above credited. Raw RMS is not loudness — a guitar's low strings carry far more electrical RMS than the ear credits, so parity (and the velocity map) without this made low notes strike loud and high notes fade (field report, verbatim). Measurement only, never the audio; in POLY each tracked note is weighted at its own pitch. `0` turns it off |
| `sampleVelRefDb` | `"auto"` \| float −60…0 | live | **the strike map's reference**, in dBFS — the peak that counts as "playing hard" (see the velocity note below). `"auto"` (default) observes it: a rolling peak of measured onsets decaying over ~20 s. A number **asserts** it, for a host that already knows how hard this player plays and should not have to wait for the engine to rediscover it note by note — TENDRIL's loudest onset of the capture, an FX layer's own rolling peak. A supplied reference is **held exactly**: neither decayed nor raised by a louder note (the `max()` inside the map still keeps that note at unity rather than past it). Returning to `"auto"` hands the reference back to observation *from where it was* rather than resetting — a reset would make the next note read as the loudest so far. Echoed in the units it is written in; `status.sampleVelRefDb` is always the number actually in force, so a pinned rig sees the two agree |
| `sampleRing` | bool | live | **let-ring.** `true` (default): a struck sample voice plays to its natural end *through* the next strike — the next note is a new string, not this one being re-fretted, so a fast figure stacks into the chord a real instrument would leave ringing. `false`: damp-on-repitch legato — the sounding voice is retired across a 6 ms fade as the new one is struck. **The ring is bounded**: from the moment it is superseded a note decays under the release ceiling — `synthReleaseMs` for ghosts, `leadReleaseMs` for the lead — with its natural end still applying if that comes sooner. Unbounded ring is not sustain, it is a wash of old notes under every new one (the ear reads it as mud). Four playback slots per voice; a fifth strike steals the slot furthest through its recording, which is the oldest and by then the quietest. Turning it off mid-set damps what is already ringing rather than stranding it |
| `sampleOctave` | `"auto"` \| int −24…+24 | live (reloads) | filename-to-**sounding** pitch offset in semitones. `"auto"` (default) uses the built-in table for the two shipped sets whose filenames are not their sounding pitch — **bass** is named an octave ABOVE what it sounds, **harpsichord** an octave BELOW — and 0 for everything else. A number overrides it for any set not in that table. Wrong here means the ghost is a clean octave off, so it is worth checking on any new instrument |
| `harmSource`, `leadSource`, `hSrc[]` | += `"sample"` | live | the source selectors all take it |

Status: `sampleNormDb` (the level correction the bank measured for itself)
and `sampleOctaveApplied` (the filename→sounding offset in force),
`sampleInstruments` (the instruments discovered under `samplePath`
— a directory counts when it holds at least one file the zone parser
recognises), `sampleVelLast` (the level the voices were last struck with — a
strike level you cannot see is one you cannot tune), `sampleVelRefDb` (the
reference that level is relative TO; a relative map is unreadable from
outside without it — 0.55 says nothing until you know what it is 0.55 of), `sampleZones` /
`sampleFiles` (what actually loaded), `polyNotesActive` (notes the chord
sampler is sounding, i.e. capped by `polyNotes` — 0 outside poly mode),
`polyDetected` (the poly detection itself, exported — see below), and
`sampleError` (the last failed
load; a failure leaves the **running bank playing** rather than going
silent, so a bad path during a set is a message, not a hole).

**`polyDetected` — the poly detection, exported.** In poly mode the
multi-f0 tracker runs whether or not a bank is striking, and status
carries its live chord as an array (empty outside poly mode, or when
nothing sounds):

```json
"polyDetected": [
  {"note": 48, "hz": 130.9, "cents": -3.2, "level": 1.0, "id": 17},
  {"note": 52, "hz": 165.1, "cents":  2.8, "level": 0.84, "id": 18}
]
```

- `note` — the same MIDI-style encoding the FOLLOW link uses (60 = engine
  degree `4×edo`, one step per degree, equave-folded to 0–127), so a
  consumer that already reads `followNote` reads this for free.
- `hz` — the raw tracked pitch, pre-snap.
- `cents` — offset from the snapped enabled-degree pitch: a **polyphonic
  tuner** with no further math.
- `level` — relative 0..1, loudest note of the frame ≈ 1. Deliberately
  not absolute velocity.
- `id` — stable for the note's whole life, so a poller can tell a
  re-struck note from a continuing one across status ticks.

The tracker updates every quarter analysis window (~21 ms at 48 kHz);
status ticks at ~10 Hz, so very short notes can be born and die between
polls — `id` is what keeps the stitching honest. `polyNotes` caps the
**sampler**, not this list: all tracked notes are exported.

**What is genuinely different here, and where a straight port of a browser
sampler goes wrong.** A ghost is continuous; a sample is struck. So a
sample voice is struck at the lead's ONSET — the same energy edge the
attack sound fires on, several detection hops **before the pitch is
known** — and then **re-pitched, never re-struck**, for as long as that
note lasts. A retrigger starts a fresh slot across a 6 ms fade rather than
cutting a sounding one (Xentar's node-swap discipline, ported).

- **Velocity is a measurement, and it is RELATIVE.** Peak over the first
  30 ms from the foot of the attack, mapped against how hard *this player
  plays when playing hard* — not against full scale:

  ```
  ref   = max(refPeak, peak)          // never asks for more than unity
  below = 20·log10(peak / ref)        // ≤ 0 dB
  t     = clamp(1 + below / 24, 0, 1) // a 24 dB window below the reference
  vel   = 0.2 + 0.8·t                 // floor 0.2
  ```

  This map is shared verbatim with Treebrain's FX layer and TENDRIL, so the
  two rigs strike the same velocity for the same playing. The constants —
  **24 dB window, 0.2 floor** — are a contract; change them in both places
  or not at all.

  It replaced an absolute map (−40 dBFS → 0, full scale → 1) that was
  field-verified wrong, and the reason is worth keeping written down
  because no synthetic test will ever show it. A real interface is set up
  with 12–20 dB of headroom, so hard playing peaks at −12…−20 dBFS and
  never approaches full scale. The absolute map therefore scored the gain
  staging rather than the playing: every velocity on the rig sat in the
  bottom half of its range, and quiet notes fell off the bottom and
  vanished. The 0.2 floor is the other half of that fix — a note the
  detector CONFIRMED is a quiet note, not an absent one.

  The **reference** is the caller's — which is why `sampleVelRefDb` lets a
  host supply it outright. Left on `"auto"` the engine observes its own: a
  rolling peak of measured onsets that decays over ~20 s, raised only by
  onsets (never by sustain, so a long held note
  cannot talk itself into being a hard strike), never decaying below
  −40 dBFS, and reported as `sampleVelRefDb`. It starts at that floor
  rather than at a plausible-looking seed, because a seed outranks the
  player until it decays and every note until then is scored against a
  number nobody played; starting low costs exactly one note — the first of
  a set reads as the loudest so far, because it is.

  The strike itself cannot wait for the 30 ms window without giving back
  the latency, so a voice is struck at the fast follower's reading and the
  window refines it.
- **The soft layer is TIMBRE, not level.** `<Note>_soft` files are softer-
  *played* recordings peak-normalised to the main layer; loudness always
  comes from the velocity gain. Below velocity 0.6 the soft pool is
  preferred **where one exists** — harpsichord has none by design, and
  individual zones can lack one too (pizzicato's D2 does); both fall back
  to the main pool rather than being special-cased.
- **Sharps are spelled both ways.** `F#1` and `Fs1` are the same zone; the
  parser takes either, so a set using `#` (pizzicato) and one using `s`
  (the older instruments) can sit side by side in one cache.
- **Round robin is per (instrument, zone, layer) and never the previous
  pick.** A repeated note that reuses its recording machine-guns at once.
  Pools load whole up to 12 variants deep (the Plucked Acoustics banjo
  ships 11 takes on one note).
- **Level is measured per BANK, not assumed.** The shipped sets are
  peak-normalised to their own targets, and those targets are ~20 dB apart
  (measured here: piano −10.7 dBFS, acoustic −31.9, over the first 300 ms
  of each main-layer recording). The balance between instruments therefore
  lives in the file mastering, not in any runtime constant — so the engine
  measures each bank at load (median over the main layer) and normalises it
  to a common −22 dBFS reference. Switching instrument changes the timbre
  and not the level, and a set dropped into the cache at any mastering
  arrives usable. Status reports `sampleNormDb`, the correction applied.
- **Full-scale recordings are reported, not tolerated.** A properly
  mastered set peaks below 0 dBFS, so a file sitting exactly at it is the
  signature of a decode gone wrong rather than a mix decision — the
  shipped pizzicato set once carried 32 files decoded 24-bit-as-16-bit,
  which loaded and played perfectly happily as full-scale noise. Status
  reports `sampleClipped`; anything above 0 means re-transcode that set,
  and the count says how much of it. It also skews the level measurement
  above, though taking the median rather than the mean limits how far
  (measured on that set: 1.4 dB with 23 files corrupt).
  The measurement deliberately excludes the soft layer, which is
  peak-matched to its main layer on purpose: normalising per file would
  destroy exactly the relationship that makes the swap a timbre change.
- **Zone then RATE.** Nearest recording by pitch, then a fractional,
  **unquantised** playback rate — that is what lands a 22-EDO degree
  exactly off a 12-per-octave map.

`synthAttackMs` / `synthReleaseMs` shape the sample envelope on the ghosts,
as they do for every other source; a sample LEAD is shaped by
`leadAttackMs` / `leadReleaseMs` instead. Either way the release is a
CEILING over a recording's own decay, never an extension of it — under
`sampleRing` that is what stops a let-ringing figure from ringing past the
end of the phrase. Drums are out of scope by agreement — every layer here
is voiced by a pitch.

### Follow — one engine names the pitch (and rides the level) of another

The multi-instance rig's (§10) instances can be linked: a SENDER posts its
corrected degree and its input envelope to a RECEIVER, whose lead then
retunes to the sender's pitch class — sing anything, sound the guitar's
note. The receiver needs no new machinery: the note arrives through MIDI
Harmony (`midiMode: true`, `midiOctaves: "nearest"` — the sender names the
pitch CLASS, the singer names the register), OR'd in like a hardware note
so it never collides with a controller's virtual chords.

| Key | Side | Type | Applies | Meaning |
|---|---|---|---|---|
| `followUrl` | sender | string | live | `host:port` (or `http://host:port`) of the receiver; `""` = off. Malformed writes are refused into `status.followError` |
| `followHold` | sender | bool | live | default `true`: the last note stays held through the sender's silence, so the receiver keeps its pitch target between phrases. `false`: sender silence releases the receiver back to its own mask correction |
| `followEnv` | receiver | float 0–1 | live | envelope-follow depth. `0` (default): pitch only. `1`: the sender's envelope IS this instance's output envelope — **the sender going silent cuts this instance**, which is the talk-box shape. The level is the sender's ABSOLUTE envelope (gated to a clean 0 below its voicing gate), deliberately not normalised against any moving reference: a moving reference maps the same played level to different gains across a set |

Transport: the sender POSTs `{"note": n|-1, "level": 0..1}` to the
receiver's `/api/follow` — on every note change, on audible level motion
(~20 ms), and at least every 150 ms as a keepalive. The receiver enforces a
**400 ms TTL**: a sender that dies mid-note cannot pin the receiver's pitch
or leave it cut; the link decays to neutral. The degree is encoded exactly
(`n = 60 + (j − 4·edo)`, folded by whole equaves into MIDI range — lossless
for the pitch class in any EDO), so nothing re-quantizes in transit. Both
instances must share `edo` / `rootNote` / reference — the degree transport
assumes one grid.

Status: sender `followNote` (last posted, −1 none) and `followError`;
receiver `followLevelIn` (1.0 = neutral) and the followed note visible in
`midiNotes`. Latency is the sender's detection lock (~31 ms at the guitar
preset) plus a loopback POST (~1 ms) plus the receiver's own retune — a
fast legato follow, not a vocoder.

### The record send

A separate output channel for the recorder, so the stem on disk and the
level on stage are independent. What leaves on it never passes through
`outputGainDb`, and nothing about the live path — routing, level, latency —
changes when the send is configured.

| Key | Type | Applies | Meaning |
|---|---|---|---|
| `sendChannel` | int 0–32 | **restart** | 1-based output-device channel the send leaves on; `0` = no send. Restart-scoped like `outputChannel` (it lives in the device channel map). A channel past the device's count fails engine start naming the channel. **Collisions with the live output are refused, not summed**: a write putting the send on the live channel (or the live output onto the send) is rejected, the old value stays, and `status.sendError` names the refusal — a silent sum is a feedback loop with a friendly face. `sendError` is `""` after any accepted send write |
| `sendContent` | `"full"` \| `"wet"` \| `"lead"` \| `"harm"` | live | what rides the send. `wet` (default): everything the engine produced with the dry blend removed — the corrected wet lead plus the harmony bus, near-silent while unvoiced instead of passing the dry through, which is what makes it a real stem against the dry track the interface is already recording. `lead` / `harm` split that in two (`lead` is pre-lead-IR). `full`: exactly the live mono mix, dry blend and `outputGainDb` included — the "second copy of the PA" the send exists to improve on |
| `sendGainDb` | float −60…+12 | live | trim on the send only; record level and stage level are independent |
| `sendOn` | bool | live | **the safety.** Mute the send (click-free, ~10 ms smoothed) without touching routing. Defaults **off** and is deliberately **not restored at launch as on** — assert it from the controller at song load, and drop it on panic, exactly like the `harmOn` guards |

Status: `sendError` (above) and `processLatencyMs` — the **engine's own
path** at current settings, algorithmic plus the engine's buffering
(an alias of `latencyMs`, present so send-aware controllers can probe it
by name). It changes with `quality`, devices and `bufferFrames`; re-read
it from the echo after any restart-scoped write. Use it to align a
recorded return against a dry track captured at the interface: the two
then stay phase-sane instead of combing. The send taps **inside** the
engine, so hardware overhead genuinely does not apply to it — which is
also why `processLatencyMs` must never absorb device numbers.

**`totalLatencyMs` and `deviceLatencyMs`** (status): the honest
**mic-to-speaker** figure, and the hardware part of it.
`deviceLatencyMs` is everything outside the engine's own path: the input
side's collection cycle (capture arrives one hardware buffer late), plus
both sides' device, stream and safety-offset latencies as the hardware
reports them (the converters live in these numbers). The engine also now
reads back the buffer size the device actually **granted** — the request
is best-effort, and a device already open elsewhere keeps its owner's
size — and runs all latency arithmetic on the granted values.
`totalLatencyMs = latencyMs + deviceLatencyMs`. Queries are best-effort:
a property a device does not publish counts as zero, so the figure is
honest-or-low, never invented. **Display `totalLatencyMs` on a panel;
keep aligning recordings by `processLatencyMs`.** On the stub backend
`deviceLatencyMs` is 0 and the two figures agree.

Loopback-capable interfaces close the circuit internally: aim
`sendChannel` at a spare output pair and record its loopback input — no
cable, no engine work beyond this section. A shared-memory or UDP tap was
considered and deliberately not built while the send covers the need.

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
| `harmLock` | `"off"` \| `"mask"` \| `"ji"` | live | ghost quantize: raw parallel / snap to lit degrees (walk outward; **on a TIE the walk goes AWAY from the lead** — up for a ghost above, down for one below, so a third never collapses onto a second or a unison when both neighbours are equally close. A unison ghost has no apartness to preserve and keeps the historical up-first rule, as does a lead being corrected onto the mask) / snap to the JI landmark set (1/1, 9/8, 7/6, 6/5, 5/4, 4/3, 11/8, 3/2, 8/5, 5/3, 7/4, 9/5, 15/8) |
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

### Detection & rendering guards (no keys — behavior contract)

Three engine-side guards a controller should know exist, all always-on:

- **Octave re-vote rebase**: a near-equave jump in the detected pitch
  within one 5 ms hop is taken into the note's centre immediately rather
  than passed through as expression. No player moves an octave in 5 ms, and
  a real octave leap wants the same response, so the rule is safe either
  way: the correction ratio stays continuous through a detector re-vote.
- **The degree is chosen from the note's CENTRE**, not the instantaneous
  pitch. A vibrato wider than half a step would otherwise flip the target
  back and forth — in 22-EDO a step is 54.5 cents, which a guitarist
  crosses without trying.
- **Release rewind + slope-freeze**: a mute or lift drops level far faster
  than a string decays naturally, while often staying "voiced" through tens
  of ms of bent, dying pitch. While the level is collapsing (> ~4 dB down
  in 40 ms) harmony retargeting is frozen; at the voiced→unvoiced edge the
  ghosts and the sustain-loop capture rewind ~40 ms to the last clean
  detection. A note that was stable while held ends on that pitch. HOLD is
  exempt (its pitches were chosen at the press).
- **Attack-sound Schmitt trigger**: one hit per onset *edge*; the trigger
  re-arms only when the fast/slow envelope ratio collapses, so a refractory
  expiring mid-note cannot double-fire.

| Key | Type | Applies | Meaning |
|---|---|---|---|
| `formantSemitones` | float −12…+12 | live | **formant OFFSET** — a deliberate shift of the vocal tract in semitones, independent of pitch: + toward a smaller instrument, − toward a larger one. A character control, not a correction; 0 = neutral. Composes with `formantHold`: hold on = tract held still under the pitch shift, then offset; hold off = tract follows the pitch, then offset. The formant stage engages when either is non-neutral, so an offset works on a `formantHold:false` guitar channel too. Applies to the lead and every shifted ghost |
| `formantHold` | bool | live | hold formants still under the pitch shift (default **true** — right for a voice). **Set `false` on guitar** and other non-vocal sources: they have no vocal tract to preserve, and off removes the formant stage from the signal path entirely (the library skips its envelope machinery), taking it off the table as a tone suspect and saving CPU. Applies to the lead and every shifted ghost |
| `midiOctaves` | `"nearest"` \| `"held"` | live | how MIDI Harmony picks the correction octave. **`nearest` (default): the held note names the pitch CLASS, the player names the register** — the played note retunes to the held class right where it was played. `held`: snap to the held note's absolute octave (the original middle-C-pivot mapping) — which turns a chord voicing octaves below the lead line into a standing transpose: the "incredibly bassy corrected guitar" when MIDI mode is driving. Use `held` only when the chord track deliberately places the voice's register |

Status additions: `shiftSt` (the lead's current shift in semitones — graph it;
a wrong-octave correction is visible as the ratio swinging instead of a
bass mystery) and `engineBuild` (git short hash of the running binary —
"which build is the rig actually on" is now data).

### Diagnosing a wrong-sounding lead (the bisection)

The lead and a unison ghost share everything upstream (same input, same
detection, same shifter code) and differ in exactly four places: the ratio
each is told, the wet/dry crossfade (lead only), the lead IR point, and the
lead's output stage. That makes a wrong lead bisectable in four steps, each
flipping ONE difference:

1. **Read the ratio.** `shiftSt` / `shiftStMin` / `shiftStMax` in status
   while the problem is audible. Near 0 the whole time → the ratio is
   innocent, go to 3. Sitting or spiking octaves away → targeting: check
   the echo for `midiMode` (+`midiOctaves`), `leadShiftSteps`, the mask,
   and `detectMinHz`.
2. **`{"amount": 0}`.** Correction math out, audio path intact. Still
   wrong → not the targeting; go to 3. Clean → targeting confirmed.
3. **Ghost-as-control, done right.** `{"leadGainDb": -60}` with a unison
   ghost (`hm[0]:0, hd[0]:-4`) and **`leadOn` still true**: the ghost now
   carries the lead's exact correction ratio through the ghost path.
   (Toggling `leadOn` off instead re-anchors ghosts to the raw played
   pitch — ratio ≈ 1 — which tests nothing.) Ghost wrong too → the ratio
   really is bad despite step 1 (report `shiftSt` history). Ghost clean →
   the fault is lead-only: go to 4.
4. **Lead-only stages.** `{"formantHold": false}` (formant stage out);
   `irLeadMix: 0` / `irLeadOn: false` (IR out); `quality` low↔high (block
   size change — if the character changes, it is inside the shifter);
   `{"bypass": true}` (hardware sanity). One of these four flips is the
   answer.

5. **Restart vs block size.** `quality` is restart-scoped, so flipping it
   changes TWO things: the shifter block AND every piece of accumulated
   state. If step 4's quality flip was the one that helped, run
   `POST /api/restart` at the ORIGINAL quality next time the problem
   appears: clean after a bare restart → accumulated state (see
   `leadMakeupDb`); still wrong → genuinely the block size.

Two live meters for the loudness-path suspects: `leadMakeupDb` (the lead
shifter's level-match gain — pinned away from 0 dB means the level match
is working hard or stuck) and `outPeakDb` (pre-clip peak of the summed
output — above ≈ −2 dB the soft clip is shaping the sound, and sustained
saturation reads as thick/dark/"bassy", not as distortion).

Attach `engineBuild` to every report.

| Key | Type | Applies | Meaning |
|---|---|---|---|
| `leadGainDb` | float −60…+12 | live | the lead's own fader, after the wet/dry mix, before the output sum; ghosts never pass through it. Unlike `leadOn:false` it does NOT re-anchor the ghosts — they keep tracking the corrected lead. The mixing control the diagnosis above needs, and generally the lead-vs-choir balance |

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

## The local audio tap (`tapUrl`, `tapContent`)

The engine's processed output, published over **loopback UDP** — no cable,
no spare device channel, no relaunch. `tapUrl` (`host:port`, live, `""` =
off) aims it; `tapContent` picks the stem with the record send's own
vocabulary (`full` / `wet` / `lead` / `harm`; default `wet`). Align by
`processLatencyMs`, exactly like the record send — the tap sits inside
the engine, so device overhead does not apply.

Packet layout (little-endian, one per block, ≤256 samples):

| bytes | field |
|---|---|
| 0–3 | magic `AETP` |
| 4 | version (1) |
| 5 | content (0 full, 1 wet, 2 lead, 3 harm) |
| 6–7 | u16 sample count N |
| 8–11 | u32 sample rate |
| 12–19 | u64 first-sample counter |
| 20– | N × float32 samples |

The counter is **monotonic from engine start** and gapless in normal
operation: a reader aligns by it and detects drops without a handshake.
Fire-and-forget: no acks, no retransmit — it is loopback.

## Per-instrument level trims (`sampleGainDb`)

`{"piano": -3.5, "vibraphone": 2.0}` — dB trims keyed by the ids
`sampleInstrument` accepts, ±24 dB, live. A write replaces the whole
stored table (assert the complete balance, not deltas); ids for
instruments not currently present are kept, so a trim outlives an
instrument swap. Applied **at strike**, like the bank's own
normalisation — a ringing note keeps the trim it was struck with.
Echoed as the object in force.
