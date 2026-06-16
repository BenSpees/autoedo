// Standalone behavioural tests for the pitch-detection + correction engine.
// No framework / no JUCE: synthesise tones, run the real DSP, measure results.
#include "../Source/dsp/YinPitchDetector.h"
#include "../Source/dsp/PsolaPitchCorrector.h"
#include "../Source/dsp/Tuning.h"

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
    }

    if (failures == 0)
        std::printf ("  all DSP tests passed\n");
    else
        std::printf ("  %d DSP test(s) FAILED\n", failures);

    return failures == 0 ? 0 : 1;
}
