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

/* Quantise an input frequency to the nearest *enabled* pitch on an N-EDO
   grid anchored at C. `enabled` is a mask of length >= edo (degree 0 == C);
   pass NULL to allow every degree. A mask that disables every degree is
   treated as "all enabled" (no snapping to an empty scale). */
static inline AeTuningResult ae_quantize_to_edo_scale (double input_hz, int edo,
                                                       const bool *enabled)
{
    AeTuningResult r = { 0.0, 0, 0.0, false };

    if (input_hz <= 0.0 || edo < 1)
        return r;

    /* Continuous position of the input on the EDO grid, in steps from C. */
    const double steps_from_c = (double) edo * log2 (input_hz / AE_REFERENCE_C0_HZ);
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
    r.target_hz = AE_REFERENCE_C0_HZ * pow (2.0, (double) best / (double) edo);
    r.cents_off = 1200.0 * log2 (input_hz / r.target_hz);
    r.valid     = true;
    return r;
}

/* Width of a single EDO step, in cents (1200 / N). */
static inline double ae_edo_step_cents (int edo)
{
    return edo > 0 ? 1200.0 / (double) edo : 0.0;
}

#endif /* AUTOEDO_TUNING_H */
