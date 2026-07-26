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

#include <stdatomic.h>
#include <stdbool.h>

#include "tuning.h"
#include "yin.h"

typedef struct
{
    /* Configuration -------------------------------------------------------- */
    double fs;
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

    double out_cents; /* current (smoothed) output pitch, cents re C0 */
    double v_gain;    /* smoothed dry(0)/wet(1) crossfade gain */

    /* Parameters (set from the audio thread between blocks) ---------------- */
    int    edo;
    double retune_ms;
    bool   enabled_deg[AE_MAX_EDO];

    /* Live read-out (audio thread -> UI thread) ---------------------------- */
    _Atomic float detected_hz_out;
    _Atomic float target_hz_out;
    _Atomic bool  voiced_out;
} AePsola;

/* Allocate buffers for a sample rate / block size. Call before processing. */
void ae_psola_prepare (AePsola *p, double sample_rate, int max_block_size);

/* Clear all internal state. */
void ae_psola_reset (AePsola *p);

/* Release buffers allocated by ae_psola_prepare(). */
void ae_psola_free (AePsola *p);

/* Process num_samples of mono audio in place. */
void ae_psola_process (AePsola *p, float *mono, int num_samples);

static inline void ae_psola_set_edo (AePsola *p, int edo)          { p->edo = edo; }
static inline void ae_psola_set_retune_ms (AePsola *p, double ms)  { p->retune_ms = ms < 0.0 ? 0.0 : ms; }

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
