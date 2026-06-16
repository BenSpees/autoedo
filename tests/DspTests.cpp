// Standalone behavioural tests for the pitch-detection + correction engine.
// No framework / no JUCE: synthesise tones, run the real DSP, measure results.
#include "../Source/dsp/YinPitchDetector.h"
#include "../Source/dsp/PsolaPitchCorrector.h"
#include "../Source/dsp/Tuning.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
int failures = 0;
constexpr double kPi = 3.14159265358979323846;

void check (bool cond, const std::string& what)
{
    if (! cond) { std::printf ("  FAIL: %s\n", what.c_str()); ++failures; }
}

double centsBetween (double a, double b) { return 1200.0 * std::log2 (a / b); }

// A mildly richer-than-sine tone (fundamental + a couple of harmonics) so YIN
// has a clear periodic structure to lock onto.
std::vector<float> makeTone (double freq, double fs, int n, double amp = 0.4)
{
    std::vector<float> v (static_cast<size_t> (n));
    for (int i = 0; i < n; ++i)
    {
        const double t = static_cast<double> (i) / fs;
        v[static_cast<size_t> (i)] = static_cast<float> (
            amp * (std::sin (2.0 * kPi * freq * t)
                   + 0.5  * std::sin (2.0 * kPi * 2.0 * freq * t)
                   + 0.25 * std::sin (2.0 * kPi * 3.0 * freq * t)));
    }
    return v;
}

double estimateFreq (const float* data, int n, double fs)
{
    autoedo::YinPitchDetector det;
    const int frame = 2048;
    det.prepare (fs, frame, 60.0, 1600.0);
    det.setThreshold (0.15);

    // Average a few frames from the steady-state region for a robust estimate.
    double sum = 0.0; int count = 0;
    for (int start = n - frame * 4; start + frame <= n; start += frame / 2)
    {
        if (start < 0) continue;
        const auto r = det.process (data + start, frame);
        if (r.voiced && r.frequencyHz > 0.0) { sum += r.frequencyHz; ++count; }
    }
    return count > 0 ? sum / count : 0.0;
}

// Run an input tone through the corrector and return the processed output.
std::vector<float> runCorrector (const std::vector<float>& in, double fs,
                                 int edo, double retuneMs)
{
    autoedo::PsolaPitchCorrector corr;
    const int block = 256;
    corr.prepare (fs, 1, block);
    corr.setEdo (edo);
    corr.setRetuneMs (retuneMs);

    std::vector<float> out (in.size());
    std::vector<float> scratch (static_cast<size_t> (block));

    for (size_t pos = 0; pos < in.size(); pos += static_cast<size_t> (block))
    {
        const int m = static_cast<int> (std::min<size_t> (block, in.size() - pos));
        for (int i = 0; i < m; ++i) scratch[static_cast<size_t> (i)] = in[pos + static_cast<size_t> (i)];

        float* ch[1] = { scratch.data() };
        corr.process (ch, 1, m);

        for (int i = 0; i < m; ++i) out[pos + static_cast<size_t> (i)] = scratch[static_cast<size_t> (i)];
    }
    return out;
}

bool finiteAndNonSilent (const std::vector<float>& v, int from)
{
    double energy = 0.0;
    for (int i = from; i < static_cast<int> (v.size()); ++i)
    {
        if (! std::isfinite (v[static_cast<size_t> (i)])) return false;
        energy += static_cast<double> (v[static_cast<size_t> (i)]) * v[static_cast<size_t> (i)];
    }
    return energy > 1.0e-4;
}

// Verify the detector itself recovers a known pitch.
void testDetector (double fs)
{
    const auto tone = makeTone (220.0, fs, static_cast<int> (fs)); // A3, 1 s
    const double f = estimateFreq (tone.data(), static_cast<int> (tone.size()), fs);
    check (std::fabs (centsBetween (f, 220.0)) < 5.0,
           "detector recovers 220 Hz within 5 cents (got " + std::to_string (f) + ")");
}

// Core test: a detuned tone should come out tuned to the EDO grid.
void testCorrection (double fs, double inputHz, int edo, double tolCents)
{
    const int n = static_cast<int> (fs * 1.5); // 1.5 s
    const auto in  = makeTone (inputHz, fs, n);
    const auto out = runCorrector (in, fs, edo, 0.0 /* hard tune */);

    const double expected = autoedo::quantizeToEdo (inputHz, edo).targetHz;

    check (finiteAndNonSilent (out, n / 2), "output is finite and non-silent");

    const double got = estimateFreq (out.data(), n, fs);
    const double err = std::fabs (centsBetween (got, expected));

    const std::string msg = "corrected " + std::to_string (inputHz) + " Hz -> "
                          + std::to_string (got) + " Hz, target "
                          + std::to_string (expected) + " Hz (" + std::to_string (err)
                          + " cents), edo=" + std::to_string (edo);
    check (err < tolCents, msg);
    std::printf ("    %s\n", msg.c_str());
}

// Degree-mask test: with only C and G enabled, an E should be retuned to G,
// and the live read-out should report the detected/target pitches.
void testScale (double fs)
{
    const int n = static_cast<int> (fs * 1.5);
    const double inHz = 330.0; // ~E4
    const auto in = makeTone (inHz, fs, n);

    bool mask[12] = { false };
    mask[0] = true; mask[7] = true; // C and G only

    autoedo::PsolaPitchCorrector corr;
    const int block = 256;
    corr.prepare (fs, 1, block);
    corr.setEdo (12);
    corr.setRetuneMs (0.0);
    corr.setEnabledDegrees (mask, 12);

    std::vector<float> out (in.size()), scratch (static_cast<size_t> (block));
    for (size_t pos = 0; pos < in.size(); pos += static_cast<size_t> (block))
    {
        const int m = static_cast<int> (std::min<size_t> (block, in.size() - pos));
        for (int i = 0; i < m; ++i) scratch[static_cast<size_t> (i)] = in[pos + static_cast<size_t> (i)];
        float* ch[1] = { scratch.data() };
        corr.process (ch, 1, m);
        for (int i = 0; i < m; ++i) out[pos + static_cast<size_t> (i)] = scratch[static_cast<size_t> (i)];
    }

    const double expected = autoedo::quantizeToEdoScale (inHz, 12, mask).targetHz;
    const double got      = estimateFreq (out.data(), n, fs);

    check (std::fabs (centsBetween (got, expected)) < 35.0,
           "scale snap: E with {C,G} enabled retunes toward G");
    check (corr.isVoicedNow(), "read-out reports voiced");
    check (std::fabs (centsBetween (corr.getDetectedHz(), inHz)) < 35.0,
           "read-out detected pitch ~ input");

    std::printf ("    scale: in=%.1f Hz got=%.2f expected=%.2f (G) | readout det=%.2f tgt=%.2f\n",
                 inHz, got, expected, corr.getDetectedHz(), corr.getTargetHz());
}

// Regression for bug #2: a block larger than the prepared maximum must not
// corrupt the output (ring-buffer overflow).
void testOversizedBlock (double fs)
{
    const int n = static_cast<int> (fs * 2.0);
    std::vector<float> in (static_cast<size_t> (n)); // a chirp (non-stationary)
    double ph = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const double f = 200.0 + 300.0 * i / n;
        ph += 2.0 * kPi * f / fs;
        in[static_cast<size_t> (i)] = static_cast<float> (0.4 * std::sin (ph));
    }

    auto run = [&] (int block, int prepMax)
    {
        autoedo::PsolaPitchCorrector c;
        c.prepare (fs, 1, prepMax);
        c.setEdo (12);
        c.setRetuneMs (0.0);
        std::vector<float> out (static_cast<size_t> (n)), s (static_cast<size_t> (block));
        for (int pos = 0; pos < n; pos += block)
        {
            const int m = std::min (block, n - pos);
            for (int i = 0; i < m; ++i) s[static_cast<size_t> (i)] = in[static_cast<size_t> (pos + i)];
            float* ch[1] = { s.data() };
            c.process (ch, 1, m);
            for (int i = 0; i < m; ++i) out[static_cast<size_t> (pos + i)] = s[static_cast<size_t> (i)];
        }
        return out;
    };

    const auto ref = run (256, 256);     // block == prepared max (reference)
    const auto big = run (16384, 256);   // block >> prepared max (was the bug)

    double diff = 0.0; int cnt = 0;
    for (int i = n / 4; i < n * 3 / 4; ++i)
    {
        diff += std::fabs (big[static_cast<size_t> (i)] - ref[static_cast<size_t> (i)]);
        ++cnt;
    }
    const double mad = diff / cnt;
    check (mad < 1.0e-4, "oversized block matches reference (no ring corruption)");
    std::printf ("    oversized-block mean-abs-diff vs reference = %.6f\n", mad);
}

// Regression for bug #3: low pitch must be detectable at high sample rates.
void testHighSampleRate()
{
    const double fs = 96000.0;
    const int    n  = static_cast<int> (fs * 1.5);
    const double inHz = 70.0; // below the old ~94 Hz floor at 96 kHz
    const auto in = makeTone (inHz, fs, n);

    autoedo::PsolaPitchCorrector c;
    const int block = 256;
    c.prepare (fs, 1, block);
    c.setEdo (12);
    c.setRetuneMs (0.0);

    std::vector<float> s (static_cast<size_t> (block));
    for (int pos = 0; pos < n; pos += block)
    {
        const int m = std::min (block, n - pos);
        for (int i = 0; i < m; ++i) s[static_cast<size_t> (i)] = in[pos + static_cast<size_t> (i)];
        float* ch[1] = { s.data() };
        c.process (ch, 1, m);
    }

    check (c.isVoicedNow(), "96 kHz: 70 Hz is detected (voiced)");
    check (std::fabs (centsBetween (c.getDetectedHz(), inHz)) < 35.0,
           "96 kHz: detected pitch ~ 70 Hz");
    std::printf ("    high-SR: fs=96k in=70 Hz detected=%.2f voiced=%d\n",
                 c.getDetectedHz(), static_cast<int> (c.isVoicedNow()));
}

// Regression for bug #1: onset cost must not scale with idle time before it.
void testPrimingNoStorm (double fs)
{
    const int block = 256;

    auto worstOnsetMs = [&] (double silenceSec)
    {
        autoedo::PsolaPitchCorrector c;
        c.prepare (fs, 1, block);
        c.setEdo (12);
        c.setRetuneMs (0.0);

        std::vector<float> z (static_cast<size_t> (block), 0.0f);
        const int sil = static_cast<int> (fs * silenceSec);
        for (int pos = 0; pos < sil; pos += block)
        {
            float* ch[1] = { z.data() };
            c.process (ch, 1, block);
        }

        double worst = 0.0;
        for (int b = 0; b < 10; ++b)
        {
            std::vector<float> s (static_cast<size_t> (block));
            for (int i = 0; i < block; ++i)
                s[static_cast<size_t> (i)] = static_cast<float> (0.4 * std::sin (2.0 * kPi * 300.0 * i / fs));
            float* ch[1] = { s.data() };
            const auto t0 = std::chrono::high_resolution_clock::now();
            c.process (ch, 1, block);
            const auto t1 = std::chrono::high_resolution_clock::now();
            worst = std::max (worst, std::chrono::duration<double, std::milli> (t1 - t0).count());
        }
        return worst;
    };

    const double shortSilence = worstOnsetMs (0.5);
    const double longSilence  = worstOnsetMs (60.0);

    // With the fix the onset cost is independent of idle time (generous slack
    // for timing noise). The bug made the 60 s case ~40x heavier.
    check (longSilence < shortSilence * 10.0 + 3.0,
           "no priming storm: onset cost independent of idle time");
    std::printf ("    priming: onset after 0.5 s = %.3f ms, after 60 s = %.3f ms\n",
                 shortSilence, longSilence);
}

} // namespace

int main()
{
    std::printf ("DspTests\n");

    for (double fs : { 44100.0, 48000.0 })
    {
        std::printf ("  sampleRate=%.0f\n", fs);
        testDetector (fs);

        // 30 cents sharp of A4 -> should snap to 440 in 12-EDO.
        testCorrection (fs, 440.0 * std::pow (2.0, 30.0 / 1200.0), 12, 18.0);

        // An in-tune note should stay put.
        testCorrection (fs, 261.6255653, 12, 18.0);

        // 24-EDO: a tone 35 cents above C4 snaps to the on-grid quarter-tone.
        const double c4 = autoedo::kReferenceC0Hz * std::pow (2.0, 4.0);
        testCorrection (fs, c4 * std::pow (2.0, 35.0 / 1200.0), 24, 22.0);

        // 19-EDO: an arbitrary detuned tone snaps to its nearest 19-EDO degree.
        testCorrection (fs, 330.0, 19, 25.0);

        // Degree selection + live read-out.
        testScale (fs);

        // Bug-fix regressions.
        testOversizedBlock (fs);
        testPrimingNoStorm (fs);
    }

    // High-sample-rate low-pitch detection (bug #3).
    testHighSampleRate();

    if (failures == 0)
        std::printf ("  all DSP tests passed\n");
    else
        std::printf ("  %d DSP test(s) FAILED\n", failures);

    return failures == 0 ? 0 : 1;
}
