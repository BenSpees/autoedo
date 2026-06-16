#include "YinPitchDetector.h"

#include <algorithm>
#include <cmath>

namespace autoedo
{
void YinPitchDetector::prepare (double newSampleRate, int newFrameSize,
                                double minFrequency, double maxFrequency)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
    frameSize  = std::max (64, newFrameSize);

    // Longest period we look for is bounded by both the requested minimum
    // frequency and half the frame (we need x[j] and x[j + tau] inside the frame).
    tauMax = std::min (frameSize / 2,
                       static_cast<int> (std::ceil (sampleRate / std::max (1.0, minFrequency))));
    tauMax = std::max (tauMax, 2);

    tauMin = std::max (2, static_cast<int> (std::floor (sampleRate / std::max (1.0, maxFrequency))));
    tauMin = std::min (tauMin, tauMax - 1);

    window = frameSize - tauMax; // integration window length
    window = std::max (window, tauMax); // keep a healthy amount of overlap data

    diff.assign (static_cast<size_t> (tauMax), 0.0);
    cumulative.assign (static_cast<size_t> (tauMax), 0.0);
}

YinPitchDetector::Result YinPitchDetector::process (const float* frame, int numSamples)
{
    Result result;

    if (frame == nullptr || numSamples < window + tauMax)
        return result;

    // Step 1: difference function d(tau).
    for (int tau = 0; tau < tauMax; ++tau)
    {
        double sum = 0.0;
        for (int j = 0; j < window; ++j)
        {
            const double delta = static_cast<double> (frame[j]) - static_cast<double> (frame[j + tau]);
            sum += delta * delta;
        }
        diff[static_cast<size_t> (tau)] = sum;
    }

    // Step 2: cumulative mean normalised difference d'(tau).
    cumulative[0] = 1.0;
    double running = 0.0;
    for (int tau = 1; tau < tauMax; ++tau)
    {
        running += diff[static_cast<size_t> (tau)];
        cumulative[static_cast<size_t> (tau)] =
            running > 0.0 ? diff[static_cast<size_t> (tau)] * tau / running : 1.0;
    }

    // Step 3: absolute threshold — first local minimum below the threshold.
    int bestTau = -1;
    for (int tau = tauMin; tau < tauMax - 1; ++tau)
    {
        if (cumulative[static_cast<size_t> (tau)] < threshold)
        {
            // Descend to the bottom of this dip.
            while (tau + 1 < tauMax
                   && cumulative[static_cast<size_t> (tau + 1)] < cumulative[static_cast<size_t> (tau)])
                ++tau;

            bestTau = tau;
            break;
        }
    }

    // Fall back to the global minimum if nothing crossed the threshold.
    if (bestTau < 0)
    {
        double minVal = cumulative[static_cast<size_t> (tauMin)];
        bestTau = tauMin;
        for (int tau = tauMin + 1; tau < tauMax; ++tau)
        {
            if (cumulative[static_cast<size_t> (tau)] < minVal)
            {
                minVal  = cumulative[static_cast<size_t> (tau)];
                bestTau = tau;
            }
        }
    }

    // Step 4: parabolic interpolation around bestTau for sub-sample accuracy.
    double betterTau = static_cast<double> (bestTau);
    if (bestTau > 0 && bestTau < tauMax - 1)
    {
        const double s0 = cumulative[static_cast<size_t> (bestTau - 1)];
        const double s1 = cumulative[static_cast<size_t> (bestTau)];
        const double s2 = cumulative[static_cast<size_t> (bestTau + 1)];
        const double denom = (2.0 * s1 - s2 - s0);
        if (std::abs (denom) > 1e-12)
            betterTau = bestTau + 0.5 * (s2 - s0) / denom;
    }

    if (betterTau <= 0.0)
        return result;

    result.frequencyHz = sampleRate / betterTau;
    result.periodicity = std::clamp (1.0 - cumulative[static_cast<size_t> (bestTau)], 0.0, 1.0);
    result.voiced      = cumulative[static_cast<size_t> (bestTau)] < threshold;
    return result;
}

} // namespace autoedo
