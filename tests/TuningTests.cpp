// Standalone unit tests for the EDO tuning math. No framework / no JUCE.
#include "../Source/dsp/Tuning.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace
{
int failures = 0;

void check (bool cond, const std::string& what)
{
    if (! cond)
    {
        std::printf ("  FAIL: %s\n", what.c_str());
        ++failures;
    }
}

void checkClose (double a, double b, double tol, const std::string& what)
{
    if (std::fabs (a - b) > tol)
    {
        std::printf ("  FAIL: %s  (got %.6f, expected %.6f, tol %.6f)\n",
                     what.c_str(), a, b, tol);
        ++failures;
    }
}
} // namespace

using namespace autoedo;

int main()
{
    std::printf ("TuningTests\n");

    // --- Reference invariant: every C is exact for any EDO --------------------
    for (int edo = kMinEdo; edo <= kMaxEdo; ++edo)
    {
        for (int oct = 0; oct <= 8; ++oct)
        {
            const double cHz = kReferenceC0Hz * std::pow (2.0, oct); // C in octave `oct`
            const auto   r   = quantizeToEdo (cHz, edo);
            checkClose (r.targetHz, cHz, 1.0e-6,
                        "C maps to itself, edo=" + std::to_string (edo)
                        + " oct=" + std::to_string (oct));
            checkClose (r.centsOff, 0.0, 1.0e-6, "C cents off == 0");
            check (r.degree == oct * edo, "C degree == oct*edo");
        }
    }

    // --- 12-EDO reproduces standard tuning: A4 == 440 ------------------------
    {
        const auto r = quantizeToEdo (440.0, 12);
        checkClose (r.targetHz, 440.0, 1.0e-6, "12-EDO A4 == 440");
        check (r.degree == 57, "A4 is 57 semitones above C0");
        checkClose (r.centsOff, 0.0, 1.0e-6, "A4 cents off == 0");
    }

    // --- Snapping: a sharp input rounds to the nearest grid pitch ------------
    {
        const double sharp = 440.0 * std::pow (2.0, 20.0 / 1200.0); // 20 cents sharp
        const auto   r     = quantizeToEdo (sharp, 12);
        checkClose (r.targetHz, 440.0, 1.0e-6, "20-cent-sharp A snaps to 440");
        checkClose (r.centsOff, 20.0, 1.0e-4, "centsOff reports +20");
    }
    {
        const double flat = 261.6255653 * std::pow (2.0, -15.0 / 1200.0); // 15 cents flat of C4
        const auto   r    = quantizeToEdo (flat, 12);
        checkClose (r.targetHz, 261.6255653, 1.0e-3, "15-cent-flat C snaps to C4");
        checkClose (r.centsOff, -15.0, 1.0e-3, "centsOff reports -15");
    }

    // --- 24-EDO has a grid point exactly a quarter-tone (50c) above C --------
    {
        const double quarter = kReferenceC0Hz * std::pow (2.0, 4.0) // C4
                                              * std::pow (2.0, 50.0 / 1200.0);
        const auto r = quantizeToEdo (quarter, 24);
        checkClose (r.centsOff, 0.0, 1.0e-6, "24-EDO quarter-tone is on-grid");
        check (r.degree == 4 * 24 + 1, "24-EDO quarter-tone degree");
    }

    // --- General property: result is always within half a step --------------
    for (int edo = kMinEdo; edo <= kMaxEdo; ++edo)
    {
        const double halfStep = edoStepCents (edo) / 2.0 + 1.0e-6;
        for (int k = 0; k < 200; ++k)
        {
            const double hz = 80.0 * std::pow (2.0, 4.0 * k / 200.0); // 80 Hz .. 1280 Hz
            const auto   r  = quantizeToEdo (hz, edo);
            check (std::fabs (r.centsOff) <= halfStep,
                   "within half a step, edo=" + std::to_string (edo));
            check (r.targetHz > 0.0, "target positive");
        }
    }

    // --- Guards --------------------------------------------------------------
    check (! quantizeToEdo (-1.0, 12).valid, "negative freq is invalid");
    check (! quantizeToEdo (440.0, 0).valid, "zero edo is invalid");

    if (failures == 0)
        std::printf ("  all tuning tests passed\n");
    else
        std::printf ("  %d tuning test(s) FAILED\n", failures);

    return failures == 0 ? 0 : 1;
}
