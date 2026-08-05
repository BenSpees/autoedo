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
    double   ref_hz;          /* frequency of degree 0 (root anchor) */
    double   period_cents;    /* octave size (1200 = true octave) */
    uint64_t degrees_lo;      /* bit d set => scale degree d enabled (d 0..63) */
    uint64_t degrees_hi;      /* degrees 64..71 in bits 0..7 */
    bool     bypass;          /* true = pass input through uncorrected */
    double   output_gain_db;  /* -60..+12 */
    bool     lead_on;         /* corrected lead voice in the mix; false =
                                 harmony only (bypass still wins) */
    int      lead_shift_steps;/* -72..72: static lead transpose in EDO steps,
                                 applied after the snap; +-edo keeps the
                                 pitch class. Locked ghosts follow it. */

    /* MIDI Harmony: held notes override the mask (middle C = degree 4*edo,
       one EDO step per semitone). Off = held notes are ignored. */
    bool     midi_mode;

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

    /* Harmony source: pitch-shifted live audio (0) or the built-in synth
       voice (1), which adds a patch and an attack/release envelope. Applies
       to all five voices. */
    int      harm_source;
    int      harm_voice_source[5];   /* per voice; -1 = follow harm_source */
    int      lead_source;            /* the corrected lead: 0 shifted, 1 synth */
    int      synth_patch;            /* index into the engine's patch table */
    double   ensemble_depth;         /* 0..1 */
    double   synth_vowel;            /* 0..1 formant transfer */
    double   harm_tilt_db;           /* -12..+12 harmony tone tilt */
    int      vowel_mode;             /* 0 channel vocoder, 1 LPC */
    double   synth_attack_ms;        /* 0..5000 */
    double   synth_release_ms;       /* 0..10000 */

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

/* Parameters that require an engine restart to change. */
typedef struct
{
    char         input_uid[AE_UID_MAX];  /* "" => system default input */
    char         output_uid[AE_UID_MAX]; /* "" => system default output */
    char         midi_source[AE_NAME_MAX]; /* "" => all MIDI inputs */
    int          input_channel;          /* 1-based capture channel of the input
                                            device; 0 = backend default (mac:
                                            first channel, win: mix of all) */
    int          output_channel;         /* 1-based playback channel: ALL output
                                            (voice + harmony, mono-folded) lands
                                            on that one device channel; 0 =
                                            default stereo on channels 1-2 */
    int          buffer_frames;          /* preferred device I/O block size */
    double       det_min_hz;             /* detection range (0 = default) */
    double       det_max_hz;
    int          quality;                /* AeShifterQuality: shifter block/latency */
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
    int    latency_samples; /* total corrector + buffering latency, output frames */
    float  detected_hz;     /* live read-out from the corrector */
    float  target_hz;
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

/* Set "virtual" held MIDI notes (bitsets, note 0..127). These merge (OR)
   with hardware MIDI input — used by the /api/midi endpoint and tests. */
void ae_audio_engine_set_midi_notes (AeAudioEngine *e, uint64_t lo, uint64_t hi);

/* Enumerate MIDI input source names into out[0..max-1]. Returns the count. */
int ae_audio_list_midi_sources (char out[][AE_NAME_MAX], int max);

/* Load {path, hash} into an IR point (0 = lead, 1 = harmony) -- control
   thread; file read, hash verify and FFT fills happen here, the audio
   thread crossfades to the result over ~30 ms. An empty path clears the
   point. Returns false with a reason in err. */
bool ae_audio_engine_load_ir (AeAudioEngine *e, int point, const char *path,
                              const char *hash, double predelay_ms,
                              char *err, size_t err_len);

#endif /* AUTOEDO_AUDIO_H */
