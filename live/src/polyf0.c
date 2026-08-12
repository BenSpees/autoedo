#include "polyf0.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Iterative in-place radix-2 FFT (same shape as yin.c's; duplicated
   deliberately -- the two analysers should stay independently tunable). */
static void pf_fft (double *re, double *im, int n)
{
    for (int i = 1, j = 0; i < n; ++i)
    {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j |= bit;
        if (i < j)
        {
            double t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1)
    {
        const double ang = -2.0 * 3.14159265358979323846 / (double) len;
        const double wl_re = cos (ang), wl_im = sin (ang);
        for (int i = 0; i < n; i += len)
        {
            double w_re = 1.0, w_im = 0.0;
            for (int k = 0; k < len / 2; ++k)
            {
                const int p = i + k, q = i + k + len / 2;
                const double v_re = re[q] * w_re - im[q] * w_im;
                const double v_im = re[q] * w_im + im[q] * w_re;
                re[q] = re[p] - v_re;
                im[q] = im[p] - v_im;
                re[p] += v_re;
                im[p] += v_im;
                const double nw_re = w_re * wl_re - w_im * wl_im;
                w_im = w_re * wl_im + w_im * wl_re;
                w_re = nw_re;
            }
        }
    }
}

void ae_polyf0_prepare (AePolyF0 *t, double fs, double min_hz, double max_hz)
{
    memset (t, 0, sizeof (*t));
    t->fs        = fs > 0.0 ? fs : 48000.0;
    t->min_hz    = min_hz >= 20.0 ? min_hz : 55.0;
    t->max_hz    = max_hz > t->min_hz * 2.0 ? max_hz : 1200.0;
    t->max_notes = AE_POLY_MAX_NOTES;

    /* Two periods of the range bottom, next power of two, floor 2048:
       multi-f0 needs frequency resolution more than the mono detector
       does (two chord tones a third apart must be separate peaks). */
    /* Resolution rules here, not just reach: two chord tones a minor
       third apart at the bottom of the range must land in separate bins
       even before interpolation. 4096 at 48 k is ~11.7 Hz bins -- barely
       enough at C3, and the 85 ms window is poly mode's already-declared
       latency neighbourhood. 2048 was measured hopeless (23 Hz bins read
       a triad as soup). */
    int need = (int) ceil (2.0 * t->fs / t->min_hz);
    t->win_size = 4096;
    while (t->win_size < need)
        t->win_size <<= 1;
    if (t->win_size > 8192)
        t->win_size = 8192;
    /* Zero-padded transform: double the bin density for FREE latency-wise
       -- the window (and so the lag) stays the same, only the sampling of
       its spectrum gets finer, which is exactly what the low-register
       refinement was starving for. */
    t->fft_size = t->win_size * 2;

    t->re  = calloc ((size_t) t->fft_size, sizeof (double));
    t->im  = calloc ((size_t) t->fft_size, sizeof (double));
    t->mag  = calloc ((size_t) (t->fft_size / 2 + 1), sizeof (double));
    t->mag0 = calloc ((size_t) (t->fft_size / 2 + 1), sizeof (double));
    t->win = calloc ((size_t) t->win_size, sizeof (float));
    for (int i = 0; i < t->win_size; ++i)
        t->win[i] = (float) (0.5 - 0.5 * cos (2.0 * 3.14159265358979323846
                                              * i / (t->win_size - 1)));
    for (int k = 0; k < AE_POLY_MAX_NOTES; ++k)
        t->notes[k].id = -1;
    t->next_id = 1;
}

void ae_polyf0_free (AePolyF0 *t)
{
    free (t->re);
    free (t->im);
    free (t->mag);
    free (t->mag0);
    free (t->win);
    t->re = t->im = t->mag = NULL;
    t->win = NULL;
}

/* Peak magnitude near bin x (parabolic-refined), or 0 outside range. */
static double mag_near (const double *mag, int nbins, double bin)
{
    const int b = (int) (bin + 0.5);
    if (b < 1 || b >= nbins - 1)
        return 0.0;
    double m = mag[b];
    if (mag[b - 1] > m) m = mag[b - 1];
    if (mag[b + 1] > m) m = mag[b + 1];
    return m;
}

/* Harmonic-sum salience of candidate f0 against the (residual) spectrum.
   1/k weighting keeps a strong low partial from being outvoted by a comb
   of overtones an octave up; the half-harmonic PENALTY is the octave
   guard: a candidate one octave HIGH leaves energy stranded at 0.5f,
   1.5f, ... which a true fundamental would own. */
static double salience (const AePolyF0 *t, const double *mag, double hz)
{
    const int nbins = t->fft_size / 2;
    const double bin_hz = t->fs / t->fft_size;
    double s = 0.0, strongest = 0.0;
    const double fund = mag_near (mag, nbins, hz / bin_hz);
    for (int k = 1; k <= 8; ++k)
    {
        const double f = hz * k;
        if (f > t->fs * 0.45)
            break;
        const double m = mag_near (mag, nbins, f / bin_hz);
        if (m > strongest) strongest = m;
        s += m / (double) k;
    }
    /* The MISSING-FUNDAMENTAL guard: a harmonic sum happily scores a
       major triad as its chord root an octave below every played note --
       the virtual pitch the ear also hears -- but a chord SAMPLER must
       strike what was played. A candidate whose own fundamental bin is
       nearly empty is a virtual pitch, not a note. */
    if (fund < 0.15 * strongest)
        return 0.0;
    for (int k = 1; k <= 3; ++k)
    {
        const double f = hz * (k - 0.5);
        if (f > t->fs * 0.45)
            break;
        s -= 0.7 * mag_near (mag, nbins, f / bin_hz) / (double) k;
    }
    return s;
}

/* Subtract candidate f0's harmonic comb from the residual (not fully to
   zero -- shared partials between chord tones must survive for the
   second tone's own score). */
static void subtract_comb (const AePolyF0 *t, double *mag, double hz)
{
    const int nbins = t->fft_size / 2;
    const double bin_hz = t->fs / t->fft_size;
    for (int k = 1; k <= 10; ++k)
    {
        const double f = hz * k;
        if (f > t->fs * 0.45)
            break;
        /* Width covers the zero-padded Hann main lobe (4 unpadded bins =
           8 padded ones); counted in bins it silently halved when the
           transform grew, and the surviving lobe skirts came back as
           ghost notes. */
        const int b = (int) (f / bin_hz + 0.5);
        const int w = 2 * t->fft_size / t->win_size; /* +-2 unpadded bins */
        for (int d = -w; d <= w; ++d)
            if (b + d >= 0 && b + d < nbins)
                mag[b + d] *= 0.12;
    }
}

/* Refine a coarse f0 against its own partials: each of the first four
   gets a LOG-magnitude parabolic peak (near-exact for a Hann main lobe,
   where a linear-magnitude parabola is biased by half-bin-scale errors --
   which a 54.5-cent EDO grid downstream cannot absorb), the implied
   fundamentals are averaged weighted by partial magnitude, and a partial
   whose implied f0 disagrees by more than a quarter tone is someone
   else's partial and gets no vote. */
static double refine_f0 (const AePolyF0 *t, const double *mag, double hz)
{
    const double bin_hz = t->fs / t->fft_size;
    double sum = 0.0, wsum = 0.0;
    for (int k = 1; k <= 4; ++k)
    {
        const int b = (int) (hz * k / bin_hz + 0.5);
        if (b < 2 || b >= t->fft_size / 2 - 2)
            continue;
        int pb = b;
        for (int d = -1; d <= 1; ++d)
            if (mag[b + d] > mag[pb])
                pb = b + d;
        if (mag[pb] <= 0.0 || mag[pb - 1] <= 0.0 || mag[pb + 1] <= 0.0)
            continue;
        const double y0 = log (mag[pb - 1]), y1 = log (mag[pb]),
                     y2 = log (mag[pb + 1]);
        const double den = y0 - 2.0 * y1 + y2;
        const double off = fabs (den) > 1e-12 ? 0.5 * (y0 - y2) / den : 0.0;
        if (fabs (off) > 0.6)
            continue; /* not a clean peak */
        const double f0k = (pb + off) * bin_hz / (double) k;
        if (fabs (1200.0 * log2 (f0k / hz)) > 50.0)
            continue; /* another note's partial */
        /* the fundamental's own vote dominates: higher partials are the
           ones a subset tone (an octave or twelfth above) contaminates */
        const double w = mag[pb] / (double) (k * k);
        sum  += f0k * w;
        wsum += w;
    }
    return wsum > 0.0 ? sum / wsum : hz;
}

void ae_polyf0_process (AePolyF0 *t, const float *frame)
{
    if (t->re == NULL || frame == NULL)
        return;

    const int n = t->fft_size;
    double power = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const double v = i < t->win_size
                             ? (double) frame[i] * t->win[i] : 0.0;
        t->re[i] = v;
        t->im[i] = 0.0;
        power += v * v;
    }
    pf_fft (t->re, t->im, n);
    const int nbins = n / 2;
    for (int b = 0; b < nbins; ++b)
    {
        t->mag[b]  = sqrt (t->re[b] * t->re[b] + t->im[b] * t->im[b]);
        t->mag0[b] = t->mag[b]; /* refinement reads the PRISTINE spectrum:
                                   subtraction dents the residual's peak
                                   shapes, and a dented peak refines flat
                                   (measured 29 cents on a triad's E3 --
                                   more than half a 22-EDO step) */
    }

    /* Frame estimates: iterative pick-and-subtract over a log-spaced
       candidate grid (~4 per semitone), gated on absolute energy so a
       silent input yields no notes rather than noise-floor ghosts. */
    int    n_cand = 0;
    double cand_hz[AE_POLY_MAX_NOTES], cand_lv[AE_POLY_MAX_NOTES];
    const double gate = 1e-4 * t->win_size; /* window power gate */
    if (power > gate)
    {
        /* Iterative pick-and-subtract over a log grid (~4 candidates per
           semitone). Two formulations were measured and rejected: joint
           selection without subtraction (salience-curve local maxima are
           lumpy pseudo-peaks -- six "notes" for a triad), and subtraction
           with the comb width counted in TRANSFORM bins (it silently
           halved when zero-padding doubled the FFT, and lobe skirts came
           back as ghosts). The width below is the Hann main lobe in
           UNPADDED bins, scaled to the transform. */
        double first_sal = 0.0;
        for (int pick = 0; pick < t->max_notes; ++pick)
        {
            double best_hz = 0.0, best_s = 0.0;
            const double step = pow (2.0, 1.0 / 48.0);
            for (double hz = t->min_hz; hz <= t->max_hz; hz *= step)
            {
                const double s2 = salience (t, t->mag, hz);
                if (s2 > best_s)
                {
                    best_s  = s2;
                    best_hz = hz;
                }
            }
            if (best_hz <= 0.0)
                break;
            if (pick == 0)
                first_sal = best_s;
            else if (best_s < 0.20 * first_sal)
                break; /* residual notes must hold their own */
            const double hz = refine_f0 (t, t->mag0, best_hz);
            bool dup = false;
            for (int c = 0; c < n_cand; ++c)
                if (fabs (1200.0 * log2 (hz / cand_hz[c])) < 70.0)
                    dup = true;
            if (! dup && n_cand < AE_POLY_MAX_NOTES)
            {
                cand_hz[n_cand] = hz;
                cand_lv[n_cand] = best_s;
                ++n_cand;
            }
            subtract_comb (t, t->mag, best_hz);
        }
        double top = 0.0;
        for (int c = 0; c < n_cand; ++c)
            if (cand_lv[c] > top) top = cand_lv[c];
        if (top > 0.0)
            for (int c = 0; c < n_cand; ++c)
                cand_lv[c] /= top;
    }

    /* Tracker: match candidates to living notes by pitch (within ~80 c),
       update matched, count misses on the rest; births need two
       consecutive sightings (one glitchy frame is not a note), deaths
       need four consecutive misses (one is not a release). */
    bool used[AE_POLY_MAX_NOTES] = { false };
    for (int k = 0; k < AE_POLY_MAX_NOTES; ++k)
    {
        if (! t->notes[k].active)
            continue;
        int    best = -1;
        double best_c = 80.0;
        for (int c = 0; c < n_cand; ++c)
        {
            if (used[c])
                continue;
            const double cents = fabs (1200.0 * log2 (cand_hz[c] / t->notes[k].hz));
            if (cents < best_c)
            {
                best_c = cents;
                best   = c;
            }
        }
        if (best >= 0)
        {
            used[best] = true;
            /* smooth the pitch a little; bends still track */
            t->notes[k].hz += (cand_hz[best] - t->notes[k].hz) * 0.5;
            t->notes[k].level = cand_lv[best];
            t->miss[k] = 0;
        }
        else if (++t->miss[k] >= 4)
        {
            t->notes[k].active = false;
            t->notes[k].id     = -1;
        }
    }
    /* births: unmatched candidates against pending slots */
    for (int c = 0; c < n_cand; ++c)
    {
        if (used[c])
            continue;
        /* find a pending slot already watching this pitch */
        int slot = -1;
        for (int k = 0; k < AE_POLY_MAX_NOTES; ++k)
            if (! t->notes[k].active && t->notes[k].id == -1 && t->seen[k] > 0
                && fabs (1200.0 * log2 (cand_hz[c] / t->cand_hz[k])) < 80.0)
            { slot = k; break; }
        if (slot < 0)
            for (int k = 0; k < AE_POLY_MAX_NOTES; ++k)
                if (! t->notes[k].active && t->notes[k].id == -1
                    && t->seen[k] == 0)
                { slot = k; break; }
        if (slot < 0)
            continue;
        t->cand_hz[slot] = cand_hz[c];
        t->cand_lv[slot] = cand_lv[c];
        if (++t->seen[slot] >= 2)
        {
            t->notes[slot].active = true;
            t->notes[slot].hz     = cand_hz[c];
            t->notes[slot].level  = cand_lv[c];
            t->notes[slot].id     = t->next_id++;
            t->miss[slot] = 0;
            t->seen[slot] = 0;
        }
    }
    /* pending slots not refreshed this frame reset */
    for (int k = 0; k < AE_POLY_MAX_NOTES; ++k)
        if (! t->notes[k].active)
        {
            bool refreshed = false;
            for (int c = 0; c < n_cand; ++c)
                if (t->seen[k] > 0
                    && fabs (1200.0 * log2 (cand_hz[c] / t->cand_hz[k])) < 80.0)
                    refreshed = true;
            if (! refreshed && t->seen[k] > 0)
                t->seen[k] = 0;
        }
}
