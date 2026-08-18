/* Platform audio backend interface.

   audio_mac.c implements this with CoreAudio (two AUHAL units bridged by a
   lock-free ring buffer, so input and output can live on different devices
   at different sample rates). audio_stub.c implements it with a synthetic
   signal generator for building/testing the rest of the app off-macOS. */

#ifndef AUTOEDO_AUDIO_H
#define AUTOEDO_AUDIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AE_UID_MAX  256
#define AE_NAME_MAX 256

typedef struct
{
    char   uid[AE_UID_MAX];   /* stable device identifier (persisted in config) */
    char   name[AE_NAME_MAX]; /* human-readable name */
    int    input_channels;
    int    output_channels;
    double nominal_rate;
    bool   is_default_input;
    bool   is_default_output;
} AeDeviceInfo;

/* Enumerate audio devices. On success *out is a malloc'd array the caller
   frees and *count its length. Returns 0 on success. */
int ae_audio_list_devices (AeDeviceInfo **out, int *count);

/* Parameters that can change while the engine runs (applied lock-free). */
typedef struct
{
    int      edo;             /* 10..72 */
    double   retune_ms;       /* 0..400: within-note retune speed */
    double   transition_ms;   /* 0..200: glide between different degrees */
    double   amount;          /* 0..1 partial correction */
    double   tolerance_cents; /* 0..50 dead zone around lit degrees */
    double   stickiness;      /* 0..1 hysteresis before re-snapping */
    double   humanize;        /* 0..1 relaxes retune on sustained notes */
    double   follow_env;      /* FOLLOW receiver depth 0..1: how much of the
                                 linked source's envelope shapes this
                                 instance's output. 1 = the source's silence
                                 cuts it; 0 (default) = pitch-only follow */
    double   expression;      /* 0..1: how much of the bend/vibrato survives
                                 correction. 0 pins the output to the degree,
                                 1 passes the whole deviation while the
                                 note's centre is still corrected */
    double   ref_hz;          /* frequency of degree 0 (root anchor) */
    double   period_cents;    /* octave size (1200 = true octave) */
    uint64_t degrees_lo;      /* bit d set => scale degree d enabled (d 0..63) */
    uint64_t degrees_hi;      /* degrees 64..71 in bits 0..7 */
    bool     bypass;          /* true = pass input through uncorrected */
    bool     bypass_mute;     /* what bypass PUTS on the output: false (the
                                 default) = the dry passthrough, true =
                                 silence. A rig whose dry already reaches the
                                 desk on its own row wants silence, and
                                 saying so is a plain switch instead of a
                                 stateful output_gain_db dance. */
    double   output_gain_db;  /* -60..+12 */
    bool     lead_on;         /* corrected lead voice in the mix; false =
                                 harmony only (bypass still wins). NOTE:
                                 also flips the ghosts' anchor to the
                                 detected pitch -- to silence the lead
                                 while ghosts keep tracking the CORRECTED
                                 lead, keep lead_on true and pull
                                 lead_gain_db down instead */
    double   lead_gain_db;    /* -60..+12: the lead's own fader, after the
                                 wet/dry mix, before the output sum. The
                                 ghosts never pass through it */

    /* The record send: a separate output channel for the recorder, so the
       stem on disk and the level on stage are independent. Content/gain/
       mute are live; the channel itself is restart-scoped (it needs the
       device's channel map). The send never passes through outputGainDb. */
    int      send_content;    /* AE_SEND_* */
    double   send_gain_db;    /* -60..+12 */
    bool     send_on;
    int      lead_shift_steps;/* -72..72: static lead transpose in EDO steps,
                                 applied after the snap; +-edo keeps the
                                 pitch class. Locked ghosts follow it. */

    /* MIDI Harmony: held notes override the mask (middle C = degree 4*edo,
       one EDO step per semitone). Off = held notes are ignored. */
    bool     midi_mode;
    bool     midi_fold;       /* fold held-note targets into the played
                                 register (retune to the held pitch class
                                 where the player is); false = absolute */

    /* Smart harmony (Xentar emulation): five independent ghost voices. */
    bool     harm_on;
    int      harm_lock;              /* 0 off, 1 mask, 2 JI */
    int      harm_interval[5];       /* signed EDO steps, 0 = voice off */
    int      harm_ext[5];            /* 0..2 octave extension, voice-directed */
    double   harm_master_db;         /* -24..+12: master over the whole ghost
                                        bus, on top of the per-voice trims;
                                        never touches the lead */
    double   harm_gain_db[5];        /* -60..+12, 0 = at the lead's level */
    double   harm_detune_cents[5];   /* -100..+100 fine offset; also what makes
                                        interval 0 a UNISON ghost, not "off" */
    double   harm_pan[5];            /* -1..1 */
    uint32_t harm_mute;              /* bit v */
    uint32_t harm_solo;              /* bit v */

    /* Harmony sustain: keep a shifted ghost sounding through its release by
       looping a slice of the end of the note (a shifted ghost has no source
       of its own once the input stops). HOLD freezes every ghost where it
       is and rings it indefinitely while the lead carries on -- momentary,
       meant for a footswitch or a MIDI CC. */
    bool     harm_sustain;
    bool     harm_hold;
    double   harm_glide_ms;          /* 0..5000 ghost portamento */

    /* Attack Sound: onset transient covering the synth voices' attack
       latency -- fires on energy, before the pitch is known; outside every
       envelope, with its own gain. 0 off, 1 noise, 2 pick, 3 click. */
    int      attack_sound;
    double   attack_gain_db;         /* -60..+12, default -26 */

    bool     formant_hold;           /* hold formants under the shift (voice);
                                        false = formant stage fully off
                                        (guitar). Default true */
    double   formant_st;             /* -12..+12: deliberate formant OFFSET in
                                        semitones -- a character control, not
                                        a correction. 0 = neutral. Works with
                                        hold on (tract held, then shifted) or
                                        off (tract follows pitch, then
                                        shifted); the stage engages if either
                                        is nonneutral */

    /* Harmony source: pitch-shifted live audio (0) or the built-in synth
       voice (1), which adds a patch and an attack/release envelope. Applies
       to all five voices. */
    int      harm_source;            /* 0 shifted, 1 synth, 2 sample */
    int      harm_voice_source[5];   /* per voice; -1 = follow harm_source */
    int      lead_source;            /* the corrected lead: 0/1/2 as above */
    double   sample_mix;             /* 0..1 layer blend, 0.5 = both at unity */
    double   sample_velocity;        /* >= 0 fixed strike level; < 0 = measure */
    double   sample_vel_ref;         /* SUPPLIED strike-velocity reference as
                                        a linear peak; < 0 = observe one.
                                        The map's reference is the caller's
                                        by definition, so a host that
                                        already knows it can say so */
    bool     sample_ring;            /* let-ring: a struck voice finishes on
                                        its own decay THROUGH the next
                                        strike. false = damp on repitch */
    double   sample_match;           /* sampleMatch 0..1: strike level source.
                                        0 = the relative velocity map alone,
                                        1 (default) = SOURCE PARITY -- the
                                        sample's body lands at the level the
                                        note was actually played */
    double   sample_tilt;            /* strikeTilt, dB/octave around A3: the
                                        strike MEASUREMENT's pitch weighting.
                                        Raw RMS overstates a guitar's low
                                        strings; +3 (default) discounts each
                                        octave below the pivot by 3 dB and
                                        credits each octave above */
    double   sample_level_db;        /* sampleLevelDb -60..+12: master over
                                        EVERY sample voice, lead and ghosts
                                        alike, applied at render BEFORE the
                                        mix with the dry. The consolidated
                                        sample fader */
    double   sample_tone_db;         /* sampleToneDb -12..+12: tilt EQ on
                                        the sample voices only, same law as
                                        harmTiltDb */
    double   gate_db;                /* noise/trigger gate in dBFS (RMS):
                                        the voicing gate, onset floors and
                                        envelope gate all scale from it.
                                        0 = the built-in default (-56) */
    double   sample_trim_db;         /* sampleGainDb: the CURRENT
                                        instrument's per-id trim, derived
                                        at config time; applied at strike */
    int      poly_notes;             /* POLY chord sampler: strike at most
                                        this many simultaneous notes (1..6).
                                        Live; only meaningful with polyMode
                                        on and leadSource "sample" */
    /* MEL layer (experimental, the EHX 9-series move): a fixed-ratio
       TRANSFORM of the playing itself, layered under the chord sampler --
       the latency-matched dry (8') plus the whole chord shifted a clean
       octave (4'), swelled in under its own envelope. Zero decisions in
       the layer: detection errors degrade as timbre, never as wrong
       notes, and a harmonic ratio is tuning-agnostic (a 22-EDO chord at
       2:1 is still 22-EDO). The sampler's fast strikes cover the attack;
       this layer blooms in and owns the sustain. Poly + sample lead only. */
    double   mel_mix;                /* melMix 0..1: layer level; 0 = off */
    int      mel_preset;             /* melPreset: index into the engine's
                                        preset table (0 = "footage", the
                                        plain dry+octave stack; the rest
                                        are spectral-remodeller
                                        instruments -- organ, flute,
                                        strings, brass, choir) */
    double   mel_oct_db;             /* melOctDb -60..+12: the 4' footage's
                                        gain against the 8' dry (footage
                                        preset only) */
    double   mel_attack_ms;          /* melAttackMs: the swell */
    double   mel_release_ms;         /* melReleaseMs: the tail ceiling */
    /* STEEL mode: pedal-steel dyad on the instrument itself -- while the
       lead is voiced, a root drone (the chart root's class, nearest pitch
       below the played note) sounds in the lead's own source family and
       holds until the playing ends. The player supplies the bend. */
    bool     steel_mode;             /* steelMode: the dyad on/off */
    double   steel_level_db;         /* steelLevelDb: drone vs the lead */
    int      steel_root_deg;         /* DERIVED (config_sync): rootNote
                                        mapped into engine degrees,
                                        round(edo * root / 12) */
    int      synth_patch;            /* index into the engine's patch table */
    double   ensemble_depth;         /* 0..1 */
    double   synth_vowel;            /* 0..1 formant transfer */
    double   harm_tilt_db;           /* -12..+12 harmony tone tilt */
    int      vowel_mode;             /* 0 channel vocoder, 1 LPC */
    double   synth_attack_ms;        /* 0..5000, the HARMONY's */
    double   synth_release_ms;       /* 0..10000, the HARMONY's */
    double   lead_attack_ms;         /* 0..5000, the LEAD's own */
    double   lead_release_ms;        /* 5..10000, the LEAD's own */

    /* The drone: one synth voice pinned to an absolute engine degree,
       sustained while on (a root-only chart chord means "drone that
       root"). Gated by harm_on; edges use the synth attack/release. */
    bool     drone_on;
    int      drone_deg;              /* absolute degree, 0..8*edo */

    /* IR points (v0.4-delta B7), the lock-free live three per point --
       path/hash/predelay ride the load call instead. Lead = the corrected
       voice (zero added latency); harm = the stereo harmony bus,
       post-ensemble pre-tilt. */
    double   ir_lead_mix;            /* 0..1 */
    double   ir_lead_gain_db;        /* -24..+12 */
    bool     ir_lead_on;
    double   ir_harm_mix;
    double   ir_harm_gain_db;
    bool     ir_harm_on;
} AeLiveParams;

/* Per-callback mix decisions handed from the atomic mirror to the audio
   callback in one struct (grew too many out-params). */
typedef struct
{
    bool  bypass;
    bool  bypass_mute;
    bool  lead_on;
    bool  harm_on;  /* the ghost bus is enabled (embed chain-feed decision) */
    bool  drone_on; /* ...and the drone (gated by harm_on in the corrector,
                       carried separately so the decision reads plainly) */
    float lead_gain;
    float master_gain;
    int   send_content; /* AE_SEND_* */
    float send_gain;
    bool  send_on;
} AeMixParams;

/* Record-send content selectors. */
#define AE_SEND_FULL 0 /* the live mix, exactly what the PA channel carries */
#define AE_SEND_WET  1 /* everything the engine produced, dry blend removed */
#define AE_SEND_LEAD 2 /* the corrected wet lead alone (pre lead-IR) */
#define AE_SEND_HARM 3 /* the harmony bus alone, mono-folded */

/* Parameters that require an engine restart to change. */
typedef struct
{
    char         input_uid[AE_UID_MAX];  /* "" => system default input */
    char         output_uid[AE_UID_MAX]; /* "" => system default output */
    char         midi_source[AE_NAME_MAX]; /* "" => all MIDI inputs */
    int          input_channel;          /* 1-based capture channel of the input
                                            device; 0 = backend default (mac:
                                            first channel, win: mix of all) */
    char         follow_url[128];        /* FOLLOW sender target ("" = off) */
    char         sample_root[1024];      /* per-rate WAV cache root */
    char         sample_manifest[1024];  /* optional file list (JSON) */
    char         sample_instrument[32];
    int          sample_octave;          /* filename->sounding semitones;
                                            AE_SMP_OCTAVE_AUTO = the table */
    int          send_channel;           /* record send: 1-based output device
                                            channel; 0 = no send. Restart-
                                            scoped (device channel map). Must
                                            not collide with the live output
                                            channels; the config layer refuses
                                            such writes */
    int          output_channel;         /* 1-based playback channel: ALL output
                                            (voice + harmony, mono-folded) lands
                                            on that one device channel; 0 =
                                            default stereo on channels 1-2 */
    int          buffer_frames;          /* preferred device I/O block size */
    double       det_min_hz;             /* detection range (0 = default) */
    double       det_max_hz;
    int          quality;                /* AeShifterQuality: shifter block/latency */
    bool         poly_mode;              /* POLY: detection bypassed, fixed-ratio
                                            shifting of the whole chordal input,
                                            doubled analysis block (more latency,
                                            chord-clean). Restart-scoped, lives
                                            with the detection-range controls */
    /* Embed backend only (audio_embed.c): the HOST's audio format, set by
       the embedding process at create time -- a fact about the process, so
       no JSON key writes it. Device backends ignore both. 0 = defaults. */
    double       embed_rate;
    int          embed_block;
    AeLiveParams params;
} AeEngineConfig;

/* How many pitch-trace points a status snapshot carries. The corrector logs
   one per detection hop (~200/s) and status ticks at ~10 Hz, so 48 covers a
   tick twice over -- consumers stitch ticks together with trace_seq. */
#define AE_TRACE_MAX 48

typedef struct
{
    bool   running;
    double input_rate;
    double output_rate;
    int    latency_samples; /* total corrector + buffering latency, output frames
                               -- the ENGINE's own path: the record send is
                               aligned by this, so it must not absorb device
                               overhead */
    int    device_latency_samples; /* hardware overhead OUTSIDE that path
                               (capture cycle, device/stream/safety latencies,
                               converters), output frames; 0 where the backend
                               has no hardware. Honest mic-to-speaker =
                               latency_samples + this */
    float  detected_hz;     /* live read-out from the corrector */
    float  target_hz;
    float  shift_st;        /* the lead's current shift, semitones: a panel
                               can SEE the ratio swing instead of diagnosing
                               a wrong-octave correction by ear */
    float  shift_st_min;    /* decaying (~1 s) extremes, so spikes between
                               10 Hz status ticks are still visible */
    float  shift_st_max;
    float  sample_vel;      /* last strike level the sample voices used */
    float  sample_vel_ref;  /* linear peak the strike map is relative TO */
    int    sample_zones;    /* loaded bank: distinct recording pitches */
    int    sample_files;    /* ...and total recordings behind them */
    float  sample_norm_db;  /* level normalisation the bank measured for
                               itself -- the shipped sets span ~20 dB */
    int    sample_octave;   /* filename->sounding offset actually applied */
    int    sample_clipped;  /* recordings peaking at full scale: a decode
                               fault, not a mix decision */
    int    poly_notes_live; /* POLY chord sampler: notes sounding right now */
#define AE_POLY_STATUS_MAX 6
    uint64_t poly_note[AE_POLY_STATUS_MAX]; /* the detection itself, one
                               packed word per tracker slot (0 = empty);
                               decode with the ae_poly_note_* helpers */
    float  lead_makeup;     /* the lead shifter's level-match gain, linear */
    float  out_peak;        /* decaying peak of the summed output BEFORE the
                               soft clip, linear: > ~0.79 means the clip is
                               shaping the sound */
    bool   voiced;
    /* Pitch trace, oldest first: detected (0 = unvoiced) and target per
       detection hop, ending at absolute detection count trace_seq. */
    int      trace_len;
    uint32_t trace_seq;
    float    trace_det[AE_TRACE_MAX];
    float    trace_tgt[AE_TRACE_MAX];
    int    harm_deg[5];     /* live ghost degrees (signed, re root); INT_MIN = silent */
    uint64_t midi_held_lo;  /* currently held MIDI notes (hardware + virtual) */
    uint64_t midi_held_hi;
    char   input_name[AE_NAME_MAX];
    char   output_name[AE_NAME_MAX];
} AeEngineStatus;

typedef struct AeAudioEngine AeAudioEngine;

/* Start live processing. Returns NULL on failure with a message in err. */
AeAudioEngine *ae_audio_engine_start (const AeEngineConfig *cfg, char *err, size_t err_len);

/* Stop and free the engine. Safe to pass NULL. */
void ae_audio_engine_stop (AeAudioEngine *e);

/* Push new live parameters (callable from any thread while running). */
void ae_audio_engine_set_params (AeAudioEngine *e, const AeLiveParams *p);

/* Snapshot current status (callable from any thread while running). */
void ae_audio_engine_get_status (AeAudioEngine *e, AeEngineStatus *out);

/* Local audio tap (loopback publish of the processed output): the audio
   thread pushes the selected stem (AE_SEND_* content values, `wet` being
   the loops' one) into an in-engine ring; the control thread drains it
   with tap_read and forwards it over loopback UDP. first_sample is a
   MONOTONIC output-sample counter from engine start, so a reader aligns
   instead of guessing; an overrun drops the oldest audio but never bends
   the counter. */
void ae_audio_engine_set_tap  (AeAudioEngine *e, bool on, int content);
int  ae_audio_engine_tap_read (AeAudioEngine *e, float *out, int max_samples,
                               long long *first_sample);

/* Set "virtual" held MIDI notes (bitsets, note 0..127). These merge (OR)
   with hardware MIDI input — used by the /api/midi endpoint and tests. */
void ae_audio_engine_set_midi_notes (AeAudioEngine *e, uint64_t lo, uint64_t hi);

/* The lead's shifted target degree, AE_HARM_DEG_OFF while unvoiced --
   what the FOLLOW link transmits. Lock-free, any thread. */
int ae_audio_engine_lead_degree (AeAudioEngine *e);

/* FOLLOW link receiver input: a note from another instance (-1 = none),
   OR'd into the MIDI-held set like a hardware note, and the source's
   envelope level (0..1; 1 = neutral). Lock-free, control thread. */
void ae_audio_engine_set_follow (AeAudioEngine *e, int note, double level);
/* This instance's own envelope (linear peak estimate, gated to 0 below
   the voicing gate) -- what a FOLLOW sender transmits. */
double ae_audio_engine_env (AeAudioEngine *e);
/* The follow level currently applied (receiver side; 1 = neutral). */
double ae_audio_engine_follow_level (AeAudioEngine *e);

/* Enumerate MIDI input source names into out[0..max-1]. Returns the count. */
int ae_audio_list_midi_sources (char out[][AE_NAME_MAX], int max);

/* Load {path, hash} into an IR point (0 = lead, 1 = harmony) -- control
   thread; file read, hash verify and FFT fills happen here, the audio
   thread crossfades to the result over ~30 ms. An empty path clears the
   point. Returns false with a reason in err. */
/* A short control-thread hold, used to let the audio thread turn a block
   over before a retired resource is considered dead. */
void ae_engine_sleep_ms (int ms);

/* Load a sample instrument into the running engine (CONTROL thread). */
bool ae_audio_engine_load_samples (AeAudioEngine *e, const char *root,
                                   const char *instrument, const char *manifest,
                                   int octave, char *err, size_t err_len);

bool ae_audio_engine_load_ir (AeAudioEngine *e, int point, const char *path,
                              const char *hash, double predelay_ms,
                              char *err, size_t err_len);

/* Which backend this binary carries: true = the embed backend (the host
   process owns the device and pushes blocks), false = a device backend.
   Echoed in status as "embedded" so a UI can hide the device pickers. */
bool ae_audio_backend_embedded (void);

/* EMBED BACKEND ONLY (audio_embed.c defines it; device backends do not).
   Host audio thread: run `n` mono input frames through the engine.
   out_l/out_r get the live PA feed (what the standalone engine's output
   jack carries -- bypassOutput semantics included). `chain` gets the feed
   for the host's own downstream chain (looper / FX / spaces): the live
   mono mix while any voice sounds, and the DRY input at unity under
   bypass OR with every voice off (lead muted, harmony off, no drone) --
   an engine mixing deliberate silence must not silence the looper's
   source, and bypassOutput:"mute" governs the PA jack, never the chain.
   The source flip is crossfaded (~10 ms) so a voice switch never bakes a
   step into a recording loop. Any of the three outputs may be NULL.
   Returns frames written. */
int ae_embed_engine_process (AeAudioEngine *e, const float *in, float *out_l,
                             float *out_r, float *chain, int n);

#endif /* AUTOEDO_AUDIO_H */
