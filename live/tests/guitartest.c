/* Scored guitar input through the real detector, in 22-EDO.

   The rig's own Xentar electric recordings (real pick transients, real
   inharmonic partials, real decay -- everything a synthetic sine is too
   polite to have) are rendered at KNOWN pitches on the 22-EDO grid over
   the practical guitar range, fed through the corrector at the guitar
   rig's own settings, and measured against the score: this note, at this
   time, at this pitch. Ground truth is machine-readable, so what the
   field captures could only suggest, this asserts:

     - detection accuracy per settled note (median / max cents error)
     - zero octave misreads inside a note
     - the 22-EDO quantize target lands on the scored degree
     - one sample strike per scored pick, none in the rests
     - silence after a damp stays silent

   The library is the FAST INNER LOOP; Ben's field captures remain the
   final exam -- these recordings are clean DI takes of one instrument,
   and thresholds tuned here must still be confirmed against real playing
   before they ship. Needs the Xentar wav tree (AE_XENTAR_WAV, or a
   sibling treeductor checkout); prints SKIPPED and exits 0 without it,
   so `make test` works on a bare clone. */

#include "corrector.h"
#include "sampler.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SR    48000.0   /* the Xentar library is native 48 k; the detector is
                           rate-parameterized, so the harness meets the files
                           where they are instead of hauling a transcode step
                           into make test */
#define CH    256
#define STEP  (1200.0 / 22.0)   /* one 22-EDO step, in cents */
#define C4_HZ 261.6255653006

/* Hard bars, calibrated against the shipped detector's first run and set
   with headroom above it -- they exist to catch REGRESSION, not to be
   aspirational. Octave misreads and strike counts are exact by design. */
#define BAR_MED_CENTS   25.0
#define BAR_MAX_CENTS   60.0
#define BAR_TGT_CENTS    3.0
#define OCTAVE_CENTS   550.0

typedef struct
{
    int    deg;        /* 22-EDO degree re C4 (deg * STEP cents) */
    double off_cents;  /* deliberate offset from the grid (bend tests) */
    double start, dur; /* seconds */
    bool   let_ring;   /* no damp fade: natural ring-down past dur */
} GtNote;

typedef struct
{
    const char   *name;
    const GtNote *notes;
    int           n;
    double        tail_s;      /* silence appended after the last note */
    bool          report_only; /* print, never fail (known-open territory) */
} GtScore;

/* ---- scores: the practical guitar range, low E to the lead top ------- */
/* deg -36 = 84.1 Hz (~E2), -14 = 168 (~E3), +8 = 337 (~E4), +30 = 673. */
#define N(d,s,du)          { d, 0.0, s, du, false }
#define NOFF(d,off,s,du)   { d, off, s, du, false }
#define NRING(d,s,du)      { d, 0.0, s, du, true }

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
static const GtNote k_ring[]   = { NRING(-14,0.2,0.4), NRING(-6,0.9,0.4) };

static const GtScore k_scores[] = {
    { "low walk  E2..C#3", k_low,    8, 0.8, false },
    { "mid walk  F#3..A#3",k_mid,    8, 0.8, false },
    { "high walk E4..C5",  k_high,   8, 0.8, false },
    { "top walk  D5..E5",  k_top,    4, 0.8, false },
    { "repick x8 same deg",k_repick, 8, 0.8, false },
    { "octave leaps",      k_leaps,  5, 0.8, false },
    { "bends +20c off grid",k_bend,  5, 0.8, false },
    { "natural ring-down", k_ring,   2, 3.0, true  },
};

/* ---- renderer -------------------------------------------------------- */

static double deg_hz (const GtNote *n)
{
    return C4_HZ * pow (2.0, (n->deg * STEP + n->off_cents) / 1200.0);
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
    const double hz = deg_hz (n);
    const AeSampleRec *r = pick_rec (b, hz, rr);
    if (r == NULL || r->len < 64) return;
    const double rate = hz / (440.0 * pow (2.0, (r->midi - 69) / 12.0));
    float peak = 1e-6f;
    for (int i = 0; i < r->len; ++i)
        if (fabsf (r->pcm[i]) > peak) peak = fabsf (r->pcm[i]);
    const double g = 0.35 / peak;             /* ~-9 dBFS peaks, field-like */
    const int s0 = (int)(n->start * SR);
    const int dur = n->let_ring ? total - s0 : (int)(n->dur * SR);
    const int fade = (int)(0.030 * SR);       /* the damp: a lifted finger */
    double pos = 0.0;
    for (int i = 0; i < dur && s0 + i < total; ++i, pos += rate)
    {
        const int ip = (int) pos;
        if (ip + 1 >= r->len) break;
        const double fr = pos - ip;
        double x = g * ((1.0 - fr) * r->pcm[ip] + fr * r->pcm[ip + 1]);
        if (! n->let_ring && i > dur - fade)
            x *= (double)(dur - i) / fade;
        buf[s0 + i] += (float) x;
    }
}

/* ---- harness --------------------------------------------------------- */

static int    g_fail = 0;
static double med (double *v, int n)
{
    if (n == 0) return 0.0;
    for (int i = 1; i < n; ++i)
        for (int j = i; j > 0 && v[j] < v[j-1]; --j)
            { double t = v[j]; v[j] = v[j-1]; v[j-1] = t; }
    return v[n / 2];
}

int main (void)
{
    const char *envr = getenv ("AE_XENTAR_WAV");
    const char *roots[] = { envr, "../treeductor/wav", "../../treeductor/wav",
                            "/home/user/treeductor/wav", NULL };
    AeSampleBank bank; char err[256];
    const char *root = NULL;
    for (int i = 0; roots[i] != NULL || i == 0; ++i)
    {
        if (roots[i] == NULL) continue;
        memset (&bank, 0, sizeof (bank));
        if (ae_sampler_load (&bank, roots[i], "electric", NULL, SR,
                             AE_SMP_OCTAVE_AUTO, err, sizeof (err)))
            { root = roots[i]; break; }
        if (roots[i+1] == NULL) break;
    }
    if (root == NULL)
    {
        printf ("guitartest: SKIPPED (no Xentar wav library; "
                "set AE_XENTAR_WAV; last error: %s)\n", err);
        return 0;
    }
    printf ("guitartest: electric bank from %s (%d recs), 22-EDO, "
            "guitar range\n", root, bank.n_recs);

    for (size_t sc = 0; sc < sizeof (k_scores) / sizeof (k_scores[0]); ++sc)
    {
        const GtScore *S = &k_scores[sc];
        const GtNote *last = &S->notes[S->n - 1];
        const int total = (int)((last->start + last->dur + S->tail_s) * SR);
        float *buf = calloc ((size_t) total, sizeof (float));
        for (int i = 0; i < S->n; ++i)
            render_note (buf, total, &bank, &S->notes[i], i);

        AeCorrector *p = calloc (1, sizeof (AeCorrector));
        ae_corrector_prepare (p, SR, CH, 70.0, 1400.0,
                              AE_SHIFT_QUALITY_BALANCED);
        ae_corrector_reset (p);
        ae_corrector_set_edo (p, 22);
        { bool m[22]; for (int d = 0; d < 22; ++d) m[d] = true;
          ae_corrector_set_enabled_degrees (p, m, 22); }
        ae_corrector_set_transition_ms (p, 0.0);
        ae_corrector_set_retune_ms (p, 0.0);
        ae_corrector_set_stickiness (p, 0.8);
        if (! ae_corrector_load_samples (p, root, "electric", NULL,
                                         err, sizeof (err)))
            { printf ("  bank into corrector failed: %s\n", err); return 1; }
        p->lead_source = AE_HARM_SRC_SAMPLE;
        ae_corrector_set_sample (p, 1.0, 1.0, false);

        const int hops = total / CH;
        double *det = malloc ((size_t) hops * sizeof (double));
        double *tgt = malloc ((size_t) hops * sizeof (double));
        int strikes = 0;
        const AeSampleRec *prevr[AE_SMP_SLOTS] = { 0 };
        double prevp[AE_SMP_SLOTS] = { 0 };
        for (int h = 0; h < hops; ++h)
        {
            ae_corrector_process (p, buf + (size_t) h * CH, NULL, NULL, CH);
            const float d = ae_corrector_detected_hz (p);
            const float t = ae_corrector_target_hz (p);
            det[h] = (p->voiced && d > 0)
                         ? 1200.0 * log2 (d / C4_HZ) : NAN;
            tgt[h] = (p->voiced && t > 0)
                         ? 1200.0 * log2 (t / C4_HZ) : NAN;
            for (int sl = 0; sl < AE_SMP_SLOTS; ++sl)
            {
                const AeSampleRec *rc = p->smp[AE_HARM_VOICES][sl].rec;
                const double ps = p->smp[AE_HARM_VOICES][sl].pos;
                if (rc != NULL && (prevr[sl] == NULL || ps < prevp[sl] - 1.0))
                    ++strikes;
                prevr[sl] = rc; prevp[sl] = ps;
            }
        }

        double meds[64], maxs = 0.0, tgterr[64]; int nmed = 0, ntg = 0;
        int oct_hops = 0, rest_voiced = 0;
        for (int i = 0; i < S->n; ++i)
        {
            const GtNote *n = &S->notes[i];
            const double want = n->deg * STEP + n->off_cents;
            const int h0 = (int)((n->start + 0.12) * SR) / CH;
            const int h1 = (int)((n->start + n->dur - 0.06) * SR) / CH;
            double errs[512]; int ne = 0; double terrs[512]; int nt = 0;
            for (int h = h0; h < h1 && h < hops; ++h)
            {
                if (isnan (det[h])) continue;
                const double e = det[h] - want;
                if (fabs (e) > OCTAVE_CENTS) { ++oct_hops; continue; }
                if (ne < 512) errs[ne++] = fabs (e);
                if (n->off_cents == 0.0 && ! isnan (tgt[h]))
                {
                    const double te = tgt[h] - n->deg * STEP;
                    if (fabs (te) < OCTAVE_CENTS && nt < 512)
                        terrs[nt++] = fabs (te);
                }
            }
            if (ne > 0 && nmed < 64)
            {
                double mx = 0.0;
                for (int k = 0; k < ne; ++k) if (errs[k] > mx) mx = errs[k];
                if (mx > maxs) maxs = mx;
                meds[nmed++] = med (errs, ne);
            }
            else if (ne == 0)
                ++oct_hops; /* a note that never voiced counts as a miss */
            if (nt > 0 && ntg < 64) tgterr[ntg++] = med (terrs, nt);
            /* the rest after this note's damp: silence must stay silent */
            if (! n->let_ring)
            {
                const double rest_end = (i + 1 < S->n)
                    ? S->notes[i + 1].start : last->start + last->dur + S->tail_s;
                const int r0 = (int)((n->start + n->dur + 0.08) * SR) / CH;
                const int r1 = (int)((rest_end - 0.01) * SR) / CH;
                for (int h = r0; h < r1 && h < hops; ++h)
                    if (! isnan (det[h])) ++rest_voiced;
            }
        }
        const double dmed = med (meds, nmed), tmed = med (tgterr, ntg);
        printf ("  %-22s notes %2d strikes %2d | det med %5.1fc max %5.1fc "
                "| tgt med %4.1fc | oct %d | rest-voiced %d%s\n",
                S->name, S->n, strikes, dmed, maxs, tmed, oct_hops,
                rest_voiced, S->report_only ? "  [report only]" : "");
        if (! S->report_only)
        {
            if (strikes != S->n)
                { ++g_fail; printf ("    FAIL strikes %d != %d\n", strikes, S->n); }
            if (oct_hops != 0)
                { ++g_fail; printf ("    FAIL octave misreads %d\n", oct_hops); }
            if (dmed > BAR_MED_CENTS)
                { ++g_fail; printf ("    FAIL det median %.1f > %.1f\n", dmed, BAR_MED_CENTS); }
            if (maxs > BAR_MAX_CENTS)
                { ++g_fail; printf ("    FAIL det max %.1f > %.1f\n", maxs, BAR_MAX_CENTS); }
            if (ntg > 0 && tmed > BAR_TGT_CENTS)
                { ++g_fail; printf ("    FAIL target median %.1f > %.1f\n", tmed, BAR_TGT_CENTS); }
            if (rest_voiced > 0)
                { ++g_fail; printf ("    FAIL voiced in damped rests: %d\n", rest_voiced); }
        }
        free (det); free (tgt); free (buf);
        ae_corrector_free_shifters (p);
        free (p);
    }
    ae_sampler_free (&bank);
    printf (g_fail ? "guitartest: %d FAILURE(S)\n" : "guitartest: ALL OK\n",
            g_fail);
    return g_fail ? 1 : 0;
}
