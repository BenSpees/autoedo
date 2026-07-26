/* Real-time monophonic pitch corrector — C (mono) port of
   Source/dsp/PsolaPitchCorrector.

   Pipeline:
     1. YIN estimates the input fundamental.
     2. The fundamental is snapped to the nearest enabled pitch on an N-EDO
        grid anchored at C (see tuning.h).
     3. A retune-speed control glides the output pitch toward that target
        (0 ms = hard snap, larger = natural slide).
     4. TD-PSOLA resynthesises the audio at the corrected pitch, preserving
        formants and duration. Unvoiced / silent passages pass through a
        latency-matched dry path with a smooth crossfade.

   Allocation happens only in ae_psola_prepare(); ae_psola_process() is
   allocation-free and safe to call from a real-time audio callback. */

#ifndef AUTOEDO_PSOLA_H
#define AUTOEDO_PSOLA_H

#include <limits.h>
#include <stdatomic.h>
#include <stdbool.h>

#include "tuning.h"
#include "yin.h"

/* Smart harmony (Xentar emulation): up to five independent ghost voices at
   signed EDO-step intervals from the corrected source, optionally locked to
   the lit-degree mask (walk-outward, up first) or to the JI landmark set. */
#define AE_HARM_VOICES 5
#define AE_HARM_DEG_OFF INT_MIN /* live-readout sentinel: voice silent */

typedef struct
{
    int    interval; /* signed EDO steps, 0 = voice off */
    int    ext_oct;  /* 0..2 extra octaves, stacking in the voice's direction */
    double gain;     /* linear */
    double pan;      /* -1 (L) .. +1 (R) */
    bool   mute;
    bool   solo;
} AeHarmVoice;

typedef struct
{
    /* Configuration -------------------------------------------------------- */
    double fs;
    double det_min_hz; /* detection range (set in prepare) */
    double det_max_hz;
    int    frame_size;
    int    hop;
    int    tau_min;
    int    tau_max;
    int    latency;
    int    max_block;

    int    buf_size;
    int    buf_mask;

    /* Ring buffers --------------------------------------------------------- */
    float *in_buf;   /* mono input history (also the detection source) */
    float *wet_acc;  /* PSOLA accumulator */
    float *wet_win;  /* window-sum accumulator */
    float *frame;    /* scratch frame for detection */

    /* Running state -------------------------------------------------------- */
    AeYin detector;

    long long in_write;       /* total input samples written */
    long long last_detect_at; /* in_write at last detection */
    long long last_touched;   /* highest output index cleared in wet_acc/wet_win */
    double    synth_mark;     /* absolute output index of next grain center */

    double current_period; /* T0 in samples (from detected pitch) */
    double synth_period;   /* T1 in samples (from corrected pitch) */
    bool   voiced;
    bool   primed;         /* becomes true once first pitch is found */

    double out_cents; /* current (smoothed) output pitch, cents re ref */
    double v_gain;    /* smoothed dry(0)/wet(1) crossfade gain */

    long long target_j;      /* current target degree (signed, re root) */
    bool      target_valid;  /* false until a voiced target exists */
    bool      in_transition; /* gliding between two different degrees */
    double    sustain_s;     /* seconds the current degree has been held */

    /* Harmony voices -------------------------------------------------------- */
    bool        harm_on;
    int         harm_lock; /* 0 = off, 1 = mask, 2 = JI landmarks */
    AeHarmVoice harm[AE_HARM_VOICES];

    float    *h_acc[AE_HARM_VOICES]; /* per-voice PSOLA accumulator */
    float    *h_win[AE_HARM_VOICES]; /* per-voice window-sum accumulator */
    double    h_mark[AE_HARM_VOICES];
    long long h_touched[AE_HARM_VOICES];
    double    h_period[AE_HARM_VOICES];  /* synthesis period */
    bool      h_active[AE_HARM_VOICES];
    double    h_gl[AE_HARM_VOICES];      /* constant-power pan gains */
    double    h_gr[AE_HARM_VOICES];
    _Atomic int h_deg_out[AE_HARM_VOICES]; /* live ghost degree (UI) */

    /* Parameters (set from the audio thread between blocks) ---------------- */
    int    edo;
    double retune_ms;
    double transition_ms;   /* glide between *different* target degrees */
    double amount;          /* 0..1 partial correction */
    double tolerance_cents; /* dead zone around each lit degree */
    double stickiness;      /* 0..1 hysteresis before re-snapping */
    double humanize;        /* 0..1 relaxes retune on sustained notes */
    double ref_hz;          /* frequency of degree 0 (the root anchor) */
    double period_cents;    /* octave size (1200 = true octave) */
    bool   enabled_deg[AE_MAX_EDO];

    /* Live read-out (audio thread -> UI thread) ---------------------------- */
    _Atomic float detected_hz_out;
    _Atomic float target_hz_out;
    _Atomic bool  voiced_out;
} AePsola;

/* Allocate buffers for a sample rate / block size. Call before processing.
   min_hz/max_hz bound the detection range (pass 0 for the defaults,
   65 / 1600 Hz). Lower min_hz costs latency: latency = 3 longest periods. */
void ae_psola_prepare (AePsola *p, double sample_rate, int max_block_size,
                       double min_hz, double max_hz);

/* Clear all internal state. */
void ae_psola_reset (AePsola *p);

/* Release buffers allocated by ae_psola_prepare(). */
void ae_psola_free (AePsola *p);

/* Process num_samples of mono audio in place (the corrected voice), and —
   when harm_l/harm_r are non-NULL — overwrite them with the latency-aligned
   harmony-voice mix for the same block (constant-power panned, gated by the
   same voiced crossfade). Pass NULL/NULL to skip harmony rendering. */
void ae_psola_process (AePsola *p, float *mono, float *harm_l, float *harm_r,
                       int num_samples);

/* Configure the harmony voices (audio thread, between blocks). */
void ae_psola_set_harmony (AePsola *p, bool on, int lock,
                           const AeHarmVoice voices[AE_HARM_VOICES]);

/* Live ghost degree of a voice (signed steps re root), AE_HARM_DEG_OFF when
   the voice is silent. Lock-free; any thread. */
static inline int ae_psola_harm_degree (const AePsola *p, int voice)
{
    return atomic_load_explicit (&((AePsola *) p)->h_deg_out[voice],
                                 memory_order_relaxed);
}

/* Xentar Snap-to-Scale walk: nearest enabled degree to j, up checked first
   at each distance. `enabled` indexes pitch classes 0..edo-1; a fully
   disabled mask returns j unchanged. */
static inline long long ae_walk_to_enabled (long long j, int edo, const bool *enabled)
{
    bool any = false;
    for (int d = 0; d < edo; ++d)
        if (enabled[d]) { any = true; break; }
    if (! any)
        return j;

    const long long pc0 = ((j % edo) + edo) % edo;
    if (enabled[pc0])
        return j;
    for (int d = 1; d <= edo; ++d)
    {
        if (enabled[(pc0 + d) % edo])
            return j + d;
        if (enabled[((pc0 - d) % edo + edo) % edo])
            return j - d;
    }
    return j;
}

static inline double ae_psola_clamp01 (double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

static inline void ae_psola_set_edo (AePsola *p, int edo)          { p->edo = edo; }
static inline void ae_psola_set_retune_ms (AePsola *p, double ms)  { p->retune_ms = ms < 0.0 ? 0.0 : ms; }
static inline void ae_psola_set_transition_ms (AePsola *p, double ms) { p->transition_ms = ms < 0.0 ? 0.0 : ms; }
static inline void ae_psola_set_amount (AePsola *p, double a)      { p->amount = ae_psola_clamp01 (a); }
static inline void ae_psola_set_tolerance_cents (AePsola *p, double c) { p->tolerance_cents = c < 0.0 ? 0.0 : (c > 50.0 ? 50.0 : c); }
static inline void ae_psola_set_stickiness (AePsola *p, double s)  { p->stickiness = ae_psola_clamp01 (s); }
static inline void ae_psola_set_humanize (AePsola *p, double h)    { p->humanize = ae_psola_clamp01 (h); }
static inline void ae_psola_set_reference (AePsola *p, double ref_hz, double period_cents)
{
    p->ref_hz       = ref_hz > 0.0 ? ref_hz : AE_REFERENCE_C0_HZ;
    p->period_cents = (period_cents >= 600.0 && period_cents <= 2400.0) ? period_cents : 1200.0;
}

/* Set which scale degrees (0..count-1, 0 == C) the corrector may tune to. */
static inline void ae_psola_set_enabled_degrees (AePsola *p, const bool *mask, int count)
{
    const int n = count < AE_MAX_EDO ? count : AE_MAX_EDO;
    for (int i = 0; i < n; ++i)
        p->enabled_deg[i] = mask[i];
}

/* Reported processing latency in samples (constant after prepare). */
static inline int ae_psola_latency (const AePsola *p) { return p->latency; }

static inline float ae_psola_detected_hz (const AePsola *p)
{
    return atomic_load_explicit (&((AePsola *) p)->detected_hz_out, memory_order_relaxed);
}
static inline float ae_psola_target_hz (const AePsola *p)
{
    return atomic_load_explicit (&((AePsola *) p)->target_hz_out, memory_order_relaxed);
}
static inline bool ae_psola_voiced (const AePsola *p)
{
    return atomic_load_explicit (&((AePsola *) p)->voiced_out, memory_order_relaxed);
}

#endif /* AUTOEDO_PSOLA_H */
