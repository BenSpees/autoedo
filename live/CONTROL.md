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
- **Launching — two options.**

  **(a) Spawn the binary directly** and own its lifetime:
  `./build/autoedo --port 8017`. Readiness = `GET /api/status` answering
  200 (poll ~2/s, allow ~10 s). Stop with SIGTERM (POSIX) / `taskkill /IM
  autoedo.exe` (Windows); it shuts down cleanly.

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
  exits. The launcher is idempotent — calling it while the engine runs
  restarts it (rebuild included), so "launch" and "relaunch after update"
  are the same call. `run.sh` is a thin alias for `autoedo.command` with
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
  config to `~/.autoedo.json` (`%USERPROFILE%\.autoedo.json` on Windows),
  and the process reloads it on start. `POST /api/midi` is runtime-only,
  never persisted. If your app owns all state itself, either mirror it into
  the engine at startup with one full config POST, or treat the engine's
  saved file as the truth and hydrate from `GET /api/status`.

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
  "latencySamples": 2160,     // total latency in output frames
  "latencyMs": 45.0,     // shifter block + input cushion + device buffer
  "detectedHz": 224.49,       // last detected input f0 (stale when !voiced)
  "targetHz": 220.0,          // current correction target
  "voiced": true,             // false = dry passthrough (show readouts dimmed)
  "harmDeg": [52, null, null, null, null],
                              // per-voice live ghost degree (signed steps re
                              // root octave 0); null = voice silent. Muted /
                              // solo-suppressed voices still report a degree
                              // so a UI can dim rather than hide them.
  "midiNotes": [60, 64, 67],  // currently held MIDI notes (hardware ∪ virtual)
  "inputName": "MacBook Pro Microphone",
  "outputName": "External Headphones",
  "shifter": "Signalsmith Stretch 1.1.1",  // the vendored pitch-shift engine
  "formantSupport": false,    // true once a library release with formant
                              // control is vendored (see third_party/README)
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
| `outputGainDb` | float −60…12 | live | master output gain |
| `quality` | `"low"` \| `"balanced"` \| `"high"` | **restart** | pitch-shifter analysis block: 25 / 45 / 120 ms, which *is* the shifter's latency. Correction is accurate at all three; harmony intervals only stay in tune on low voices at `high` |
| `range` | string | **restart** | detection window preset: `bass` (55–400 Hz), `baritone` (65–450), `tenor` (80–600), `alto` (100–800), `soprano` (130–1200), `instrument` (65–1600, default), `wide` (40–2000). Unknown names behave as `instrument`. Latency follows the low limit |

### Devices
| Key | Type | Applies | Meaning |
|---|---|---|---|
| `inputUid`, `outputUid` | string | **restart** | device UIDs from `/api/devices`; `""` = system default |
| `bufferFrames` | int 32–2048 | **restart** | preferred hardware block size |

### Harmony (Xentar `hm`/`hx` packing)
| Key | Type | Applies | Meaning |
|---|---|---|---|
| `harmOn` | bool | live | master harmony switch |
| `harmLock` | `"off"` \| `"mask"` \| `"ji"` | live | ghost quantize: raw parallel / snap to lit degrees (walk outward, up first per distance) / snap to the JI landmark set (1/1, 9/8, 7/6, 6/5, 5/4, 4/3, 11/8, 3/2, 8/5, 5/3, 7/4, 9/5, 15/8) |
| `hm` | int[5], each −72…72 | live | voice intervals in EDO steps; 0 = voice off. Keep within ±`edo` (the pool is ±equave); intervals are *steps*, so re-clamp when you lower the EDO |
| `hx` | int[5], 0–2 | live | per-voice octave extension, stacking in the voice's direction: `eff = hm + sign(hm)·hx·edo` |
| `hg` | float[5], −60…6 | live | per-voice gain, dB |
| `hp` | float[5], −1…1 | live | per-voice pan (constant-power) |
| `hMute`, `hSolo` | 0/1[5] | live | mute / solo (solo among harmony voices only) |

Voices ride the *corrected* pitch; voices landing on one pitch dedupe
(no gain doubling) but still report their degree in `harmDeg`. Down-shift
depth is bounded by the detection range's longest period.

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
handles to persist; names are display-only.

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
EDO change · re-sends virtual MIDI after restarts.
