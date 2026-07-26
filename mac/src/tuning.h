/* EDO (Equal Divisions of the Octave) tuning math — C port of
   Source/dsp/Tuning.h (see that file for the full design notes).

   C is the reference note, anchored at standard-tuning C0
   (16.3515978... Hz, derived from A4 = 440 Hz). The set of allowed
   pitches for an N-EDO scale is

       f(j) = C0 * 2^(j / N)     for every integer j

   Because every octave of C is an exact power-of-two multiple of the
   anchor, all C's keep their standard-tuning frequencies for any N, and
   N = 12 reproduces 12-TET exactly (A4 = 440 Hz). */

#ifndef AUTOEDO_TUNING_H
#define AUTOEDO_TUNING_H

#include <math.h>
#include <stdbool.h>
#include <stddef.h>

#define AE_REFERENCE_C0_HZ 16.351597831287414
#define AE_MIN_EDO 10
#define AE_MAX_EDO 72

typedef struct
{
    double target_hz; /* frequency of the nearest pitch on the EDO grid */
    int    degree;    /* signed EDO degree index j relative to C0 */
    double cents_off; /* cents the input sits above(+)/below(-) the target */
    bool   valid;
} AeTuningResult;

/* Continuous position of a frequency on the EDO grid, in (fractional) steps
   from the reference. `period_cents` is the size of the repeating interval
   (1200 = a true octave; other values give stretched/compressed octaves). */
static inline double ae_steps_from_ref (double input_hz, int edo,
                                        double ref_hz, double period_cents)
{
    return (double) edo * (1200.0 * log2 (input_hz / ref_hz)) / period_cents;
}

/* Frequency of EDO degree j (signed, relative to the reference). */
static inline double ae_degree_hz (long j, int edo, double ref_hz, double period_cents)
{
    return ref_hz * pow (2.0, (double) j * period_cents / (1200.0 * (double) edo));
}

/* Quantise an input frequency to the nearest *enabled* pitch on an N-EDO
   grid anchored at `ref_hz` (degree 0 == the root). `enabled` is a mask of
   length >= edo; pass NULL to allow every degree. A mask that disables every
   degree is treated as "all enabled" (no snapping to an empty scale). */
static inline AeTuningResult ae_quantize_to_edo_scale_ex (double input_hz, int edo,
                                                          const bool *enabled,
                                                          double ref_hz,
                                                          double period_cents)
{
    AeTuningResult r = { 0.0, 0, 0.0, false };

    if (input_hz <= 0.0 || edo < 1 || ref_hz <= 0.0 || period_cents <= 0.0)
        return r;

    /* Continuous position of the input on the EDO grid, in steps from root. */
    const double steps_from_c = ae_steps_from_ref (input_hz, edo, ref_hz, period_cents);
    const long   nearest      = lround (steps_from_c);

    bool any_enabled = false;
    if (enabled != NULL)
        for (int d = 0; d < edo; ++d)
            if (enabled[d]) { any_enabled = true; break; }

    long best = nearest;
    if (enabled != NULL && any_enabled)
    {
        /* Search out to one octave each way for the closest enabled degree. */
        bool   found     = false;
        double best_dist = 0.0;
        for (long j = nearest - edo; j <= nearest + edo; ++j)
        {
            long d = j % edo;
            if (d < 0) d += edo;
            if (! enabled[d])
                continue;

            const double dist = fabs (steps_from_c - (double) j);
            if (! found || dist < best_dist)
            {
                found     = true;
                best_dist = dist;
                best      = j;
            }
        }
    }

    r.degree    = (int) best;
    r.target_hz = ae_degree_hz (best, edo, ref_hz, period_cents);
    r.cents_off = 1200.0 * log2 (input_hz / r.target_hz);
    r.valid     = true;
    return r;
}

/* Convenience: standard C-anchored, true-octave grid. */
static inline AeTuningResult ae_quantize_to_edo_scale (double input_hz, int edo,
                                                       const bool *enabled)
{
    return ae_quantize_to_edo_scale_ex (input_hz, edo, enabled,
                                        AE_REFERENCE_C0_HZ, 1200.0);
}

/* Width of a single EDO step, in cents (period / N). */
static inline double ae_edo_step_cents_ex (int edo, double period_cents)
{
    return edo > 0 ? period_cents / (double) edo : 0.0;
}

static inline double ae_edo_step_cents (int edo)
{
    return ae_edo_step_cents_ex (edo, 1200.0);
}

#endif /* AUTOEDO_TUNING_H */
