/* Shared live-parameter plumbing for audio backends: an atomic mirror of
   AeLiveParams that any thread may store into and the audio thread loads
   from once per block (each field is individually lock-free). */

#ifndef AUTOEDO_AUDIO_PARAMS_H
#define AUTOEDO_AUDIO_PARAMS_H

#include <math.h>
#include <stdatomic.h>

#include "audio.h"
#include "corrector.h"

/* Output-stage soft clip: transparent below the knee, then a smooth
   saturation that approaches (never reaches) full scale. The corrected
   voice plus five near-unity harmony voices can sum well past 1.0 --
   folded onto a single bound channel they otherwise hit the converter as
   hard digital clipping, heard as crackle that gets worse per voice
   added. Monotonic and C1 at the knee; the fast path is one compare. */
static inline float ae_soft_clip (float x)
{
    const float knee = 0.8f; /* linear below ~ -1.9 dBFS */
    const float ax = fabsf (x);
    if (ax <= knee)
        return x;
    const float t = (ax - knee) / (1.0f - knee);
    const float y = knee + (1.0f - knee) * (t / (1.0f + t));
    return x < 0.0f ? -y : y;
}

/* What bypass PUTS on the output, as a gain on the dry passthrough:
   1 = "dry" (pass the input through), 0 = "mute" (silence). Shared by the
   backends so the decision is made once and can be tested without a
   device. Only meaningful while bypass is engaged. */
static inline float ae_bypass_gain (const AeMixParams *mix)
{
    return (mix->bypass && mix->bypass_mute) ? 0.0f : 1.0f;
}

typedef struct
{
    _Atomic int      edo;
    _Atomic double   retune_ms;
    _Atomic double   transition_ms;
    _Atomic double   amount;
    _Atomic double   tolerance_cents;
    _Atomic double   stickiness;
    _Atomic double   humanize;
    _Atomic double   expression;
    _Atomic double   follow_env;
    _Atomic double   ref_hz;
    _Atomic double   period_cents;
    _Atomic uint64_t deg_lo;
    _Atomic uint64_t deg_hi;
    _Atomic bool     bypass;
    _Atomic bool     bypass_mute; /* bypass puts SILENCE on the output rather
                                     than the dry passthrough */
    _Atomic bool     lead_on;
    _Atomic double   gain_lin;

    _Atomic bool     midi_mode;
    _Atomic uint64_t vmidi_lo; /* virtual held notes (API/tests); backends OR
                                  their hardware bits in at apply time */
    _Atomic uint64_t vmidi_hi;
    /* FOLLOW link input: a note from another engine instance, OR'd into
       the held set like a hardware note, plus the source's envelope
       level. Stored note+1 so a zeroed struct means "none", and kept
       OUTSIDE AeLiveParams so a config write can never clobber a live
       link. */
    _Atomic int      follow_note_p1;
    _Atomic double   follow_level;

    _Atomic bool     harm_on;
    _Atomic int      harm_lock;
    _Atomic int      harm_interval[AE_HARM_VOICES];
    _Atomic int      harm_ext[AE_HARM_VOICES];
    _Atomic int      lead_shift_steps;
    _Atomic double   harm_master_lin;
    _Atomic double   harm_gain_lin[AE_HARM_VOICES];
    _Atomic double   harm_detune_cents[AE_HARM_VOICES];
    _Atomic double   harm_glide_ms;
    _Atomic double   lead_gain_lin;
    _Atomic int      send_content;
    _Atomic double   send_gain_lin;
    _Atomic bool     send_on;
    _Atomic bool     midi_fold;
    _Atomic bool     formant_hold;
    _Atomic double   formant_st;
    _Atomic double   sample_mix;
    _Atomic double   sample_velocity;
    _Atomic double   sample_vel_ref;
    _Atomic bool     sample_ring;
    _Atomic int      poly_notes;
    _Atomic double   gate_rms;
    _Atomic double   sample_trim_lin;
    _Atomic int      attack_sound;
    _Atomic double   attack_gain_lin;
    _Atomic bool     harm_sustain;
    _Atomic bool     harm_hold;
    _Atomic double   harm_pan[AE_HARM_VOICES];
    _Atomic uint32_t harm_mute;
    _Atomic uint32_t harm_solo;

    _Atomic int      harm_source;
    _Atomic int      harm_voice_source[AE_HARM_VOICES];
    _Atomic int      lead_source;
    _Atomic int      synth_patch;
    _Atomic double   ensemble_depth;
    _Atomic double   synth_vowel;
    _Atomic double   harm_tilt_db;
    _Atomic int      vowel_mode;
    _Atomic double   synth_attack_ms;
    _Atomic double   synth_release_ms;
    _Atomic double   lead_attack_ms;
    _Atomic double   lead_release_ms;
    _Atomic bool     drone_on;
    _Atomic int      drone_deg;
    _Atomic double   ir_lead_mix;
    _Atomic double   ir_lead_gain_db;
    _Atomic bool     ir_lead_on;
    _Atomic double   ir_harm_mix;
    _Atomic double   ir_harm_gain_db;
    _Atomic bool     ir_harm_on;
} AeAtomicParams;

/* Local audio tap ring: SPSC -- the audio thread is the only writer, the
   control thread's sender the only reader. Fixed power-of-two capacity;
   the write counter is TOTAL samples ever written, which doubles as the
   alignment counter the tap packets carry. */
#define AE_TAP_RING (1 << 15)
typedef struct
{
    float buf[AE_TAP_RING];
    _Atomic long long w;   /* total samples written (monotonic) */
    long long r;           /* reader-owned cursor */
    _Atomic bool on;
    _Atomic int  content;  /* AE_SEND_* */
} AeTapRing;

static inline void ae_tap_push (AeTapRing *t, float v)
{
    const long long w = atomic_load_explicit (&t->w, memory_order_relaxed);
    t->buf[w & (AE_TAP_RING - 1)] = v;
    atomic_store_explicit (&t->w, w + 1, memory_order_release);
}

static inline int ae_tap_ring_read (AeTapRing *t, float *out, int max,
                                    long long *first)
{
    const long long w = atomic_load_explicit (&t->w, memory_order_acquire);
    long long r = t->r;
    if (w - r > AE_TAP_RING)
        r = w - AE_TAP_RING; /* overrun: drop oldest, keep the counter true */
    int n = (int) (w - r);
    if (n > max)
        n = max;
    for (int i = 0; i < n; ++i)
        out[i] = t->buf[(r + i) & (AE_TAP_RING - 1)];
    *first = r;
    t->r = r + n;
    return n;
}

/* One tap sample from the stems every backend already has in hand. */
static inline float ae_tap_value (int content, float wet, float hm, float full)
{
    switch (content)
    {
        case AE_SEND_WET:  return wet + hm;
        case AE_SEND_LEAD: return wet;
        case AE_SEND_HARM: return hm;
        default:           return full;
    }
}

/* The FOLLOW note as held-set bits, for status readouts: the panel's
   "following note X" lamp reads the same truth the corrector plays. */
static inline void ae_follow_bits (const AeAtomicParams *a,
                                   uint64_t *lo, uint64_t *hi)
{
    const int fn = atomic_load_explicit (
        &((AeAtomicParams *) a)->follow_note_p1, memory_order_relaxed) - 1;
    *lo = fn >= 0 && fn < 64 ? 1ull << fn : 0;
    *hi = fn >= 64 && fn < 128 ? 1ull << (fn - 64) : 0;
}

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
    atomic_store_explicit (&a->expression,      p->expression,      memory_order_relaxed);
    atomic_store_explicit (&a->follow_env,      p->follow_env,      memory_order_relaxed);
    atomic_store_explicit (&a->ref_hz,          p->ref_hz,          memory_order_relaxed);
    atomic_store_explicit (&a->period_cents,    p->period_cents,    memory_order_relaxed);
    atomic_store_explicit (&a->deg_lo,          p->degrees_lo,      memory_order_relaxed);
    atomic_store_explicit (&a->deg_hi,          p->degrees_hi,      memory_order_relaxed);
    atomic_store_explicit (&a->bypass,          p->bypass,          memory_order_relaxed);
    atomic_store_explicit (&a->bypass_mute,     p->bypass_mute,     memory_order_relaxed);
    atomic_store_explicit (&a->lead_on,         p->lead_on,         memory_order_relaxed);
    atomic_store_explicit (&a->gain_lin, pow (10.0, p->output_gain_db / 20.0),
                           memory_order_relaxed);

    atomic_store_explicit (&a->midi_mode, p->midi_mode, memory_order_relaxed);
    atomic_store_explicit (&a->harm_on,   p->harm_on,   memory_order_relaxed);
    atomic_store_explicit (&a->harm_lock, p->harm_lock, memory_order_relaxed);
    atomic_store_explicit (&a->lead_shift_steps, p->lead_shift_steps,
                           memory_order_relaxed);
    atomic_store_explicit (&a->harm_master_lin,
                           pow (10.0, p->harm_master_db / 20.0),
                           memory_order_relaxed);
    for (int v = 0; v < AE_HARM_VOICES; ++v)
    {
        atomic_store_explicit (&a->harm_interval[v], p->harm_interval[v], memory_order_relaxed);
        atomic_store_explicit (&a->harm_ext[v],      p->harm_ext[v],      memory_order_relaxed);
        atomic_store_explicit (&a->harm_gain_lin[v], pow (10.0, p->harm_gain_db[v] / 20.0),
                               memory_order_relaxed);
        atomic_store_explicit (&a->harm_pan[v],      p->harm_pan[v],      memory_order_relaxed);
        atomic_store_explicit (&a->harm_detune_cents[v], p->harm_detune_cents[v],
                               memory_order_relaxed);
    }
    atomic_store_explicit (&a->harm_glide_ms, p->harm_glide_ms, memory_order_relaxed);
    atomic_store_explicit (&a->lead_gain_lin,
                           pow (10.0, p->lead_gain_db / 20.0), memory_order_relaxed);
    atomic_store_explicit (&a->send_content, p->send_content, memory_order_relaxed);
    atomic_store_explicit (&a->send_gain_lin,
                           pow (10.0, p->send_gain_db / 20.0), memory_order_relaxed);
    atomic_store_explicit (&a->send_on, p->send_on, memory_order_relaxed);
    atomic_store_explicit (&a->midi_fold,    p->midi_fold,    memory_order_relaxed);
    atomic_store_explicit (&a->formant_hold, p->formant_hold, memory_order_relaxed);
    atomic_store_explicit (&a->formant_st,   p->formant_st,   memory_order_relaxed);
    atomic_store_explicit (&a->sample_mix,      p->sample_mix,      memory_order_relaxed);
    atomic_store_explicit (&a->sample_velocity, p->sample_velocity, memory_order_relaxed);
    atomic_store_explicit (&a->sample_ring,     p->sample_ring,     memory_order_relaxed);
    atomic_store_explicit (&a->poly_notes,      p->poly_notes,      memory_order_relaxed);
    atomic_store_explicit (&a->gate_rms,
                           p->gate_db < 0.0 ? pow (10.0, p->gate_db / 20.0)
                                            : 0.0,
                           memory_order_relaxed);
    atomic_store_explicit (&a->sample_trim_lin,
                           pow (10.0, p->sample_trim_db / 20.0),
                           memory_order_relaxed);
    atomic_store_explicit (&a->sample_vel_ref,  p->sample_vel_ref,  memory_order_relaxed);
    atomic_store_explicit (&a->attack_sound, p->attack_sound, memory_order_relaxed);
    atomic_store_explicit (&a->attack_gain_lin,
                           pow (10.0, p->attack_gain_db / 20.0),
                           memory_order_relaxed);
    atomic_store_explicit (&a->harm_sustain, p->harm_sustain, memory_order_relaxed);
    atomic_store_explicit (&a->harm_hold,    p->harm_hold,    memory_order_relaxed);
    atomic_store_explicit (&a->harm_mute, p->harm_mute, memory_order_relaxed);
    atomic_store_explicit (&a->harm_solo, p->harm_solo, memory_order_relaxed);

    atomic_store_explicit (&a->harm_source,      p->harm_source,      memory_order_relaxed);
    atomic_store_explicit (&a->lead_source,      p->lead_source,      memory_order_relaxed);
    atomic_store_explicit (&a->ensemble_depth,   p->ensemble_depth,   memory_order_relaxed);
    atomic_store_explicit (&a->synth_vowel,      p->synth_vowel,      memory_order_relaxed);
    atomic_store_explicit (&a->harm_tilt_db,     p->harm_tilt_db,     memory_order_relaxed);
    atomic_store_explicit (&a->vowel_mode,       p->vowel_mode,       memory_order_relaxed);
    for (int v = 0; v < AE_HARM_VOICES; ++v)
        atomic_store_explicit (&a->harm_voice_source[v], p->harm_voice_source[v],
                               memory_order_relaxed);
    atomic_store_explicit (&a->synth_patch,      p->synth_patch,      memory_order_relaxed);
    atomic_store_explicit (&a->synth_attack_ms,  p->synth_attack_ms,  memory_order_relaxed);
    atomic_store_explicit (&a->synth_release_ms, p->synth_release_ms, memory_order_relaxed);
    atomic_store_explicit (&a->lead_attack_ms,   p->lead_attack_ms,   memory_order_relaxed);
    atomic_store_explicit (&a->lead_release_ms,  p->lead_release_ms,  memory_order_relaxed);
    atomic_store_explicit (&a->drone_on,         p->drone_on,         memory_order_relaxed);
    atomic_store_explicit (&a->drone_deg,        p->drone_deg,        memory_order_relaxed);
    atomic_store_explicit (&a->ir_lead_mix,      p->ir_lead_mix,      memory_order_relaxed);
    atomic_store_explicit (&a->ir_lead_gain_db,  p->ir_lead_gain_db,  memory_order_relaxed);
    atomic_store_explicit (&a->ir_lead_on,       p->ir_lead_on,       memory_order_relaxed);
    atomic_store_explicit (&a->ir_harm_mix,      p->ir_harm_mix,      memory_order_relaxed);
    atomic_store_explicit (&a->ir_harm_gain_db,  p->ir_harm_gain_db,  memory_order_relaxed);
    atomic_store_explicit (&a->ir_harm_on,       p->ir_harm_on,       memory_order_relaxed);
}

/* Audio thread, once per block: push the current parameters into the
   corrector and report bypass/lead/gain for the output stage. hw_midi_lo/hi
   are the backend's hardware held-note bits (0 if it has no MIDI input). */
static inline void ae_atomic_params_apply (AeAtomicParams *a, AeCorrector *ps,
                                           uint64_t hw_midi_lo, uint64_t hw_midi_hi,
                                           AeMixParams *mix)
{
    /* The FOLLOW note rides in like a hardware note: OR'd into the held
       set, never stored in the config-owned virtual set, so a config
       write cannot clobber a live link (and vice versa). */
    {
        const int fn = atomic_load_explicit (&a->follow_note_p1,
                                             memory_order_relaxed) - 1;
        uint64_t flo = 0, fhi = 0;
        if (fn >= 0 && fn < 64)        flo = 1ull << fn;
        else if (fn >= 64 && fn < 128) fhi = 1ull << (fn - 64);
        ae_corrector_set_midi (ps,
                atomic_load_explicit (&a->midi_mode, memory_order_relaxed),
                hw_midi_lo | flo
                    | atomic_load_explicit (&a->vmidi_lo, memory_order_relaxed),
                hw_midi_hi | fhi
                    | atomic_load_explicit (&a->vmidi_hi, memory_order_relaxed));
        ae_corrector_set_follow_level (ps,
                atomic_load_explicit (&a->follow_level, memory_order_relaxed));
        ae_corrector_set_follow (ps,
                atomic_load_explicit (&a->follow_env, memory_order_relaxed));
    }
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
        voices[v].detune_cents =
            atomic_load_explicit (&a->harm_detune_cents[v], memory_order_relaxed);
        voices[v].mute     = ((mute >> v) & 1u) != 0;
        voices[v].solo     = ((solo >> v) & 1u) != 0;
    }
    ae_corrector_set_harmony (ps,
                          atomic_load_explicit (&a->harm_on, memory_order_relaxed),
                          atomic_load_explicit (&a->harm_lock, memory_order_relaxed),
                          voices);
    ae_corrector_set_midi_fold (ps,
                          atomic_load_explicit (&a->midi_fold, memory_order_relaxed));
    ae_corrector_set_formant_hold (ps,
                          atomic_load_explicit (&a->formant_hold, memory_order_relaxed));
    ae_corrector_set_formant_st (ps,
                          atomic_load_explicit (&a->formant_st, memory_order_relaxed));
    ae_corrector_set_sample (ps,
                          atomic_load_explicit (&a->sample_mix, memory_order_relaxed),
                          atomic_load_explicit (&a->sample_velocity, memory_order_relaxed),
                          atomic_load_explicit (&a->sample_ring, memory_order_relaxed));
    ae_corrector_set_vel_ref (ps,
                          atomic_load_explicit (&a->sample_vel_ref, memory_order_relaxed));
    ae_corrector_set_poly_notes (ps,
                          atomic_load_explicit (&a->poly_notes, memory_order_relaxed));
    ae_corrector_set_gate (ps,
                          atomic_load_explicit (&a->gate_rms, memory_order_relaxed));
    ae_corrector_set_sample_trim (ps,
                          atomic_load_explicit (&a->sample_trim_lin, memory_order_relaxed));
    ae_corrector_set_attack (ps,
                          atomic_load_explicit (&a->attack_sound, memory_order_relaxed),
                          atomic_load_explicit (&a->attack_gain_lin, memory_order_relaxed));
    ae_corrector_set_harm_glide_ms (ps,
                          atomic_load_explicit (&a->harm_glide_ms, memory_order_relaxed));
    ae_corrector_set_harm_sustain (ps,
                          atomic_load_explicit (&a->harm_sustain, memory_order_relaxed));
    ae_corrector_set_harm_hold (ps,
                          atomic_load_explicit (&a->harm_hold, memory_order_relaxed));
    ae_corrector_set_harm_master (ps,
                          atomic_load_explicit (&a->harm_master_lin, memory_order_relaxed));
    ae_corrector_set_lead_shift (ps,
                          atomic_load_explicit (&a->lead_shift_steps, memory_order_relaxed));
    ae_corrector_set_lead_on (ps,
                          atomic_load_explicit (&a->lead_on, memory_order_relaxed));
    ae_corrector_set_synth (ps,
                        atomic_load_explicit (&a->harm_source, memory_order_relaxed),
                        atomic_load_explicit (&a->synth_patch, memory_order_relaxed),
                        atomic_load_explicit (&a->synth_attack_ms, memory_order_relaxed),
                        atomic_load_explicit (&a->synth_release_ms, memory_order_relaxed));
    ae_corrector_set_lead_env (ps,
                        atomic_load_explicit (&a->lead_attack_ms, memory_order_relaxed),
                        atomic_load_explicit (&a->lead_release_ms, memory_order_relaxed));
    int vsrc[AE_HARM_VOICES];
    for (int v = 0; v < AE_HARM_VOICES; ++v)
        vsrc[v] = atomic_load_explicit (&a->harm_voice_source[v], memory_order_relaxed);
    ae_corrector_set_voice_sources (ps, vsrc,
                        atomic_load_explicit (&a->lead_source, memory_order_relaxed));
    ae_corrector_set_synth_shape (ps,
                        atomic_load_explicit (&a->ensemble_depth, memory_order_relaxed),
                        atomic_load_explicit (&a->synth_vowel, memory_order_relaxed),
                        atomic_load_explicit (&a->harm_tilt_db, memory_order_relaxed),
                        atomic_load_explicit (&a->vowel_mode, memory_order_relaxed));
    ae_corrector_set_drone (ps,
                        atomic_load_explicit (&a->drone_on, memory_order_relaxed),
                        atomic_load_explicit (&a->drone_deg, memory_order_relaxed));
    ae_corrector_set_ir_params (ps, 0,
                        atomic_load_explicit (&a->ir_lead_mix, memory_order_relaxed),
                        atomic_load_explicit (&a->ir_lead_gain_db, memory_order_relaxed),
                        atomic_load_explicit (&a->ir_lead_on, memory_order_relaxed));
    ae_corrector_set_ir_params (ps, 1,
                        atomic_load_explicit (&a->ir_harm_mix, memory_order_relaxed),
                        atomic_load_explicit (&a->ir_harm_gain_db, memory_order_relaxed),
                        atomic_load_explicit (&a->ir_harm_on, memory_order_relaxed));

    mix->bypass      = atomic_load_explicit (&a->bypass, memory_order_relaxed);
    mix->bypass_mute = atomic_load_explicit (&a->bypass_mute, memory_order_relaxed);
    mix->lead_on     = atomic_load_explicit (&a->lead_on, memory_order_relaxed);
    mix->lead_gain   = (float) atomic_load_explicit (&a->lead_gain_lin, memory_order_relaxed);
    mix->master_gain = (float) atomic_load_explicit (&a->gain_lin, memory_order_relaxed);
    mix->send_content = atomic_load_explicit (&a->send_content, memory_order_relaxed);
    mix->send_gain    = (float) atomic_load_explicit (&a->send_gain_lin, memory_order_relaxed);
    mix->send_on      = atomic_load_explicit (&a->send_on, memory_order_relaxed);
}

#endif /* AUTOEDO_AUDIO_PARAMS_H */
