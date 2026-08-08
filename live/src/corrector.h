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
#include <stddef.h>
#include <stdint.h>

#include "irconv.h"
#include "sampler.h"
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
/* A third source: the Xentar pitched library. Unlike the other two a
   sample is STRUCK, not continuous -- so a sample ghost is struck at the
   lead's onset and then RE-PITCHED, never re-struck, for as long as that
   note lasts. sampleMix layers it against the shifted rendering of the
   same ghost, so "sample" is a layer with a blend rather than a swap. */
#define AE_HARM_SRC_SAMPLE 2
/* Per-voice sentinel: "whatever the global harmSource says". Voices default
   to this, so the single global switch keeps working untouched and a chart
   only names the voices it actually wants to differ. */
#define AE_HARM_SRC_DEFAULT (-1)

/* Playback slots per sample voice. Two is enough to crossfade a retrigger
   past a dying note; let-ring needs a ring, because every slot is a note
   still sounding on its own decay. Four covers a fast repeated figure at
   the pizzicato and piano decays without stealing, and steals the OLDEST
   when it cannot. */
#define AE_SMP_SLOTS 4

/* The strike-velocity map (vel_from_peak). Both constants are shared with
   Treebrain's FX layer and TENDRIL so the two rigs strike the same
   velocity for the same playing -- change them here and there together. */
#define AE_VEL_WINDOW_DB 24.0 /* dynamic range below the reference peak */
#define AE_VEL_FLOOR      0.2 /* a CONFIRMED note is never near-silent */
/* How long the reference peak takes to forget a loud passage. Long enough
   that a soft section is heard as soft rather than renormalised up to
   sounding hard, short enough to follow a set that changes instrument or
   player. */
#define AE_VEL_REF_TAU_S 20.0
/* The reference never decays below this (-40 dBFS), so a silent rig maps
   its own noise floor to the velocity floor instead of to fortissimo. */
#define AE_VEL_REF_MIN  0.01

#define AE_SYNTH_PARTIALS 6

/* Attack Sound: a transient fired at note ONSET -- energy appearing, before
   the detector has settled on a pitch -- to cover the synth voices' attack
   latency. Deliberately OUTSIDE every envelope: a long synthAttackMs hides
   the machinery of the note arriving, and the attack sound covers the
   moment of the pick itself, at its own gain. */
#define AE_ATK_OFF   0
#define AE_ATK_NOISE 1 /* randomized white-noise chiff */
#define AE_ATK_PICK  2 /* the Xentar pick set: 3 ranges x 2 directions */
#define AE_ATK_CLICK 3 /* damped high tick */

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
    int    interval; /* signed EDO steps; 0 = unison (see detune_cents) */
    int    ext_oct;  /* 0..2 extra octaves, stacking in the voice's direction */
    double detune_cents; /* fine offset applied AFTER the lock quantize, so it
                            survives snapping -- a deliberate few cents is the
                            point, not an error to be corrected away */
    double gain;     /* linear */
    double pan;      /* -1 (L) .. +1 (R) */
    bool   mute;
    bool   solo;
} AeHarmVoice;

/* A voice sounds when it has somewhere to be: any interval, or -- at
   interval 0 -- a detune, which is what makes a UNISON ghost expressible.
   Interval 0 with no detune stays "off", both because it is the historical
   meaning of 0 and because a ghost at an exact unison is a phase-coherent
   double of the lead, which is a comb filter rather than a voice. */
static inline bool ae_harm_voice_on (const AeHarmVoice *hv)
{
    return hv->interval != 0 || hv->detune_cents != 0.0;
}

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
    _Atomic float shift_st_out; /* the same, for the status read-out: lets a
                                   panel SEE the lead's ratio swing instead
                                   of diagnosing bassiness by ear */
    _Atomic float shift_st_min; /* decaying extremes (~1 s memory): status
                                   ticks at 10 Hz and a 50 ms spike between
                                   ticks would otherwise never be seen */
    _Atomic float shift_st_max;
    bool   formant_hold;      /* hold formants still under the shift; off =
                                 no formant processing at all (a guitar has
                                 no vocal tract to preserve) */
    double formant_st;        /* deliberate formant offset, semitones */
    float *lead_wet;          /* last block's wet-only lead (v_gain * shifted
                                 voice, no dry blend, pre lead-IR): the
                                 record send's "wet"/"lead" tap */
    bool   voiced;
    bool   primed;            /* becomes true once first pitch is found */

    double out_cents; /* the corrected NOTE CENTRE, cents re ref */

    /* Expression. A played pitch is a note plus what the player is doing to
       it. `centre_cents` tracks the note (a ~180 ms follower, slower than
       any bend or vibrato and faster than a phrase); the difference between
       it and the instantaneous pitch IS the bend, the vibrato, the scoop.
       Correction is applied to the CENTRE alone and the deviation is added
       back, so the note lands on the degree and the playing survives.
       Without this the law shift = target - detected cancels a bend exactly
       as it happens -- the harder you bend, the harder the engine bends
       back -- which is why expression never reached the output. */
    double centre_cents;
    double expression;   /* 0 = pin hard (the old behaviour), 1 = pass it all */
    double expr_cents;   /* the live deviation, for the ghosts and the trace */
    double v_gain;    /* smoothed dry(0)/wet(1) crossfade gain */

    /* Octave re-vote rebase: the previous hop's (detected, target) pair,
       so a same-hop equave jump in BOTH -- the detector re-labeling the
       octave of a sustained note, not the player moving -- can re-base the
       glide instead of swinging the lead's ratio through an octave. */
    double prev_det_cents;
    double prev_tgt_cents;
    bool   prev_pair_valid;

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
    bool     midi_fold; /* fold the chosen held note by whole equaves into
                           the register actually being played (retune to the
                           held PITCH CLASS where the player is), instead of
                           yanking the note to the held note's absolute
                           octave. Chord voicings live wherever the chord
                           track puts them -- often octaves below a lead
                           line -- and absolute snapping turns that distance
                           into a standing transpose. */
    uint64_t midi_lo; /* held-note bitset, notes 0..63 */
    uint64_t midi_hi; /* notes 64..127 */

    /* Harmony voices -------------------------------------------------------- */
    bool        harm_on;
    int         lead_shift; /* static lead transpose, EDO steps (see setter) */
    bool        lead_on;   /* corrected lead is in the output: decides which
                              pitch the ghosts anchor to (see set_lead_on) */
    int         harm_lock; /* 0 = off, 1 = mask, 2 = JI landmarks */
    AeHarmVoice harm[AE_HARM_VOICES];

    double    h_semitones[AE_HARM_VOICES]; /* shift re input, semitones */
    bool      h_active[AE_HARM_VOICES];    /* configured and sounding */
    bool      h_fed[AE_HARM_VOICES];       /* shifter has continuous history */
    double    h_mix[AE_HARM_VOICES];       /* smoothed mix gain (click-free) */
    double    h_gl[AE_HARM_VOICES];        /* constant-power pan gains */
    double    h_gr[AE_HARM_VOICES];

    /* Harmony-bus master, linear. Rides on top of the per-voice trims and
       reaches every ghost -- shifted, synth and drone -- and never the
       lead. `_cur` is the smoothed value the audio actually sees. */
    double    harm_master;
    double    harm_master_cur;
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
    /* The LEAD's own envelope. Separate from the harmony's above because
       the two jobs are different: the harmony envelope hides the ghosts'
       arrival latency, while the lead's shapes the corrected voice itself
       -- and under let-ring the release is a CEILING over a sample's
       natural decay rather than a tail added to it. */
    double lead_attack_ms;
    double lead_release_ms;
    double lead_env;         /* the lead envelope's current value */
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

    /* Sample voices ---------------------------------------------------------- */
    /* Two bank slots and an atomic index: the control thread fills the
       idle one and swaps. The slot it refills has been dead since the
       PREVIOUS swap (the loader holds off long enough for a block to
       turn over), so no audio-thread voice can still be reading it. */
    AeSampleBank  smp_bank[2];
    _Atomic int   smp_live;      /* -1 = no bank loaded */
    _Atomic int   smp_gen;       /* bumped per swap; a voice from an older
                                    generation releases instead of reading
                                    a freed buffer */
    int           smp_mode;      /* AE_SMP_* placeholder, reserved */
    double        smp_mix;       /* 0 = shifted only, 1 = sample only */
    double        smp_vel_fixed; /* >= 0 = fixed strike level; < 0 = measure */
    bool          smp_ring;      /* let-ring: a struck voice plays to its
                                    natural end THROUGH the next strike */
    _Atomic float smp_vel_out;   /* last strike level, for the read-out */
    _Atomic float smp_vel_ref;   /* the reference the map is measuring against */
    signed char   smp_rr[AE_SMP_MAX_ZONES * 2]; /* last RR pick per zone+layer */
    unsigned      smp_rng;
    int           smp_octave;    /* filename -> sounding offset, or AUTO */
    double        smp_gain_a;    /* per-sample coefficient of the ~15 ms
                                    ramp the velocity refinement rides */

    /* Onset: one pulse per note edge, shared by the attack sound and the
       sample strike -- both need the same "energy appeared" moment, and
       both need it BEFORE the detector has a pitch. */
    bool   onset_pulse;
    int    vel_win;      /* samples left in the velocity measuring window */
    double vel_peak;     /* running peak inside it */
    /* The velocity REFERENCE: "how hard this player plays when playing
       hard". A rolling peak that decays over ~20 s, so the map follows the
       rig's actual headroom instead of assuming the signal reaches full
       scale. See vel_from_peak. */
    double vel_ref;
    /* A reference SUPPLIED by the caller (linear peak), overriding the
       observation above. < 0 = observe. The map's reference is the
       caller's by definition -- "how hard this player plays when playing
       hard" is a fact about the performance, and a host that already knows
       it (TENDRIL's loudest onset of the capture, an FX layer's own rolling
       peak) should not have to wait for this engine to rediscover it from
       the notes it happens to hear. */
    double vel_ref_fixed;

    struct
    {
        const AeSampleRec *rec;
        double pos, rate, gain, gain_t, fade, norm;
        int    gen;
        bool   retiring;  /* damp-on-repitch: this slot is on its way out
                             across the 6 ms fade. Never set under let-ring,
                             where a struck note is left to finish. */
        bool   releasing; /* let-ring, superseded: the next strike landed,
                             and this note now decays under the RELEASE
                             CEILING (synthReleaseMs for ghosts,
                             leadReleaseMs for the lead) rather than
                             ringing to the recording's natural end.
                             Without this the ring is unbounded -- on a
                             many-second piano recording that is a wash of
                             old notes under every new one, which the ear
                             reads as mud, not as sustain. */
        double renv;      /* the ceiling's own envelope, 1 -> 0 */
    } smp[AE_HARM_VOICES + 1][AE_SMP_SLOTS];
    /* [AE_HARM_VOICES] = the lead. Slots are a ring, not a pair: under
       let-ring every strike needs its own, because the old one is still
       sounding rather than being faded past. */
    int    smp_cur[AE_HARM_VOICES + 1];
    bool   smp_pending[AE_HARM_VOICES + 1]; /* an onset is waiting for a
                                               pitch to strike at */
    int    smp_wait[AE_HARM_VOICES + 1];    /* samples it may keep waiting */
    double smp_env[AE_HARM_VOICES + 1];

    /* Attack Sound ---------------------------------------------------------- */
    int    atk_mode;             /* AE_ATK_* */
    double atk_gain;             /* linear; its own volume, no envelope */
    double atk_fast, atk_slow;   /* onset follower (block RMS) */
    int    atk_refract;          /* samples until the next hit may fire */
    bool   atk_armed;            /* Schmitt: a hit fires once per onset EDGE;
                                    re-arms only when the fast/slow ratio
                                    collapses (note settled or ended), so a
                                    refractory expiring mid-swell cannot
                                    double-fire */
    const short *atk_smp;        /* playing pick sample, NULL = synthesized */
    int    atk_smp_len;
    double atk_smp_rms;
    double atk_pos, atk_rate;    /* sample cursor + per-hit rate jitter */
    double atk_amp;              /* level match: ratchets toward the onset */
    double atk_jit;              /* per-hit volume jitter */
    double atk_env, atk_env_a;   /* synthesized burst envelope */
    double atk_hp_x, atk_hp_y;   /* one-pole highpass state (noise chiff) */
    double atk_ph, atk_ph_inc;   /* click body oscillator */
    int    atk_active;           /* 0 idle, else the AE_ATK_* mode playing */
    uint32_t atk_rng;            /* per-hit randomization (LCG) */
    int    atk_last_range;       /* economy-picking state machine: last */
    int    atk_last_dir;         /*   range played and last direction */

    double in_level;                  /* smoothed voiced input RMS; frozen
                                         while unvoiced so release tails hold
                                         their level like they hold pitch */

    /* Harmony sustain: a shifted ghost is made OF the input, so when the
       lead stops there is nothing left for its release to shape. This is
       the material that keeps it going -- a whole number of pitch periods
       lifted from the end of the note and crossfaded into a seamless loop,
       fed to the harmony shifters (never the lead) while the release rings
       out. The ghost sustains in the performer's own timbre instead of
       fading a silence. */
    bool   harm_sustain;     /* feature on/off */
    float *sus_buf;          /* the loop itself */
    int    sus_cap;          /* allocated samples (~250 ms) */
    int    sus_len;          /* loop length in use, 0 = nothing captured */
    int    sus_read;         /* circular read cursor */
    double sus_mix;          /* smoothed live(0) / loop(1) source blend */
    float *sus_block;        /* per-block harmony input when the loop is up */
    double last_voiced_hz;   /* f0 at the moment voicing dropped */

    /* Release rewind: the last few hops before voicing drops are
       contaminated by the release itself -- the mute or finger-lift bends
       the string while the level is still above the gate -- and a ghost
       that follows them ends its note somewhere the player never played.
       A short ring of recent voiced detections lets the note END on what
       was true ~40 ms before the artifact. */
#define AE_REL_RING 16
    float   rel_det[AE_REL_RING];                 /* detected Hz per hop */
    float   rel_rms[AE_REL_RING];                 /* frame RMS per hop */
    float   rel_hc[AE_REL_RING][AE_HARM_VOICES];  /* ghost glide position */
    float   rel_hs[AE_REL_RING][AE_HARM_VOICES];  /* ghost shift, st */
    uint8_t rel_pos;

    /* HOLD: freeze the ghosts where they are and let them ring on their own
       while the lead carries on. Momentary by design -- a controller maps it
       to a footswitch or a MIDI CC. `latched` is the edge-taken state, so
       engaging HOLD captures the sustain loop and the level once rather than
       chasing them. */
    bool   harm_hold;
    bool   hold_latched;
    double hold_level;       /* input RMS frozen at the moment of the hold */
    /* Portamento: how long a ghost takes to travel to a new target. One
       number for BOTH sources, applied once here, so a shifted voice and a
       synth voice on the same interval never disagree about where they are
       mid-slide. 0 = arrive immediately, which is the classic harmonizer. */
    double harm_glide_ms;
    double h_cents_cur[AE_HARM_VOICES]; /* the glide's current position */
    double h_glide_rate[AE_HARM_VOICES]; /* cents/sec for the current leg */
    double h_glide_tgt[AE_HARM_VOICES];  /* last hop's target (leg detector) */
    bool   h_glide_valid[AE_HARM_VOICES]; /* the voice is still SOUNDING, so
                                             h_cents_cur is a real position to
                                             slide from. Deliberately not the
                                             per-frame gate: voicing drops for
                                             a frame on every consonant and at
                                             every note change, and treating
                                             that as "arrived from silence"
                                             snaps the glide shut exactly when
                                             it was supposed to be working. */
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

/* Sample source (audio thread, between blocks): the layer blend against the
   shifted rendering of the same ghost, and the strike level. `mix` 0 = the
   shifted voice alone, 1 = the sample alone, 0.5 = BOTH AT UNITY -- a
   plateau at centre with an equal-power taper either side, so one number
   means the same thing here as it does in the rig's other layers.
   `velocity` >= 0 pins the strike level; < 0 measures it from the lead's
   own attack. */
void ae_corrector_set_sample (AeCorrector *p, double mix, double velocity,
                              bool ring);
/* Supply the strike-velocity reference (linear peak) instead of letting
   the engine observe one; negative = observe. See vel_ref_fixed. */
void ae_corrector_set_vel_ref (AeCorrector *p, double ref_lin);

/* The LEAD voice's attack and release, in ms. Distinct from the harmony's
   (ae_corrector_set_synth): the harmony envelope hides the ghosts' arrival
   latency, this one shapes the corrected lead itself. */
void ae_corrector_set_lead_env (AeCorrector *p, double attack_ms,
                                double release_ms);

/* Load one instrument into the idle bank slot and swap it live (CONTROL
   thread -- reads files, allocates). Returns false with a reason in err;
   a failed load leaves the running bank untouched. */
/* Filename-to-sounding pitch offset in semitones for the NEXT load;
   AE_SMP_OCTAVE_AUTO uses the built-in table. */
void ae_corrector_set_sample_octave (AeCorrector *p, int semitones);

bool ae_corrector_load_samples (AeCorrector *p, const char *root,
                                const char *instrument, const char *manifest,
                                char *err, size_t err_len);

/* The last strike level the sample voices were struck with (0..1): a
   strike level you cannot see is one you cannot tune. */
/* The reference the strike map is measuring against, linear peak. Exposed
   because a RELATIVE map is otherwise unreadable from outside: without it
   a velocity of 0.55 says nothing about whether the player is loud. */
static inline float ae_corrector_sample_vel_ref (const AeCorrector *p)
{
    return atomic_load_explicit (&p->smp_vel_ref, memory_order_relaxed);
}

static inline float ae_corrector_sample_vel (const AeCorrector *p)
{
    return atomic_load_explicit (&((AeCorrector *) p)->smp_vel_out,
                                 memory_order_relaxed);
}

/* Attack Sound (audio thread, between blocks): mode AE_ATK_*, gain LINEAR.
   Fires on energy onsets whenever a synth voice is in the signal path (a
   synth ghost, a synth lead, not a purely-shifted rig, whose real signal
   covers its own onsets). */
void ae_corrector_set_attack (AeCorrector *p, int mode, double gain_lin);

/* Harmony portamento in milliseconds (audio thread, between blocks): the
   time constant a ghost takes to slide to a new target when the lead moves.
   0 = jump. A voice arriving from silence always starts ON pitch rather
   than sliding in from wherever it last was. */
void ae_corrector_set_harm_glide_ms (AeCorrector *p, double ms);

/* HOLD (audio thread, between blocks): freeze every ghost at its current
   pitch and keep it sounding, indefinitely, while the lead goes on
   normally. Sing a chord in, hold it, and keep singing over your own
   choir. Shifted ghosts ride the sustain loop; synth ghosts simply hold
   their note and their level. Releasing HOLD hands the ghosts back to the
   live pitch, releasing naturally if nothing is being sung. */
void ae_corrector_set_harm_hold (AeCorrector *p, bool on);

/* Harmony sustain (audio thread, between blocks). When on, a shifted ghost
   keeps sounding through its release instead of cutting off with the lead:
   the engine loops a period-aligned, crossfaded slice of the end of the
   note and feeds that to the harmony shifters. Bounded entirely by
   synthReleaseMs -- a short release sustains nothing. Never reaches the
   lead, which must stay the performer and nothing else. */
void ae_corrector_set_harm_sustain (AeCorrector *p, bool on);

/* Harmony-bus master gain, LINEAR (audio thread, between blocks). One knob
   over the whole ghost bus: it multiplies every harmony voice -- shifted,
   synth and drone alike -- on top of their per-voice trims, and never
   touches the lead. Smoothed internally, so it is safe to sweep live. */
void ae_corrector_set_harm_master (AeCorrector *p, double gain_lin);

/* Whether the corrected lead is in the engine's output. This is not a
   level: it tells the harmony which pitch the player is actually hearing
   as the lead, so the ghosts can be an exact interval above it (see
   ae_corrector_set_harmony). With the lead in the mix that reference is
   the corrected pitch; with it muted the performer is hearing their own
   instrument, so the reference is the pitch they really played. */
void ae_corrector_set_lead_on (AeCorrector *p, bool on);

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

static inline void ae_corrector_set_midi_fold (AeCorrector *p, bool fold)
{
    p->midi_fold = fold;
}

static inline void ae_corrector_set_formant_hold (AeCorrector *p, bool hold)
{
    p->formant_hold = hold;
}

/* How much of the playing survives correction: 0 pins the output to the
   degree (bends and vibrato removed), 1 passes the whole deviation through
   while the note's centre is still corrected. Steady notes are unaffected
   either way -- only motion faster than the ~180 ms centre follower is at
   stake, which is exactly the expression. */
static inline void ae_corrector_set_expression (AeCorrector *p, double amt)
{
    p->expression = amt < 0.0 ? 0.0 : (amt > 1.0 ? 1.0 : amt);
}

static inline void ae_corrector_set_formant_st (AeCorrector *p, double st)
{
    p->formant_st = st < -12.0 ? -12.0 : (st > 12.0 ? 12.0 : st);
}

/* The wet-only lead from the LAST process call (valid for its length, up to
   the prepared max block): the corrected voice with the dry blend removed,
   pre lead-IR. What a record send calls "lead". */
static inline const float *ae_corrector_lead_wet (const AeCorrector *p)
{
    return p->lead_wet;
}

static inline float ae_corrector_shift_st (const AeCorrector *p)
{
    return atomic_load_explicit (&((AeCorrector *) p)->shift_st_out, memory_order_relaxed);
}
static inline float ae_corrector_shift_st_min (const AeCorrector *p)
{
    return atomic_load_explicit (&((AeCorrector *) p)->shift_st_min, memory_order_relaxed);
}
static inline float ae_corrector_lead_makeup (const AeCorrector *p)
{
    return ae_shifter_makeup (p->shifter);
}
static inline float ae_corrector_shift_st_max (const AeCorrector *p)
{
    return atomic_load_explicit (&((AeCorrector *) p)->shift_st_max, memory_order_relaxed);
}

/* Live ghost degree of a voice (signed steps re root), AE_HARM_DEG_OFF when
   the voice is silent. Lock-free; any thread. */
static inline int ae_corrector_harm_degree (const AeCorrector *p, int voice)
{
    return atomic_load_explicit (&((AeCorrector *) p)->h_deg_out[voice],
                                 memory_order_relaxed);
}

/* Xentar Snap-to-Scale walk: nearest enabled degree to j. `enabled` indexes
   pitch classes 0..edo-1; a fully disabled mask returns j unchanged.

   TIE-BREAK: at equal distance, `prefer_up` decides. A fixed up-first rule
   is right for a ghost ABOVE the lead and exactly wrong for one below --
   on a tie it pulls the ghost TOWARD the lead, so a third below can land
   on the unison and a third above on a second. Both candidates are equally
   far from what was asked for, but one is still an interval and the other
   is a beat. So callers pass the direction that keeps the ghost further
   from the lead: up for a ghost above, down for one below. A voice with no
   apartness to preserve (a unison ghost, or a lead being corrected onto
   the mask with no second voice in the picture) passes true and keeps the
   historical up-first rule. */
static inline long long ae_walk_to_enabled_dir (long long j, int edo,
                                                const bool *enabled,
                                                bool prefer_up)
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
        const bool up   = enabled[(pc0 + d) % edo];
        const bool down = enabled[((pc0 - d) % edo + edo) % edo];
        if (up && down)
            return prefer_up ? j + d : j - d; /* the tie */
        if (up)   return j + d;
        if (down) return j - d;
    }
    return j;
}

static inline long long ae_walk_to_enabled (long long j, int edo, const bool *enabled)
{
    return ae_walk_to_enabled_dir (j, edo, enabled, true);
}

static inline double ae_corrector_clamp01 (double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

static inline void ae_corrector_set_edo (AeCorrector *p, int edo)          { p->edo = edo; }
/* Static transpose of the corrected LEAD, in EDO steps, applied AFTER the
   snap: the detector still hears and classifies the real note; the shift
   moves what comes out. A whole number of steps, so +-edo is an exact
   equave and keeps the pitch class -- the degree mask never notices.
   Locked ghosts take their intervals from the SHIFTED lead target, so the
   harmony stays a scale interval from the note the audience hears. */
static inline void ae_corrector_set_lead_shift (AeCorrector *p, int steps)
{
    p->lead_shift = steps < -72 ? -72 : (steps > 72 ? 72 : steps);
}
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
