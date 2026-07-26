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
    int      edo;            /* 10..72 */
    double   retune_ms;      /* 0..500 */
    uint64_t degrees_lo;     /* bit d set => scale degree d enabled (d 0..63) */
    uint64_t degrees_hi;     /* degrees 64..71 in bits 0..7 */
    bool     bypass;         /* true = pass input through uncorrected */
    double   output_gain_db; /* -60..+12 */
} AeLiveParams;

/* Parameters that require an engine restart to change. */
typedef struct
{
    char         input_uid[AE_UID_MAX];  /* "" => system default input */
    char         output_uid[AE_UID_MAX]; /* "" => system default output */
    int          buffer_frames;          /* preferred device I/O block size */
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

#endif /* AUTOEDO_AUDIO_H */
