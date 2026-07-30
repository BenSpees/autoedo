/* Stub audio backend for non-macOS builds (development / CI). Feeds the
   corrector a slightly-detuned synthetic tone from a timer thread so the
   web UI, config plumbing and DSP can all be exercised without CoreAudio.
   The real Mac backend lives in audio_mac.c. */

#include "audio.h"
#include "audio_params.h"
#include "corrector.h"

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define STUB_RATE  48000.0
#define STUB_BLOCK 256

int ae_audio_list_devices (AeDeviceInfo **out, int *count)
{
    AeDeviceInfo *d = calloc (2, sizeof (AeDeviceInfo));
    if (d == NULL)
        return -1;
    snprintf (d[0].uid,  sizeof (d[0].uid),  "stub-in");
    snprintf (d[0].name, sizeof (d[0].name), "Stub Test Tone (input)");
    d[0].input_channels = 2; /* ch 1 = 220 Hz, ch 2 = 293.66 Hz (see stub_thread) */
    d[0].nominal_rate = STUB_RATE;
    d[0].is_default_input = true;
    snprintf (d[1].uid,  sizeof (d[1].uid),  "stub-out");
    snprintf (d[1].name, sizeof (d[1].name), "Stub Null Sink (output)");
    d[1].output_channels = 2;
    d[1].nominal_rate = STUB_RATE;
    d[1].is_default_output = true;
    *out = d;
    *count = 2;
    return 0;
}

struct AeAudioEngine
{
    pthread_t     thread;
    volatile bool stopping;

    AeCorrector corrector;
    double  base_hz;
    double  phase;
    double  wobble_phase;

    AeAtomicParams params;
};

static void *stub_thread (void *arg)
{
    AeAudioEngine *e = arg;
    float block[STUB_BLOCK];

    while (! e->stopping)
    {
        /* A tone with harmonics, wobbling ~±35 cents at 0.5 Hz, so the
           corrector always has something to fix. The base frequency is per
           capture channel (220 / 293.66 Hz) so tests can tell which channel
           an engine bound. */
        for (int i = 0; i < STUB_BLOCK; ++i)
        {
            const double wobble = 35.0 * sin (e->wobble_phase);
            const double f = e->base_hz * pow (2.0, wobble / 1200.0);
            e->phase        += 2.0 * M_PI * f / STUB_RATE;
            e->wobble_phase += 2.0 * M_PI * 0.5 / STUB_RATE;
            block[i] = (float) (0.4 * sin (e->phase)
                              + 0.2 * sin (2.0 * e->phase)
                              + 0.1 * sin (3.0 * e->phase));
        }

        bool  bypass = false;
        float gain   = 1.0f;
        float harm_l[STUB_BLOCK], harm_r[STUB_BLOCK];
        ae_atomic_params_apply (&e->params, &e->corrector, 0, 0, &bypass, &gain);
        ae_corrector_process (&e->corrector, block, harm_l, harm_r, STUB_BLOCK);

        struct timespec ts = { 0, (long) (1e9 * STUB_BLOCK / STUB_RATE) };
        nanosleep (&ts, NULL);
    }
    return NULL;
}

void ae_audio_engine_set_params (AeAudioEngine *e, const AeLiveParams *p)
{
    if (e != NULL)
        ae_atomic_params_store (&e->params, p);
}

void ae_audio_engine_set_midi_notes (AeAudioEngine *e, uint64_t lo, uint64_t hi)
{
    if (e == NULL)
        return;
    atomic_store_explicit (&e->params.vmidi_lo, lo, memory_order_relaxed);
    atomic_store_explicit (&e->params.vmidi_hi, hi, memory_order_relaxed);
}

int ae_audio_list_midi_sources (char out[][AE_NAME_MAX], int max)
{
    if (max < 1)
        return 0;
    snprintf (out[0], AE_NAME_MAX, "Stub Virtual Keyboard");
    return 1;
}

void ae_audio_engine_get_status (AeAudioEngine *e, AeEngineStatus *out)
{
    memset (out, 0, sizeof (*out));
    if (e == NULL)
        return;
    out->running         = true;
    out->input_rate      = STUB_RATE;
    out->output_rate     = STUB_RATE;
    out->latency_samples = ae_corrector_latency (&e->corrector);
    out->detected_hz     = ae_corrector_detected_hz (&e->corrector);
    out->target_hz       = ae_corrector_target_hz (&e->corrector);
    out->voiced          = ae_corrector_voiced (&e->corrector);
    for (int v = 0; v < AE_HARM_VOICES; ++v)
        out->harm_deg[v] = ae_corrector_harm_degree (&e->corrector, v);
    out->trace_len = ae_corrector_trace (&e->corrector, &out->trace_seq,
                                         out->trace_det, out->trace_tgt, AE_TRACE_MAX);
    out->midi_held_lo = atomic_load_explicit (&e->params.vmidi_lo, memory_order_relaxed);
    out->midi_held_hi = atomic_load_explicit (&e->params.vmidi_hi, memory_order_relaxed);
    snprintf (out->input_name,  sizeof (out->input_name),  "Stub Test Tone (input)");
    snprintf (out->output_name, sizeof (out->output_name), "Stub Null Sink (output)");
}

AeAudioEngine *ae_audio_engine_start (const AeEngineConfig *cfg, char *err, size_t err_len)
{
    /* Mirror the real backend's unknown-device failure mode. */
    if (cfg->input_uid[0] != '\0' && strcmp (cfg->input_uid, "stub-in") != 0)
    {
        snprintf (err, err_len, "input device not found: %s", cfg->input_uid);
        return NULL;
    }
    if (cfg->output_uid[0] != '\0' && strcmp (cfg->output_uid, "stub-out") != 0)
    {
        snprintf (err, err_len, "output device not found: %s", cfg->output_uid);
        return NULL;
    }
    if (cfg->input_channel > 2)
    {
        snprintf (err, err_len, "device \"Stub Test Tone (input)\" has no input "
                                "channel %d (2 available)", cfg->input_channel);
        return NULL;
    }
    if (cfg->output_channel > 2)
    {
        snprintf (err, err_len, "device \"Stub Null Sink (output)\" has no output "
                                "channel %d (2 available)", cfg->output_channel);
        return NULL;
    }

    AeAudioEngine *e = calloc (1, sizeof (*e));
    if (e == NULL)
    {
        snprintf (err, err_len, "out of memory");
        return NULL;
    }
    /* Channel 2 sings a different note (D4) than channels 0/1 (A3). */
    e->base_hz = cfg->input_channel == 2 ? 293.66 : 220.0;
    ae_corrector_prepare (&e->corrector, STUB_RATE, STUB_BLOCK,
                          cfg->det_min_hz, cfg->det_max_hz, cfg->quality);
    ae_audio_engine_set_params (e, &cfg->params);

    if (pthread_create (&e->thread, NULL, stub_thread, e) != 0)
    {
        snprintf (err, err_len, "pthread_create failed");
        ae_corrector_free (&e->corrector);
        free (e);
        return NULL;
    }
    return e;
}

void ae_audio_engine_stop (AeAudioEngine *e)
{
    if (e == NULL)
        return;
    e->stopping = true;
    pthread_join (e->thread, NULL);
    ae_corrector_free (&e->corrector);
    free (e);
}
