// Standalone behavioural tests for the pitch-detection + correction engine.
// No framework / no JUCE: synthesise tones, run the real DSP, measure results.
#include "../Source/dsp/YinPitchDetector.h"
#include "../Source/dsp/PsolaPitchCorrector.h"
#include "../Source/dsp/Tuning.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
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

// --- Output-quality measurement -------------------------------------------
// Pitch accuracy on its own says nothing about how the result *sounds*: a
// corrector can land dead on the target and still hand back a mush of
// artefacts. For a steady periodic input corrected to a steady target, the
// ideal output is perfectly periodic at that target, so the honest measure is
// how much of the output energy sits on harmonics of it and how much does not.

void fft (std::vector<std::complex<double>>& a, bool inverse)
{
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i)
    {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j |= bit;
        if (i < j) std::swap (a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1)
    {
        const double ang = 2.0 * kPi / static_cast<double> (len) * (inverse ? 1.0 : -1.0);
        const std::complex<double> wl (std::cos (ang), std::sin (ang));
        for (size_t i = 0; i < n; i += len)
        {
            std::complex<double> w (1.0, 0.0);
            for (size_t k = 0; k < len / 2; ++k)
            {
                const auto u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
    if (inverse) for (auto& x : a) x /= static_cast<double> (n);
}

/** Harmonic-to-artefact ratio in dB over a window of @c sig starting at @c from:
    energy within a few bins of every multiple of @c f0, against everything else.

    The band has to be wide enough to swallow the analysis window's own leakage
    skirt, or the reading swings by ~10 dB purely on where the partials happen
    to land between bins — which is measurement noise, not signal quality. */
double harmonicRatioDb (const std::vector<float>& sig, double fs, double f0, int from, int len)
{
    size_t n = 1;
    while (n < static_cast<size_t> (len)) n <<= 1;
    n >>= 1; // stay inside the requested region

    std::vector<std::complex<double>> a (n);
    for (size_t i = 0; i < n; ++i)
    {
        const double w = 0.5 - 0.5 * std::cos (2.0 * kPi * static_cast<double> (i) / static_cast<double> (n));
        a[i] = static_cast<double> (sig[from + static_cast<int> (i)]) * w;
    }
    fft (a, false);

    const size_t half = n / 2;
    std::vector<char> isHarmonic (half + 1, 0);
    for (int k = 1; k * f0 < fs / 2.0; ++k)
    {
        const double bin = k * f0 * static_cast<double> (n) / fs;
        for (int d = -6; d <= 6; ++d)
        {
            const long b = std::lround (bin) + d;
            if (b >= 0 && b <= static_cast<long> (half)) isHarmonic[static_cast<size_t> (b)] = 1;
        }
    }

    double onHarmonics = 0.0, elsewhere = 0.0;
    for (size_t b = 1; b <= half; ++b)
        (isHarmonic[b] ? onHarmonics : elsewhere) += std::norm (a[b]);

    return 10.0 * std::log10 (onHarmonics / std::max (elsewhere, 1.0e-30));
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

// Regression for the PSOLA grain-placement phase bugs.
//
// Analysis marks used to be rebuilt from an absolute grid, round(centre / T0) * T0,
// which ties the read position to the *current* period estimate across the whole
// timeline. A 0.1 % wobble in T0 -- ordinary frame-to-frame detector jitter on a
// dead-steady note -- then moved that mark by up to half a period, so consecutive
// grains were overlap-added near antiphase. Separately, the read position was
// pinned to the rounded output centre, adding +/-0.5 sample of fresh phase jitter
// per grain. Together they cost 15-20 dB of harmonic purity on material the
// corrector should have passed through essentially untouched.
//
// So: put in a tone that is already on the grid, and require the output to be as
// clean as the input. This is the test that fails loudly if grain phase coherence
// is ever broken again -- pitch accuracy alone will not notice.
void testTransparency (double fs)
{
    // maxLoss differs by case because the cases promise different things. An
    // on-grid note asks for no correction at all, so it must come back as clean
    // as it went in. A note being retuned cannot: shifting by grain placement
    // always leaves a trace of the original pitch behind (measurably ~-65 dB
    // here, as sidebands at multiples of the *input* f0). For that case the
    // absolute floor below is the real guard.
    struct Case { const char* what; double hz; int edo; double maxLoss; };
    const Case cases[] = {
        { "on-grid A4 (no correction needed)", 440.0,       12, 3.0 },
        { "on-grid C3 (low)",                  130.8127826, 12, 3.0 },
        { "on-grid A5 (high)",                 880.0,       12, 7.0 },
        { "30 cents sharp -> A4",              440.0 * std::pow (2.0, 30.0 / 1200.0), 12, 14.0 },
    };

    for (const auto& c : cases)
    {
        const int  n   = static_cast<int> (fs * 2.0);
        const auto in  = makeTone (c.hz, fs, n);
        const auto out = runCorrector (in, fs, c.edo, 0.0);
        const double target = autoedo::quantizeToEdo (c.hz, c.edo).targetHz;

        const double dryDb = harmonicRatioDb (in,  fs, c.hz,   n / 2, n / 4);
        const double wetDb = harmonicRatioDb (out, fs, target, n / 2, n / 4);
        const double loss  = dryDb - wetDb;

        // Two bounds, because either one alone is misleading. The absolute
        // figure is what a listener hears; the relative one catches an engine
        // that turns a pristine input into a merely "good" output. The pre-fix
        // engine failed both by a wide margin: 28 dB absolute against 47-62 dB
        // now, and 18 dB of loss on a note it should not have touched.
        check (wetDb > 40.0,
               std::string ("clean output: ") + c.what + " measures only "
                   + std::to_string (wetDb) + " dB harmonic-to-artefact");
        check (loss < c.maxLoss,
               std::string ("transparent: ") + c.what + " loses "
                   + std::to_string (loss) + " dB of harmonic purity");
        std::printf ("    transparency %-36s in %5.1f dB -> out %5.1f dB (loss %+5.1f dB)\n",
                     c.what, dryDb, wetDb, loss);
    }
}

// Vibrato is the case where the period estimate genuinely moves frame to frame,
// which is exactly what the old absolute-grid placement could not survive: the
// output measured *worse* than -8 dB, i.e. artefacts louder than the note.
void testVibrato (double fs)
{
    const int n = static_cast<int> (fs * 2.0);
    for (double depthCents : { 20.0, 50.0 })
    {
        std::vector<float> in (static_cast<size_t> (n));
        double p1 = 0.0, p2 = 0.0, p3 = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const double t  = static_cast<double> (i) / fs;
            const double f  = 440.0 * std::pow (2.0, depthCents * std::sin (2.0 * kPi * 5.0 * t) / 1200.0);
            p1 += 2.0 * kPi * f / fs;
            p2 += 2.0 * kPi * 2.0 * f / fs;
            p3 += 2.0 * kPi * 3.0 * f / fs;
            in[static_cast<size_t> (i)] = static_cast<float> (
                0.4 * (std::sin (p1) + 0.5 * std::sin (p2) + 0.25 * std::sin (p3)));
        }

        // Slow retune keeps the vibrato rather than flattening it, so the output
        // should still be a clean tone centred on A4.
        const auto out = runCorrector (in, fs, 12, 200.0);
        const double wetDb = harmonicRatioDb (out, fs, 440.0, n / 2, n / 4);

        check (wetDb > 25.0,
               "vibrato +/-" + std::to_string (static_cast<int> (depthCents))
                   + " cents stays musical (" + std::to_string (wetDb) + " dB)");
        std::printf ("    vibrato +/-%3.0f cents @5Hz -> %5.1f dB harmonic ratio\n",
                     depthCents, wetDb);
    }
}

// The FFT-accelerated YIN difference function must agree with the textbook
// double loop it replaced, and must make high sample rates affordable: the
// direct form needed ~2x realtime at 192 kHz, which is simply a dropout.
void testDetectionCost()
{
    const double fs    = 192000.0;
    const int    block = 256;
    const int    n     = static_cast<int> (fs * 2.0);

    autoedo::PsolaPitchCorrector c;
    c.prepare (fs, 2, block);
    c.setEdo (19);
    c.setRetuneMs (20.0);

    std::vector<float> l (static_cast<size_t> (block)), r (static_cast<size_t> (block));
    double ph = 0.0;
    const auto t0 = std::chrono::high_resolution_clock::now();
    for (int pos = 0; pos < n; pos += block)
    {
        for (int i = 0; i < block; ++i)
        {
            ph += 2.0 * kPi * 233.0 / fs;
            l[static_cast<size_t> (i)] = static_cast<float> (0.4 * (std::sin (ph) + 0.4 * std::sin (3.0 * ph)));
            r[static_cast<size_t> (i)] = l[static_cast<size_t> (i)];
        }
        float* ch[2] = { l.data(), r.data() };
        c.process (ch, 2, block);
    }
    const auto t1 = std::chrono::high_resolution_clock::now();
    const double rt = std::chrono::duration<double> (t1 - t0).count() / 2.0;

    // Measures ~0.15 on this machine; 0.75 leaves generous room for slow CI
    // hardware while still failing if the O(tauMax * window) form comes back.
    check (rt < 0.75, "192 kHz stereo runs in realtime (factor " + std::to_string (rt) + ")");
    std::printf ("    192 kHz stereo realtime factor = %.3f\n", rt);
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

        // Output quality, not just pitch accuracy.
        testTransparency (fs);
        testVibrato (fs);

        // Bug-fix regressions.
        testOversizedBlock (fs);
        testPrimingNoStorm (fs);
    }

    // High-sample-rate low-pitch detection (bug #3).
    testHighSampleRate();

    // Detection cost at the highest supported rate.
    testDetectionCost();

    if (failures == 0)
        std::printf ("  all DSP tests passed\n");
    else
        std::printf ("  %d DSP test(s) FAILED\n", failures);

    return failures == 0 ? 0 : 1;
}
