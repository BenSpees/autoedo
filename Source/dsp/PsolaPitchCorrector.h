#pragma once

#include <array>
#include <atomic>
#include <vector>
#include "YinPitchDetector.h"
#include "Tuning.h"

namespace autoedo
{
/**
    Real-time monophonic pitch corrector.

    Pipeline per the Phase-1 spec:
      1. YIN estimates the input fundamental.
      2. The fundamental is snapped to the nearest pitch on an N-EDO grid
         anchored at C (see Tuning.h).
      3. A retune-speed control glides the output pitch toward that target
         (0 ms = hard snap, larger = natural slide).
      4. TD-PSOLA resynthesises the audio at the corrected pitch, preserving
         formants and duration.

    Detection and timing are shared across channels (the source is assumed
    monophonic), so every channel receives an identical pitch shift, keeping a
    stereo signal phase-coherent. Unvoiced / silent passages are passed through
    a latency-matched dry path with a smooth crossfade to avoid artefacts.

    Pure C++ (no JUCE) so the engine is unit-testable on its own. Allocation
    happens only in @c prepare(); @c process() is allocation-free.
*/
class PsolaPitchCorrector
{
public:
    PsolaPitchCorrector() = default;

    /** Allocate buffers for a given configuration. Call before processing. */
    void prepare (double sampleRate, int numChannels, int maxBlockSize);

    /** Clear all internal state (e.g. on transport restart). */
    void reset();

    /** Process @c numSamples of @c numChannels interleaved-by-pointer audio
        in place. @c channelData[ch] points to a buffer of @c numSamples. */
    void process (float* const* channelData, int numChannels, int numSamples);

    // --- Parameters (safe to call from the audio thread between blocks) -----
    void setEdo (int edoValue) noexcept           { edo = edoValue; }
    void setRetuneMs (double ms) noexcept         { retuneMs = ms < 0.0 ? 0.0 : ms; }

    /** Set which scale degrees (0..count-1, 0 == C) the corrector may tune to.
        Degrees not covered by @c count are left enabled. */
    void setEnabledDegrees (const bool* mask, int count) noexcept
    {
        const int n = std::min (count, static_cast<int> (enabledDeg.size()));
        for (int i = 0; i < n; ++i)
            enabledDeg[static_cast<size_t> (i)] = mask[i];
    }

    /** Reported processing latency in samples (constant after prepare). */
    int  getLatencySamples() const noexcept       { return latency; }

    // --- Live read-out for the UI (lock-free; written on the audio thread) --
    float getDetectedHz() const noexcept { return detectedHzOut.load (std::memory_order_relaxed); }
    float getTargetHz()   const noexcept { return targetHzOut.load   (std::memory_order_relaxed); }
    bool  isVoicedNow()   const noexcept { return voicedOut.load     (std::memory_order_relaxed); }

private:
    void runDetection();
    void placeGrain (long long centerOut);

    // Configuration ---------------------------------------------------------
    double fs        = 44100.0;
    int    channels  = 0;
    int    frameSize = 2048;
    int    hop       = 256;
    int    tauMin    = 30;
    int    tauMax    = 1024;
    int    latency   = 0;

    int    bufSize   = 1 << 15;
    int    bufMask   = (1 << 15) - 1;

    // Ring buffers ----------------------------------------------------------
    std::vector<std::vector<float>> inBuf;   // per-channel input history
    std::vector<float>              monoBuf; // mono mix for pitch detection
    std::vector<std::vector<float>> wetAcc;  // per-channel PSOLA accumulator
    std::vector<float>              wetWin;  // shared window-sum accumulator
    std::vector<float>              frame;   // scratch frame for detection

    // Running state ---------------------------------------------------------
    YinPitchDetector detector;

    long long inWrite      = 0;   // total input samples written
    long long lastDetectAt = 0;   // inWrite at last detection
    long long lastTouched  = -1;  // highest output index cleared in wetAcc/wetWin
    double    synthMark    = 0.0;  // absolute output index of next grain center

    double currentPeriod = 200.0; // T0 in samples (from detected pitch)
    double synthPeriod   = 200.0; // T1 in samples (from corrected pitch)
    bool   voiced        = false;
    bool   primed        = false; // becomes true once first pitch is found

    double outCents = 0.0;        // current (smoothed) output pitch, cents re C0
    double vGain    = 0.0;        // smoothed dry(0)/wet(1) crossfade gain

    // Parameters ------------------------------------------------------------
    int    edo      = 12;
    double retuneMs = 20.0;
    std::array<bool, kMaxEdo> enabledDeg {}; // initialised all-true in reset()

    // Live read-out (audio thread -> UI) ------------------------------------
    std::atomic<float> detectedHzOut { 0.0f };
    std::atomic<float> targetHzOut   { 0.0f };
    std::atomic<bool>  voicedOut     { false };
};

} // namespace autoedo
