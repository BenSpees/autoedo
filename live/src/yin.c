#include "yin.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static int imax (int a, int b) { return a > b ? a : b; }
static int imin (int a, int b) { return a < b ? a : b; }

/* Iterative in-place radix-2 FFT; n must be a power of two. */
static void yin_fft (double *re, double *im, int n, int inverse)
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
        const double ang = 2.0 * 3.14159265358979323846 / (double) len * (inverse ? 1.0 : -1.0);
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

    if (inverse)
        for (int i = 0; i < n; ++i)
        {
            re[i] /= (double) n;
            im[i] /= (double) n;
        }
}

/* d(tau) = sum_j (x[j] - x[j+tau])^2 = P(0) + P(tau) - 2 r(tau), with
   P(tau) = sum_j x[j+tau]^2 and r(tau) = sum_j x[j] x[j+tau]. r comes from one
   FFT cross-correlation; P slides in constant time. The transform is at least
   window + tau_max long, which keeps every lag in [0, tau_max] clear of
   circular wrap-around. */
static void yin_difference (AeYin *y, const float *x)
{
    const int n = y->fft_size;

    for (int i = 0; i < n; ++i)
    {
        y->a_re[i] = (i < y->window) ? (double) x[i] : 0.0;
        y->b_re[i] = (i < y->window + y->tau_max) ? (double) x[i] : 0.0;
        y->a_im[i] = 0.0;
        y->b_im[i] = 0.0;
    }

    yin_fft (y->a_re, y->a_im, n, 0);
    yin_fft (y->b_re, y->b_im, n, 0);

    for (int i = 0; i < n; ++i) /* conj(A) * B */
    {
        const double re = y->a_re[i] * y->b_re[i] + y->a_im[i] * y->b_im[i];
        const double im = y->a_re[i] * y->b_im[i] - y->a_im[i] * y->b_re[i];
        y->a_re[i] = re;
        y->a_im[i] = im;
    }

    yin_fft (y->a_re, y->a_im, n, 1);

    double power = 0.0;
    for (int j = 0; j < y->window; ++j)
        power += (double) x[j] * (double) x[j];
    const double power0 = power;

    for (int tau = 0; tau < y->tau_max; ++tau)
    {
        if (tau > 0)
        {
            const double leaving  = (double) x[tau - 1] * (double) x[tau - 1];
            const double entering = (double) x[tau + y->window - 1]
                                  * (double) x[tau + y->window - 1];
            power += entering - leaving;
        }

        const double d = power0 + power - 2.0 * y->a_re[tau];
        y->diff[tau] = d > 0.0 ? d : 0.0; /* cancellation can go a hair negative */
    }
}

void ae_yin_prepare (AeYin *y, double sample_rate, int frame_size,
                     double min_frequency, double max_frequency)
{
    y->sample_rate = sample_rate > 0.0 ? sample_rate : 44100.0;
    y->frame_size  = imax (64, frame_size);
    if (y->threshold <= 0.0)
        y->threshold = 0.12;

    /* Longest period we look for is bounded by both the requested minimum
       frequency and half the frame (we need x[j] and x[j + tau] inside it). */
    y->tau_max = imin (y->frame_size / 2,
                       (int) ceil (y->sample_rate / (min_frequency > 1.0 ? min_frequency : 1.0)));
    y->tau_max = imax (y->tau_max, 2);

    y->tau_min = imax (2, (int) floor (y->sample_rate / (max_frequency > 1.0 ? max_frequency : 1.0)));
    y->tau_min = imin (y->tau_min, y->tau_max - 1);

    y->window = y->frame_size - y->tau_max;
    y->window = imax (y->window, y->tau_max); /* keep a healthy amount of overlap data */
    y->last_best_tau = 0;

    y->fft_size = 1;
    while (y->fft_size < y->window + y->tau_max)
        y->fft_size <<= 1;

    ae_yin_free (y);
    y->diff       = calloc ((size_t) y->tau_max, sizeof (double));
    y->cumulative = calloc ((size_t) y->tau_max, sizeof (double));
    y->a_re       = calloc ((size_t) y->fft_size, sizeof (double));
    y->a_im       = calloc ((size_t) y->fft_size, sizeof (double));
    y->b_re       = calloc ((size_t) y->fft_size, sizeof (double));
    y->b_im       = calloc ((size_t) y->fft_size, sizeof (double));
}

void ae_yin_free (AeYin *y)
{
    free (y->diff);
    free (y->cumulative);
    free (y->a_re);
    free (y->a_im);
    free (y->b_re);
    free (y->b_im);
    y->diff = y->cumulative = NULL;
    y->a_re = y->a_im = y->b_re = y->b_im = NULL;
}

AeYinResult ae_yin_process (AeYin *y, const float *frame, int num_samples)
{
    AeYinResult result = { 0.0, 0.0, false };

    if (frame == NULL || y->diff == NULL || y->a_re == NULL
        || num_samples < y->window + y->tau_max)
        return result;

    /* Step 1: difference function d(tau). */
    yin_difference (y, frame);

    /* Step 2: cumulative mean normalised difference d'(tau). */
    y->cumulative[0] = 1.0;
    double running = 0.0;
    for (int tau = 1; tau < y->tau_max; ++tau)
    {
        running += y->diff[tau];
        y->cumulative[tau] = running > 0.0 ? y->diff[tau] * tau / running : 1.0;
    }

    /* Step 3: absolute threshold — first local minimum below the threshold. */
    int best_tau = -1;
    for (int tau = y->tau_min; tau < y->tau_max - 1; ++tau)
    {
        if (y->cumulative[tau] < y->threshold)
        {
            /* Descend to the bottom of this dip. */
            while (tau + 1 < y->tau_max && y->cumulative[tau + 1] < y->cumulative[tau])
                ++tau;

            best_tau = tau;
            break;
        }
    }

    /* Fall back to the global minimum if nothing crossed the threshold. */
    if (best_tau < 0)
    {
        double min_val = y->cumulative[y->tau_min];
        best_tau = y->tau_min;
        for (int tau = y->tau_min + 1; tau < y->tau_max; ++tau)
        {
            if (y->cumulative[tau] < min_val)
            {
                min_val  = y->cumulative[tau];
                best_tau = tau;
            }
        }
    }

    /* Voicing is decided by the dip the selection above found, before the
       guard below moves the lag: the guard chooses which octave the note is
       in, it does not get a vote on whether there is a note at all. */
    const double d_voiced = y->cumulative[best_tau];

    /* Step 3b: octave guard. Both selections above can land an octave (or a
       twelfth) low. A plucked string is rich enough that d'(tau) at the true
       period sometimes sits just above the threshold while 2*tau -- which is
       also a period, just not the shortest one -- dips below it; the first
       crossing is then the subharmonic, and the fallback's global minimum
       prefers the longer lag for the same reason. Both make a guitar
       suddenly an octave down.

       So walk the submultiples smallest-lag-first and take the first one
       that is a genuine dip of comparable quality (periodicity within 90% of
       the best). "Comparable" is the important part: a real octave error and
       its true fundamental are near-equal dips, whereas the tau/2 of a note
       that really is low is a shallow nothing and fails the test. */
    {
        const double q_best = 1.0 - d_voiced;
        for (int k = 4; k >= 2; --k)
        {
            int c = best_tau / k;
            if (c < y->tau_min || c >= y->tau_max - 1)
                continue;
            /* Snap to the bottom of whatever dip sits near tau/k -- the
               submultiple of an interpolated lag is rarely exactly on it. */
            const int slack = imax (1, c / 16);
            const int lo = imax (y->tau_min, c - slack);
            const int hi = imin (y->tau_max - 2, c + slack);
            for (int t = lo; t <= hi; ++t)
                if (y->cumulative[t] < y->cumulative[c])
                    c = t;
            /* A local minimum, not a point on a slope. */
            if (y->cumulative[c] > y->cumulative[c - 1]
                || y->cumulative[c] > y->cumulative[c + 1])
                continue;
            /* Octave continuity: moving to a lag that matches the octave
               we reported LAST frame needs only comparable quality (0.90);
               CHANGING octave against the running note must be clearly
               better (0.95). This is what keeps the guard from flip-
               flopping mid-note on marginal dips -- each flip used to cost
               the corrector an equave-sized re-vote. */
            const bool matches_last = y->last_best_tau > 0
                && abs (c - y->last_best_tau) <= y->last_best_tau / 8;
            /* A CLEARED history is the least reliable frame there is --
               the pluck itself, where a guitar's second harmonic is often
               the loudest thing in the frame -- so it must meet the same
               clearly-better bar an octave CHANGE does, not the lax one.
               Letting the fresh-note case in at 0.90 is how a note started
               an octave high and stayed there (a genuine subharmonic fix
               dips near-equal, ~0.98, and still passes 0.95). */
            const double need = matches_last ? 0.90 : 0.95;
            if (1.0 - y->cumulative[c] >= need * q_best)
            {
                best_tau = c;
                break;
            }
        }
    }

    /* Step 4: parabolic interpolation around best_tau for sub-sample accuracy. */
    double better_tau = (double) best_tau;
    if (best_tau > 0 && best_tau < y->tau_max - 1)
    {
        const double s0 = y->cumulative[best_tau - 1];
        const double s1 = y->cumulative[best_tau];
        const double s2 = y->cumulative[best_tau + 1];
        const double denom = 2.0 * s1 - s2 - s0;
        if (fabs (denom) > 1e-12)
            better_tau = best_tau + 0.5 * (s2 - s0) / denom;
    }

    if (better_tau <= 0.0)
        return result;

    double p = 1.0 - d_voiced;
    if (p < 0.0) p = 0.0;
    if (p > 1.0) p = 1.0;

    result.frequency_hz = y->sample_rate / better_tau;
    result.periodicity  = p;
    result.voiced       = d_voiced < y->threshold;
    y->last_best_tau = result.voiced ? best_tau : 0;
    return result;
}
