/* Shared live-parameter plumbing for audio backends: an atomic mirror of
   AeLiveParams that any thread may store into and the audio thread loads
   from once per block (each field is individually lock-free). */

#ifndef AUTOEDO_AUDIO_PARAMS_H
#define AUTOEDO_AUDIO_PARAMS_H

#include <math.h>
#include <stdatomic.h>

#include "audio.h"
#include "corrector.h"

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

    _Atomic bool     midi_mode;
    _Atomic uint64_t vmidi_lo; /* virtual held notes (API/tests); backends OR
                                  their hardware bits in at apply time */
    _Atomic uint64_t vmidi_hi;

    _Atomic bool     harm_on;
    _Atomic int      harm_lock;
    _Atomic int      harm_interval[AE_HARM_VOICES];
    _Atomic int      harm_ext[AE_HARM_VOICES];
    _Atomic double   harm_gain_lin[AE_HARM_VOICES];
    _Atomic double   harm_pan[AE_HARM_VOICES];
    _Atomic uint32_t harm_mute;
    _Atomic uint32_t harm_solo;
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

    atomic_store_explicit (&a->midi_mode, p->midi_mode, memory_order_relaxed);
    atomic_store_explicit (&a->harm_on,   p->harm_on,   memory_order_relaxed);
    atomic_store_explicit (&a->harm_lock, p->harm_lock, memory_order_relaxed);
    for (int v = 0; v < AE_HARM_VOICES; ++v)
    {
        atomic_store_explicit (&a->harm_interval[v], p->harm_interval[v], memory_order_relaxed);
        atomic_store_explicit (&a->harm_ext[v],      p->harm_ext[v],      memory_order_relaxed);
        atomic_store_explicit (&a->harm_gain_lin[v], pow (10.0, p->harm_gain_db[v] / 20.0),
                               memory_order_relaxed);
        atomic_store_explicit (&a->harm_pan[v],      p->harm_pan[v],      memory_order_relaxed);
    }
    atomic_store_explicit (&a->harm_mute, p->harm_mute, memory_order_relaxed);
    atomic_store_explicit (&a->harm_solo, p->harm_solo, memory_order_relaxed);
}

/* Audio thread, once per block: push the current parameters into the
   corrector and report bypass/gain for the output stage. hw_midi_lo/hi are
   the backend's hardware held-note bits (0 if it has no MIDI input). */
static inline void ae_atomic_params_apply (AeAtomicParams *a, AeCorrector *ps,
                                           uint64_t hw_midi_lo, uint64_t hw_midi_hi,
                                           bool *bypass_out, float *gain_out)
{
    ae_corrector_set_midi (ps,
                       atomic_load_explicit (&a->midi_mode, memory_order_relaxed),
                       hw_midi_lo | atomic_load_explicit (&a->vmidi_lo, memory_order_relaxed),
                       hw_midi_hi | atomic_load_explicit (&a->vmidi_hi, memory_order_relaxed));
    ae_corrector_set_edo             (ps, atomic_load_explicit (&a->edo, memory_order_relaxed));
    ae_corrector_set_retune_ms       (ps, atomic_load_explicit (&a->retune_ms, memory_order_relaxed));
    ae_corrector_set_transition_ms   (ps, atomic_load_explicit (&a->transition_ms, memory_order_relaxed));
    ae_corrector_set_amount          (ps, atomic_load_explicit (&a->amount, memory_order_relaxed));
    ae_corrector_set_tolerance_cents (ps, atomic_load_explicit (&a->tolerance_cents, memory_order_relaxed));
    ae_corrector_set_stickiness      (ps, atomic_load_explicit (&a->stickiness, memory_order_relaxed));
    ae_corrector_set_humanize        (ps, atomic_load_explicit (&a->humanize, memory_order_relaxed));
    ae_corrector_set_reference       (ps, atomic_load_explicit (&a->ref_hz, memory_order_relaxed),
                                      atomic_load_explicit (&a->period_cents, memory_order_relaxed));

    const uint64_t lo = atomic_load_explicit (&a->deg_lo, memory_order_relaxed);
    const uint64_t hi = atomic_load_explicit (&a->deg_hi, memory_order_relaxed);
    bool mask[AE_MAX_EDO];
    for (int d = 0; d < AE_MAX_EDO; ++d)
        mask[d] = d < 64 ? ((lo >> d) & 1u) != 0 : ((hi >> (d - 64)) & 1u) != 0;
    ae_corrector_set_enabled_degrees (ps, mask, AE_MAX_EDO);

    AeHarmVoice voices[AE_HARM_VOICES];
    const uint32_t mute = atomic_load_explicit (&a->harm_mute, memory_order_relaxed);
    const uint32_t solo = atomic_load_explicit (&a->harm_solo, memory_order_relaxed);
    for (int v = 0; v < AE_HARM_VOICES; ++v)
    {
        voices[v].interval = atomic_load_explicit (&a->harm_interval[v], memory_order_relaxed);
        voices[v].ext_oct  = atomic_load_explicit (&a->harm_ext[v], memory_order_relaxed);
        voices[v].gain     = atomic_load_explicit (&a->harm_gain_lin[v], memory_order_relaxed);
        voices[v].pan      = atomic_load_explicit (&a->harm_pan[v], memory_order_relaxed);
        voices[v].mute     = ((mute >> v) & 1u) != 0;
        voices[v].solo     = ((solo >> v) & 1u) != 0;
    }
    ae_corrector_set_harmony (ps,
                          atomic_load_explicit (&a->harm_on, memory_order_relaxed),
                          atomic_load_explicit (&a->harm_lock, memory_order_relaxed),
                          voices);

    *bypass_out = atomic_load_explicit (&a->bypass, memory_order_relaxed);
    *gain_out   = (float) atomic_load_explicit (&a->gain_lin, memory_order_relaxed);
}

#endif /* AUTOEDO_AUDIO_PARAMS_H */
