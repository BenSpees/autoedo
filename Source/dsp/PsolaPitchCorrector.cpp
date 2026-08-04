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

    // Voicing hysteresis. It takes a clearly periodic frame to enter the wet
    // path, but a clearly *aperiodic* one to leave it. With a single threshold
    // the decision chatters on any frame sitting near it, and the dry/wet
    // crossfade audibly flutters through sustained notes.
    constexpr double kVoicedEnter  = 0.88; // periodicity (= 1 - YIN's d')
    constexpr double kVoicedLeave  = 0.80;
    constexpr double kGateLeaveMul = 0.5;

    /** Fractional-delay read for a grain: a 6-tap, 5th-order Lagrange FIR.

        The analysis mark is fractional by construction, and rounding it to the
        nearest sample would put up to half a sample of phase jitter on every
        grain — near the top of the vocal range that is a sizeable slice of a
        period. Every tap of a grain sits an *integer* offset from the same
        centre, though, so the fractional part is constant across the whole
        grain and both channels: the coefficients are built once per grain and
        then applied as a fixed filter. A 5th-order interpolator therefore costs
        about what a 4-point cubic did, while dropping interpolation error far
        enough below the artefact floor that it stops being the limit for high
        notes at 44.1 kHz. */
    struct FractionalTap
    {
        double c[6] {};

        /** @c t is the fractional position within the tap centred on index 0,
            with taps laid out at offsets -2 .. +3. */
        void set (double t) noexcept
        {
            for (int i = 0; i < 6; ++i)
            {
                const double xi = static_cast<double> (i - 2);
                double basis = 1.0;
                for (int j = 0; j < 6; ++j)
                {
                    if (j == i)
                        continue;
                    const double xj = static_cast<double> (j - 2);
                    basis *= (t - xj) / (xi - xj);
                }
                c[i] = basis;
            }
        }

        float read (const std::vector<float>& buf, int mask, long long i0) const noexcept
        {
            double s = 0.0;
            for (int k = 0; k < 6; ++k)
                s += c[k] * static_cast<double> (buf[static_cast<size_t> ((i0 + k - 2) & mask)]);
            return static_cast<float> (s);
        }
    };
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

    // A frame's estimate describes the middle of that frame, not its trailing
    // edge, so the pitch rings are written half a frame behind the input
    // frontier. Grains must be placed behind *that* to read a stamped estimate.
    detectLag = frameSize / 2;
    grainLag  = std::max (2 * tauMax, detectLag + 1);

    // Latency must guarantee that every delivered output sample is fully covered
    // by already-placed grains: the grain lag plus a grain's own half-width.
    latency = grainLag + tauMax;

    const int need = latency + maxBlock + grainLag + frameSize + 16;
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
    periodRing.assign (static_cast<size_t> (bufSize), 0.0f);
    ratioRing.assign  (static_cast<size_t> (bufSize), 1.0f);
    voicedRing.assign (static_cast<size_t> (bufSize), 0u);

    detector.prepare (fs, frameSize, kMinFreq, kMaxFreq);

    reset();
}

void PsolaPitchCorrector::reset()
{
    for (auto& c : inBuf)  std::fill (c.begin(), c.end(), 0.0f);
    for (auto& c : wetAcc) std::fill (c.begin(), c.end(), 0.0f);
    std::fill (monoBuf.begin(), monoBuf.end(), 0.0f);
    std::fill (wetWin.begin(),  wetWin.end(),  0.0f);
    std::fill (ratioRing.begin(),  ratioRing.end(),  1.0f);
    std::fill (voicedRing.begin(), voicedRing.end(), 0u);

    inWrite      = 0;
    lastDetectAt = 0;
    lastTouched  = -1;
    synthMark    = static_cast<double> (tauMax);
    analysisMark = static_cast<double> (tauMax);

    currentPeriod = std::clamp (fs / 220.0, static_cast<double> (tauMin), static_cast<double> (tauMax));
    currentRatio  = 1.0;
    std::fill (periodRing.begin(), periodRing.end(), static_cast<float> (currentPeriod));
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

    const auto res = detector.process (frame.data(), frameSize);

    // Asymmetric thresholds: hard to enter, easy to stay. See kVoicedEnter.
    const double periodicityNeeded = voiced ? kVoicedLeave : kVoicedEnter;
    const double rmsNeeded         = voiced ? kGateRms * kGateLeaveMul : kGateRms;
    const bool   nowVoiced         = res.frequencyHz > 0.0
                                  && res.periodicity >= periodicityNeeded
                                  && rms > rmsNeeded;

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
        currentRatio = beta;

        primed = true;
    }
    else
    {
        currentRatio = 1.0; // identity timeline; crossfade handles the rest
    }

    voiced = nowVoiced;
    voicedOut.store (nowVoiced, std::memory_order_relaxed);
}

void PsolaPitchCorrector::placeGrain (long long centerOut, double synthPos, double T0)
{
    const int half = std::clamp (static_cast<int> (std::llround (T0)), tauMin, tauMax);

    // Analysis marks are a continuous chain — each one T0 on from the last —
    // and this grain reads from whichever mark sits nearest the output centre.
    // Consecutive grains are therefore always a whole number of local periods
    // apart in the source, which is the whole point of PSOLA: they overlap-add
    // in phase. Advancing the chain also *is* the pitch shift, since the marks
    // step by T0 while the output centres step by T0/ratio, so a mark is
    // naturally reused (shift up) or skipped (shift down).
    //
    // Deriving the mark from an absolute grid instead — round(centreOut / T0) * T0,
    // as this did — ties the read position to the *current* T0 across the whole
    // timeline, so tiny frame-to-frame jitter in T0 relocates it by up to half a
    // period. See tests/DspTests.cpp: that alone cost ~18 dB of harmonic purity
    // on a note that needed no correction at all.
    if (std::abs (analysisMark - synthPos) > 4.0 * tauMax)
        analysisMark = synthPos; // resync after a gap/reset

    while (analysisMark + 0.5 * T0 < synthPos)
        analysisMark += T0;

    // Everything above is tracked against the *unrounded* synthesis position.
    // The output centre has to be a whole sample, so the read centre carries the
    // rounding instead: the grain-to-grain phase offset (analysisMark - synthPos)
    // then moves smoothly, where pinning the read to the rounded centre would put
    // a fresh +/-0.5 sample of jitter on every grain.
    const double readCenter = static_cast<double> (centerOut) + (analysisMark - synthPos);

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

    // Hann-windowed overlap-add, read at fractional sample positions. The
    // fractional part is the same for every tap of this grain, so build the
    // interpolation filter once here rather than per sample per channel.
    const long long baseIdx = static_cast<long long> (std::floor (readCenter));
    FractionalTap   tap;
    tap.set (readCenter - static_cast<double> (baseIdx));

    const long long lo = std::max (0LL, inWrite - bufSize + 4);
    const long long hi = inWrite - 1;
    const bool wholeGrainReadable = (baseIdx - half - 2 >= lo) && (baseIdx + half + 3 <= hi);

    const double norm = 1.0 / static_cast<double> (2 * half);
    for (int j = -half; j <= half; ++j)
    {
        const double p  = static_cast<double> (j + half) * norm; // 0..1
        const float  wv = static_cast<float> (0.5 - 0.5 * std::cos (2.0 * kPi * p));

        const size_t    ok = static_cast<size_t> ((centerOut + j) & bufMask);
        const long long i0 = baseIdx + j;

        wetWin[ok] += wv;

        if (! wholeGrainReadable && (i0 - 2 < lo || i0 + 3 > hi))
            continue; // start-up edge: leave the slot at zero, the crossfade covers it

        for (int ch = 0; ch < channels; ++ch)
            wetAcc[static_cast<size_t> (ch)][ok]
                += wv * tap.read (inBuf[static_cast<size_t> (ch)], bufMask, i0);
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

        // Stamp the current pitch state onto the moment it actually describes.
        // A detection made at the frontier analysed the frame ending there, so
        // it belongs half a frame back; grains and the crossfade then read the
        // estimate for their own position instead of one ~10 ms in their future.
        const long long stampAt = inWrite - 1 - detectLag;
        if (stampAt >= 0)
        {
            const size_t sk = static_cast<size_t> (stampAt & bufMask);
            periodRing[sk] = static_cast<float> (currentPeriod);
            ratioRing[sk]  = static_cast<float> (currentRatio);
            voicedRing[sk] = voiced ? 1u : 0u;
        }

        if (! primed)
        {
            // Hold the synthesis pointer at the input frontier until the first
            // pitch primes the engine, so a long silent intro doesn't force the
            // onset block to "catch up" a backlog of grains (CPU spike/dropout).
            synthMark    = static_cast<double> (inWrite);
            analysisMark = static_cast<double> (inWrite);
            lastTouched  = inWrite - 1;
        }

        if (inWrite - lastDetectAt >= hop && inWrite >= frameSize)
            runDetection();

        while (primed && synthMark <= static_cast<double> (inWrite - grainLag))
        {
            const long long c  = static_cast<long long> (std::llround (synthMark));
            const size_t    ck = static_cast<size_t> (c & bufMask);

            const double T0 = std::clamp (static_cast<double> (periodRing[ck]),
                                          static_cast<double> (tauMin),
                                          static_cast<double> (tauMax));
            const double r  = std::clamp (static_cast<double> (ratioRing[ck]), 0.5, 2.0);

            placeGrain (c, synthMark, T0);
            synthMark += T0 / r;
        }
    }

    // Deliver: output sample i is the corrected input from `latency` samples ago.
    const long long blockStart = inWrite - numSamples;
    const double gainAlpha = 1.0 - std::exp (-1.0 / (0.005 * fs)); // ~5 ms crossfade

    for (int i = 0; i < numSamples; ++i)
    {
        const long long tOut = blockStart + i - latency;
        if (tOut < 0)
        {
            for (int ch = 0; ch < numChannels; ++ch)
                channelData[ch][i] = 0.0f;
            continue;
        }

        const size_t idx = static_cast<size_t> (tOut & bufMask);

        // Voicing read from the timeline, not from the frontier: an onset used
        // to open the wet path a full latency (~46 ms) before the note it
        // belongs to actually reached the output.
        const double target = voicedRing[idx] != 0u ? 1.0 : 0.0;
        vGain += (target - vGain) * gainAlpha;

        // Soft floor on the window sum instead of a hard wet-or-dry switch.
        // Where the accumulated window collapses (a large downward shift can
        // space grains most of a window apart) the old code flipped individual
        // samples to the dry path, i.e. a discontinuity mid-note. Normal
        // overlap sums to ~1, so at -60 dB this floor is inaudible there.
        const float win     = wetWin[idx];
        const float invWin  = 1.0f / (win + 1.0e-3f);
        const float wetMix  = win * invWin;

        for (int ch = 0; ch < numChannels; ++ch)
        {
            const int   sch = std::min (ch, channels - 1);
            const float dry = inBuf[static_cast<size_t> (sch)][idx];
            const float wet = wetAcc[static_cast<size_t> (sch)][idx] * invWin;
            const float cor = wetMix * wet + (1.0f - wetMix) * dry;
            channelData[ch][i] = static_cast<float> (vGain * cor + (1.0 - vGain) * dry);
        }
    }
}

} // namespace autoedo
