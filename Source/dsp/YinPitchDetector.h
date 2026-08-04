#pragma once

#include <complex>
#include <vector>

namespace autoedo
{
/**
    Monophonic fundamental-frequency estimator using the YIN algorithm
    (de Cheveigné & Kawahara, 2002).

    Plain C++ with no JUCE dependency so it can be unit-tested in isolation.
    Designed for voice / monophonic instruments, which is what pitch correction
    operates on.

    The squared-difference function is evaluated through an FFT cross-correlation
    rather than the textbook double loop. The result is identical to within
    floating-point noise, but the cost drops from O(tauMax * window) to
    O(N log N) — at 96 kHz that is the difference between ~3.9 M and ~0.2 M
    multiply-adds per analysis frame, which is what makes a 5 ms detection hop
    affordable at high sample rates.
*/
class YinPitchDetector
{
public:
    struct Result
    {
        double frequencyHz = 0.0; ///< Estimated f0 (0 if no pitch found).
        double periodicity = 0.0; ///< Confidence in [0, 1] (1 = perfectly periodic).
        bool   voiced      = false;
    };

    YinPitchDetector() = default;

    /** Prepare for a given sample rate and analysis frame size.
        @param sampleRate    host sample rate
        @param frameSize     number of samples per analysis frame (power of two recommended)
        @param minFrequency  lowest detectable pitch, Hz (sets the longest period searched)
        @param maxFrequency  highest detectable pitch, Hz (sets the shortest period searched)
    */
    void prepare (double sampleRate, int frameSize,
                  double minFrequency = 60.0, double maxFrequency = 1600.0);

    /** Analyse one frame of @c frameSize samples. */
    Result process (const float* frame, int numSamples);

    /** Threshold on the cumulative-mean-normalised difference below which a tau
        is accepted as the period (lower = stricter). YIN's default is ~0.1–0.15. */
    void setThreshold (double t) { threshold = t; }

    int  getFrameSize() const noexcept { return frameSize; }

private:
    /** Fill @c diff with d(tau) for tau in [0, tauMax). */
    void differenceFunction (const float* x);

    double sampleRate = 44100.0;
    int    frameSize  = 2048;
    int    tauMin     = 2;
    int    tauMax     = 1024;
    int    window     = 1024; // integration window length
    int    fftSize    = 2048; // >= window + tauMax, power of two
    double threshold  = 0.12;

    std::vector<double> diff;       // difference function d(tau)
    std::vector<double> cumulative; // cumulative mean normalised difference d'(tau)

    // Scratch for the FFT cross-correlation (allocated in prepare()).
    std::vector<std::complex<double>> specA, specB;
};

} // namespace autoedo
