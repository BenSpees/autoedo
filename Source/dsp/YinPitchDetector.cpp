#include "YinPitchDetector.h"

#include <algorithm>
#include <cmath>

namespace autoedo
{
namespace
{
    constexpr double kPi = 3.14159265358979323846;

    /** Iterative in-place radix-2 FFT. @c n must be a power of two. */
    void fft (std::complex<double>* a, int n, bool inverse)
    {
        for (int i = 1, j = 0; i < n; ++i)
        {
            int bit = n >> 1;
            for (; j & bit; bit >>= 1)
                j ^= bit;
            j |= bit;
            if (i < j)
                std::swap (a[i], a[j]);
        }

        for (int len = 2; len <= n; len <<= 1)
        {
            const double ang = 2.0 * kPi / static_cast<double> (len) * (inverse ? 1.0 : -1.0);
            const std::complex<double> wl (std::cos (ang), std::sin (ang));
            for (int i = 0; i < n; i += len)
            {
                std::complex<double> w (1.0, 0.0);
                for (int k = 0; k < len / 2; ++k)
                {
                    const std::complex<double> u = a[i + k];
                    const std::complex<double> v = a[i + k + len / 2] * w;
                    a[i + k]               = u + v;
                    a[i + k + len / 2]     = u - v;
                    w *= wl;
                }
            }
        }

        if (inverse)
            for (int i = 0; i < n; ++i)
                a[i] /= static_cast<double> (n);
    }
}

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

    // A cross-correlation of a length-`window` signal against `window + tauMax`
    // samples is free of circular wrap-around for every lag in [0, tauMax] as
    // long as the transform is at least that long.
    fftSize = 1;
    while (fftSize < window + tauMax)
        fftSize <<= 1;

    diff.assign (static_cast<size_t> (tauMax), 0.0);
    cumulative.assign (static_cast<size_t> (tauMax), 0.0);
    specA.assign (static_cast<size_t> (fftSize), {});
    specB.assign (static_cast<size_t> (fftSize), {});
}

void YinPitchDetector::differenceFunction (const float* x)
{
    // d(tau) = sum_j (x[j] - x[j+tau])^2
    //        = P(0) + P(tau) - 2 r(tau)
    // with P(tau) = sum_j x[j+tau]^2 and r(tau) = sum_j x[j] x[j+tau].
    // r comes from one FFT cross-correlation; P slides in constant time.
    const int n = fftSize;

    for (int i = 0; i < n; ++i)
    {
        const double a = (i < window)            ? static_cast<double> (x[i]) : 0.0;
        const double b = (i < window + tauMax)   ? static_cast<double> (x[i]) : 0.0;
        specA[static_cast<size_t> (i)] = { a, 0.0 };
        specB[static_cast<size_t> (i)] = { b, 0.0 };
    }

    fft (specA.data(), n, false);
    fft (specB.data(), n, false);
    for (int i = 0; i < n; ++i)
        specA[static_cast<size_t> (i)] = std::conj (specA[static_cast<size_t> (i)])
                                       * specB[static_cast<size_t> (i)];
    fft (specA.data(), n, true);

    double power = 0.0;
    for (int j = 0; j < window; ++j)
        power += static_cast<double> (x[j]) * x[j];
    const double power0 = power;

    for (int tau = 0; tau < tauMax; ++tau)
    {
        if (tau > 0)
        {
            const double leaving  = static_cast<double> (x[tau - 1]) * x[tau - 1];
            const double entering = static_cast<double> (x[tau + window - 1]) * x[tau + window - 1];
            power += entering - leaving;
        }

        const double d = power0 + power - 2.0 * specA[static_cast<size_t> (tau)].real();
        diff[static_cast<size_t> (tau)] = d > 0.0 ? d : 0.0; // cancellation can go a hair negative
    }
}

YinPitchDetector::Result YinPitchDetector::process (const float* frame, int numSamples)
{
    Result result;

    if (frame == nullptr || numSamples < window + tauMax)
        return result;

    // Step 1: difference function d(tau).
    differenceFunction (frame);

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
    double bestValue = cumulative[static_cast<size_t> (bestTau)];
    if (bestTau > 0 && bestTau < tauMax - 1)
    {
        const double s0 = cumulative[static_cast<size_t> (bestTau - 1)];
        const double s1 = cumulative[static_cast<size_t> (bestTau)];
        const double s2 = cumulative[static_cast<size_t> (bestTau + 1)];
        const double denom = (2.0 * s1 - s2 - s0);
        if (std::abs (denom) > 1e-12)
        {
            const double shift = 0.5 * (s2 - s0) / denom;
            betterTau = bestTau + shift;
            // Value at the interpolated minimum, not at the integer bin it sits
            // next to: the confidence read-out then moves smoothly with the pitch
            // instead of stepping every time the peak crosses a sample boundary.
            bestValue = s1 - 0.25 * (s0 - s2) * shift;
        }
    }

    if (betterTau <= 0.0)
        return result;

    result.frequencyHz = sampleRate / betterTau;
    result.periodicity = std::clamp (1.0 - bestValue, 0.0, 1.0);
    result.voiced      = bestValue < threshold;
    return result;
}

} // namespace autoedo
