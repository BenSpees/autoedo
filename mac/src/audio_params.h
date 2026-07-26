/* Shared live-parameter plumbing for audio backends: an atomic mirror of
   AeLiveParams that any thread may store into and the audio thread loads
   from once per block (each field is individually lock-free). */

#ifndef AUTOEDO_AUDIO_PARAMS_H
#define AUTOEDO_AUDIO_PARAMS_H

#include <math.h>
#include <stdatomic.h>

#include "audio.h"
#include "psola.h"

typedef struct
{
    _Atomic int      edo;
    _Atomic double   retune_ms;
    _Atomic double   transition_ms;
    _Atomic double   amount;
    _Atomic double   tolerance_cents;
    _Atomic double   stickiness;
    _Atomic double   humanize;
    _Atomic double   ref_hz;
    _Atomic double   period_cents;
    _Atomic uint64_t deg_lo;
    _Atomic uint64_t deg_hi;
    _Atomic bool     bypass;
    _Atomic double   gain_lin;
} AeAtomicParams;

static inline void ae_atomic_params_store (AeAtomicParams *a, const AeLiveParams *p)
{
    int edo = p->edo;
    if (edo < AE_MIN_EDO) edo = AE_MIN_EDO;
    if (edo > AE_MAX_EDO) edo = AE_MAX_EDO;
    atomic_store_explicit (&a->edo,             edo,                memory_order_relaxed);
    atomic_store_explicit (&a->retune_ms,       p->retune_ms,       memory_order_relaxed);
    atomic_store_explicit (&a->transition_ms,   p->transition_ms,   memory_order_relaxed);
    atomic_store_explicit (&a->amount,          p->amount,          memory_order_relaxed);
    atomic_store_explicit (&a->tolerance_cents, p->tolerance_cents, memory_order_relaxed);
    atomic_store_explicit (&a->stickiness,      p->stickiness,      memory_order_relaxed);
    atomic_store_explicit (&a->humanize,        p->humanize,        memory_order_relaxed);
    atomic_store_explicit (&a->ref_hz,          p->ref_hz,          memory_order_relaxed);
    atomic_store_explicit (&a->period_cents,    p->period_cents,    memory_order_relaxed);
    atomic_store_explicit (&a->deg_lo,          p->degrees_lo,      memory_order_relaxed);
    atomic_store_explicit (&a->deg_hi,          p->degrees_hi,      memory_order_relaxed);
    atomic_store_explicit (&a->bypass,          p->bypass,          memory_order_relaxed);
    atomic_store_explicit (&a->gain_lin, pow (10.0, p->output_gain_db / 20.0),
                           memory_order_relaxed);
}

/* Audio thread, once per block: push the current parameters into the
   corrector and report bypass/gain for the output stage. */
static inline void ae_atomic_params_apply (AeAtomicParams *a, AePsola *ps,
                                           bool *bypass_out, float *gain_out)
{
    ae_psola_set_edo             (ps, atomic_load_explicit (&a->edo, memory_order_relaxed));
    ae_psola_set_retune_ms       (ps, atomic_load_explicit (&a->retune_ms, memory_order_relaxed));
    ae_psola_set_transition_ms   (ps, atomic_load_explicit (&a->transition_ms, memory_order_relaxed));
    ae_psola_set_amount          (ps, atomic_load_explicit (&a->amount, memory_order_relaxed));
    ae_psola_set_tolerance_cents (ps, atomic_load_explicit (&a->tolerance_cents, memory_order_relaxed));
    ae_psola_set_stickiness      (ps, atomic_load_explicit (&a->stickiness, memory_order_relaxed));
    ae_psola_set_humanize        (ps, atomic_load_explicit (&a->humanize, memory_order_relaxed));
    ae_psola_set_reference       (ps, atomic_load_explicit (&a->ref_hz, memory_order_relaxed),
                                      atomic_load_explicit (&a->period_cents, memory_order_relaxed));

    const uint64_t lo = atomic_load_explicit (&a->deg_lo, memory_order_relaxed);
    const uint64_t hi = atomic_load_explicit (&a->deg_hi, memory_order_relaxed);
    bool mask[AE_MAX_EDO];
    for (int d = 0; d < AE_MAX_EDO; ++d)
        mask[d] = d < 64 ? ((lo >> d) & 1u) != 0 : ((hi >> (d - 64)) & 1u) != 0;
    ae_psola_set_enabled_degrees (ps, mask, AE_MAX_EDO);

    *bypass_out = atomic_load_explicit (&a->bypass, memory_order_relaxed);
    *gain_out   = (float) atomic_load_explicit (&a->gain_lin, memory_order_relaxed);
}

#endif /* AUTOEDO_AUDIO_PARAMS_H */
