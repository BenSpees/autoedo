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

    /* MIDI Harmony: held notes override the mask (middle C = degree 4*edo,
       one EDO step per semitone). Off = held notes are ignored. */
    bool     midi_mode;

    /* Smart harmony (Xentar emulation): five independent ghost voices. */
    bool     harm_on;
    int      harm_lock;              /* 0 off, 1 mask, 2 JI */
    int      harm_interval[5];       /* signed EDO steps, 0 = voice off */
    int      harm_ext[5];            /* 0..2 octave extension, voice-directed */
    double   harm_gain_db[5];        /* -60..+6 */
    double   harm_pan[5];            /* -1..1 */
    uint32_t harm_mute;              /* bit v */
    uint32_t harm_solo;              /* bit v */
} AeLiveParams;

/* Parameters that require an engine restart to change. */
typedef struct
{
    char         input_uid[AE_UID_MAX];  /* "" => system default input */
    char         output_uid[AE_UID_MAX]; /* "" => system default output */
    char         midi_source[AE_NAME_MAX]; /* "" => all MIDI inputs */
    int          buffer_frames;          /* preferred device I/O block size */
    double       det_min_hz;             /* detection range (0 = default) */
    double       det_max_hz;
    AeLiveParams params;
} AeEngineConfig;

typedef struct
{
    bool   running;
    double input_rate;
    double output_rate;
    int    latency_samples; /* total corrector + buffering latency, output frames */
    float  detected_hz;     /* live read-out from the corrector */
    float  target_hz;
    bool   voiced;
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

#endif /* AUTOEDO_AUDIO_H */
