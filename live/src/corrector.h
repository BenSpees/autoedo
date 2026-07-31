/* Real-time monophonic pitch corrector.

   Pipeline:
     1. YIN estimates the input fundamental.
     2. The fundamental is snapped to the nearest enabled pitch on an N-EDO
        grid anchored at the root (see tuning.h).
     3. A retune-speed control glides the output pitch toward that target
        (0 ms = hard snap, larger = natural slide).
     4. Signalsmith Stretch (see shifter.h) resynthesises the audio at the
        corrected pitch; each harmony voice is another instance of the same
        shifter at its own interval. Unvoiced / silent passages pass through
        a latency-matched dry path with a smooth crossfade.

   Allocation happens only in ae_corrector_prepare(); ae_corrector_process() is
   allocation-free and safe to call from a real-time audio callback. */

#ifndef AUTOEDO_CORRECTOR_H
#define AUTOEDO_CORRECTOR_H

#include <limits.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "irconv.h"
#include "shifter.h"
#include "tuning.h"
#include "yin.h"

/* Smart harmony (Xentar emulation): up to five independent ghost voices at
   signed EDO-step intervals from the corrected source, optionally locked to
   the lit-degree mask (walk-outward, up first) or to the JI landmark set. */
#define AE_HARM_VOICES 5
#define AE_HARM_DEG_OFF INT_MIN /* live-readout sentinel: voice silent */

/* Harmony source: ghosts as pitch-shifted copies of the live input (the
   classic harmonizer) or as synthesized voices at the same target degrees
   (a backing pad that rings past the sung note by its release time).
   Phase 1: the source applies to all five voices at once. */
#define AE_HARM_SRC_VOICE 0
#define AE_HARM_SRC_SYNTH 1
/* Per-voice sentinel: "whatever the global harmSource says". Voices default
   to this, so the single global switch keeps working untouched and a chart
   only names the voices it actually wants to differ. */
#define AE_HARM_SRC_DEFAULT (-1)

#define AE_SYNTH_PARTIALS 6

/* Formant (vowel) transfer: a channel vocoder that lifts the live voice's
   spectral envelope onto the synth, so a sung "ah" -> "oo" moves the synth
   with it. Band count is the resolution/cost trade: 16 log-spaced bands
   across the vowel range resolve F1/F2 comfortably. */
#define AE_VOC_BANDS 16
/* Signals filtered independently: harmony L, harmony R, and the lead when it
   is synth. */
#define AE_VOC_SIGNALS 3

/* Vowel transfer modes. `vocoder` is the 16-band channel vocoder: cheap,
   unconditionally stable, moves vowel COLOUR. `lpc` estimates the vocal
   tract itself -- an all-pole fit per frame, imposed on the synth -- which
   resolves formants continuously instead of quantizing them to bands, lets
   the tract stay put while the pitch moves (the "formant-corrected" part),
   and can carry consonants through its residual. */
#define AE_VOWEL_MODE_VOCODER 0
#define AE_VOWEL_MODE_LPC     1

/* All-pole order: 18 poles resolves F1..F4 plus spectral tilt at either
   session rate, which is the usual choice for a ~20 ms speech window. */
#define AE_LPC_ORDER  18
#define AE_LPC_WINDOW 1024

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

    /* Buffers -------------------------------------------------------------- */
    float *in_buf;   /* mono input history: detection source + dry path */
    float *frame;    /* scratch frame for detection */
    float *in_block; /* scratch: this block's input (mono is written in place) */
    float *wet_buf;  /* scratch: corrected block from the shifter */
    float *voice_buf;/* scratch: one harmony voice's block, mixed then reused */

    /* Pitch shifters: the corrected voice plus one per harmony voice. */
    AeShifter *shifter;
    AeShifter *h_shifter[AE_HARM_VOICES];
    int        block_samples; /* shifter block = quality/latency setting */

    /* Running state -------------------------------------------------------- */
    AeYin detector;

    long long in_write;       /* total input samples written */
    long long last_detect_at; /* in_write at last detection */

    double shift_semitones;   /* current correction shift, semitones */
    bool   voiced;
    bool   primed;            /* becomes true once first pitch is found */

    double out_cents; /* current (smoothed) output pitch, cents re ref */
    double v_gain;    /* smoothed dry(0)/wet(1) crossfade gain */

    long long target_j;      /* current target degree (signed, re root) */
    bool      target_valid;  /* false until a voiced target exists */
    bool      in_transition; /* gliding between two different degrees */
    double    sustain_s;     /* seconds the current degree has been held */

    /* MIDI Harmony mode ------------------------------------------------------ */
    /* While any note is held, the held notes override the mask: correction
       snaps to the nearest held note (absolute degree) and harmony's Mask
       lock quantizes within the held pitch classes. MIDI note 60 (middle C)
       is the pivot, mapping to degree 4*edo (the root, four equaves up);
       each MIDI semitone is one EDO step. No notes held = normal behavior. */
    bool     midi_mode;
    uint64_t midi_lo; /* held-note bitset, notes 0..63 */
    uint64_t midi_hi; /* notes 64..127 */

    /* Harmony voices -------------------------------------------------------- */
    bool        harm_on;
    int         harm_lock; /* 0 = off, 1 = mask, 2 = JI landmarks */
    AeHarmVoice harm[AE_HARM_VOICES];

    double    h_semitones[AE_HARM_VOICES]; /* shift re input, semitones */
    bool      h_active[AE_HARM_VOICES];    /* configured and sounding */
    bool      h_fed[AE_HARM_VOICES];       /* shifter has continuous history */
    double    h_mix[AE_HARM_VOICES];       /* smoothed mix gain (click-free) */
    double    h_gl[AE_HARM_VOICES];        /* constant-power pan gains */
    double    h_gr[AE_HARM_VOICES];
    _Atomic int h_deg_out[AE_HARM_VOICES]; /* live ghost degree (UI) */

    /* Synth harmony source ------------------------------------------------- */
    int    harm_source;      /* AE_HARM_SRC_*: the DEFAULT for every voice */
    int    h_source[AE_HARM_VOICES]; /* per-voice override, AE_HARM_SRC_DEFAULT
                                        = follow harm_source */
    int    lead_source;      /* the corrected LEAD voice: shifted input, or the
                                synth playing its corrected pitch */
    int    synth_patch;      /* index into the built-in patch table */
    double synth_attack_ms;
    double synth_release_ms;
    double ensemble_depth;   /* 0..1 scaling on the ensemble's wet blend */
    double synth_vowel;      /* 0..1 formant transfer from the live voice */
    double harm_tilt_db;     /* -12..+12: harmony-bus tone tilt, - dark/+ bright */

    /* Tilt EQ state: a one-pole split into low and high halves, recombined
       with complementary gains. Per side of the harmony bus; the lead never
       reaches this bus, which is exactly the scope asked for. */
    double tilt_lp[2];
    double tilt_a;      /* one-pole coefficient for the ~700 Hz pivot */
    double tilt_g_lo, tilt_g_hi;
    double tilt_db_cur; /* what the gains were built for */

    double in_level;                  /* smoothed voiced input RMS; frozen
                                         while unvoiced so release tails hold
                                         their level like they hold pitch */
    double h_cents_t[AE_HARM_VOICES]; /* ghost target, absolute cents re ref;
                                         holds its last value while unvoiced
                                         so the release tail keeps its pitch */
    double s_cents[AE_HARM_VOICES];   /* smoothed (glided) synth pitch */
    double s_env[AE_HARM_VOICES];     /* attack/release envelope */
    double s_lp[AE_HARM_VOICES];      /* one-pole low-pass state */
    double s_phase[AE_HARM_VOICES][AE_SYNTH_PARTIALS]; /* 0..1 per partial */
    double s_lfo[AE_HARM_VOICES][AE_SYNTH_PARTIALS];   /* vibrato phase, rad */

    /* The LEAD as a synth voice (lead_source == AE_HARM_SRC_SYNTH): its own
       oscillator state, playing the corrected pitch the shifter would have
       produced. */
    double lead_phase[AE_SYNTH_PARTIALS];
    double lead_lfo[AE_SYNTH_PARTIALS];
    double lead_lp;

    /* DRONE: one synth voice pinned to an ABSOLUTE degree, sustained while
       on regardless of what the singer does (a root-only chord in the chart
       means "drone that root"). Level rides in_level like the ghosts --
       frozen while unvoiced -- so it breathes with the set instead of
       blasting before the first note. Renders after the vowel stage (a
       drone has no mouth to follow) and before the ensemble. */
    bool      drone_on;
    long long drone_j;    /* absolute engine degree */
    double    drone_env;
    double    drone_cents;
    double    drone_phase[AE_SYNTH_PARTIALS];
    double    drone_lfo[AE_SYNTH_PARTIALS];
    double    drone_lp;

    /* IR points (v0.4-delta B7): convolution spaces from the shared irconv
       library (vendored byte-identical with Treebrain). The LEAD point sits
       on the corrected voice -- a live monitored path, which is why the
       convolver's first partition is direct time-domain: ZERO added
       latency. The HARMONY point is the one stereo point (L/R convolved
       independently) and sits post-ensemble, PRE-tilt: the tilt stays the
       performer's final tone trim over whatever space the IR imposes.
       Created in prepare (2 s ceiling at the engine rate); loading runs on
       the control thread and crossfades in over ~30 ms. */
    IrcPoint *ir_lead;
    IrcPoint *ir_harm[2];

    /* Ensemble (string-machine chorus): a stereo pair of delay lines the
       whole synth harmony bus runs through when the patch asks for it.
       Allocated in prepare (fs-sized), so the audio thread never allocates. */
    float  *ens_buf_l;
    float  *ens_buf_r;
    int     ens_len;   /* power of two */
    int     ens_mask;
    int     ens_write;
    double  ens_lfo[6]; /* tap LFO phases (3 slow + 3 fast), radians */

    /* Vowel/formant transfer. One shared analysis of the live input drives
       per-signal synthesis filters; every state here is plain scalars, so
       the whole stage is allocation-free. */
    double voc_a1[AE_VOC_BANDS], voc_a2[AE_VOC_BANDS]; /* shared bandpass */
    double voc_b0[AE_VOC_BANDS];                       /* (b1 = 0, b2 = -b0) */
    double voc_env[AE_VOC_BANDS];                      /* input band levels */
    double voc_ax1[AE_VOC_BANDS], voc_ax2[AE_VOC_BANDS]; /* analysis state */
    double voc_ay1[AE_VOC_BANDS], voc_ay2[AE_VOC_BANDS];
    double voc_sx1[AE_VOC_SIGNALS][AE_VOC_BANDS], voc_sx2[AE_VOC_SIGNALS][AE_VOC_BANDS];
    double voc_sy1[AE_VOC_SIGNALS][AE_VOC_BANDS], voc_sy2[AE_VOC_SIGNALS][AE_VOC_BANDS];
    double voc_norm[AE_VOC_SIGNALS]; /* smoothed level match, per signal */
    double voc_atk, voc_rel; /* envelope follower coefficients */
    bool   voc_ready;        /* coefficients built for the current fs */

    /* LPC vowel mode. The vocal tract is estimated per detection hop as
       REFLECTION coefficients: a lattice built from them is stable for any
       |k| < 1, and -- unlike direct-form coefficients -- interpolating them
       between frames stays stable too, which is the classic way this filter
       blows up. Analysis writes lpc_k_t; the audio thread slews lpc_k
       toward it and filters with that. */
    int    vowel_mode;
    double lpc_k[AE_LPC_ORDER];    /* slewed, what the filters use */
    double lpc_k_t[AE_LPC_ORDER];  /* analysis target */
    bool   lpc_valid;              /* an analysis has succeeded */
    double lpc_lat[AE_VOC_SIGNALS][AE_LPC_ORDER + 1]; /* synthesis lattices */
    double lpc_inv[AE_LPC_ORDER + 1];                 /* analysis lattice */
    double lpc_norm[AE_VOC_SIGNALS];                  /* smoothed level match */
    float *lpc_res;                /* this block's residual (max_block) */

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

    /* Pitch-trace ring: one (detected, target) pair per detection hop
       (~200/s), for the graph view. Each slot is one atomic u64 (two packed
       float32s) so a pair can never tear; trace_seq counts detections ever
       made and its release-store publishes the slot. */
#define AE_TRACE_SLOTS 64
    _Atomic uint64_t trace_slots[AE_TRACE_SLOTS];
    _Atomic uint32_t trace_seq;
} AeCorrector;

/* Allocate buffers and pitch shifters for a sample rate / block size. Call
   before processing. min_hz/max_hz bound the detection range (pass 0 for the
   defaults, 65 / 1600 Hz). `quality` is an AeShifterQuality, which sets the
   shifter block size and therefore the latency. */
void ae_corrector_prepare (AeCorrector *p, double sample_rate, int max_block_size,
                           double min_hz, double max_hz, int quality);

/* Clear all internal state. */
void ae_corrector_reset (AeCorrector *p);

/* Destroy the pitch shifters (called by prepare/free; exposed so a
   re-prepare can't leak them). */
void ae_corrector_free_shifters (AeCorrector *p);

/* Release buffers allocated by ae_corrector_prepare(). */
void ae_corrector_free (AeCorrector *p);

/* Process num_samples of mono audio in place (the corrected voice), and —
   when harm_l/harm_r are non-NULL — overwrite them with the latency-aligned
   harmony-voice mix for the same block (constant-power panned, gated by the
   same voiced crossfade). Pass NULL/NULL to skip harmony rendering. */
void ae_corrector_process (AeCorrector *p, float *mono, float *harm_l, float *harm_r,
                       int num_samples);

/* Configure the harmony voices (audio thread, between blocks). */
void ae_corrector_set_harmony (AeCorrector *p, bool on, int lock,
                           const AeHarmVoice voices[AE_HARM_VOICES]);

/* Configure the harmony source and synth voice (audio thread, between
   blocks). Out-of-range patch indices clamp into the table. */
void ae_corrector_set_synth (AeCorrector *p, int source, int patch,
                             double attack_ms, double release_ms);

/* Per-voice source overrides plus the lead's own source (audio thread,
   between blocks). `sources` entries are AE_HARM_SRC_DEFAULT (follow the
   global switch), _VOICE or _SYNTH; `lead` is _VOICE or _SYNTH. */
void ae_corrector_set_voice_sources (AeCorrector *p,
                                     const int sources[AE_HARM_VOICES], int lead);

/* Ensemble depth (0..1, scaling the wet blend of patches that use it), vowel
   transfer amount (0..1), and the harmony bus's tone tilt in dB (-12..+12,
   negative darker / positive brighter). All live. The tilt reaches every
   harmony voice -- shifted and synth alike -- and never the lead. */
void ae_corrector_set_synth_shape (AeCorrector *p, double ensemble_depth,
                                   double vowel, double tilt_db, int vowel_mode);

/* The drone: on/off and its ABSOLUTE engine degree (clamped to 0..8 equaves
   above the reference anchor). Live; the synth attack/release envelope
   shapes the edges, and the master harmony switch still gates it. */
void ae_corrector_set_drone (AeCorrector *p, bool on, long long degree);

/* IR points: 0 = lead, 1 = harmony. Loading (CONTROL thread -- FFT fills)
   arms a ~30 ms crossfade to the new impulse; ir_r NULL uses ir_l on both
   sides of the stereo harmony point; ir_l NULL/len 0 fades to bypass.
   Returns false while a previous swap is still fading. */
bool ae_corrector_load_ir (AeCorrector *p, int point, const float *ir_l,
                           const float *ir_r, int len, double predelay_ms);

/* Live IR parameters for a point: wet mix 0..1, wet gain dB, on/off (off
   lets the tail ride out through the mix smoothing). Lock-free. */
void ae_corrector_set_ir_params (AeCorrector *p, int point, double mix,
                                 double gain_db, bool on);

/* The source actually in force for a harmony voice, resolving the
   per-voice sentinel against the global switch. */
static inline int ae_corrector_voice_source (const AeCorrector *p, int v)
{
    const int s = p->h_source[v];
    return s == AE_HARM_SRC_DEFAULT ? p->harm_source : s;
}

/* The built-in synth patch table (any thread; the table is static). */
int         ae_synth_patch_count (void);
const char *ae_synth_patch_name (int i);
int         ae_synth_patch_find (const char *name); /* -1 = unknown */

/* Configure MIDI Harmony (audio thread, between blocks). */
static inline void ae_corrector_set_midi (AeCorrector *p, bool mode,
                                      uint64_t held_lo, uint64_t held_hi)
{
    p->midi_mode = mode;
    p->midi_lo   = held_lo;
    p->midi_hi   = held_hi;
}

/* Live ghost degree of a voice (signed steps re root), AE_HARM_DEG_OFF when
   the voice is silent. Lock-free; any thread. */
static inline int ae_corrector_harm_degree (const AeCorrector *p, int voice)
{
    return atomic_load_explicit (&((AeCorrector *) p)->h_deg_out[voice],
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

static inline double ae_corrector_clamp01 (double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

static inline void ae_corrector_set_edo (AeCorrector *p, int edo)          { p->edo = edo; }
static inline void ae_corrector_set_retune_ms (AeCorrector *p, double ms)  { p->retune_ms = ms < 0.0 ? 0.0 : ms; }
static inline void ae_corrector_set_transition_ms (AeCorrector *p, double ms) { p->transition_ms = ms < 0.0 ? 0.0 : ms; }
static inline void ae_corrector_set_amount (AeCorrector *p, double a)      { p->amount = ae_corrector_clamp01 (a); }
static inline void ae_corrector_set_tolerance_cents (AeCorrector *p, double c) { p->tolerance_cents = c < 0.0 ? 0.0 : (c > 50.0 ? 50.0 : c); }
static inline void ae_corrector_set_stickiness (AeCorrector *p, double s)  { p->stickiness = ae_corrector_clamp01 (s); }
static inline void ae_corrector_set_humanize (AeCorrector *p, double h)    { p->humanize = ae_corrector_clamp01 (h); }
static inline void ae_corrector_set_reference (AeCorrector *p, double ref_hz, double period_cents)
{
    p->ref_hz       = ref_hz > 0.0 ? ref_hz : AE_REFERENCE_C0_HZ;
    p->period_cents = (period_cents >= 600.0 && period_cents <= 2400.0) ? period_cents : 1200.0;
}

/* Set which scale degrees (0..count-1, 0 == C) the corrector may tune to. */
static inline void ae_corrector_set_enabled_degrees (AeCorrector *p, const bool *mask, int count)
{
    const int n = count < AE_MAX_EDO ? count : AE_MAX_EDO;
    for (int i = 0; i < n; ++i)
        p->enabled_deg[i] = mask[i];
}

/* Reported processing latency in samples (constant after prepare). */
static inline int ae_corrector_latency (const AeCorrector *p) { return p->latency; }

static inline float ae_corrector_detected_hz (const AeCorrector *p)
{
    return atomic_load_explicit (&((AeCorrector *) p)->detected_hz_out, memory_order_relaxed);
}
static inline float ae_corrector_target_hz (const AeCorrector *p)
{
    return atomic_load_explicit (&((AeCorrector *) p)->target_hz_out, memory_order_relaxed);
}
static inline bool ae_corrector_voiced (const AeCorrector *p)
{
    return atomic_load_explicit (&((AeCorrector *) p)->voiced_out, memory_order_relaxed);
}

/* Copy the most recent trace points, oldest first: det[i] is the detected
   pitch (0 = unvoiced) and tgt[i] the correction target of one detection
   hop. Fills *seq_out with the count of detections ever made -- a consumer
   dedupes overlapping reads by remembering the seq it last drew. Safe
   against the audio thread; a slot the writer laps mid-read yields a
   newer-generation point, never a torn one. */
static inline int ae_corrector_trace (const AeCorrector *p, uint32_t *seq_out,
                                      float *det, float *tgt, int max)
{
    AeCorrector *c = (AeCorrector *) p;
    const uint32_t seq = atomic_load_explicit (&c->trace_seq, memory_order_acquire);
    int n = max < AE_TRACE_SLOTS ? max : AE_TRACE_SLOTS;
    if ((uint32_t) n > seq)
        n = (int) seq;
    for (int i = 0; i < n; ++i)
    {
        const uint32_t s = seq - (uint32_t) n + (uint32_t) i;
        const uint64_t packed =
            atomic_load_explicit (&c->trace_slots[s % AE_TRACE_SLOTS], memory_order_relaxed);
        union { uint32_t u; float f; } d, t;
        d.u = (uint32_t) (packed >> 32);
        t.u = (uint32_t) (packed & 0xffffffffu);
        det[i] = d.f;
        tgt[i] = t.f;
    }
    *seq_out = seq;
    return n;
}

#endif /* AUTOEDO_CORRECTOR_H */
