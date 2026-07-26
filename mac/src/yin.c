#include "yin.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static int imax (int a, int b) { return a > b ? a : b; }
static int imin (int a, int b) { return a < b ? a : b; }

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

    free (y->diff);
    free (y->cumulative);
    y->diff       = calloc ((size_t) y->tau_max, sizeof (double));
    y->cumulative = calloc ((size_t) y->tau_max, sizeof (double));
}

void ae_yin_free (AeYin *y)
{
    free (y->diff);
    free (y->cumulative);
    y->diff = y->cumulative = NULL;
}

AeYinResult ae_yin_process (AeYin *y, const float *frame, int num_samples)
{
    AeYinResult result = { 0.0, 0.0, false };

    if (frame == NULL || y->diff == NULL || num_samples < y->window + y->tau_max)
        return result;

    /* Step 1: difference function d(tau). */
    for (int tau = 0; tau < y->tau_max; ++tau)
    {
        double sum = 0.0;
        for (int j = 0; j < y->window; ++j)
        {
            const double delta = (double) frame[j] - (double) frame[j + tau];
            sum += delta * delta;
        }
        y->diff[tau] = sum;
    }

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

    double p = 1.0 - y->cumulative[best_tau];
    if (p < 0.0) p = 0.0;
    if (p > 1.0) p = 1.0;

    result.frequency_hz = y->sample_rate / better_tau;
    result.periodicity  = p;
    result.voiced       = y->cumulative[best_tau] < y->threshold;
    return result;
}
