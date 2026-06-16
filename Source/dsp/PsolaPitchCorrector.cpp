#include "PsolaPitchCorrector.h"
#include "Tuning.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace autoedo
{
namespace
{
    constexpr double kPi      = 3.14159265358979323846;
    constexpr double kMinFreq = 65.0;   // lowest detectable pitch (Hz)
    constexpr double kMaxFreq = 1600.0; // highest detectable pitch (Hz)
    constexpr double kGateRms = 0.0015; // ~ -56 dBFS noise gate for detection
}

void PsolaPitchCorrector::prepare (double sampleRate, int numChannels, int maxBlockSize)
{
    fs        = sampleRate > 0.0 ? sampleRate : 44100.0;
    channels  = std::max (1, numChannels);
    maxBlock  = std::max (maxBlockSize, 16);

    // Frame must be large enough that frameSize/2 >= the longest period we want
    // to detect (= fs / kMinFreq), so the lowest detectable pitch is independent
    // of sample rate. Keep it a power of two for the analysis window.
    const int longestPeriod = static_cast<int> (std::ceil (fs / kMinFreq));
    frameSize = 2048;
    while (frameSize < 2 * longestPeriod)
        frameSize <<= 1;
    frameSize = std::min (frameSize, 1 << 15);

    // Detection hop ~5 ms (keeps detection rate bounded at high sample rates).
    hop = std::max (128, static_cast<int> (fs * 0.005));

    tauMax = std::min (frameSize / 2, longestPeriod);
    tauMax = std::max (tauMax, 64);
    tauMin = std::max (2, static_cast<int> (std::floor (fs / kMaxFreq)));
    tauMin = std::min (tauMin, tauMax - 1);

    // Latency must guarantee that every delivered output sample is fully covered
    // by already-placed grains (see header / design notes): 3 * longest period.
    latency = 3 * tauMax;

    const int need = latency + maxBlock + 2 * tauMax + frameSize + 16;
    bufSize = 1;
    while (bufSize < need)
        bufSize <<= 1;
    bufMask = bufSize - 1;

    inBuf.assign  (static_cast<size_t> (channels), std::vector<float> (static_cast<size_t> (bufSize), 0.0f));
    wetAcc.assign (static_cast<size_t> (channels), std::vector<float> (static_cast<size_t> (bufSize), 0.0f));
    monoBuf.assign (static_cast<size_t> (bufSize), 0.0f);
    wetWin.assign  (static_cast<size_t> (bufSize), 0.0f);
    frame.assign   (static_cast<size_t> (frameSize), 0.0f);
    chanPtrs.assign (static_cast<size_t> (channels), nullptr);

    detector.prepare (fs, frameSize, kMinFreq, kMaxFreq);

    reset();
}

void PsolaPitchCorrector::reset()
{
    for (auto& c : inBuf)  std::fill (c.begin(), c.end(), 0.0f);
    for (auto& c : wetAcc) std::fill (c.begin(), c.end(), 0.0f);
    std::fill (monoBuf.begin(), monoBuf.end(), 0.0f);
    std::fill (wetWin.begin(),  wetWin.end(),  0.0f);

    inWrite      = 0;
    lastDetectAt = 0;
    lastTouched  = -1;
    synthMark    = static_cast<double> (tauMax);

    currentPeriod = std::clamp (fs / 220.0, static_cast<double> (tauMin), static_cast<double> (tauMax));
    synthPeriod   = currentPeriod;
    voiced        = false;
    primed        = false;
    outCents      = 0.0;
    vGain         = 0.0;

    enabledDeg.fill (true); // default: every degree usable (full chromatic EDO)
    detectedHzOut.store (0.0f, std::memory_order_relaxed);
    targetHzOut.store   (0.0f, std::memory_order_relaxed);
    voicedOut.store     (false, std::memory_order_relaxed);
}

void PsolaPitchCorrector::runDetection()
{
    const long long start = inWrite - frameSize;
    double sum = 0.0;
    for (int k = 0; k < frameSize; ++k)
    {
        const long long idx = start + k;
        const float s = (idx >= 0) ? monoBuf[static_cast<size_t> (idx & bufMask)] : 0.0f;
        frame[static_cast<size_t> (k)] = s;
        sum += static_cast<double> (s) * s;
    }

    const double rms     = std::sqrt (sum / frameSize);
    const double elapsed = static_cast<double> (inWrite - lastDetectAt) / fs;
    lastDetectAt = inWrite;

    const auto res        = detector.process (frame.data(), frameSize);
    const bool nowVoiced  = res.voiced && res.frequencyHz > 0.0 && rms > kGateRms;

    if (nowVoiced)
    {
        currentPeriod = std::clamp (fs / res.frequencyHz,
                                    static_cast<double> (tauMin),
                                    static_cast<double> (tauMax));

        const double detectedCents = 1200.0 * std::log2 (res.frequencyHz / kReferenceC0Hz);
        const auto   t             = quantizeToEdoScale (res.frequencyHz, edo, enabledDeg.data());
        const double targetCents   = 1200.0 * std::log2 (t.targetHz / kReferenceC0Hz);

        detectedHzOut.store (static_cast<float> (res.frequencyHz), std::memory_order_relaxed);
        targetHzOut.store   (static_cast<float> (t.targetHz),      std::memory_order_relaxed);

        // On a fresh onset, start from the pitch actually sung so the correction
        // glides from there (retune speed) instead of jumping from a stale value.
        if (! voiced || ! primed)
            outCents = detectedCents;

        const double tauSec = retuneMs / 1000.0;
        const double alpha  = (tauSec <= 0.0) ? 1.0 : (1.0 - std::exp (-elapsed / tauSec));
        outCents += (targetCents - outCents) * alpha;

        double beta = std::pow (2.0, (outCents - detectedCents) / 1200.0);
        beta = std::clamp (beta, 0.5, 2.0); // safety net; correction ratios stay ~1
        synthPeriod = currentPeriod / beta;

        primed = true;
    }
    else
    {
        synthPeriod = currentPeriod; // identity timeline; crossfade handles the rest
    }

    voiced = nowVoiced;
    voicedOut.store (nowVoiced, std::memory_order_relaxed);
}

void PsolaPitchCorrector::placeGrain (long long centerOut)
{
    const double T0   = currentPeriod;
    const int    half = std::clamp (static_cast<int> (std::llround (T0)), tauMin, tauMax);

    // Analysis mark: the input-period grid point nearest this output center.
    // (Epoch-free placement — a uniform T0 grid. Peak/epoch snapping was tried
    // but caused period-doubling on harmonically rich tones; a robust glottal
    // epoch detector would be the route to higher quality, see README.)
    const long long m       = std::llround (static_cast<double> (centerOut) / T0);
    const long long aCenter = std::llround (static_cast<double> (m) * T0);

    // Clear any output slots this grain newly reaches, before accumulating.
    const long long top = centerOut + half;
    for (long long s = lastTouched + 1; s <= top; ++s)
    {
        const size_t k = static_cast<size_t> (s & bufMask);
        wetWin[k] = 0.0f;
        for (int ch = 0; ch < channels; ++ch)
            wetAcc[static_cast<size_t> (ch)][k] = 0.0f;
    }
    if (top > lastTouched)
        lastTouched = top;

    // Hann-windowed overlap-add.
    const double norm = 1.0 / static_cast<double> (2 * half);
    for (int j = -half; j <= half; ++j)
    {
        const double p  = static_cast<double> (j + half) * norm; // 0..1
        const float  wv = static_cast<float> (0.5 - 0.5 * std::cos (2.0 * kPi * p));

        const long long outI = centerOut + j;
        const long long inI  = aCenter + j;
        const size_t    ok   = static_cast<size_t> (outI & bufMask);

        wetWin[ok] += wv;
        for (int ch = 0; ch < channels; ++ch)
        {
            float sample = 0.0f;
            if (inI >= 0 && inI > inWrite - bufSize && inI < inWrite)
                sample = inBuf[static_cast<size_t> (ch)][static_cast<size_t> (inI & bufMask)];
            wetAcc[static_cast<size_t> (ch)][ok] += wv * sample;
        }
    }
}

void PsolaPitchCorrector::process (float* const* channelData, int numChannels, int numSamples)
{
    if (channels == 0 || numSamples <= 0)
        return;

    // Sub-chunk so a block larger than the prepared maximum can never overflow
    // the ring buffers (e.g. offline bounce, or hosts that ignore the hint).
    int done = 0;
    while (done < numSamples)
    {
        const int m = std::min (numSamples - done, maxBlock);
        const int nch = std::min (numChannels, static_cast<int> (chanPtrs.size()));
        for (int ch = 0; ch < nch; ++ch)
            chanPtrs[static_cast<size_t> (ch)] = channelData[ch] + done;
        processChunk (chanPtrs.data(), nch, m);
        done += m;
    }
}

void PsolaPitchCorrector::processChunk (float* const* channelData, int numChannels, int numSamples)
{
    const int nch = std::min (numChannels, channels);

    for (int i = 0; i < numSamples; ++i)
    {
        const size_t w = static_cast<size_t> (inWrite & bufMask);
        float mono = 0.0f;
        for (int ch = 0; ch < nch; ++ch)
        {
            const float s = channelData[ch][i];
            inBuf[static_cast<size_t> (ch)][w] = s;
            mono += s;
        }
        for (int ch = nch; ch < channels; ++ch)
            inBuf[static_cast<size_t> (ch)][w] = 0.0f;

        monoBuf[w] = (nch > 0) ? mono / static_cast<float> (nch) : 0.0f;
        ++inWrite;

        if (! primed)
        {
            // Hold the synthesis pointer at the input frontier until the first
            // pitch primes the engine, so a long silent intro doesn't force the
            // onset block to "catch up" a backlog of grains (CPU spike/dropout).
            synthMark   = static_cast<double> (inWrite);
            lastTouched = inWrite - 1;
        }

        if (inWrite - lastDetectAt >= hop && inWrite >= frameSize)
            runDetection();

        while (primed && synthMark <= static_cast<double> (inWrite - 2 * tauMax))
        {
            placeGrain (static_cast<long long> (std::llround (synthMark)));
            synthMark += synthPeriod;
        }
    }

    // Deliver: output sample i is the corrected input from `latency` samples ago.
    const long long blockStart = inWrite - numSamples;
    const double gainAlpha = 1.0 - std::exp (-1.0 / (0.005 * fs)); // ~5 ms crossfade

    for (int i = 0; i < numSamples; ++i)
    {
        const double target = voiced ? 1.0 : 0.0;
        vGain += (target - vGain) * gainAlpha;

        const long long tOut = blockStart + i - latency;
        if (tOut < 0)
        {
            for (int ch = 0; ch < numChannels; ++ch)
                channelData[ch][i] = 0.0f;
            continue;
        }

        const size_t idx = static_cast<size_t> (tOut & bufMask);
        const float  win = wetWin[idx];

        for (int ch = 0; ch < numChannels; ++ch)
        {
            const int   sch = std::min (ch, channels - 1);
            const float dry = inBuf[static_cast<size_t> (sch)][idx];
            const float wet = (win > 1.0e-6f) ? (wetAcc[static_cast<size_t> (sch)][idx] / win) : dry;
            channelData[ch][i] = static_cast<float> (vGain * wet + (1.0 - vGain) * dry);
        }
    }
}

} // namespace autoedo
