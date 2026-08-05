/* Self-tests for the C port: tuning math, YIN detection, pitch correction,
   the JSON helpers and the stub audio engine. Pure POSIX — runs anywhere
   (`make test`). */

#include "../src/audio.h"
#include "../src/audio_params.h"
#include "../src/json.h"
#include "../src/corrector.h"
#include "../src/shifter.h"
#include "../src/tuning.h"
#include "../src/yin.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Same portability split as main.c's ae_sleep_ms: mingw-w64 under strict
   C11 hides nanosleep, and Sleep() is always there on Windows. */
#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  static void st_sleep_ms (int ms) { Sleep ((DWORD) ms); }
#else
  #include <time.h>
  static void st_sleep_ms (int ms)
  {
      struct timespec ts = { ms / 1000, (long) (ms % 1000) * 1000000L };
      nanosleep (&ts, NULL);
  }
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int failures = 0;

#define CHECK(cond, ...)                                        \
    do {                                                        \
        if (! (cond))                                           \
        {                                                       \
            ++failures;                                         \
            printf ("FAIL %s:%d: ", __FILE__, __LINE__);        \
            printf (__VA_ARGS__);                               \
            printf ("\n");                                      \
        }                                                       \
    } while (0)

static void test_tuning (void)
{
    /* 12-EDO must reproduce 12-TET: A4 stays 440. */
    AeTuningResult r = ae_quantize_to_edo_scale (440.0, 12, NULL);
    CHECK (r.valid, "quantize valid");
    CHECK (fabs (r.target_hz - 440.0) < 1e-9, "A4 in 12-EDO: got %f", r.target_hz);
    CHECK (r.degree == 9 + 4 * 12, "A4 degree: got %d", r.degree);

    /* C stays anchored in any EDO. */
    for (int edo = 10; edo <= 72; ++edo)
    {
        r = ae_quantize_to_edo_scale (261.6255653005986, edo, NULL); /* C4 */
        CHECK (fabs (r.target_hz - 261.6255653005986) < 1e-6,
               "C4 anchored in %d-EDO: got %f", edo, r.target_hz);
    }

    /* Slightly sharp A4 still snaps to A4. */
    r = ae_quantize_to_edo_scale (445.0, 12, NULL);
    CHECK (fabs (r.target_hz - 440.0) < 1e-9, "sharp A4 snaps: got %f", r.target_hz);
    CHECK (r.cents_off > 0.0, "cents_off sign");

    /* Degree masking: only C enabled -> everything snaps to some C. */
    bool mask[AE_MAX_EDO] = { false };
    mask[0] = true;
    r = ae_quantize_to_edo_scale (440.0, 12, mask);
    CHECK (r.degree % 12 == 0, "masked degree: got %d", r.degree);
    const double octaves = log2 (r.target_hz / AE_REFERENCE_C0_HZ);
    CHECK (fabs (octaves - round (octaves)) < 1e-9, "masked target is a C: %f Hz", r.target_hz);

    /* All-disabled mask falls back to full chromatic. */
    bool none[AE_MAX_EDO] = { false };
    r = ae_quantize_to_edo_scale (440.0, 12, none);
    CHECK (fabs (r.target_hz - 440.0) < 1e-9, "empty mask falls back: got %f", r.target_hz);

    /* Custom root anchor: with A as root at 27.5 Hz (A0), A4 = 440 sits
       exactly on degree 4*12 of a 12-EDO grid. */
    r = ae_quantize_to_edo_scale_ex (441.0, 12, NULL, 27.5, 1200.0);
    CHECK (fabs (r.target_hz - 440.0) < 1e-9, "A-rooted grid: got %f", r.target_hz);
    CHECK (r.degree == 48, "A-rooted degree: got %d", r.degree);

    /* Octave stretch: period 1210 cents makes degree 12 land 10 cents high. */
    r = ae_quantize_to_edo_scale_ex (2.0 * 27.5, 12, NULL, 27.5, 1210.0);
    const double got_cents = 1200.0 * log2 (r.target_hz / 27.5);
    CHECK (r.degree == 12, "stretched degree: got %d", r.degree);
    CHECK (fabs (got_cents - 1210.0) < 1e-6, "stretched octave: %f cents", got_cents);

    /* Old 3-arg helper still means C-anchored true octave. */
    r = ae_quantize_to_edo_scale (440.0, 12, NULL);
    CHECK (fabs (r.target_hz - 440.0) < 1e-9, "compat wrapper: got %f", r.target_hz);
}

static void test_yin (void)
{
    AeYin y;
    memset (&y, 0, sizeof (y));
    ae_yin_prepare (&y, 48000.0, 4096, 60.0, 1600.0);

    float frame[4096];
    for (int i = 0; i < 4096; ++i)
        frame[i] = (float) (0.5 * sin (2.0 * M_PI * 220.0 * i / 48000.0));

    const AeYinResult r = ae_yin_process (&y, frame, 4096);
    CHECK (r.voiced, "220 Hz sine voiced");
    CHECK (fabs (r.frequency_hz - 220.0) < 1.0, "220 Hz sine: got %f", r.frequency_hz);
    ae_yin_free (&y);
}

/* The octave guard. A plucked string with a weak fundamental and alternating
   partial phases is the classic subharmonic trap: d'(tau) at the true period
   sits above the threshold while 2*tau dips below it, so an unguarded YIN
   reports the note an octave down -- the "guitar suddenly goes bassy" bug. */
static void test_yin_octave_guard (void)
{
    AeYin y;
    memset (&y, 0, sizeof (y));
    ae_yin_prepare (&y, 48000.0, 4096, 60.0, 1600.0);

    float frame[4096];
    const double f0 = 165.0; /* E3, low on a guitar but well inside the range */
    for (int i = 0; i < 4096; ++i)
    {
        const double t = 2.0 * M_PI * f0 * i / 48000.0;
        /* Fundamental barely there, the partials carrying the note, and
           every other one inverted so the waveform does not repeat cleanly
           until two periods have gone by. */
        frame[i] = (float) (0.04 * sin (t)
                          + 0.50 * sin (2.0 * t + 3.0)
                          - 0.45 * sin (3.0 * t)
                          + 0.40 * sin (4.0 * t + 1.0)
                          - 0.30 * sin (5.0 * t));
    }

    const AeYinResult r = ae_yin_process (&y, frame, 4096);
    CHECK (r.voiced, "octave guard: rich tone reads as voiced");
    CHECK (r.frequency_hz > f0 * 0.94,
           "octave guard: no subharmonic (got %.1f Hz, note is %.1f)",
           r.frequency_hz, f0);

    /* And the guard must not invent an octave: a pure tone stays put, and a
       genuinely low note is not dragged up to its own second partial. */
    for (int i = 0; i < 4096; ++i)
        frame[i] = (float) (0.5 * sin (2.0 * M_PI * 82.4 * i / 48000.0));
    const AeYinResult lo = ae_yin_process (&y, frame, 4096);
    CHECK (fabs (lo.frequency_hz - 82.4) < 1.5,
           "octave guard: low E stays low (got %.1f Hz)", lo.frequency_hz);
    ae_yin_free (&y);
}

static void test_correction (void)
{
    AeCorrector *p = calloc (1, sizeof (AeCorrector));
    ae_corrector_prepare (p, 48000.0, 512, 0.0, 0.0, AE_SHIFT_QUALITY_BALANCED); /* default detection range */
    ae_corrector_set_edo (p, 12);
    ae_corrector_set_retune_ms (p, 0.0);     /* hard snap */
    ae_corrector_set_transition_ms (p, 0.0); /* and hard degree changes */

    /* 30-cents-flat A3 (220 Hz * 2^(-30/1200)) for 1 second. */
    const double f_in = 220.0 * pow (2.0, -30.0 / 1200.0);
    const int total = 48000;
    float *buf = malloc ((size_t) total * sizeof (float));
    double phase = 0.0;
    for (int i = 0; i < total; ++i)
    {
        phase += 2.0 * M_PI * f_in / 48000.0;
        buf[i] = (float) (0.4 * sin (phase) + 0.2 * sin (2.0 * phase));
    }

    for (int off = 0; off < total; off += 512)
    {
        const int n = total - off < 512 ? total - off : 512;
        ae_corrector_process (p, buf + off, NULL, NULL, n);
    }

    CHECK (ae_corrector_voiced (p), "voiced after 1 s of tone");
    const float det = ae_corrector_detected_hz (p);
    const float tgt = ae_corrector_target_hz (p);
    CHECK (fabs (det - f_in) < 2.0, "detected %f (expected ~%f)", det, f_in);
    CHECK (fabs (tgt - 220.0) < 1e-2, "target %f (expected 220)", tgt);

    /* Output must contain signal (not silence, not NaN). */
    double energy = 0.0;
    int nan = 0;
    for (int i = total / 2; i < total; ++i)
    {
        if (isnan (buf[i])) ++nan;
        energy += (double) buf[i] * buf[i];
    }
    CHECK (nan == 0, "no NaNs in output");
    CHECK (energy > 1.0, "output has energy: %f", energy);

    /* Estimate the output pitch with a fresh YIN on the tail — it should sit
       at the corrected 220 Hz, not the flat input. */
    AeYin y;
    memset (&y, 0, sizeof (y));
    ae_yin_prepare (&y, 48000.0, 4096, 60.0, 1600.0);
    const AeYinResult r = ae_yin_process (&y, buf + total - 4096, 4096);
    CHECK (r.voiced, "output tail voiced");
    CHECK (fabs (r.frequency_hz - 220.0) < 2.0,
           "output pitch corrected: got %f (input was %f)", r.frequency_hz, f_in);
    ae_yin_free (&y);

    ae_corrector_free (p);
    free (p);
    free (buf);
}

/* Goertzel power of frequency f in buf. */
static double goertzel (const float *buf, int n, double f, double fs)
{
    const double w = 2.0 * M_PI * f / fs;
    const double coeff = 2.0 * cos (w);
    double s0 = 0.0, s1 = 0.0, s2 = 0.0;
    for (int i = 0; i < n; ++i)
    {
        s0 = buf[i] + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return s1 * s1 + s2 * s2 - coeff * s1 * s2;
}

static void test_walk (void)
{
    bool mask[AE_MAX_EDO] = { false };
    mask[0] = mask[4] = mask[7] = true; /* C-major-ish triad in 12 */

    CHECK (ae_walk_to_enabled (4, 12, mask) == 4, "walk: already lit");
    CHECK (ae_walk_to_enabled (5, 12, mask) == 4, "walk: 5 -> 4 (down at d=1... up first?)");
    /* Xentar: up checked first at each distance. 5+1=6 unlit, 5-1=4 lit -> 4.
       6: 6+1=7 lit -> 7 (up wins at the same distance). */
    CHECK (ae_walk_to_enabled (6, 12, mask) == 7, "walk: up wins ties, got %lld",
           ae_walk_to_enabled (6, 12, mask));
    CHECK (ae_walk_to_enabled (17, 12, mask) == 16, "walk: octave above, pc math");
    /* -2 is pc 10: up d=2 reaches pc 0 before down d=3 reaches pc 7 -> 0. */
    CHECK (ae_walk_to_enabled (-2, 12, mask) == 0, "walk: negative degrees, got %lld",
           ae_walk_to_enabled (-2, 12, mask));

    bool none[AE_MAX_EDO] = { false };
    CHECK (ae_walk_to_enabled (5, 12, none) == 5, "walk: empty mask unchanged");
}

static void test_harmony (void)
{
    AeCorrector *p = calloc (1, sizeof (AeCorrector));
    ae_corrector_prepare (p, 48000.0, 512, 0.0, 0.0, AE_SHIFT_QUALITY_BALANCED);
    ae_corrector_set_edo (p, 12);
    ae_corrector_set_retune_ms (p, 0.0);
    ae_corrector_set_transition_ms (p, 0.0);

    /* Voice 1: +7 steps (P5 above), centered. Voice 2: same -> must dedupe.
       Voice 3: +8 steps, mask-locked to a C-major-triad mask -> snaps to +7. */
    bool mask[AE_MAX_EDO] = { false };
    mask[0] = mask[4] = mask[7] = true;
    ae_corrector_set_enabled_degrees (p, mask, 12);

    AeHarmVoice voices[AE_HARM_VOICES];
    memset (voices, 0, sizeof (voices));
    for (int v = 0; v < AE_HARM_VOICES; ++v)
        voices[v].gain = 1.0;
    voices[0].interval = 7;
    voices[1].interval = 7; /* duplicate: must not double the level */
    voices[2].interval = 8; /* locks onto 7 via the mask -> deduped too */
    ae_corrector_set_harmony (p, true, 1, voices);

    /* A3 at exactly 220 Hz (also degree 9+3*12 of the C grid is 220 with
       default ref) so the corrected voice stays at 220 and the P5 ghost
       lands at 220 * 2^(7/12) = 329.63 Hz. */
    const int total = 48000;
    float *in = malloc ((size_t) total * sizeof (float));
    float *hl = malloc ((size_t) total * sizeof (float));
    float *hr = malloc ((size_t) total * sizeof (float));
    double phase = 0.0;
    for (int i = 0; i < total; ++i)
    {
        phase += 2.0 * M_PI * 220.0 / 48000.0;
        in[i] = (float) (0.4 * sin (phase) + 0.2 * sin (2.0 * phase));
    }
    for (int off = 0; off < total; off += 512)
    {
        const int n = total - off < 512 ? total - off : 512;
        ae_corrector_process (p, in + off, hl + off, hr + off, n);
    }

    CHECK (ae_corrector_voiced (p), "harmony test voiced");
    CHECK (ae_corrector_harm_degree (p, 0) != AE_HARM_DEG_OFF, "voice 1 sounding");
    /* Voices 2 and 3 still REPORT their degree (UI shows the chord) even
       though their audio deduped into voice 1's. */
    const int d0 = ae_corrector_harm_degree (p, 0);
    const int d2 = ae_corrector_harm_degree (p, 2);
    CHECK ((d0 % 12 + 12) % 12 == ((d2 % 12) + 12) % 12,
           "mask lock: +8 snapped to the fifth (deg0 %d deg2 %d)", d0, d2);

    /* Spectral check on the harmony bus: energy at 329.63, not at 220-only. */
    const int tail = 24000;
    const double p_fifth = goertzel (hl + total - tail, tail, 329.63, 48000.0);
    const double p_root  = goertzel (hl + total - tail, tail, 220.0, 48000.0);
    CHECK (p_fifth > 10.0 * p_root,
           "harmony bus is the fifth (P5 %.3g vs root %.3g)", p_fifth, p_root);

    /* Dedupe: re-run with ONLY voice 1 and compare harmony level (~equal). */
    double rms3 = 0.0;
    for (int i = total - tail; i < total; ++i) rms3 += (double) hl[i] * hl[i];

    ae_corrector_reset (p);
    ae_corrector_set_edo (p, 12);
    ae_corrector_set_retune_ms (p, 0.0);
    ae_corrector_set_transition_ms (p, 0.0);
    ae_corrector_set_enabled_degrees (p, mask, 12);
    voices[1].interval = 0;
    voices[2].interval = 0;
    ae_corrector_set_harmony (p, true, 1, voices);
    phase = 0.0;
    for (int i = 0; i < total; ++i)
    {
        phase += 2.0 * M_PI * 220.0 / 48000.0;
        in[i] = (float) (0.4 * sin (phase) + 0.2 * sin (2.0 * phase));
    }
    for (int off = 0; off < total; off += 512)
    {
        const int n = total - off < 512 ? total - off : 512;
        ae_corrector_process (p, in + off, hl + off, hr + off, n);
    }
    double rms1 = 0.0;
    for (int i = total - tail; i < total; ++i) rms1 += (double) hl[i] * hl[i];
    CHECK (rms3 < rms1 * 1.5 + 1e-9,
           "dedupe: 3 coincident voices no louder than 1 (%.4g vs %.4g)", rms3, rms1);

    ae_corrector_free (p);
    free (p);
    free (in); free (hl); free (hr);
}

static void test_synth_harmony (void)
{
    CHECK (ae_synth_patch_count() >= 5, "synth: patch table populated");
    CHECK (ae_synth_patch_find ("pad") == 0, "synth: pad is the default patch");
    CHECK (ae_synth_patch_find ("sine") >= 0, "synth: sine patch exists");
    CHECK (ae_synth_patch_find ("nope") == -1, "synth: unknown patch is -1");

    AeCorrector *p = calloc (1, sizeof (AeCorrector));
    ae_corrector_prepare (p, 48000.0, 512, 0.0, 0.0, AE_SHIFT_QUALITY_BALANCED);
    ae_corrector_set_edo (p, 12);
    ae_corrector_set_retune_ms (p, 0.0);
    ae_corrector_set_transition_ms (p, 0.0);

    AeHarmVoice voices[AE_HARM_VOICES];
    memset (voices, 0, sizeof (voices));
    for (int v = 0; v < AE_HARM_VOICES; ++v)
        voices[v].gain = 1.0;
    voices[0].interval = 7; /* P5: A3 in -> ghost at E4, 329.63 Hz */
    ae_corrector_set_harmony (p, true, 0, voices);
    ae_corrector_set_synth (p, AE_HARM_SRC_SYNTH,
                            ae_synth_patch_find ("sine"), 5.0, 200.0);

    /* One second of A3, then silence long enough to watch the release. */
    const int sung = 48000, quiet = 72000, total = sung + quiet;
    float *in = calloc ((size_t) total, sizeof (float));
    float *hl = malloc ((size_t) total * sizeof (float));
    float *hr = malloc ((size_t) total * sizeof (float));
    double phase = 0.0;
    for (int i = 0; i < sung; ++i)
    {
        phase += 2.0 * M_PI * 220.0 / 48000.0;
        in[i] = (float) (0.4 * sin (phase) + 0.2 * sin (2.0 * phase));
    }
    for (int off = 0; off < total; off += 512)
    {
        const int n = total - off < 512 ? total - off : 512;
        ae_corrector_process (p, in + off, hl + off, hr + off, n);
    }

    /* While sung: the harmony bus is the synth ghost at the fifth, and only
       the ghost -- the synth adds nothing at the sung root. */
    const double p_fifth = goertzel (hl + sung - 24000, 24000, 329.63, 48000.0);
    const double p_root  = goertzel (hl + sung - 24000, 24000, 220.0, 48000.0);
    CHECK (p_fifth > 10.0 * p_root,
           "synth ghost is the fifth (P5 %.3g vs root %.3g)", p_fifth, p_root);

    /* Volume match: the ghost sits at the sung level, like a shifted copy
       would. A centred voice at 0 dB reaches each side of the bus at UNITY
       -- the same level the mono lead reaches each side with -- so parity
       with the lead is parity with the input RMS, no pan-law discount. */
    double in_sq = 0.0, gl_sq = 0.0;
    for (int i = sung - 24000; i < sung; ++i)
    {
        in_sq += (double) in[i] * in[i];
        gl_sq += (double) hl[i] * hl[i];
    }
    const double want_rms = sqrt (in_sq / 24000.0);
    const double got_rms  = sqrt (gl_sq / 24000.0);
    CHECK (got_rms > want_rms * 0.6 && got_rms < want_rms * 1.6,
           "synth volume-matches the voice (got %.3g want %.3g)",
           got_rms, want_rms);

    /* Release: the pad keeps ringing at its pitch just after the voice
       stops (200 ms release), and has died out by the end. Equal windows --
       Goertzel power scales with window length. */
    const double p_sung = goertzel (hl + sung - 4800, 4800, 329.63, 48000.0);
    const double p_tail = goertzel (hl + sung + 2400, 4800, 329.63, 48000.0);
    CHECK (p_tail > p_sung * 0.05,
           "synth rings into the release (%.3g vs sung %.3g)", p_tail, p_sung);
    const double p_gone = goertzel (hl + total - 4800, 4800, 329.63, 48000.0);
    CHECK (p_gone < p_sung * 0.001,
           "synth release dies out (%.3g vs sung %.3g)", p_gone, p_sung);

    /* Switching back to the shifter source still produces the fifth (the
       idled shifters refill). */
    ae_corrector_set_synth (p, AE_HARM_SRC_VOICE, 0, 80.0, 500.0);
    phase = 0.0;
    for (int i = 0; i < sung; ++i)
    {
        phase += 2.0 * M_PI * 220.0 / 48000.0;
        in[i] = (float) (0.4 * sin (phase) + 0.2 * sin (2.0 * phase));
    }
    for (int off = 0; off < sung; off += 512)
        ae_corrector_process (p, in + off, hl + off, hr + off, 512);
    const double p_back = goertzel (hl + sung - 24000, 24000, 329.63, 48000.0);
    CHECK (p_back > 10.0 * goertzel (hl + sung - 24000, 24000, 220.0, 48000.0) || p_back > 1.0,
           "voice source works after synth (P5 %.3g)", p_back);

    ae_corrector_free (p);
    free (p);
    free (in); free (hl); free (hr);
}

/* Drive one steady tone through a corrector and report the RMS of the lead
   and of the harmony bus's left side. Shared by the two tests below. */
static void harm_levels (int source, double master_lin, double voice_gain,
                         double *lead_rms, double *harm_rms, double *in_rms)
{
    AeCorrector *p = calloc (1, sizeof (AeCorrector));
    ae_corrector_prepare (p, 48000.0, 512, 0.0, 0.0, AE_SHIFT_QUALITY_BALANCED);
    ae_corrector_set_edo (p, 12);
    ae_corrector_set_retune_ms (p, 0.0);
    ae_corrector_set_transition_ms (p, 0.0);

    AeHarmVoice voices[AE_HARM_VOICES];
    memset (voices, 0, sizeof (voices));
    for (int v = 0; v < AE_HARM_VOICES; ++v)
        voices[v].gain = 1.0;
    voices[0].interval = 12;         /* an equave up: one voice, centred */
    voices[0].gain     = voice_gain;
    ae_corrector_set_harmony (p, true, 0, voices);
    ae_corrector_set_harm_master (p, master_lin);
    ae_corrector_set_synth (p, source, ae_synth_patch_find ("sine"), 5.0, 200.0);

    const int total = 96000;
    float *in = calloc ((size_t) total, sizeof (float));
    float *hl = calloc ((size_t) total, sizeof (float));
    float *hr = calloc ((size_t) total, sizeof (float));
    double phase = 0.0;
    for (int i = 0; i < total; ++i)
    {
        phase += 2.0 * M_PI * 220.0 / 48000.0;
        in[i] = (float) (0.4 * sin (phase));
    }
    float *lead = malloc ((size_t) total * sizeof (float));
    memcpy (lead, in, (size_t) total * sizeof (float));
    for (int off = 0; off < total; off += 512)
    {
        const int n = total - off < 512 ? total - off : 512;
        ae_corrector_process (p, lead + off, hl + off, hr + off, n);
    }

    /* Measure over the last half second, well past every ramp. */
    const int w = 24000, s = total - w;
    double l_sq = 0.0, h_sq = 0.0, i_sq = 0.0;
    for (int i = s; i < total; ++i)
    {
        l_sq += (double) lead[i] * lead[i];
        h_sq += (double) hl[i] * hl[i];
        i_sq += (double) in[i] * in[i];
    }
    *lead_rms = sqrt (l_sq / w);
    *harm_rms = sqrt (h_sq / w);
    *in_rms   = sqrt (i_sq / w);

    ae_corrector_free (p);
    free (p); free (in); free (hl); free (hr); free (lead);
}

/* harmGainDb: one master over the whole ghost bus, on top of the per-voice
   trims, that never touches the lead. Plus the parity the master rides on --
   a 0 dB voice sits at the lead's level rather than 3 dB under it. */
static void test_harm_master (void)
{
    double lead0, harm0, in0;
    harm_levels (AE_HARM_SRC_SYNTH, 1.0, 1.0, &lead0, &harm0, &in0);
    CHECK (harm0 > in0 * 0.8 && harm0 < in0 * 1.25,
           "harmony parity: 0 dB synth ghost sits at the lead's level "
           "(ghost %.4g, input %.4g)", harm0, in0);

    double leadS, harmS, inS;
    harm_levels (AE_HARM_SRC_VOICE, 1.0, 1.0, &leadS, &harmS, &inS);
    CHECK (harmS > inS * 0.7 && harmS < inS * 1.4,
           "harmony parity: 0 dB shifted ghost sits at the lead's level "
           "(ghost %.4g, input %.4g)", harmS, inS);

    /* -12 dB master: a quarter of the amplitude on the bus, and the lead
       bit-for-bit untouched. */
    double lead1, harm1, in1;
    harm_levels (AE_HARM_SRC_SYNTH, 0.25, 1.0, &lead1, &harm1, &in1);
    CHECK (fabs (harm1 - harm0 * 0.25) < harm0 * 0.02,
           "harmGainDb scales the ghost bus (%.4g vs %.4g expected)",
           harm1, harm0 * 0.25);
    CHECK (fabs (lead1 - lead0) < lead0 * 1e-6,
           "harmGainDb leaves the lead alone (%.6g vs %.6g)", lead1, lead0);

    /* Per-voice trim rides ON TOP of the master, not instead of it: -6 dB
       on the voice under a -12 dB master is -18 dB in total. */
    double lead2, harm2, in2;
    harm_levels (AE_HARM_SRC_SYNTH, 0.25, 0.5, &lead2, &harm2, &in2);
    CHECK (fabs (harm2 - harm0 * 0.125) < harm0 * 0.02,
           "per-voice trim multiplies the master (%.4g vs %.4g expected)",
           harm2, harm0 * 0.125);

    /* And the master reaches the shifted voices too, not just the synth. */
    double lead3, harm3, in3;
    harm_levels (AE_HARM_SRC_VOICE, 0.25, 1.0, &lead3, &harm3, &in3);
    CHECK (fabs (harm3 - harmS * 0.25) < harmS * 0.05,
           "harmGainDb reaches shifted ghosts (%.4g vs %.4g expected)",
           harm3, harmS * 0.25);
}

/* synthAttackMs / synthReleaseMs really shape the ghost envelope: they are
   read live off the config every block, and a longer release measurably
   holds the pad up after the input stops. */
static void test_synth_envelope (void)
{
    const int sung = 48000, quiet = 48000, total = sung + quiet;
    const double rel_ms[2] = { 50.0, 1500.0 };
    double tail[2];

    for (int c = 0; c < 2; ++c)
    {
        AeCorrector *p = calloc (1, sizeof (AeCorrector));
        ae_corrector_prepare (p, 48000.0, 512, 0.0, 0.0, AE_SHIFT_QUALITY_BALANCED);
        ae_corrector_set_edo (p, 12);
        ae_corrector_set_retune_ms (p, 0.0);
        ae_corrector_set_transition_ms (p, 0.0);

        AeHarmVoice voices[AE_HARM_VOICES];
        memset (voices, 0, sizeof (voices));
        for (int v = 0; v < AE_HARM_VOICES; ++v)
            voices[v].gain = 1.0;
        voices[0].interval = 7;
        ae_corrector_set_harmony (p, true, 0, voices);
        ae_corrector_set_synth (p, AE_HARM_SRC_SYNTH,
                                ae_synth_patch_find ("sine"), 5.0, rel_ms[c]);

        float *in = calloc ((size_t) total, sizeof (float));
        float *hl = calloc ((size_t) total, sizeof (float));
        float *hr = calloc ((size_t) total, sizeof (float));
        double phase = 0.0;
        for (int i = 0; i < sung; ++i)
        {
            phase += 2.0 * M_PI * 220.0 / 48000.0;
            in[i] = (float) (0.4 * sin (phase) + 0.2 * sin (2.0 * phase));
        }
        for (int off = 0; off < total; off += 512)
        {
            const int n = total - off < 512 ? total - off : 512;
            ae_corrector_process (p, in + off, hl + off, hr + off, n);
        }

        /* Level 300 ms after the input stopped: six release constants for
           the short setting, a fifth of one for the long. */
        double sq = 0.0;
        for (int i = sung + 12000; i < sung + 16800; ++i)
            sq += (double) hl[i] * hl[i];
        tail[c] = sqrt (sq / 4800.0);

        ae_corrector_free (p);
        free (p); free (in); free (hl); free (hr);
    }

    CHECK (tail[1] > tail[0] * 20.0,
           "synthReleaseMs shapes the tail (50 ms %.3g vs 1500 ms %.3g)",
           tail[0], tail[1]);

    /* Attack: a slow attack must not be at full level a few milliseconds in.
       Measured on the synth LEAD, whose envelope is the input crossfade, so
       this is specifically the ghost's own attack being tested. */
    const double atk_ms[2] = { 1.0, 1000.0 };
    double onset[2];
    for (int c = 0; c < 2; ++c)
    {
        AeCorrector *p = calloc (1, sizeof (AeCorrector));
        ae_corrector_prepare (p, 48000.0, 512, 0.0, 0.0, AE_SHIFT_QUALITY_BALANCED);
        ae_corrector_set_edo (p, 12);
        ae_corrector_set_retune_ms (p, 0.0);
        ae_corrector_set_transition_ms (p, 0.0);

        AeHarmVoice voices[AE_HARM_VOICES];
        memset (voices, 0, sizeof (voices));
        for (int v = 0; v < AE_HARM_VOICES; ++v)
            voices[v].gain = 1.0;
        voices[0].interval = 7;
        ae_corrector_set_harmony (p, true, 0, voices);
        ae_corrector_set_synth (p, AE_HARM_SRC_SYNTH,
                                ae_synth_patch_find ("sine"), atk_ms[c], 200.0);

        float *in = calloc ((size_t) sung, sizeof (float));
        float *hl = calloc ((size_t) sung, sizeof (float));
        float *hr = calloc ((size_t) sung, sizeof (float));
        double phase = 0.0;
        for (int i = 0; i < sung; ++i)
        {
            phase += 2.0 * M_PI * 220.0 / 48000.0;
            in[i] = (float) (0.4 * sin (phase) + 0.2 * sin (2.0 * phase));
        }
        for (int off = 0; off < sung; off += 512)
        {
            const int n = sung - off < 512 ? sung - off : 512;
            ae_corrector_process (p, in + off, hl + off, hr + off, n);
        }

        /* Peak reached in the first 100 ms of the ghost sounding, relative
           to the settled level at the end of the note. */
        double pk = 0.0;
        int first = -1;
        for (int i = 0; i < sung; ++i)
            if (fabs (hl[i]) > 1e-4) { first = i; break; }
        if (first >= 0)
            for (int i = first; i < first + 4800 && i < sung; ++i)
                if (fabs (hl[i]) > pk) pk = fabs (hl[i]);
        double settled = 0.0;
        for (int i = sung - 4800; i < sung; ++i)
            if (fabs (hl[i]) > settled) settled = fabs (hl[i]);
        onset[c] = settled > 0.0 ? pk / settled : 0.0;

        ae_corrector_free (p);
        free (p); free (in); free (hl); free (hr);
    }
    CHECK (onset[0] > 0.8,
           "synthAttackMs: a 1 ms attack is up immediately (%.3g)", onset[0]);
    CHECK (onset[1] < 0.35,
           "synthAttackMs: a 1 s attack is still climbing (%.3g vs fast %.3g)",
           onset[1], onset[0]);
}

/* Peak-search the strongest partial near `guess` (Goertzel over a fine grid).
   YIN would average the inharmonic upper partials; this reads the one
   frequency being asked about. */
static double peak_near (const float *buf, int n, double guess, double fs)
{
    double best_f = guess, best_p = -1.0;
    for (double f = guess * 0.94; f <= guess * 1.06; f += guess * 0.0005)
    {
        const double p = goertzel (buf, n, f, fs);
        if (p > best_p) { best_p = p; best_f = f; }
    }
    return best_f;
}

/* A ghost takes its INTERVAL from the snapped degrees but stacks it on the
   pitch actually being heard as the lead. With the lead only half-corrected,
   a ghost pinned to the degree's ideal frequency would beat against it; this
   pins the failure at 20 cents, which is audible and was the report. */
static void test_harmony_anchor (void)
{
    /* A3 (degree 45 at 12-EDO off the C0 anchor), sung 40 cents sharp, with
       Amount at 0.5 -- so the lead comes out 20 cents sharp of 220. */
    const double sharp_c = 40.0;
    const double in_hz   = 220.0 * pow (2.0, sharp_c / 1200.0);
    const double lead_hz = 220.0 * pow (2.0, 0.5 * sharp_c / 1200.0);
    const double fifth   = pow (2.0, 7.0 / 12.0);

    AeCorrector *p = calloc (1, sizeof (AeCorrector));
    ae_corrector_prepare (p, 48000.0, 512, 0.0, 0.0, AE_SHIFT_QUALITY_BALANCED);
    ae_corrector_set_edo (p, 12);
    ae_corrector_set_retune_ms (p, 0.0);
    ae_corrector_set_transition_ms (p, 0.0);
    ae_corrector_set_amount (p, 0.5);

    AeHarmVoice voices[AE_HARM_VOICES];
    memset (voices, 0, sizeof (voices));
    for (int v = 0; v < AE_HARM_VOICES; ++v)
        voices[v].gain = 1.0;
    voices[0].interval = 7;
    ae_corrector_set_harmony (p, true, 1 /* mask lock */, voices);
    ae_corrector_set_synth (p, AE_HARM_SRC_SYNTH,
                            ae_synth_patch_find ("sine"), 5.0, 200.0);

    const int total = 98304;
    float *in = calloc ((size_t) total, sizeof (float));
    float *hl = calloc ((size_t) total, sizeof (float));
    float *hr = calloc ((size_t) total, sizeof (float));
    double phase = 0.0;
    for (int i = 0; i < total; ++i)
    {
        phase += 2.0 * M_PI * in_hz / 48000.0;
        in[i] = (float) (0.4 * sin (phase) + 0.15 * sin (2.0 * phase));
    }
    for (int off = 0; off < total; off += 512)
    {
        const int n = total - off < 512 ? total - off : 512;
        ae_corrector_process (p, in + off, hl + off, hr + off, n);
    }

    const double want = lead_hz * fifth;         /* on the audible lead */
    const double ideal = 220.0 * fifth;          /* on the degree's ideal */
    const double got = peak_near (hl + total - 48000, 48000, want, 48000.0);
    const double err_c = 1200.0 * log2 (got / want);
    CHECK (fabs (err_c) < 6.0,
           "harmony anchors to the audible lead (%.2f Hz, wanted %.2f, "
           "%.1f cents off; the old degree-anchored answer was %.2f)",
           got, want, err_c, ideal);

    /* With Amount back at 1 the two definitions coincide, so a fully
       corrected lead is unchanged by any of this. */
    ae_corrector_set_amount (p, 1.0);
    for (int off = 0; off < total; off += 512)
    {
        const int n = total - off < 512 ? total - off : 512;
        float blk[512];
        memcpy (blk, in + off, (size_t) n * sizeof (float));
        ae_corrector_process (p, blk, hl + off, hr + off, n);
    }
    const double got1 = peak_near (hl + total - 48000, 48000, ideal, 48000.0);
    CHECK (fabs (1200.0 * log2 (got1 / ideal)) < 6.0,
           "a fully corrected lead still gives the exact degree (%.2f Hz, "
           "wanted %.2f)", got1, ideal);

    ae_corrector_free (p);
    free (p); free (in); free (hl); free (hr);
}

static void test_midi_harmony (void)
{
    AeCorrector *p = calloc (1, sizeof (AeCorrector));
    ae_corrector_prepare (p, 48000.0, 512, 0.0, 0.0, AE_SHIFT_QUALITY_BALANCED);
    ae_corrector_set_edo (p, 12);
    ae_corrector_set_retune_ms (p, 0.0);
    ae_corrector_set_transition_ms (p, 0.0);

    /* Full-chromatic mask, but hold C4+E4+G4 (60/64/67): with MIDI mode on,
       an A3 input must retune to the nearest held note = C4 (261.63 Hz),
       and a +7 mask-locked harmony voice must land within the held pcs. */
    AeHarmVoice voices[AE_HARM_VOICES];
    memset (voices, 0, sizeof (voices));
    for (int v = 0; v < AE_HARM_VOICES; ++v) voices[v].gain = 1.0;
    voices[0].interval = 7;
    ae_corrector_set_harmony (p, true, 1, voices);
    ae_corrector_set_midi (p, true, (1ull << 60) | (1ull << 63), 1ull << (67 - 64));
    /* note: bit 63 is note 63 (D#4) — held set {60, 63, 67} */

    const int total = 48000;
    float *in = malloc ((size_t) total * sizeof (float));
    float *hl = malloc ((size_t) total * sizeof (float));
    float *hr = malloc ((size_t) total * sizeof (float));
    double phase = 0.0;
    for (int i = 0; i < total; ++i)
    {
        phase += 2.0 * M_PI * 220.0 / 48000.0;
        in[i] = (float) (0.4 * sin (phase) + 0.2 * sin (2.0 * phase));
    }
    for (int off = 0; off < total; off += 512)
    {
        const int n = total - off < 512 ? total - off : 512;
        ae_corrector_process (p, in + off, hl + off, hr + off, n);
    }

    CHECK (ae_corrector_voiced (p), "midi test voiced");
    const float tgt = ae_corrector_target_hz (p);
    CHECK (fabs (tgt - 261.63) < 0.5,
           "A3 retunes to held C4: got %.2f (expect 261.63)", tgt);
    /* voice +7 from C4 (deg 48) = 55 (pc 7 = G, held) -> stays 55 */
    const int d0 = ae_corrector_harm_degree (p, 0);
    CHECK (d0 == 55, "harmony locks to held pcs: got %d (expect 55)", d0);

    /* Release all notes: normal behavior resumes (A3 stays 220 on the
       chromatic mask). */
    ae_corrector_set_midi (p, true, 0, 0);
    phase = 0.0;
    for (int i = 0; i < total; ++i)
    {
        phase += 2.0 * M_PI * 220.0 / 48000.0;
        in[i] = (float) (0.4 * sin (phase) + 0.2 * sin (2.0 * phase));
    }
    for (int off = 0; off < total; off += 512)
    {
        const int n = total - off < 512 ? total - off : 512;
        ae_corrector_process (p, in + off, hl + off, hr + off, n);
    }
    CHECK (fabs (ae_corrector_target_hz (p) - 220.0) < 0.5,
           "release -> normal behavior: got %.2f (expect 220)", ae_corrector_target_hz (p));

    ae_corrector_free (p);
    free (p); free (in); free (hl); free (hr);
}

static void test_json (void)
{
    const char *j = "{\"edo\":19, \"retuneMs\": 42.5, \"bypass\":true,"
                    "\"name\":\"Mac \\\"Pro\\\" \\u00e9\", "
                    "\"nested\":{\"edo\":99,\"a\":[1,2]}, "
                    "\"degrees\":[1,0,true,false,1]}";
    double d;
    bool b;
    char s[64];
    unsigned char flags[8];

    CHECK (ae_json_get_number (j, "edo", &d) && d == 19.0, "edo");
    CHECK (ae_json_get_number (j, "retuneMs", &d) && d == 42.5, "retuneMs");
    CHECK (ae_json_get_bool (j, "bypass", &b) && b, "bypass");
    CHECK (ae_json_get_string (j, "name", s, sizeof (s)), "name parse");
    CHECK (strcmp (s, "Mac \"Pro\" ?") == 0, "name value: '%s'", s);
    CHECK (! ae_json_get_number (j, "a", &d), "nested keys are not top-level");
    const int n = ae_json_get_flag_array (j, "degrees", flags, 8);
    CHECK (n == 5, "degrees count: %d", n);
    CHECK (flags[0] == 1 && flags[1] == 0 && flags[2] == 1 && flags[3] == 0 && flags[4] == 1,
           "degrees values");
    CHECK (! ae_json_get_number (j, "missing", &d), "missing key");

    const char *j2 = "{\"hm\":[7,-5,0,12,-24],\"hp\":[0,-0.5,1,0,0]}";
    double nums[5];
    CHECK (ae_json_get_num_array (j2, "hm", nums, 5) == 5, "num array count");
    CHECK (nums[0] == 7 && nums[1] == -5 && nums[4] == -24, "num array values");
    CHECK (ae_json_get_num_array (j2, "hp", nums, 5) == 5 && nums[1] == -0.5,
           "num array floats");
    CHECK (ae_json_get_num_array (j2, "nope", nums, 5) == -1, "num array missing");

    char esc[64] = "";
    ae_json_escape_append (esc, sizeof (esc), "a\"b\\c\nd");
    CHECK (strcmp (esc, "a\\\"b\\\\c\\u000ad") == 0, "escape: '%s'", esc);
}

/* Run a stub engine and report the pitch it settles on. The stub backend
   sings a different note per capture channel (A3 on 1, D4 on 2), so the
   settled pitch tells us which channel `input_channel` bound. Returns 0.0
   if the engine never reports a voiced detection. */
static double engine_settled_hz (int input_channel)
{
    AeEngineConfig cfg;
    memset (&cfg, 0, sizeof (cfg));
    cfg.input_channel      = input_channel;
    cfg.buffer_frames      = 256;
    cfg.params.edo         = 12;
    cfg.params.amount      = 0.0; /* detection is what we probe, not correction */
    cfg.params.degrees_lo  = ~0ull;
    cfg.params.degrees_hi  = 0xffull;

    char err[256] = "";
    AeAudioEngine *e = ae_audio_engine_start (&cfg, err, sizeof (err));
    if (e == NULL)
    {
        printf ("engine start failed: %s\n", err);
        return 0.0;
    }

    /* Wait for the first voiced detection, then let it settle for ~250 ms
       more so an onset octave-glitch can't decide the reading. */
    double hz = 0.0;
    int settle = -1;
    for (int tick = 0; tick < 100 && settle != 0; ++tick) /* ≤ ~5 s */
    {
        st_sleep_ms (50);
        AeEngineStatus st;
        ae_audio_engine_get_status (e, &st);
        if (st.voiced && st.detected_hz > 0.0f)
        {
            hz = st.detected_hz;
            if (settle < 0)
                settle = 5;
            else
                --settle;
        }
    }
    ae_audio_engine_stop (e);
    return hz;
}

static void test_engine_channels (void)
{
    /* Default channel handling: the stub tone is A3 (220 Hz ± 35-cent wobble). */
    double hz = engine_settled_hz (0);
    CHECK (hz > 210.0 && hz < 230.0, "default channel detects A3: got %.1f Hz", hz);

    /* Channel 1 is the same voice as the default. */
    hz = engine_settled_hz (1);
    CHECK (hz > 210.0 && hz < 230.0, "channel 1 detects A3: got %.1f Hz", hz);

    /* Channel 2 carries a different note (D4, 293.66 Hz). */
    hz = engine_settled_hz (2);
    CHECK (hz > 280.0 && hz < 308.0, "channel 2 detects D4: got %.1f Hz", hz);

    /* A channel past the device's count refuses to start, with a message —
       on either side of the engine. */
    AeEngineConfig cfg;
    memset (&cfg, 0, sizeof (cfg));
    cfg.input_channel = 3;
    cfg.params.edo    = 12;
    char err[256] = "";
    AeAudioEngine *e = ae_audio_engine_start (&cfg, err, sizeof (err));
    CHECK (e == NULL, "input channel 3 on a 2-channel device fails to start");
    CHECK (strstr (err, "no input channel 3") != NULL, "error names the channel: '%s'", err);
    if (e != NULL)
        ae_audio_engine_stop (e);

    memset (&cfg, 0, sizeof (cfg));
    cfg.output_channel = 3;
    cfg.params.edo     = 12;
    err[0] = '\0';
    e = ae_audio_engine_start (&cfg, err, sizeof (err));
    CHECK (e == NULL, "output channel 3 on a 2-channel sink fails to start");
    CHECK (strstr (err, "no output channel 3") != NULL, "error names the channel: '%s'", err);
    if (e != NULL)
        ae_audio_engine_stop (e);

    /* A valid output channel runs; the stub sink is silent either way. */
    memset (&cfg, 0, sizeof (cfg));
    cfg.output_channel = 2;
    cfg.params.edo     = 12;
    err[0] = '\0';
    e = ae_audio_engine_start (&cfg, err, sizeof (err));
    CHECK (e != NULL, "output channel 2 starts: %s", err);
    ae_audio_engine_stop (e);
}

static void test_synth_string_machine (void)
{
    /* The string-machine patches exist and carry the ensemble; the plain
       ones do not (a pad that suddenly swirled would be a regression). */
    const int strings = ae_synth_patch_find ("strings");
    const int choir   = ae_synth_patch_find ("choir");
    const int brass   = ae_synth_patch_find ("brass");
    CHECK (strings >= 0, "strings patch exists");
    CHECK (choir >= 0, "choir patch exists");
    CHECK (brass >= 0, "brass patch exists");

    /* Render a held note through `strings` and through `sine`, and compare
       what the ensemble does to the stereo image. */
    const int total = 96000; /* 2 s: several sweeps of the slowest LFO */
    float *in = malloc ((size_t) total * sizeof (float));
    float *hl = malloc ((size_t) total * sizeof (float));
    float *hr = malloc ((size_t) total * sizeof (float));
    double phase = 0.0;
    for (int i = 0; i < total; ++i)
    {
        phase += 2.0 * M_PI * 220.0 / 48000.0;
        in[i] = (float) (0.4 * sin (phase) + 0.2 * sin (2.0 * phase));
    }

    /* Centre-panned single voice: without an ensemble L and R are identical,
       with one they decorrelate. That difference IS the effect. */
    double corr[2] = { 0.0, 0.0 };
    double rms_out[2] = { 0.0, 0.0 };
    for (int pass = 0; pass < 2; ++pass)
    {
        AeCorrector *p = calloc (1, sizeof (AeCorrector));
        ae_corrector_prepare (p, 48000.0, 512, 0.0, 0.0, AE_SHIFT_QUALITY_BALANCED);
        ae_corrector_set_edo (p, 12);
        ae_corrector_set_retune_ms (p, 0.0);
        ae_corrector_set_transition_ms (p, 0.0);
        AeHarmVoice voices[AE_HARM_VOICES];
        memset (voices, 0, sizeof (voices));
        for (int v = 0; v < AE_HARM_VOICES; ++v)
            voices[v].gain = 1.0;
        voices[0].interval = 7;
        ae_corrector_set_harmony (p, true, 0, voices);
        ae_corrector_set_synth (p, AE_HARM_SRC_SYNTH,
                                pass == 0 ? ae_synth_patch_find ("sine") : strings,
                                5.0, 200.0);
        for (int off = 0; off < total; off += 512)
        {
            const int n = total - off < 512 ? total - off : 512;
            ae_corrector_process (p, in + off, hl + off, hr + off, n);
        }
        /* Normalised L/R correlation over the settled tail. */
        double sxy = 0.0, sxx = 0.0, syy = 0.0, sq = 0.0;
        for (int i = total / 2; i < total; ++i)
        {
            sxy += (double) hl[i] * hr[i];
            sxx += (double) hl[i] * hl[i];
            syy += (double) hr[i] * hr[i];
            sq  += (double) hl[i] * hl[i];
        }
        corr[pass] = (sxx > 1e-12 && syy > 1e-12) ? sxy / sqrt (sxx * syy) : 1.0;
        rms_out[pass] = sqrt (sq / (total / 2));
        ae_corrector_free (p);
        free (p);
    }
    CHECK (corr[0] > 0.99, "no ensemble: L and R stay identical (%.4f)", corr[0]);
    CHECK (corr[1] < 0.95, "ensemble decorrelates the stereo image (%.4f)", corr[1]);
    CHECK (rms_out[1] > 0.2 * rms_out[0] && rms_out[1] < 5.0 * rms_out[0],
           "ensemble keeps a sane level (%.4g vs %.4g)", rms_out[1], rms_out[0]);

    /* And the movement is real: an ensembled note's short-window level
       varies over time, where a static patch's does not. */
    AeCorrector *p = calloc (1, sizeof (AeCorrector));
    ae_corrector_prepare (p, 48000.0, 512, 0.0, 0.0, AE_SHIFT_QUALITY_BALANCED);
    ae_corrector_set_edo (p, 12);
    ae_corrector_set_retune_ms (p, 0.0);
    ae_corrector_set_transition_ms (p, 0.0);
    AeHarmVoice voices[AE_HARM_VOICES];
    memset (voices, 0, sizeof (voices));
    for (int v = 0; v < AE_HARM_VOICES; ++v)
        voices[v].gain = 1.0;
    voices[0].interval = 7;
    ae_corrector_set_harmony (p, true, 0, voices);
    ae_corrector_set_synth (p, AE_HARM_SRC_SYNTH, strings, 5.0, 200.0);
    for (int off = 0; off < total; off += 512)
    {
        const int n = total - off < 512 ? total - off : 512;
        ae_corrector_process (p, in + off, hl + off, hr + off, n);
    }
    double lo = 1e9, hi = 0.0;
    for (int w = total / 2; w + 4800 <= total; w += 4800)
    {
        double s = 0.0;
        for (int i = w; i < w + 4800; ++i)
            s += (double) hl[i] * hl[i];
        s = sqrt (s / 4800.0);
        if (s < lo) lo = s;
        if (s > hi) hi = s;
    }
    CHECK (hi > lo * 1.02, "ensemble breathes (window RMS %.4g..%.4g)", lo, hi);
    /* No NaNs escaping the delay lines. */
    int nan = 0;
    for (int i = 0; i < total; ++i)
        if (isnan (hl[i]) || isnan (hr[i])) ++nan;
    CHECK (nan == 0, "ensemble output is finite");
    ae_corrector_free (p);
    free (p);
    free (in); free (hl); free (hr);
}

/* Build a corrector with one P5 harmony voice on a 220 Hz input, run it,
   and hand back the buses. Shared by the source-routing tests below. */
static void run_synth_case_ex (int sources[AE_HARM_VOICES], int lead, double vowel,
                               const char *patch, int bright, float *out_mono,
                               float *out_hl, float *out_hr, int total)
{
    AeCorrector *p = calloc (1, sizeof (AeCorrector));
    ae_corrector_prepare (p, 48000.0, 512, 0.0, 0.0, AE_SHIFT_QUALITY_BALANCED);
    ae_corrector_set_edo (p, 12);
    ae_corrector_set_retune_ms (p, 0.0);
    ae_corrector_set_transition_ms (p, 0.0);
    AeHarmVoice voices[AE_HARM_VOICES];
    memset (voices, 0, sizeof (voices));
    for (int v = 0; v < AE_HARM_VOICES; ++v)
        voices[v].gain = 1.0;
    voices[0].interval = 7;  /* P5 above */
    voices[1].interval = 4;  /* M3 above */
    ae_corrector_set_harmony (p, true, 0, voices);
    ae_corrector_set_synth (p, AE_HARM_SRC_SYNTH, ae_synth_patch_find (patch),
                            5.0, 200.0);
    ae_corrector_set_voice_sources (p, sources, lead);
    ae_corrector_set_synth_shape (p, 1.0, vowel, 0.0, AE_VOWEL_MODE_VOCODER);

    double phase = 0.0;
    for (int i = 0; i < total; ++i)
    {
        phase += 2.0 * M_PI * 220.0 / 48000.0;
        /* Two "vowels" at the same pitch: a dark one whose energy sits on
           the low harmonics, and a bright one with a high formant cluster.
           Same f0, so correction and the ghosts are identical -- only the
           timbre the vocoder can transfer differs. */
        out_mono[i] = bright
            ? (float) (0.35 * sin (phase) + 0.30 * sin (8.0 * phase)
                     + 0.30 * sin (10.0 * phase))
            : (float) (0.45 * sin (phase) + 0.30 * sin (2.0 * phase));
    }
    for (int off = 0; off < total; off += 512)
    {
        const int n = total - off < 512 ? total - off : 512;
        ae_corrector_process (p, out_mono + off, out_hl + off, out_hr + off, n);
    }
    ae_corrector_free (p);
    free (p);
}

static void run_synth_case (int sources[AE_HARM_VOICES], int lead, double vowel,
                            const char *patch, float *out_mono,
                            float *out_hl, float *out_hr, int total)
{
    run_synth_case_ex (sources, lead, vowel, patch, /*bright=*/0,
                       out_mono, out_hl, out_hr, total);
}

static void test_synth_sources_and_vowel (void)
{
    CHECK (ae_synth_patch_find ("bass") >= 0, "bass patch exists");
    CHECK (ae_synth_patch_find ("solina bright") >= 0, "solina bright exists");

    const int total = 48000;
    float *mono = malloc ((size_t) total * sizeof (float));
    float *hl = malloc ((size_t) total * sizeof (float));
    float *hr = malloc ((size_t) total * sizeof (float));
    int src[AE_HARM_VOICES];

    /* Per-voice routing: voice 1 shifted, voice 2 synth. Both must sound --
       the shifter pass and the synth pass each render their own and add to
       one bus, so neither can silence the other. */
    for (int v = 0; v < AE_HARM_VOICES; ++v)
        src[v] = AE_HARM_SRC_DEFAULT;
    src[0] = AE_HARM_SRC_VOICE;
    src[1] = AE_HARM_SRC_SYNTH;
    run_synth_case (src, AE_HARM_SRC_VOICE, 0.0, "sine", mono, hl, hr, total);
    const int tail = 24000;
    const double p5 = goertzel (hl + total - tail, tail, 329.63, 48000.0);  /* +7 */
    const double m3 = goertzel (hl + total - tail, tail, 277.18, 48000.0);  /* +4 */
    CHECK (p5 > 1.0, "mixed sources: the shifted voice sounds (%.3g)", p5);
    CHECK (m3 > 1.0, "mixed sources: the synth voice sounds (%.3g)", m3);

    /* All voices explicitly shifted: the synth adds nothing, so the harmony
       bus has no energy at a degree only the synth would have produced. */
    for (int v = 0; v < AE_HARM_VOICES; ++v)
        src[v] = AE_HARM_SRC_VOICE;
    run_synth_case (src, AE_HARM_SRC_VOICE, 0.0, "sine", mono, hl, hr, total);
    const double p5_all_shift = goertzel (hl + total - tail, tail, 329.63, 48000.0);
    CHECK (p5_all_shift > 1.0, "all-shifted still harmonises (%.3g)", p5_all_shift);

    /* The LEAD as a synth: the corrected output is an oscillator at the
       target, and the input's own timbre is gone -- the sung 3rd and 4th
       harmonics do not survive into a sine-patch lead. */
    for (int v = 0; v < AE_HARM_VOICES; ++v)
        src[v] = AE_HARM_SRC_DEFAULT;
    run_synth_case (src, AE_HARM_SRC_SYNTH, 0.0, "sine", mono, hl, hr, total);
    const double lead_f0 = goertzel (mono + total - tail, tail, 220.0, 48000.0);
    const double lead_h3 = goertzel (mono + total - tail, tail, 660.0, 48000.0);
    CHECK (lead_f0 > 1.0, "synth lead sounds at the corrected pitch (%.3g)", lead_f0);
    CHECK (lead_h3 < lead_f0 * 0.05,
           "synth lead replaces the singer's timbre (h3 %.3g vs f0 %.3g)",
           lead_h3, lead_f0);

    /* Vowel transfer reshapes the carrier's spectrum, so it needs a carrier
       with harmonics to reshape -- a channel vocoder filters, it cannot
       invent partials (a `sine` carrier barely changes, which is worth
       knowing and is why the docs say so).

       The definitional test: sing two different vowels at the SAME pitch.
       With the transfer off the synth is identical either way (same notes,
       same patch); with it on, a bright vowel must make a brighter synth.
       `organ` is the carrier -- rich harmonics, no filter of its own to
       fight. */
    for (int v = 0; v < AE_HARM_VOICES; ++v)
        src[v] = AE_HARM_SRC_DEFAULT;
    /* Band energies of the harmony bus, low (around the ghosts' own
       fundamentals) against high (the bright vowel's formant region). */
    double tilt[2][2]; /* [vowel off/on][dark/bright] */
    double level[2];
    for (int on = 0; on < 2; ++on)
        for (int b = 0; b < 2; ++b)
        {
            run_synth_case_ex (src, AE_HARM_SRC_VOICE, on ? 1.0 : 0.0, "organ", b,
                               mono, hl, hr, total);
            double hi = 0.0, lo = 0.0, sq = 0.0;
            for (double f = 1400.0; f <= 2600.0; f += 200.0)
                hi += goertzel (hl + total - tail, tail, f, 48000.0);
            for (double f = 260.0; f <= 700.0; f += 60.0)
                lo += goertzel (hl + total - tail, tail, f, 48000.0);
            for (int i = total - tail; i < total; ++i)
                sq += (double) hl[i] * hl[i];
            tilt[on][b] = hi / (lo + 1e-12);
            if (b == 0)
                level[on] = sq;
        }
    /* Off: the vowel cannot reach the synth, so both vowels give the same
       spectrum. */
    CHECK (fabs (tilt[0][1] - tilt[0][0]) < 0.15 * (tilt[0][0] + 1e-12),
           "vowel off: the synth ignores the sung vowel (%.4g vs %.4g)",
           tilt[0][0], tilt[0][1]);
    /* On: the bright vowel makes a measurably brighter synth. */
    CHECK (tilt[1][1] > tilt[1][0] * 2.0,
           "vowel on: a brighter vowel brightens the synth (%.4g vs %.4g)",
           tilt[1][1], tilt[1][0]);
    /* And it does not run away with the level: the normaliser holds the bus
       within a couple of dB of where it was. */
    CHECK (level[1] > level[0] * 0.25 && level[1] < level[0] * 4.0,
           "vowel transfer keeps the level sane (%.4g vs %.4g)", level[1], level[0]);

    /* Ensemble depth 0 collapses the effect back to the dry ranks. */
    AeCorrector *p = calloc (1, sizeof (AeCorrector));
    ae_corrector_prepare (p, 48000.0, 512, 0.0, 0.0, AE_SHIFT_QUALITY_BALANCED);
    ae_corrector_set_synth_shape (p, 0.0, 0.0, 0.0, AE_VOWEL_MODE_VOCODER);
    CHECK (p->ensemble_depth == 0.0, "ensemble depth takes 0");
    ae_corrector_set_synth_shape (p, 2.0, -1.0, 99.0, AE_VOWEL_MODE_VOCODER);
    CHECK (p->ensemble_depth == 1.0 && p->synth_vowel == 0.0
           && p->harm_tilt_db == 12.0, "synth shape clamps out of range");
    ae_corrector_free (p);
    free (p);

    free (mono); free (hl); free (hr);
}

static void test_lpc_vowel_mode (void)
{
    /* LPC mode estimates the vocal tract rather than 16 fixed bands. The
       definitional test is the same as the vocoder's -- two vowels at one
       pitch must give different synth spectra -- plus the two things LPC
       is FOR: it must resolve the difference more sharply than the band
       vocoder, and its all-pole filter must stay stable while the tract
       moves under it. */
    const int total = 96000, tail = 48000;
    float *mono = malloc ((size_t) total * sizeof (float));
    float *hl = malloc ((size_t) total * sizeof (float));
    float *hr = malloc ((size_t) total * sizeof (float));
    int src[AE_HARM_VOICES];
    for (int v = 0; v < AE_HARM_VOICES; ++v)
        src[v] = AE_HARM_SRC_DEFAULT;

    /* [mode][vowel] -> high/low spectral balance of the harmony bus. */
    double tilt[2][2];
    double peak[2] = { 0.0, 0.0 };
    int nan_count = 0;
    for (int mode = 0; mode < 2; ++mode)
        for (int bright = 0; bright < 2; ++bright)
        {
            AeCorrector *p = calloc (1, sizeof (AeCorrector));
            ae_corrector_prepare (p, 48000.0, 512, 0.0, 0.0, AE_SHIFT_QUALITY_BALANCED);
            ae_corrector_set_edo (p, 12);
            ae_corrector_set_retune_ms (p, 0.0);
            ae_corrector_set_transition_ms (p, 0.0);
            AeHarmVoice voices[AE_HARM_VOICES];
            memset (voices, 0, sizeof (voices));
            for (int v = 0; v < AE_HARM_VOICES; ++v)
                voices[v].gain = 1.0;
            voices[0].interval = 7;
            ae_corrector_set_harmony (p, true, 0, voices);
            ae_corrector_set_synth (p, AE_HARM_SRC_SYNTH,
                                    ae_synth_patch_find ("organ"), 5.0, 200.0);
            ae_corrector_set_voice_sources (p, src, AE_HARM_SRC_VOICE);
            ae_corrector_set_synth_shape (p, 1.0, 1.0, 0.0,
                                          mode ? AE_VOWEL_MODE_LPC
                                               : AE_VOWEL_MODE_VOCODER);

            double phase = 0.0;
            for (int i = 0; i < total; ++i)
            {
                phase += 2.0 * M_PI * 220.0 / 48000.0;
                mono[i] = bright
                    ? (float) (0.35 * sin (phase) + 0.30 * sin (8.0 * phase)
                             + 0.30 * sin (10.0 * phase))
                    : (float) (0.45 * sin (phase) + 0.30 * sin (2.0 * phase));
            }
            for (int off = 0; off < total; off += 512)
            {
                const int n = total - off < 512 ? total - off : 512;
                ae_corrector_process (p, mono + off, hl + off, hr + off, n);
            }
            double hi = 0.0, lo = 0.0;
            for (double f = 1400.0; f <= 2600.0; f += 200.0)
                hi += goertzel (hl + total - tail, tail, f, 48000.0);
            for (double f = 260.0; f <= 700.0; f += 60.0)
                lo += goertzel (hl + total - tail, tail, f, 48000.0);
            tilt[mode][bright] = hi / (lo + 1e-12);
            for (int i = 0; i < total; ++i)
            {
                if (isnan (hl[i]) || isinf (hl[i])) ++nan_count;
                if (fabs (hl[i]) > peak[mode]) peak[mode] = fabs (hl[i]);
            }
            ae_corrector_free (p);
            free (p);
        }

    CHECK (nan_count == 0, "LPC: no NaN or inf escapes the all-pole lattice");
    /* Stability AND level sanity: the wet-path saturator caps a single
       LPC signal at 2.5 and the ensemble blend can add at most ~1.4x, so
       anything past 4 means the safety net has a hole in it. */
    CHECK (peak[1] < 4.0, "LPC: output stays bounded (peak %.3g)", peak[1]);
    CHECK (tilt[1][1] > tilt[1][0] * 2.0,
           "LPC: a brighter vowel brightens the synth (%.4g vs %.4g)",
           tilt[1][1], tilt[1][0]);
    /* The point of LPC over the band vocoder: a continuous tract estimate
       separates the two vowels further than 16 fixed bands can. */
    const double sep_voc = tilt[0][1] / (tilt[0][0] + 1e-12);
    const double sep_lpc = tilt[1][1] / (tilt[1][0] + 1e-12);
    CHECK (sep_lpc > sep_voc,
           "LPC resolves vowels more sharply than the band vocoder "
           "(%.3gx vs %.3gx)", sep_lpc, sep_voc);

    /* Stability under a MOVING tract: the coefficients are interpolated
       every block, which is exactly where a direct-form fit would blow up. */
    AeCorrector *p = calloc (1, sizeof (AeCorrector));
    ae_corrector_prepare (p, 48000.0, 512, 0.0, 0.0, AE_SHIFT_QUALITY_BALANCED);
    ae_corrector_set_edo (p, 12);
    AeHarmVoice voices[AE_HARM_VOICES];
    memset (voices, 0, sizeof (voices));
    for (int v = 0; v < AE_HARM_VOICES; ++v)
        voices[v].gain = 1.0;
    voices[0].interval = 7;
    ae_corrector_set_harmony (p, true, 0, voices);
    ae_corrector_set_synth (p, AE_HARM_SRC_SYNTH,
                            ae_synth_patch_find ("organ"), 5.0, 200.0);
    ae_corrector_set_voice_sources (p, src, AE_HARM_SRC_VOICE);
    ae_corrector_set_synth_shape (p, 1.0, 1.0, 0.0, AE_VOWEL_MODE_LPC);
    double phase = 0.0;
    for (int i = 0; i < total; ++i)
    {
        /* Sweep the "vowel" continuously between the two shapes. */
        const double m = 0.5 + 0.5 * sin (2.0 * M_PI * 3.0 * i / 48000.0);
        phase += 2.0 * M_PI * 220.0 / 48000.0;
        mono[i] = (float) (0.4 * sin (phase)
                         + 0.3 * (1.0 - m) * sin (2.0 * phase)
                         + 0.3 * m * sin (9.0 * phase));
    }
    for (int off = 0; off < total; off += 512)
    {
        const int n = total - off < 512 ? total - off : 512;
        ae_corrector_process (p, mono + off, hl + off, hr + off, n);
    }
    double moving_peak = 0.0;
    int moving_bad = 0;
    for (int i = 0; i < total; ++i)
    {
        if (isnan (hl[i]) || isinf (hl[i])) ++moving_bad;
        if (fabs (hl[i]) > moving_peak) moving_peak = fabs (hl[i]);
    }
    CHECK (moving_bad == 0 && moving_peak < 4.0,
           "LPC: stable while the tract moves (peak %.3g, bad %d)",
           moving_peak, moving_bad);
    ae_corrector_free (p);
    free (p);
    free (mono); free (hl); free (hr);
}

static void test_drone (void)
{
    /* The drone: an absolute-pitch synth voice that SUSTAINS while the
       singer stops -- a root-only chart chord means "drone that root".
       Measured, not assumed: the drone's fundamental is where the degree
       says, it keeps sounding through silence (the ghosts do not), the
       master harmony switch gates it, and droneOn false releases it. */
    const int fs = 48000, total = 6 * fs;
    float *mono = malloc ((size_t) total * sizeof (float));
    float *hl = malloc ((size_t) total * sizeof (float));
    float *hr = malloc ((size_t) total * sizeof (float));

    AeCorrector *p = calloc (1, sizeof (AeCorrector));
    ae_corrector_prepare (p, (double) fs, 512, 0.0, 0.0, AE_SHIFT_QUALITY_BALANCED);
    ae_corrector_set_edo (p, 12);
    AeHarmVoice voices[AE_HARM_VOICES];
    memset (voices, 0, sizeof (voices));
    for (int v = 0; v < AE_HARM_VOICES; ++v)
        voices[v].gain = 1.0;
    voices[0].interval = 7; /* one ordinary ghost, to contrast the sustain */
    ae_corrector_set_harmony (p, true, 0, voices);
    ae_corrector_set_synth (p, AE_HARM_SRC_SYNTH,
                            ae_synth_patch_find ("organ"), 5.0, 60.0);
    int src[AE_HARM_VOICES];
    for (int v = 0; v < AE_HARM_VOICES; ++v)
        src[v] = AE_HARM_SRC_DEFAULT;
    ae_corrector_set_voice_sources (p, src, AE_HARM_SRC_VOICE);
    ae_corrector_set_synth_shape (p, 0.0, 0.0, 0.0, AE_VOWEL_MODE_VOCODER);
    /* Degree 4*12 - 3 = A3-region in the C-anchored grid; its frequency is
       ref * 2^(45/12). */
    ae_corrector_set_drone (p, true, 45);
    const double drone_hz = AE_REFERENCE_C0_HZ * pow (2.0, 45.0 / 12.0);

    /* 0..2 s: sing 220 Hz (establishes in_level; the ghost sounds too).
       2..4 s: silence -- the ghost releases, the drone holds.
       4..6 s: silence with droneOn false -- everything dies. */
    double phase = 0.0;
    for (int i = 0; i < total; ++i)
    {
        phase += 2.0 * M_PI * 220.0 / fs;
        mono[i] = i < 2 * fs
            ? (float) (0.4 * sin (phase) + 0.25 * sin (2.0 * phase)) : 0.0f;
    }
    for (int off = 0; off < total; off += 512)
    {
        if (off == 4 * fs)
            ae_corrector_set_drone (p, false, 45);
        const int n = total - off < 512 ? total - off : 512;
        ae_corrector_process (p, mono + off, hl + off, hr + off, n);
    }

    /* While singing: the drone's fundamental is present at its own pitch. */
    const double sung = goertzel (hl + fs, fs, drone_hz, fs);
    CHECK (sung > 1.0, "drone sounds at its degree while singing (%.3g)", sung);
    /* Through the silence: still there (window well past the ghost's
       release), while the interval ghost is gone. */
    const double held = goertzel (hl + 3 * fs, fs, drone_hz, fs);
    CHECK (held > 1.0, "drone SUSTAINS through silence (%.3g)", held);
    const double ghost_hz = 220.0 * pow (2.0, 7.0 / 12.0);
    const double ghost = goertzel (hl + 3 * fs, fs, ghost_hz, fs);
    CHECK (ghost < held * 0.05,
           "the ordinary ghost released while the drone held (%.3g vs %.3g)",
           ghost, held);
    /* droneOn false: released (give it the 60 ms release, measure after). */
    const double off_e = goertzel (hl + total - fs / 2, fs / 2, drone_hz, fs);
    CHECK (off_e < held * 0.01, "droneOn false releases (%.3g)", off_e);

    /* The master harmony switch gates it: same setup, harmony off. */
    ae_corrector_reset (p);
    ae_corrector_set_harmony (p, false, 0, voices);
    ae_corrector_set_drone (p, true, 45);
    for (int off = 0; off < 2 * fs; off += 512)
        ae_corrector_process (p, mono + off, hl + off, hr + off, 512);
    const double gated = goertzel (hl + fs, fs, drone_hz, fs);
    CHECK (gated < 1e-3, "harmOn false silences the drone (%.3g)", gated);

    ae_corrector_free (p);
    free (p);
    free (mono); free (hl); free (hr);
}

static void test_ir_points (void)
{
    /* The IR points in the ENGINE chain (v0.4-delta B7): a unit-impulse IR
       at mix 1 must be transparent through the whole corrector -- which is
       simultaneously the zero-added-latency proof on the lead's monitored
       path -- and a delayed-impulse IR on the harmony point must shift the
       bus by exactly its delay. The convolver's own math is proven in its
       shared library tests; THIS pins the wiring. */
    const int fs = 48000, total = 3 * fs;
    float *mono  = malloc ((size_t) total * sizeof (float));
    float *m_ref = malloc ((size_t) total * sizeof (float));
    float *hl    = malloc ((size_t) total * sizeof (float));
    float *hr    = malloc ((size_t) total * sizeof (float));
    float *h_ref = malloc ((size_t) total * sizeof (float));
    float *in    = malloc ((size_t) total * sizeof (float));

    double phase = 0.0;
    for (int i = 0; i < total; ++i)
    {
        phase += 2.0 * M_PI * 220.0 / fs;
        in[i] = (float) (0.4 * sin (phase) + 0.25 * sin (2.0 * phase));
    }

    AeCorrector *p = calloc (1, sizeof (AeCorrector));
    ae_corrector_prepare (p, (double) fs, 512, 0.0, 0.0, AE_SHIFT_QUALITY_BALANCED);
    ae_corrector_set_edo (p, 12);
    AeHarmVoice voices[AE_HARM_VOICES];
    memset (voices, 0, sizeof (voices));
    for (int v = 0; v < AE_HARM_VOICES; ++v)
        voices[v].gain = 1.0;
    voices[0].interval = 7;
    ae_corrector_set_harmony (p, true, 0, voices);
    ae_corrector_set_synth (p, AE_HARM_SRC_SYNTH,
                            ae_synth_patch_find ("organ"), 5.0, 200.0);
    int src[AE_HARM_VOICES];
    for (int v = 0; v < AE_HARM_VOICES; ++v)
        src[v] = AE_HARM_SRC_DEFAULT;
    ae_corrector_set_voice_sources (p, src, AE_HARM_SRC_VOICE);
    ae_corrector_set_synth_shape (p, 0.0, 0.0, 0.0, AE_VOWEL_MODE_VOCODER);

    /* Reference pass: no IR anywhere. */
    memcpy (mono, in, (size_t) total * sizeof (float));
    for (int off = 0; off < total; off += 512)
        ae_corrector_process (p, mono + off, hl + off, hr + off,
                              total - off < 512 ? total - off : 512);
    memcpy (m_ref, mono, (size_t) total * sizeof (float));
    memcpy (h_ref, hl, (size_t) total * sizeof (float));

    /* Unit impulses on BOTH points at mix 1: bit-transparent -- the same
       output to the sample, which is also the lead's zero-latency proof. */
    ae_corrector_reset (p);
    float imp = 1.0f;
    CHECK (ae_corrector_load_ir (p, 0, &imp, NULL, 1, 0.0), "IR: lead load");
    CHECK (ae_corrector_load_ir (p, 1, &imp, NULL, 1, 0.0), "IR: harm load");
    /* A second load while the first still fades must refuse. */
    CHECK (! ae_corrector_load_ir (p, 0, &imp, NULL, 1, 0.0),
           "IR: a swap in flight refuses another");
    ae_corrector_set_ir_params (p, 0, 1.0, 0.0, true);
    ae_corrector_set_ir_params (p, 1, 1.0, 0.0, true);
    memcpy (mono, in, (size_t) total * sizeof (float));
    for (int off = 0; off < total; off += 512)
        ae_corrector_process (p, mono + off, hl + off, hr + off,
                              total - off < 512 ? total - off : 512);
    /* Past the 30 ms swap fade and the 10 ms mix smoothing, the output must
       BE the reference. */
    double worst = 0.0;
    for (int i = fs / 2; i < total; ++i)
    {
        const double dm = fabs ((double) mono[i] - m_ref[i]);
        const double dh = fabs ((double) hl[i] - h_ref[i]);
        if (dm > worst) worst = dm;
        if (dh > worst) worst = dh;
    }
    CHECK (worst < 1e-5, "IR: unit impulse transparent through the chain "
           "(worst %.3g)", worst);

    /* A delayed impulse on the HARMONY point: the bus arrives exactly
       delay samples late, proving the point actually convolves the bus. */
    const int delay = 4800; /* 100 ms */
    float *dimp = calloc ((size_t) (delay + 1), sizeof (float));
    dimp[delay] = 1.0f;
    ae_corrector_reset (p);
    CHECK (ae_corrector_load_ir (p, 1, dimp, NULL, delay + 1, 0.0),
           "IR: delayed impulse load");
    ae_corrector_set_ir_params (p, 0, 1.0, 0.0, false); /* lead back to dry */
    memcpy (mono, in, (size_t) total * sizeof (float));
    for (int off = 0; off < total; off += 512)
        ae_corrector_process (p, mono + off, hl + off, hr + off,
                              total - off < 512 ? total - off : 512);
    double err = 0.0, ref_e = 0.0;
    for (int i = fs; i < total; ++i)
    {
        const double want = h_ref[i - delay];
        err   += (hl[i] - want) * ((double) hl[i] - want);
        ref_e += want * want;
    }
    CHECK (ref_e > 1e-6 && sqrt (err / (ref_e + 1e-30)) < 1e-4,
           "IR: harmony bus shifted by exactly the IR's delay (rel %.3g)",
           sqrt (err / (ref_e + 1e-30)));

    free (dimp);
    ae_corrector_free (p);
    free (p);
    free (mono); free (m_ref); free (hl); free (hr); free (h_ref); free (in);
}

static void test_harmony_formant_preservation (void)
{
    /* The harmony shifters go through the same set_shift() as the lead,
       which asks Signalsmith Stretch to hold formants still while the
       pitch moves. Definitional measurement: a voice-like tone whose
       spectral envelope has one strong bump at 1800 Hz, harmonised a
       fourth DOWN. With preservation the output envelope keeps its bump
       at 1800 Hz; without it the bump rides down with the pitch to
       ~1350 Hz -- the "slowed tape" sound this feature exists to kill. */
    if (! ae_shifter_has_formant_support ())
    {
        printf ("note: shifter lacks formant support, preservation untested\n");
        return;
    }

    const int total = 96000, tail = 48000;
    float *mono = malloc ((size_t) total * sizeof (float));
    float *hl = malloc ((size_t) total * sizeof (float));
    float *hr = malloc ((size_t) total * sizeof (float));

    AeCorrector *p = calloc (1, sizeof (AeCorrector));
    ae_corrector_prepare (p, 48000.0, 512, 0.0, 0.0, AE_SHIFT_QUALITY_BALANCED);
    ae_corrector_set_edo (p, 12);
    ae_corrector_set_retune_ms (p, 0.0);
    ae_corrector_set_transition_ms (p, 0.0);
    AeHarmVoice voices[AE_HARM_VOICES];
    memset (voices, 0, sizeof (voices));
    for (int v = 0; v < AE_HARM_VOICES; ++v)
        voices[v].gain = 1.0;
    voices[0].interval = -5; /* perfect fourth down in 12-EDO */
    ae_corrector_set_harmony (p, true, 0, voices);

    /* 14 harmonics of A3 (220 Hz), amplitudes drawn from a fixed envelope:
       a floor plus a Gaussian formant bump centred on 1800 Hz. Phases are
       scattered so the crest stays sane. */
    const double f0 = 220.0;
    double amp[15];
    for (int h = 1; h <= 14; ++h)
    {
        const double f = f0 * h;
        const double d = (f - 1800.0) / 300.0;
        amp[h] = 0.05 + 0.5 * exp (-d * d);
    }
    for (int i = 0; i < total; ++i)
    {
        double s = 0.0;
        for (int h = 1; h <= 14; ++h)
            s += amp[h] * sin (2.0 * M_PI * f0 * h * i / 48000.0 + 1.7 * h);
        mono[i] = (float) (0.35 * s);
    }
    for (int off = 0; off < total; off += 512)
    {
        const int n = total - off < 512 ? total - off : 512;
        ae_corrector_process (p, mono + off, hl + off, hr + off, n);
    }

    /* The harmony fundamental is 220 * 2^(-5/12) = 164.81 Hz. Its 11th
       harmonic (1813 Hz) sits inside the preserved bump; its 8th (1319 Hz)
       sits where the bump would have LANDED had the envelope moved with
       the pitch. Preservation makes the first dominate; a transposed
       envelope reverses the ratio by ~20 dB. */
    const double hf0 = 220.0 * pow (2.0, -5.0 / 12.0);
    const double e_kept  = goertzel (hl + total - tail, tail, 11.0 * hf0, 48000.0);
    const double e_moved = goertzel (hl + total - tail, tail,  8.0 * hf0, 48000.0);
    CHECK (e_kept > 4.0 * e_moved,
           "harmony formants preserved: bump stays at 1800 Hz "
           "(kept %.3g vs moved %.3g)", e_kept, e_moved);

    ae_corrector_free (p);
    free (p);
    free (mono); free (hl); free (hr);
}

static void test_harmony_tilt (void)
{
    /* The tilt is a property of the harmony BUS: it must reach shifted and
       synth voices alike, and never touch the lead. */
    const int total = 48000, tail = 24000;
    float *mono = malloc ((size_t) total * sizeof (float));
    float *hl = malloc ((size_t) total * sizeof (float));
    float *hr = malloc ((size_t) total * sizeof (float));
    int src[AE_HARM_VOICES];

    /* Measure the harmony bus's high/low balance and the LEAD's, at three
       tilts, for a shifted and then a synth voice. */
    for (int synth = 0; synth < 2; ++synth)
    {
        double ratio[3], lead_ratio[3];
        for (int t = 0; t < 3; ++t)
        {
            const double tilt = t == 0 ? -9.0 : t == 1 ? 0.0 : 9.0;
            AeCorrector *p = calloc (1, sizeof (AeCorrector));
            ae_corrector_prepare (p, 48000.0, 512, 0.0, 0.0, AE_SHIFT_QUALITY_BALANCED);
            ae_corrector_set_edo (p, 12);
            ae_corrector_set_retune_ms (p, 0.0);
            ae_corrector_set_transition_ms (p, 0.0);
            AeHarmVoice voices[AE_HARM_VOICES];
            memset (voices, 0, sizeof (voices));
            for (int v = 0; v < AE_HARM_VOICES; ++v)
                voices[v].gain = 1.0;
            voices[0].interval = 7;
            ae_corrector_set_harmony (p, true, 0, voices);
            ae_corrector_set_synth (p, AE_HARM_SRC_SYNTH,
                                    ae_synth_patch_find ("organ"), 5.0, 200.0);
            for (int v = 0; v < AE_HARM_VOICES; ++v)
                src[v] = synth ? AE_HARM_SRC_SYNTH : AE_HARM_SRC_VOICE;
            ae_corrector_set_voice_sources (p, src, AE_HARM_SRC_VOICE);
            ae_corrector_set_synth_shape (p, 1.0, 0.0, tilt, AE_VOWEL_MODE_VOCODER);

            double phase = 0.0;
            for (int i = 0; i < total; ++i)
            {
                phase += 2.0 * M_PI * 220.0 / 48000.0;
                mono[i] = (float) (0.4 * sin (phase) + 0.25 * sin (4.0 * phase)
                                 + 0.2 * sin (8.0 * phase));
            }
            for (int off = 0; off < total; off += 512)
            {
                const int n = total - off < 512 ? total - off : 512;
                ae_corrector_process (p, mono + off, hl + off, hr + off, n);
            }
            double hi = 0.0, lo = 0.0, lhi = 0.0, llo = 0.0;
            for (double f = 1300.0; f <= 2700.0; f += 200.0)
            {
                hi  += goertzel (hl + total - tail, tail, f, 48000.0);
                lhi += goertzel (mono + total - tail, tail, f, 48000.0);
            }
            for (double f = 200.0; f <= 500.0; f += 50.0)
            {
                lo  += goertzel (hl + total - tail, tail, f, 48000.0);
                llo += goertzel (mono + total - tail, tail, f, 48000.0);
            }
            ratio[t] = hi / (lo + 1e-15);
            lead_ratio[t] = lhi / (llo + 1e-15);
            ae_corrector_free (p);
            free (p);
        }
        const char *what = synth ? "synth" : "shifted";
        CHECK (ratio[2] > ratio[1] * 1.5,
               "%s voices: +9 dB tilt brightens (%.4g vs %.4g)", what, ratio[2], ratio[1]);
        CHECK (ratio[0] < ratio[1] * 0.67,
               "%s voices: -9 dB tilt darkens (%.4g vs %.4g)", what, ratio[0], ratio[1]);
        /* The lead is on its own bus and must not move at all. */
        CHECK (fabs (lead_ratio[2] - lead_ratio[1]) < 0.05 * (lead_ratio[1] + 1e-15)
               && fabs (lead_ratio[0] - lead_ratio[1]) < 0.05 * (lead_ratio[1] + 1e-15),
               "%s: the lead is untouched by the tilt (%.4g / %.4g / %.4g)",
               what, lead_ratio[0], lead_ratio[1], lead_ratio[2]);
    }
    free (mono); free (hl); free (hr);
}

static void test_96k (void)
{
    /* The 96 kHz latency halving: shifter blocks are fixed SAMPLE counts
       (48k-referenced), so at 96 k the same samples span half the time.
       Same latency in samples => half the milliseconds. */
    AeCorrector *p48 = calloc (1, sizeof (AeCorrector));
    AeCorrector *p96 = calloc (1, sizeof (AeCorrector));
    ae_corrector_prepare (p48, 48000.0, 512, 0.0, 0.0, AE_SHIFT_QUALITY_BALANCED);
    ae_corrector_prepare (p96, 96000.0, 512, 0.0, 0.0, AE_SHIFT_QUALITY_BALANCED);
    const int l48 = ae_corrector_latency (p48);
    const int l96 = ae_corrector_latency (p96);
    CHECK (l96 < l48 * 12 / 10 && l96 > l48 * 8 / 10,
           "96k latency in samples stays put (%d vs %d at 48k)", l96, l48);
    const double ms48 = 1000.0 * l48 / 48000.0, ms96 = 1000.0 * l96 / 96000.0;
    CHECK (ms96 < ms48 * 0.6,
           "96k latency in ms halves (%.1f ms vs %.1f ms)", ms96, ms48);
    ae_corrector_free (p48);
    free (p48);

    /* And the corrector still corrects at the fast rate: the flat A3 lands
       on 220, with a shifted P5 ghost above it. */
    ae_corrector_set_edo (p96, 12);
    ae_corrector_set_retune_ms (p96, 0.0);
    ae_corrector_set_transition_ms (p96, 0.0);
    AeHarmVoice voices[AE_HARM_VOICES];
    memset (voices, 0, sizeof (voices));
    for (int v = 0; v < AE_HARM_VOICES; ++v)
        voices[v].gain = 1.0;
    voices[0].interval = 7;
    ae_corrector_set_harmony (p96, true, 0, voices);

    const double f_in = 220.0 * pow (2.0, -30.0 / 1200.0);
    const int total = 96000;
    float *in = malloc ((size_t) total * sizeof (float));
    float *hl = malloc ((size_t) total * sizeof (float));
    float *hr = malloc ((size_t) total * sizeof (float));
    double phase = 0.0;
    for (int i = 0; i < total; ++i)
    {
        phase += 2.0 * M_PI * f_in / 96000.0;
        in[i] = (float) (0.4 * sin (phase) + 0.2 * sin (2.0 * phase));
    }
    for (int off = 0; off < total; off += 512)
    {
        const int n = total - off < 512 ? total - off : 512;
        ae_corrector_process (p96, in + off, hl + off, hr + off, n);
    }

    CHECK (ae_corrector_voiced (p96), "96k: voiced after 1 s of tone");
    const float det = ae_corrector_detected_hz (p96);
    const float tgt = ae_corrector_target_hz (p96);
    CHECK (fabs (det - f_in) < 2.0, "96k: detected %f (expected ~%f)", det, f_in);
    CHECK (fabs (tgt - 220.0) < 1e-2, "96k: target %f (expected 220)", tgt);
    const double p_fifth = goertzel (hl + total / 2, total / 2, 329.63, 96000.0);
    const double p_root  = goertzel (hl + total / 2, total / 2, 220.0, 96000.0);
    CHECK (p_fifth > 10.0 * p_root,
           "96k: harmony bus is the fifth (P5 %.3g vs root %.3g)", p_fifth, p_root);

    ae_corrector_free (p96);
    free (p96);
    free (in); free (hl); free (hr);
}

static void test_soft_clip (void)
{
    /* Transparent below the knee: exact passthrough, sign included. */
    CHECK (ae_soft_clip (0.0f) == 0.0f, "soft clip: zero is zero");
    CHECK (ae_soft_clip (0.5f) == 0.5f, "soft clip: below knee passes through");
    CHECK (ae_soft_clip (-0.5f) == -0.5f, "soft clip: negative passthrough");
    CHECK (ae_soft_clip (0.8f) == 0.8f, "soft clip: continuous at the knee");

    /* Bounded: a five-voice pile-up must stay inside full scale. */
    CHECK (ae_soft_clip (4.5f) < 1.0f, "soft clip: bounded above");
    CHECK (ae_soft_clip (-4.5f) > -1.0f, "soft clip: bounded below");
    CHECK (fabsf (ae_soft_clip (5.0f) + ae_soft_clip (-5.0f)) < 1e-7f,
           "soft clip: symmetric");

    /* Monotonic through the knee (no fold-back distortion). */
    float prev = -2.0f;
    for (float x = -1.99f; x < 2.0f; x += 0.01f)
    {
        const float y = ae_soft_clip (x);
        CHECK (y > ae_soft_clip (prev) - 1e-7f, "soft clip: monotonic at %.2f", x);
        prev = x;
    }
}

int main (void)
{
    test_tuning();
    test_yin();
    test_yin_octave_guard();
    test_correction();
    test_harm_master();
    test_harmony_anchor();
    test_synth_envelope();
    test_walk();
    test_harmony();
    test_synth_harmony();
    test_synth_string_machine();
    test_synth_sources_and_vowel();
    test_harmony_tilt();
    test_lpc_vowel_mode();
    test_drone();
    test_ir_points();
    test_harmony_formant_preservation();
    test_midi_harmony();
    test_json();
    test_engine_channels();
    test_96k();
    test_soft_clip();

    if (failures == 0)
    {
        printf ("All self-tests passed.\n");
        return 0;
    }
    printf ("%d failure(s).\n", failures);
    return 1;
}
