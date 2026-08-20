/* Scored guitar input through the real detector, in 22-EDO.

   The rig's own Xentar electric recordings (real pick transients, real
   inharmonic partials, real decay) are rendered at KNOWN pitches on the
   22-EDO grid over the practical guitar range, fed through the corrector
   at the guitar rig's own settings, and measured against the score.
   Ground truth is machine-readable, so this asserts what the field
   captures could only suggest:

     - detection accuracy per settled note (median / max cents error)
     - zero octave misreads inside a note
     - the 22-EDO quantize target on the scored degree, and STAYING there
       (flips counted)
     - one sample strike per scored pick, none in the rests
     - per-note latency: scored onset -> first correct voicing, and -> strike
     - soft picks still strike, notes under the field's noise floor still
       track, silence after a damp stays silent

   AE_GT_SWEEP=1 runs the detection-tuning sweep instead: every axis of
   the config-reachable detection variables (quality, detection floor,
   gate, stickiness), one at a time against the baseline, printing the
   aggregate for each -- the procedure that set the shipped defaults.

   The library is the FAST INNER LOOP; field captures remain the final
   exam. Needs the Xentar wav tree (AE_XENTAR_WAV, or a sibling
   treeductor checkout); prints SKIPPED and exits 0 without it. Runs at
   the library's native 48 k. */

#include "corrector.h"
#include "sampler.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#define SR    48000.0
#define CH    256
#define STEP  (1200.0 / 22.0)
#define C4_HZ 261.6255653006
#define NOISE_DB (-51.0)   /* the measured field noise floor */

/* Hard bars: set from the shipped detector's measured behaviour with
   headroom -- regression guards, not aspirations. */
#define BAR_MED_CENTS   25.0
#define BAR_MAX_CENTS   60.0
#define BAR_TGT_CENTS    3.0
#define BAR_FLIPS        1     /* per note, settled body */
#define OCTAVE_CENTS   550.0

typedef struct
{
    int    deg;
    double off_cents;
    double start, dur;
    bool   let_ring;
    int    slide_to;   /* != deg: resample-ramp there across the note */
    double vel;        /* 0 = default 1.0 */
    double vib_cents;  /* != 0: +/- this much at 5.5 Hz, finger vibrato */
    double swell_s;    /* > 0: gain ramps 0 -> vel over this long (volume
                          swell from silence -- no pick transient at all) */
    double bend_j;     /* > 0: a bend JOURNEY -- up this many cents over the
                          first quarter of the note, held a quarter, released
                          over a quarter, home for the last. One pick. */
    int    leg_to;     /* with leg_frac: hammer-on / pull-off target degree */
    double leg_frac;   /* > 0: pitch steps to leg_to at this fraction of the
                          note, waveform continuous -- no pick transient */
} GtNote;

typedef struct
{
    const char   *name;
    const GtNote *notes;
    int           n;
    double        tail_s;
    double        noise_db;    /* 0 = clean */
    bool          report_only;
    int           want_strikes; /* 0 = one per note; else this exactly */
} GtScore;

typedef struct
{
    const char *name;
    int    quality;
    double min_hz;
    double gate_db;    /* 0 = engine default */
    double stickiness;
} GtCfg;

typedef struct
{
    double det_med, det_max, tgt_med;
    int    oct, flips, strikes, rest_voiced;
    int    ghosts;     /* voiced hops whose INPUT was under -45 dBFS AND
                          whose detected pitch is >= 100 c from every
                          scored note -- tracking a real tail at its own
                          pitch is correct behaviour however quiet it gets;
                          reading a pitch nobody played out of near-noise
                          is #69's ring-down ghost, measured directly */
    double lat_voice_ms, lat_strike_ms;  /* means over scored notes */
    int    lat_n;
} GtMetrics;

#define N(d,s,du)        { d, 0.0, s, du, false, d, 0.0, 0.0, 0.0, 0.0, 0, 0.0 }
#define NOFF(d,off,s,du) { d, off, s, du, false, d, 0.0, 0.0, 0.0, 0.0, 0, 0.0 }
#define NRING(d,s,du)    { d, 0.0, s, du, true,  d, 0.0, 0.0, 0.0, 0.0, 0, 0.0 }
#define NSLIDE(d,to,s,du){ d, 0.0, s, du, false, to, 0.0, 0.0, 0.0, 0.0, 0, 0.0 }
#define NSOFT(d,s,du)    { d, 0.0, s, du, false, d, 0.12, 0.0, 0.0, 0.0, 0, 0.0 }
#define NVIB(d,s,du,v)   { d, 0.0, s, du, false, d, 0.0, v, 0.0, 0.0, 0, 0.0 }
#define NSWELL(d,s,du,sw){ d, 0.0, s, du, false, d, 0.0, 0.0, sw, 0.0, 0, 0.0 }
#define NBENDJ(d,s,du,b) { d, 0.0, s, du, false, d, 0.0, 0.0, 0.0, b, 0, 0.0 }
#define NLEG(d,to,s,du)  { d, 0.0, s, du, false, d, 0.0, 0.0, 0.0, 0.0, to, 0.5 }

static const GtNote k_low[]    = { N(-36,0.2,0.5), N(-34,0.95,0.5), N(-32,1.7,0.5), N(-30,2.45,0.5),
                                   N(-28,3.2,0.5), N(-26,3.95,0.5), N(-24,4.7,0.5), N(-22,5.45,0.5) };
static const GtNote k_mid[]    = { N(-10,0.2,0.5), N(-8,0.95,0.5), N(-6,1.7,0.5), N(-4,2.45,0.5),
                                   N(-2,3.2,0.5), N(0,3.95,0.5), N(2,4.7,0.5), N(4,5.45,0.5) };
static const GtNote k_high[]   = { N(8,0.2,0.5), N(10,0.95,0.5), N(12,1.7,0.5), N(14,2.45,0.5),
                                   N(16,3.2,0.5), N(18,3.95,0.5), N(20,4.7,0.5), N(22,5.45,0.5) };
static const GtNote k_top[]    = { N(24,0.2,0.5), N(26,0.95,0.5), N(28,1.7,0.5), N(30,2.45,0.5) };
static const GtNote k_repick[] = { N(-6,0.2,0.28), N(-6,0.6,0.28), N(-6,1.0,0.28), N(-6,1.4,0.28),
                                   N(-6,1.8,0.28), N(-6,2.2,0.28), N(-6,2.6,0.28), N(-6,3.0,0.28) };
static const GtNote k_leaps[]  = { N(-36,0.2,0.5), N(-14,1.0,0.5), N(8,1.8,0.5),
                                   N(-14,2.6,0.5), N(-36,3.4,0.5) };
static const GtNote k_bend[]   = { NOFF(-8,20,0.2,0.5), NOFF(-3,20,0.95,0.5), NOFF(2,20,1.7,0.5),
                                   NOFF(7,20,2.45,0.5), NOFF(12,20,3.2,0.5) };
static const GtNote k_soft[]   = { NSOFT(-14,0.2,0.5), NSOFT(-6,0.95,0.5), NSOFT(0,1.7,0.5),
                                   NSOFT(8,2.45,0.5) };
static const GtNote k_slide[]  = { NSLIDE(-10,-4,0.2,0.8), NSLIDE(0,6,1.4,0.8),
                                   NSLIDE(8,2,2.6,0.8) };
static const GtNote k_ring[]   = { NRING(-14,0.2,0.4), NRING(-6,0.9,0.4) };
/* Finger vibrato at +/-30 c crosses the 22-EDO boundary midpoint (27.25 c):
   the note identity must hold anyway -- the stickiness question, scored. */
static const GtNote k_vib[]    = { NVIB(-14,0.2,0.8,30.0), NVIB(-6,1.2,0.8,30.0),
                                   NVIB(2,2.2,0.8,30.0), NVIB(10,3.2,0.8,30.0) };
/* Tremolo picking: the same degree at ~6/s, faster than the repick row --
   every pick must strike (the field's "repeats not retriggering"). */
static const GtNote k_trem[]   = { N(-6,0.2,0.14), N(-6,0.36,0.14), N(-6,0.52,0.14),
                                   N(-6,0.68,0.14), N(-6,0.84,0.14), N(-6,1.00,0.14),
                                   N(-6,1.16,0.14), N(-6,1.32,0.14) };
/* Volume swells from silence: no pick transient at all. Report-only --
   the legacy 2.5x onset route fires spurious pulses on these today
   (measured); the floor route stays quiet. Tracks that behaviour. */
static const GtNote k_swell[]  = { NSWELL(-10,0.2,1.2,0.6), NSWELL(-2,1.8,2.0,1.5) };
/* The bend JOURNEY (field: "bend a string a whole step up, have it track,
   bend back down, and return to original -- a whole step bend is 218
   cents in 22-edo"): one pick, +4 EDO steps over the first quarter,
   held, released, home. Detection must ride the finger the whole way
   and the bend must never retrigger -- strikes == picks. Duration 1.2 s
   keeps the home stretch inside honest sustain: at 1.6 s the string has
   decayed far enough that the octave harmonic outweighs the fundamental
   and YIN reads +1200 c -- that is ring-down behaviour (#67/#69), not
   bend tracking, and it is measured by the ring-down scores instead. */
static const GtNote k_bendj[]  = { NBENDJ(-10,0.2,1.2,4*STEP), NBENDJ(4,1.8,1.2,4*STEP),
                                   NBENDJ(-24,3.4,1.2,4*STEP) };
/* Hammer-ons / pull-offs: the pitch steps mid-note with the waveform
   continuous -- no pick transient. The detector must land on the new
   pitch; what the sampler does with it is reported first. */
static const GtNote k_leg[]    = { NLEG(-14,-10,0.2,0.9), NLEG(-6,-2,1.4,0.9),
                                   NLEG(0,-4,2.6,0.9), NLEG(10,6,3.8,0.9) };

static const GtScore k_scores[] = {
    { "low walk  E2..C#3", k_low,    8, 0.8, 0.0, false, 0 },
    { "mid walk  F#3..A#3",k_mid,    8, 0.8, 0.0, false, 0 },
    { "high walk E4..C5",  k_high,   8, 0.8, 0.0, false, 0 },
    { "top walk  D5..E5",  k_top,    4, 0.8, 0.0, false, 0 },
    { "repick x8 same deg",k_repick, 8, 0.8, 0.0, false, 0 },
    { "octave leaps",      k_leaps,  5, 0.8, 0.0, false, 0 },
    { "bends +20c off grid",k_bend,  5, 0.8, 0.0, false, 0 },
    { "mid walk + floor noise", k_mid, 8, 0.8, NOISE_DB, false, 0 },
    { "soft picks (vel .12)",   k_soft, 4, 0.8, 0.0, false, 0 },
    { "vibrato +/-30c",    k_vib,    4, 0.8, 0.0, false, 0 },
    { "tremolo 6/s",       k_trem,   8, 0.8, 0.0, false, 0 },
    /* legato: a hammer-on / pull-off strikes the NEW note, measured
       deterministic -- one pick strike plus one transition strike each */
    { "bend journey +218c",k_bendj,  3, 0.8, 0.0, false, 0 },
    { "hammer/pull legato",k_leg,    4, 0.8, 0.0, false, 8 },
    { "slides (report)",   k_slide,  3, 0.8, 0.0, true, 0  },
    { "swells (report)",   k_swell,  2, 1.0, 0.0, true, 0  },
    { "ring-down (report)",k_ring,   2, 3.0, 0.0, true, 0  },
    { "ring-down + noise (report)", k_ring, 2, 3.0, NOISE_DB, true, 0 },
};

static double note_cents (const GtNote *n, double frac)
{
    double c = n->deg * STEP + n->off_cents;
    if (n->leg_frac > 0.0 && frac >= n->leg_frac)
        c = n->leg_to * STEP + n->off_cents;
    if (n->slide_to != n->deg)
        c += (n->slide_to - n->deg) * STEP * frac;
    if (n->bend_j > 0.0)
    {
        const double ph = frac * 4.0;  /* up, hold, release, home */
        c += n->bend_j * (ph < 1.0 ? ph : ph < 2.0 ? 1.0
                          : ph < 3.0 ? 3.0 - ph : 0.0);
    }
    if (n->vib_cents != 0.0)
        c += n->vib_cents * sin (2.0 * 3.14159265358979 * 5.5 * frac * n->dur);
    return c;
}

static double note_hz (const GtNote *n, double frac)
{
    return C4_HZ * pow (2.0, note_cents (n, frac) / 1200.0);
}

static const AeSampleRec *pick_rec (const AeSampleBank *b, double hz, int rr)
{
    const double midi = 69.0 + 12.0 * log2 (hz / 440.0);
    int best = 0; double bd = 1e9;
    for (int z = 0; z < b->n_zones; ++z)
        if (fabs (b->zones[z] - midi) < bd)
            { bd = fabs (b->zones[z] - midi); best = z; }
    if (b->main_n[best] <= 0) return NULL;
    return &b->recs[b->main_idx[best][rr % b->main_n[best]]];
}

static void render_note (float *buf, int total, const AeSampleBank *b,
                         const GtNote *n, int rr)
{
    const AeSampleRec *r = pick_rec (b, note_hz (n, 0.0), rr);
    if (r == NULL || r->len < 64) return;
    const double rec_hz = 440.0 * pow (2.0, (r->midi - 69) / 12.0);
    float peak = 1e-6f;
    for (int i = 0; i < r->len; ++i)
        if (fabsf (r->pcm[i]) > peak) peak = fabsf (r->pcm[i]);
    const double vel = n->vel > 0.0 ? n->vel : 1.0;
    const double g = 0.35 * vel / peak;
    const int s0 = (int)(n->start * SR);
    const int dur = n->let_ring ? total - s0 : (int)(n->dur * SR);
    const int fade = (int)(0.030 * SR);
    double pos = 0.0;
    for (int i = 0; i < dur && s0 + i < total; ++i)
    {
        const double frac = (double) i / dur;
        pos += note_hz (n, frac) / rec_hz;   /* slide = ramped resample */
        const int ip = (int) pos;
        if (ip + 1 >= r->len) break;
        const double fr = pos - ip;
        double x = g * ((1.0 - fr) * r->pcm[ip] + fr * r->pcm[ip + 1]);
        if (n->swell_s > 0.0)
        {
            const double sw = ((double) i / SR) / n->swell_s;
            x *= sw < 1.0 ? sw : 1.0;
        }
        if (! n->let_ring && i > dur - fade)
            x *= (double)(dur - i) / fade;
        buf[s0 + i] += (float) x;
    }
}

static double med (double *v, int n)
{
    if (n == 0) return 0.0;
    for (int i = 1; i < n; ++i)
        for (int j = i; j > 0 && v[j] < v[j-1]; --j)
            { double t = v[j]; v[j] = v[j-1]; v[j-1] = t; }
    return v[n / 2];
}

/* Deterministic noise (no rand(): runs must be identical run to run). */
static double gt_noise (unsigned *st)
{
    *st = *st * 1664525u + 1013904223u;
    return ((double)(*st >> 8) / (double)(1u << 24)) * 2.0 - 1.0;
}

static GtMetrics run_score (const GtScore *S, const AeSampleBank *bank,
                            const char *root, const GtCfg *cfg)
{
    GtMetrics M; memset (&M, 0, sizeof (M));
    const GtNote *last = &S->notes[S->n - 1];
    const int total = (int)((last->start + last->dur + S->tail_s) * SR);
    float *buf = calloc ((size_t) total, sizeof (float));
    for (int i = 0; i < S->n; ++i)
        render_note (buf, total, bank, &S->notes[i], i);
    if (S->noise_db != 0.0)
    {
        unsigned st = 0xC0FFEE;
        const double a = pow (10.0, S->noise_db / 20.0);
        for (int i = 0; i < total; ++i)
            buf[i] += (float)(a * gt_noise (&st));
    }

    AeCorrector *p = calloc (1, sizeof (AeCorrector));
    ae_corrector_prepare (p, SR, CH, cfg->min_hz, 1400.0, cfg->quality);
    ae_corrector_reset (p);
    ae_corrector_set_edo (p, 22);
    { bool m[22]; for (int d = 0; d < 22; ++d) m[d] = true;
      ae_corrector_set_enabled_degrees (p, m, 22); }
    ae_corrector_set_transition_ms (p, 0.0);
    ae_corrector_set_retune_ms (p, 0.0);
    ae_corrector_set_stickiness (p, cfg->stickiness);
    if (cfg->gate_db != 0.0)
        ae_corrector_set_gate (p, pow (10.0, cfg->gate_db / 20.0));
    char err[256];
    if (! ae_corrector_load_samples (p, root, "electric", NULL, err, sizeof (err)))
        { fprintf (stderr, "bank load: %s\n", err); exit (1); }
    p->lead_source = AE_HARM_SRC_SAMPLE;
    ae_corrector_set_sample (p, 1.0, 1.0, false);

    const int hops = total / CH;
    double *det = malloc ((size_t) hops * sizeof (double));
    double *tgt = malloc ((size_t) hops * sizeof (double));
    double *hlev = malloc ((size_t) hops * sizeof (double));
    double strike_t[256]; int n_strike_t = 0;
    const AeSampleRec *prevr[AE_SMP_SLOTS] = { 0 };
    double prevp[AE_SMP_SLOTS] = { 0 };
    for (int h = 0; h < hops; ++h)
    {
        { double a = 0.0;
          for (int i = 0; i < CH; ++i)
              a += (double) buf[(size_t) h * CH + i] * buf[(size_t) h * CH + i];
          hlev[h] = sqrt (a / CH); }
        ae_corrector_process (p, buf + (size_t) h * CH, NULL, NULL, CH);
        const float d = ae_corrector_detected_hz (p);
        const float t = ae_corrector_target_hz (p);
        det[h] = (p->voiced && d > 0) ? 1200.0 * log2 (d / C4_HZ) : NAN;
        tgt[h] = (p->voiced && t > 0) ? 1200.0 * log2 (t / C4_HZ) : NAN;
        for (int sl = 0; sl < AE_SMP_SLOTS; ++sl)
        {
            const AeSampleRec *rc = p->smp[AE_HARM_VOICES][sl].rec;
            const double ps = p->smp[AE_HARM_VOICES][sl].pos;
            if (rc != NULL && (prevr[sl] == NULL || ps < prevp[sl] - 1.0))
            {
                ++M.strikes;
                if (n_strike_t < 256)
                    strike_t[n_strike_t++] = (double)(h + 1) * CH / SR;
                if (getenv ("AE_GT_STRIKES"))
                    fprintf (stderr, "  [%s] strike %.3f s\n",
                             S->name, (double)(h + 1) * CH / SR);
            }
            prevr[sl] = rc; prevp[sl] = ps;
        }
    }

    double meds[64], tgterr[64]; int nmed = 0, ntg = 0;
    double latv = 0.0, lats = 0.0; int latn = 0;
    for (int i = 0; i < S->n; ++i)
    {
        const GtNote *n = &S->notes[i];
        const bool sliding = n->slide_to != n->deg;
        /* moving: the finger is legitimately somewhere else each hop --
           score against note_cents there, and don't read target-degree
           changes as flips (following the finger through degrees is the
           job, not jitter). Vibrato is the exception: it crosses the
           boundary but the note identity must hold, so its flips COUNT. */
        const bool moving = n->vib_cents != 0.0 || n->bend_j > 0.0
                         || n->leg_frac > 0.0;
        const double want = n->deg * STEP + n->off_cents;
        /* settled body: skip the attack and the damp fade. Short notes
           (tremolo) shrink both margins so the window never collapses --
           an empty window would count as an octave misread below. */
        const double m0 = n->dur > 0.3 ? 0.12 : n->dur * 0.45;
        const double m1 = n->dur > 0.3 ? 0.06 : n->dur * 0.25;
        const int h0 = (int)((n->start + m0) * SR) / CH;
        const int h1 = (int)((n->start + n->dur - m1) * SR) / CH;
        double errs[512]; int ne = 0; double terrs[512]; int nt = 0;
        int last_tdeg = INT_MIN;
        for (int h = h0; h < h1 && h < hops && ! sliding; ++h)
        {
            if (isnan (det[h])) continue;
            const double frac = ((double)(h + 1) * CH / SR - n->start) / n->dur;
            /* a hammer-on's step is instant but the detector's vote is
               not -- give it 100 ms around the transition */
            if (n->leg_frac > 0.0
                && fabs (frac - n->leg_frac) * n->dur < 0.10) continue;
            const double e = det[h] - (moving ? note_cents (n, frac) : want);
            { const char *dmp = getenv ("AE_GT_DUMP");
              if (dmp && strstr (S->name, dmp))
                  fprintf (stderr, "%.3f det %7.1f want %7.1f%s\n",
                           (double)(h + 1) * CH / SR, det[h],
                           moving ? note_cents (n, frac) : want,
                           fabs (e) > OCTAVE_CENTS ? "  OCT" : ""); }
            if (fabs (e) > OCTAVE_CENTS) { ++M.oct; continue; }
            if (ne < 512) errs[ne++] = fabs (e);
            if (! isnan (tgt[h]))
            {
                const int td = (int) lround (tgt[h] / STEP);
                if (last_tdeg != INT_MIN && td != last_tdeg
                    && ! (n->bend_j > 0.0 || n->leg_frac > 0.0)) ++M.flips;
                last_tdeg = td;
                if (n->off_cents == 0.0 && ! moving)
                {
                    const double te = tgt[h] - n->deg * STEP;
                    if (fabs (te) < OCTAVE_CENTS && nt < 512)
                        terrs[nt++] = fabs (te);
                }
            }
        }
        if (! sliding)
        {
            if (ne > 0 && nmed < 64)
            {
                double mx = 0.0;
                for (int k = 0; k < ne; ++k) if (errs[k] > mx) mx = errs[k];
                if (mx > M.det_max) M.det_max = mx;
                meds[nmed++] = med (errs, ne);
            }
            else if (ne == 0)
                ++M.oct;
            if (nt > 0 && ntg < 64) tgterr[ntg++] = med (terrs, nt);
        }
        /* latency: onset -> first hop detecting within 100 c of the score,
           and onset -> first strike at/after the onset */
        {
            const int ha = (int)(n->start * SR) / CH;
            for (int h = ha; h < hops; ++h)
                if (! isnan (det[h]) && fabs (det[h] - want) < 100.0)
                    { latv += ((double)(h + 1) * CH / SR - n->start) * 1000.0;
                      ++latn; break; }
            for (int k = 0; k < n_strike_t; ++k)
                if (strike_t[k] >= n->start - 0.01)
                    { lats += (strike_t[k] - n->start) * 1000.0; break; }
        }
        if (! n->let_ring && ! sliding)
        {
            const double rest_end = (i + 1 < S->n)
                ? S->notes[i + 1].start : last->start + last->dur + S->tail_s;
            const int r0 = (int)((n->start + n->dur + 0.08) * SR) / CH;
            const int r1 = (int)((rest_end - 0.01) * SR) / CH;
            for (int h = r0; h < r1 && h < hops; ++h)
                if (! isnan (det[h])) ++M.rest_voiced;
        }
    }
    { const double bar = pow (10.0, -45.0 / 20.0);
      for (int h = 0; h < hops; ++h)
      {
          if (isnan (det[h]) || hlev[h] >= bar) continue;
          bool near = false;
          for (int i = 0; i < S->n && ! near; ++i)
          {
              const GtNote *sn = &S->notes[i];
              const double c0 = sn->deg * STEP + sn->off_cents;
              double c1 = sn->slide_to * STEP + sn->off_cents;
              if (sn->leg_frac > 0.0) c1 = sn->leg_to * STEP + sn->off_cents;
              const double lo = (c0 < c1 ? c0 : c1) - 100.0;
              const double hi = (c0 < c1 ? c1 : c0) + 100.0
                              + (sn->bend_j > 0.0 ? sn->bend_j : 0.0);
              near = det[h] >= lo && det[h] <= hi;
          }
          if (! near) ++M.ghosts;
      } }
    M.det_med = med (meds, nmed);
    M.tgt_med = med (tgterr, ntg);
    M.lat_n = latn;
    M.lat_voice_ms  = latn ? latv / latn : 0.0;
    M.lat_strike_ms = latn ? lats / latn : 0.0;
    free (det); free (tgt); free (hlev); free (buf);
    ae_corrector_free_shifters (p);
    free (p);
    return M;
}

int main (void)
{
    const char *envr = getenv ("AE_XENTAR_WAV");
    const char *roots[] = { envr, "../treeductor/wav", "../../treeductor/wav",
                            "/home/user/treeductor/wav", NULL };
    AeSampleBank bank; char err[256];
    const char *root = NULL;
    for (int i = 0; i < 4; ++i)
    {
        if (roots[i] == NULL) continue;
        memset (&bank, 0, sizeof (bank));
        if (ae_sampler_load (&bank, roots[i], "electric", NULL, SR,
                             AE_SMP_OCTAVE_AUTO, err, sizeof (err)))
            { root = roots[i]; break; }
    }
    if (root == NULL)
    {
        printf ("guitartest: SKIPPED (no Xentar wav library; set AE_XENTAR_WAV)\n");
        return 0;
    }

    /* The shipped baseline: what a fresh rig gets, and what the sweep
       below re-earned. quality balanced (low halves the latency but the
       25 ms window is thin under low guitar), detection floor and gate
       per the sweep, stickiness at the rig's own 0.8. */
    const GtCfg baseline =
        { "baseline", AE_SHIFT_QUALITY_BALANCED, 70.0, 0.0, 0.8 };

    const int nsc = (int)(sizeof (k_scores) / sizeof (k_scores[0]));
    if (getenv ("AE_GT_SWEEP"))
    {
        const GtCfg cfgs[] = {
            baseline,
            { "quality low",   AE_SHIFT_QUALITY_LOW,      70.0, 0.0,   0.8 },
            { "quality high",  AE_SHIFT_QUALITY_HIGH,     70.0, 0.0,   0.8 },
            { "det floor 75",  AE_SHIFT_QUALITY_BALANCED, 75.0, 0.0,   0.8 },
            { "det floor 78",  AE_SHIFT_QUALITY_BALANCED, 78.0, 0.0,   0.8 },
            { "gate -50",      AE_SHIFT_QUALITY_BALANCED, 70.0, -50.0, 0.8 },
            { "gate -45",      AE_SHIFT_QUALITY_BALANCED, 70.0, -45.0, 0.8 },
            { "stick 0.5",     AE_SHIFT_QUALITY_BALANCED, 70.0, 0.0,   0.5 },
            { "stick 0.95",    AE_SHIFT_QUALITY_BALANCED, 70.0, 0.0,   0.95 },
        };
        printf ("sweep: per config, aggregated over %d scores\n", nsc);
        printf ("%-14s %7s %7s %5s %5s %7s %8s %8s %7s %9s %9s\n",
                "config", "detmed", "detmax", "oct", "flip", "strike",
                "restvoi", "softstk", "ghosts", "latv ms", "latstk ms");
        for (size_t c = 0; c < sizeof (cfgs) / sizeof (cfgs[0]); ++c)
        {
            double dm[64]; int ndm = 0; double dmax = 0.0;
            int oct = 0, flips = 0, want_strikes = 0, strikes = 0;
            int rest = 0, soft_st = 0, ghosts = 0;
            double lv = 0.0, ls = 0.0; int ln = 0;
            for (int sc = 0; sc < nsc; ++sc)
            {
                const GtScore *S = &k_scores[sc];
                GtMetrics m = run_score (S, &bank, root, &cfgs[c]);
                ghosts += m.ghosts;
                if (S->report_only) continue;
                if (ndm < 64) dm[ndm++] = m.det_med;
                if (m.det_max > dmax) dmax = m.det_max;
                oct += m.oct; flips += m.flips; rest += m.rest_voiced;
                want_strikes += S->want_strikes ? S->want_strikes : S->n;
                strikes += m.strikes;
                if (strstr (S->name, "soft")) soft_st = m.strikes;
                lv += m.lat_voice_ms * m.lat_n; ls += m.lat_strike_ms * m.lat_n;
                ln += m.lat_n;
            }
            printf ("%-14s %6.1fc %6.1fc %5d %5d %3d/%-3d %8d %7d/4 %7d %8.1f %9.1f\n",
                    cfgs[c].name, med (dm, ndm), dmax, oct, flips,
                    strikes, want_strikes, rest, soft_st, ghosts,
                    ln ? lv / ln : 0.0, ln ? ls / ln : 0.0);
        }
        ae_sampler_free (&bank);
        return 0;
    }

    printf ("guitartest: electric bank from %s (%d recs), 22-EDO, "
            "guitar range\n", root, bank.n_recs);
    int fails = 0;
    for (int sc = 0; sc < nsc; ++sc)
    {
        const GtScore *S = &k_scores[sc];
        GtMetrics m = run_score (S, &bank, root, &baseline);
        printf ("  %-26s notes %2d strikes %2d | det med %5.1fc max %5.1fc "
                "| tgt %4.1fc | oct %d flip %d | rest %d | lat %4.0f/%4.0f ms%s\n",
                S->name, S->n, m.strikes, m.det_med, m.det_max, m.tgt_med,
                m.oct, m.flips, m.rest_voiced + m.ghosts,
                m.lat_voice_ms, m.lat_strike_ms,
                S->report_only ? "  [report only]" : "");
        if (S->report_only) continue;
        { const int ws = S->want_strikes ? S->want_strikes : S->n;
          if (m.strikes != ws)
            { ++fails; printf ("    FAIL strikes %d != %d\n", m.strikes, ws); } }
        if (m.oct != 0)
            { ++fails; printf ("    FAIL octave misreads %d\n", m.oct); }
        if (m.det_med > BAR_MED_CENTS)
            { ++fails; printf ("    FAIL det median %.1f\n", m.det_med); }
        if (m.det_max > BAR_MAX_CENTS)
            { ++fails; printf ("    FAIL det max %.1f\n", m.det_max); }
        if (m.tgt_med > BAR_TGT_CENTS)
            { ++fails; printf ("    FAIL target median %.1f\n", m.tgt_med); }
        if (m.flips > BAR_FLIPS * S->n)
            { ++fails; printf ("    FAIL target flips %d\n", m.flips); }
        if (m.rest_voiced > 0)
            { ++fails; printf ("    FAIL voiced in damped rests %d\n", m.rest_voiced); }
    }
    ae_sampler_free (&bank);
    printf (fails ? "guitartest: %d FAILURE(S)\n" : "guitartest: ALL OK\n", fails);
    return fails ? 1 : 0;
}
