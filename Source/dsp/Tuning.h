#pragma once

#include <cmath>

namespace autoedo
{
/**
    EDO (Equal Divisions of the Octave) tuning math.

    Phase 1 design: C is the reference note, fixed at its standard-tuning
    frequency, and every other pitch is derived relative to that anchor.

    The set of allowed pitches for an N-EDO scale is:

        f(j) = C_anchor * 2^(j / N)        for all integers j

    where @c C_anchor is a reference C frequency. We use C0 (the lowest C in
    the usual MIDI octave numbering, where middle C = C4), whose standard-tuning
    frequency is derived from A4 = 440 Hz:

        C0 = 440 * 2^((12 - 69) / 12) = 16.3515978... Hz

    Because every octave of C is an exact power-of-two multiple of this anchor,
    *all* C's land on their standard-tuning frequencies for any N. At N = 12 the
    whole grid reproduces standard 12-TET exactly (including A4 = 440 Hz); other
    values of N subdivide the octave differently while keeping every C fixed.
*/

/** C0 in standard tuning (A4 = 440 Hz). This is the pitch anchor. */
constexpr double kReferenceC0Hz = 16.351597831287414;

/** Inclusive range of EDO values supported by the plugin (per the spec). */
constexpr int kMinEdo = 10;
constexpr int kMaxEdo = 72;

struct TuningResult
{
    double targetHz = 0.0; ///< Frequency of the nearest pitch on the EDO grid.
    int    degree   = 0;   ///< Signed EDO degree index j relative to C0.
    double centsOff = 0.0; ///< Cents the input sits above(+)/below(-) the target.
    bool   valid    = false;
};

/** Quantise an input frequency to the nearest *enabled* pitch on an N-EDO grid
    anchored at C.

    @param inputHz     detected fundamental frequency, in Hz (> 0)
    @param edo         number of equal divisions of the octave (>= 1)
    @param enabled     optional mask of length >= @c edo; @c enabled[d] is true if
                       scale degree @c d (0..edo-1, where 0 == C) may be tuned to.
                       Pass @c nullptr to allow every degree. If the mask disables
                       every degree, it is treated as "all enabled" (no snapping
                       to an empty scale).
    @param referenceC  reference C frequency; defaults to standard-tuning C0
*/
inline TuningResult quantizeToEdoScale (double inputHz, int edo, const bool* enabled,
                                        double referenceC = kReferenceC0Hz)
{
    TuningResult r;

    if (inputHz <= 0.0 || edo < 1 || referenceC <= 0.0)
        return r;

    // Continuous position of the input on the EDO grid, in steps from C.
    const double stepsFromC = static_cast<double> (edo) * std::log2 (inputHz / referenceC);
    const long   nearest    = std::lround (stepsFromC);

    auto degreeOf = [edo] (long j)
    {
        long d = j % edo;
        if (d < 0) d += edo;
        return static_cast<int> (d);
    };

    bool anyEnabled = false;
    if (enabled != nullptr)
        for (int d = 0; d < edo; ++d)
            if (enabled[d]) { anyEnabled = true; break; }

    long best = nearest;
    if (enabled != nullptr && anyEnabled)
    {
        // Search out to one octave each way for the closest enabled degree.
        bool   found    = false;
        double bestDist = 0.0;
        for (long j = nearest - edo; j <= nearest + edo; ++j)
        {
            if (! enabled[degreeOf (j)])
                continue;

            const double dist = std::abs (stepsFromC - static_cast<double> (j));
            if (! found || dist < bestDist)
            {
                found    = true;
                bestDist = dist;
                best     = j;
            }
        }
    }

    r.degree   = static_cast<int> (best);
    r.targetHz = referenceC * std::pow (2.0, static_cast<double> (best)
                                              / static_cast<double> (edo));
    r.centsOff = 1200.0 * std::log2 (inputHz / r.targetHz);
    r.valid    = true;
    return r;
}

/** Quantise to the nearest pitch using every degree (full chromatic EDO). */
inline TuningResult quantizeToEdo (double inputHz, int edo,
                                   double referenceC = kReferenceC0Hz)
{
    return quantizeToEdoScale (inputHz, edo, nullptr, referenceC);
}

/** Width of a single EDO step, in cents (1200 / N). Useful for UI / metering. */
inline double edoStepCents (int edo)
{
    return edo > 0 ? 1200.0 / static_cast<double> (edo) : 0.0;
}

} // namespace autoedo
