/* Embed audio backend: no device, no thread. The HOST process owns the
   audio device and pushes mono float32 blocks through
   ae_embed_engine_process() from its own audio thread -- the B2 shape of
   the Treebrain record-send ask ("the engine as a library"). Everything
   else (corrector, params, tap, samples, IRs) is the same machinery the
   device backends drive; only the transport differs.

   Threading matches the device backends exactly: one audio thread calls
   the process function, control threads call everything else, and the app
   layer guarantees an engine is never stopped until the host has had a
   block's grace to stop touching it (ae_app_engine_live). */

#include "audio.h"
#include "audio_params.h"
#include "corrector.h"
#include "ir_load.h"

#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EMBED_DEFAULT_RATE  48000.0
#define EMBED_DEFAULT_BLOCK 512

bool ae_audio_backend_embedded (void) { return true; }

int ae_audio_list_devices (AeDeviceInfo **out, int *count)
{
    /* One virtual device: the host's audio graph. Listed so the device
       card in the web UI has something honest to show; picking anything
       else is impossible by construction. */
    AeDeviceInfo *d = calloc (1, sizeof (AeDeviceInfo));
    if (d == NULL)
        return -1;
    snprintf (d[0].uid,  sizeof (d[0].uid),  "embedded");
    snprintf (d[0].name, sizeof (d[0].name), "Embedded (host audio)");
    d[0].input_channels  = 1;
    d[0].output_channels = 2;
    d[0].is_default_input = d[0].is_default_output = true;
    *out = d;
    *count = 1;
    return 0;
}

struct AeAudioEngine
{
    AeCorrector corrector;
    double  rate;
    int     max_block;

    float *proc;   /* mono processing block */
    float *dry;    /* latency-free dry copy for bypass */
    float *harm_l; /* harmony-voice mix, left / right */
    float *harm_r;

    _Atomic float out_peak; /* decaying pre-clip peak of the summed output */

    AeAtomicParams params;
    AeTapRing      tap;
};

int ae_embed_engine_process (AeAudioEngine *e, const float *in, float *out_l,
                             float *out_r, float *chain, int n)
{
    if (e == NULL || in == NULL || n <= 0)
        return 0;

    float pk = atomic_load_explicit (&e->out_peak, memory_order_relaxed) * 0.98f;
    int done = 0;
    while (done < n)
    {
        const int m = n - done > e->max_block ? e->max_block : n - done;
        memcpy (e->proc, in + done, (size_t) m * sizeof (float));
        memcpy (e->dry,  e->proc,  (size_t) m * sizeof (float));

        AeMixParams mix;
        ae_atomic_params_apply (&e->params, &e->corrector, 0, 0, &mix);
        const bool  bypass = mix.bypass;
        const bool  lead   = mix.lead_on;
        const float lead_g = mix.lead_gain;
        const float gain   = mix.master_gain;
        ae_corrector_process (&e->corrector, e->proc, e->harm_l, e->harm_r, m);

        const float *src   = bypass ? e->dry : e->proc;
        const float  byp_g = ae_bypass_gain (&mix);
        const float *wet   = ae_corrector_lead_wet (&e->corrector);

        const bool tap_on = atomic_load_explicit (&e->tap.on,
                                                  memory_order_relaxed);
        const int  tap_content =
            atomic_load_explicit (&e->tap.content, memory_order_relaxed);

        for (int i = 0; i < m; ++i)
        {
            const float hm   = 0.5f * (e->harm_l[i] + e->harm_r[i]);
            const float mono = bypass ? byp_g * e->dry[i]
                                      : (lead ? lead_g * src[i] : 0.0f) + hm;
            const float full = mono * gain;
            if (tap_on)
                ae_tap_push (&e->tap, ae_tap_value (tap_content,
                                                    wet ? wet[i] : 0.0f, hm,
                                                    full));
            /* The PA feed, per side (mono fold of the ghosts' pan happens
               only when the host takes a single channel of it). */
            if (out_l != NULL || out_r != NULL)
            {
                const float base = bypass ? byp_g * src[i]
                                          : (lead ? lead_g * src[i] : 0.0f);
                const float sl = bypass ? base : base + e->harm_l[i];
                const float sr = bypass ? base : base + e->harm_r[i];
                const float pre_l = fabsf (sl * gain);
                const float pre_r = fabsf (sr * gain);
                if (pre_l > pk) pk = pre_l;
                if (pre_r > pk) pk = pre_r;
                if (out_l != NULL) out_l[done + i] = ae_soft_clip (sl * gain);
                if (out_r != NULL) out_r[done + i] = ae_soft_clip (sr * gain);
            }
            /* The chain feed (see audio.h): bypass passes the DRY through
               at unity whatever bypassOutput says -- the host's looper and
               FX must never lose their source to a PA-side mute. */
            if (chain != NULL)
                chain[done + i] = bypass ? e->dry[i] : ae_soft_clip (full);
        }
        done += m;
    }
    atomic_store_explicit (&e->out_peak, pk, memory_order_relaxed);
    return done;
}

void ae_audio_engine_set_tap (AeAudioEngine *e, bool on, int content)
{
    if (e == NULL)
        return;
    atomic_store_explicit (&e->tap.content, content, memory_order_relaxed);
    atomic_store_explicit (&e->tap.on, on, memory_order_relaxed);
}

int ae_audio_engine_tap_read (AeAudioEngine *e, float *out, int max_samples,
                              long long *first_sample)
{
    if (e == NULL)
        return 0;
    return ae_tap_ring_read (&e->tap, out, max_samples, first_sample);
}

void ae_audio_engine_set_params (AeAudioEngine *e, const AeLiveParams *p)
{
    if (e != NULL)
        ae_atomic_params_store (&e->params, p);
}

bool ae_audio_engine_load_samples (AeAudioEngine *e, const char *root,
                                   const char *instrument, const char *manifest,
                                   int octave, char *err, size_t err_len)
{
    if (e == NULL)
    {
        snprintf (err, err_len, "engine not running");
        return false;
    }
    ae_corrector_set_sample_octave (&e->corrector, octave);
    const bool ok = ae_corrector_load_samples (&e->corrector, root, instrument,
                                               manifest, err, err_len);
    /* Let the host's audio thread turn a block over before the retired
       slot may be refilled (same contract as the device backends). */
    ae_engine_sleep_ms (60);
    return ok;
}

bool ae_audio_engine_load_ir (AeAudioEngine *e, int point, const char *path,
                              const char *hash, double predelay_ms,
                              char *err, size_t err_len)
{
    if (e == NULL)
    {
        snprintf (err, err_len, "engine not running");
        return false;
    }
    return ae_ir_load_point (&e->corrector, point, path, hash, predelay_ms,
                             e->rate, err, err_len);
}

void ae_audio_engine_set_follow (AeAudioEngine *e, int note, double level)
{
    if (e == NULL)
        return;
    atomic_store_explicit (&e->params.follow_note_p1,
                           note >= 0 && note < 128 ? note + 1 : 0,
                           memory_order_relaxed);
    atomic_store_explicit (&e->params.follow_level, level, memory_order_relaxed);
}

double ae_audio_engine_env (AeAudioEngine *e)
{
    return e == NULL ? 0.0 : (double) ae_corrector_env (&e->corrector);
}

double ae_audio_engine_follow_level (AeAudioEngine *e)
{
    return e == NULL ? 1.0
        : atomic_load_explicit (&e->params.follow_level, memory_order_relaxed);
}

int ae_audio_engine_lead_degree (AeAudioEngine *e)
{
    return ae_corrector_lead_degree (&e->corrector);
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
    /* Hardware MIDI belongs to the host process; the virtual set
       (/api/midi) and the FOLLOW link still work. */
    (void) out; (void) max;
    return 0;
}

void ae_audio_engine_get_status (AeAudioEngine *e, AeEngineStatus *out)
{
    memset (out, 0, sizeof (*out));
    if (e == NULL)
        return;
    out->running         = true;
    out->input_rate      = e->rate;
    out->output_rate     = e->rate;
    out->latency_samples = ae_corrector_latency (&e->corrector);
    out->device_latency_samples = 0; /* the host owns the hardware numbers */
    out->detected_hz     = ae_corrector_detected_hz (&e->corrector);
    out->target_hz       = ae_corrector_target_hz (&e->corrector);
    out->shift_st        = ae_corrector_shift_st (&e->corrector);
    out->shift_st_min    = ae_corrector_shift_st_min (&e->corrector);
    out->shift_st_max    = ae_corrector_shift_st_max (&e->corrector);
    out->lead_makeup     = ae_corrector_lead_makeup (&e->corrector);
    out->sample_vel      = ae_corrector_sample_vel (&e->corrector);
    out->sample_vel_ref  = ae_corrector_sample_vel_ref (&e->corrector);
    out->poly_notes_live = ae_corrector_poly_active (&e->corrector);
    for (int k = 0; k < AE_POLY_STATUS_MAX; ++k)
        out->poly_note[k] = ae_corrector_poly_note (&e->corrector, k);
    {
        const int lv = atomic_load_explicit (&e->corrector.smp_live,
                                             memory_order_relaxed);
        out->sample_zones = lv >= 0 ? e->corrector.smp_bank[lv].n_zones : 0;
        out->sample_files = lv >= 0 ? e->corrector.smp_bank[lv].n_recs  : 0;
        out->sample_norm_db = lv >= 0 && e->corrector.smp_bank[lv].norm > 0.0
            ? (float) (20.0 * log10 (e->corrector.smp_bank[lv].norm)) : 0.0f;
        out->sample_octave  = lv >= 0 ? e->corrector.smp_bank[lv].octave : 0;
        out->sample_clipped = lv >= 0 ? e->corrector.smp_bank[lv].clipped : 0;
    }
    out->out_peak = atomic_load_explicit (&e->out_peak, memory_order_relaxed);
    out->voiced   = ae_corrector_voiced (&e->corrector);
    for (int v = 0; v < AE_HARM_VOICES; ++v)
        out->harm_deg[v] = ae_corrector_harm_degree (&e->corrector, v);
    out->trace_len = ae_corrector_trace (&e->corrector, &out->trace_seq,
                                         out->trace_det, out->trace_tgt,
                                         AE_TRACE_MAX);
    {
        uint64_t flo, fhi;
        ae_follow_bits (&e->params, &flo, &fhi);
        out->midi_held_lo = flo | atomic_load_explicit (&e->params.vmidi_lo,
                                                        memory_order_relaxed);
        out->midi_held_hi = fhi | atomic_load_explicit (&e->params.vmidi_hi,
                                                        memory_order_relaxed);
    }
    snprintf (out->input_name,  sizeof (out->input_name),  "Embedded (host audio)");
    snprintf (out->output_name, sizeof (out->output_name), "Embedded (host audio)");
}

AeAudioEngine *ae_audio_engine_start (const AeEngineConfig *cfg, char *err,
                                      size_t err_len)
{
    /* Device UIDs persisted by a rig that once ran standalone are ignored,
       not refused: the host owns the device, and an engine that cannot
       start because its old config remembers an interface would be the
       "stuck on Macbook Speakers" trap all over again. */
    AeAudioEngine *e = calloc (1, sizeof (*e));
    if (e == NULL)
    {
        snprintf (err, err_len, "out of memory");
        return NULL;
    }
    e->rate      = cfg->embed_rate  > 0.0 ? cfg->embed_rate  : EMBED_DEFAULT_RATE;
    e->max_block = cfg->embed_block > 0   ? cfg->embed_block : EMBED_DEFAULT_BLOCK;
    if (e->max_block < 64)
        e->max_block = 64;
    e->proc   = calloc ((size_t) e->max_block, sizeof (float));
    e->dry    = calloc ((size_t) e->max_block, sizeof (float));
    e->harm_l = calloc ((size_t) e->max_block, sizeof (float));
    e->harm_r = calloc ((size_t) e->max_block, sizeof (float));
    if (e->proc == NULL || e->dry == NULL || e->harm_l == NULL
        || e->harm_r == NULL)
    {
        snprintf (err, err_len, "out of memory");
        free (e->proc); free (e->dry); free (e->harm_l); free (e->harm_r);
        free (e);
        return NULL;
    }
    ae_corrector_prepare (&e->corrector, e->rate, e->max_block,
                          cfg->det_min_hz, cfg->det_max_hz,
                          cfg->quality
                              | (cfg->poly_mode ? AE_SHIFT_QUALITY_POLY_FLAG : 0));
    ae_audio_engine_set_params (e, &cfg->params);
    atomic_store_explicit (&e->params.follow_level, 1.0, memory_order_relaxed);
    return e;
}

void ae_audio_engine_stop (AeAudioEngine *e)
{
    if (e == NULL)
        return;
    ae_corrector_free (&e->corrector);
    free (e->proc);
    free (e->dry);
    free (e->harm_l);
    free (e->harm_r);
    free (e);
}
