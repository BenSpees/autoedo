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

/* The mask tie-break: at equal distance the walk must go AWAY from the
   lead, not up by fiat. Up-first is right for a ghost above and exactly
   wrong for one below -- on a tie it pulls the ghost toward the lead, so a
   third can collapse onto a second or a unison. */
static void test_walk_tiebreak (void)
{
    bool mask[AE_MAX_EDO];
    memset (mask, 0, sizeof (mask));
    /* 12-EDO with pc 5 dark and both neighbours lit: a tie at distance 1. */
    for (int d = 0; d < 12; ++d) mask[d] = true;
    mask[5] = false;

    CHECK (ae_walk_to_enabled_dir (5, 12, mask, true) == 6,
           "tie-break: a ghost ABOVE the lead resolves upward");
    CHECK (ae_walk_to_enabled_dir (5, 12, mask, false) == 4,
           "tie-break: a ghost BELOW the lead resolves downward");
    /* The historical wrapper keeps up-first, which is what a lead being
       corrected onto the mask still wants (no second voice to stay away
       from). */
    CHECK (ae_walk_to_enabled (5, 12, mask) == 6,
           "tie-break: the plain walk is unchanged (up-first)");

    /* No tie: the nearer side wins whatever the preference. */
    mask[4] = false;
    CHECK (ae_walk_to_enabled_dir (5, 12, mask, false) == 6,
           "tie-break: only applies to a TIE, never overrides distance");
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

    /* Attack, on BOTH sources. It means the same thing either side -- how
       long the ghost takes to arrive under the lead -- so a slow attack
       must not be at full level a few milliseconds in whether the ghost is
       an oscillator or a pitch-shifted copy of the input. */
    const double atk_ms[2] = { 1.0, 1000.0 };
    const int    srcs[2]   = { AE_HARM_SRC_SYNTH, AE_HARM_SRC_VOICE };
    double onset[2][2];
    for (int src = 0; src < 2; ++src)
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
        ae_corrector_set_synth (p, srcs[src],
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
        onset[src][c] = settled > 0.0 ? pk / settled : 0.0;

        ae_corrector_free (p);
        free (p); free (in); free (hl); free (hr);
    }
    for (int src = 0; src < 2; ++src)
    {
        const char *nm = src == 0 ? "synth" : "shifted";
        /* 0.65, not 0.7: when detection lock got faster the ghost's
           first audible sample moved EARLIER, into the voiced crossfade
           and shifter warm-up that used to complete before it -- so the
           first-100 ms peak sits slightly lower while the ghost arrives
           sooner, which is the trade this test exists to protect. */
        CHECK (onset[src][0] > 0.65,
               "attack: a fast attack is up immediately (%s, %.3g)",
               nm, onset[src][0]);
        CHECK (onset[src][1] < 0.35,
               "attack: a 1 s attack is still climbing (%s, %.3g vs fast %.3g)",
               nm, onset[src][1], onset[src][0]);
    }
}

/* Peak-search the strongest partial near `guess` (Goertzel over a fine grid).
   YIN would average the inharmonic upper partials; this reads the one
   frequency being asked about. */
static double peak_span (const float *buf, int n, double lo, double hi, double fs)
{
    double best_f = lo, best_p = -1.0;
    for (double f = lo; f <= hi; f += lo * 0.0005)
    {
        const double p = goertzel (buf, n, f, fs);
        if (p > best_p) { best_p = p; best_f = f; }
    }
    return best_f;
}

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

/* Portamento reaches the SHIFTED ghosts, not just the synth ones: the glide
   is applied once, upstream of both, and the shifter's transposition is
   derived from the glided position. Lead jumps A3 -> C4; the ghost a fifth
   above must still be on its way when the glide is long. */
static void test_harmony_glide (void)
{
    const int hold = 49152, total = hold * 2;   /* ~1 s each, block-aligned */
    const double f0 = 220.0, f1 = 261.6256;     /* A3 then C4 */
    const double fifth = pow (2.0, 7.0 / 12.0);
    const double g0 = f0 * fifth, g1 = f1 * fifth;  /* 329.63 -> 392.00 */
    const double glide_ms[2] = { 0.0, 1500.0 };
    double got[2];

    for (int c = 0; c < 2; ++c)
    {
        AeCorrector *p = calloc (1, sizeof (AeCorrector));
        ae_corrector_prepare (p, 48000.0, 512, 0.0, 0.0, AE_SHIFT_QUALITY_BALANCED);
        ae_corrector_set_edo (p, 12);
        ae_corrector_set_retune_ms (p, 0.0);
        ae_corrector_set_transition_ms (p, 0.0);
        ae_corrector_set_harm_glide_ms (p, glide_ms[c]);

        AeHarmVoice voices[AE_HARM_VOICES];
        memset (voices, 0, sizeof (voices));
        for (int v = 0; v < AE_HARM_VOICES; ++v)
            voices[v].gain = 1.0;
        voices[0].interval = 7;
        ae_corrector_set_harmony (p, true, 0, voices);
        /* The SHIFTED source -- the one that used to jump. */
        ae_corrector_set_synth (p, AE_HARM_SRC_VOICE, 0, 5.0, 200.0);

        float *in = calloc ((size_t) total, sizeof (float));
        float *hl = calloc ((size_t) total, sizeof (float));
        float *hr = calloc ((size_t) total, sizeof (float));
        double phase = 0.0;
        for (int i = 0; i < total; ++i)
        {
            phase += 2.0 * M_PI * (i < hold ? f0 : f1) / 48000.0;
            in[i] = (float) (0.4 * sin (phase) + 0.15 * sin (2.0 * phase));
        }
        for (int off = 0; off < total; off += 512)
            ae_corrector_process (p, in + off, hl + off, hr + off, 512);

        /* 150-450 ms after the lead moved, past the shifter's own latency. */
        got[c] = peak_span (hl + hold + 7200, 14400, g0 * 0.97, g1 * 1.03, 48000.0);

        ae_corrector_free (p);
        free (p); free (in); free (hl); free (hr);
    }

    CHECK (fabs (1200.0 * log2 (got[0] / g1)) < 25.0,
           "glide 0: the shifted ghost is already there (%.1f Hz, target %.1f)",
           got[0], g1);
    CHECK (got[1] < got[0] * 0.985 && got[1] > g0 * 0.99,
           "glide 1500 ms: the shifted ghost is still travelling "
           "(%.1f Hz, between %.1f and %.1f)", got[1], g0, g1);

    /* Arrival: the slide is constant-time, not a one-pole. A 400 ms glide
       must be ON target well before 700 ms -- the exponential this replaced
       would still be ~50 cents short there and never truly land. */
    {
        AeCorrector *p = calloc (1, sizeof (AeCorrector));
        ae_corrector_prepare (p, 48000.0, 512, 0.0, 0.0, AE_SHIFT_QUALITY_BALANCED);
        ae_corrector_set_edo (p, 12);
        ae_corrector_set_retune_ms (p, 0.0);
        ae_corrector_set_transition_ms (p, 0.0);
        ae_corrector_set_harm_glide_ms (p, 400.0);

        AeHarmVoice voices[AE_HARM_VOICES];
        memset (voices, 0, sizeof (voices));
        for (int v = 0; v < AE_HARM_VOICES; ++v)
            voices[v].gain = 1.0;
        voices[0].interval = 7;
        ae_corrector_set_harmony (p, true, 0, voices);
        ae_corrector_set_synth (p, AE_HARM_SRC_VOICE, 0, 5.0, 200.0);

        float *in = calloc ((size_t) total, sizeof (float));
        float *hl = calloc ((size_t) total, sizeof (float));
        float *hr = calloc ((size_t) total, sizeof (float));
        double phase = 0.0;
        for (int i = 0; i < total; ++i)
        {
            phase += 2.0 * M_PI * (i < hold ? f0 : f1) / 48000.0;
            in[i] = (float) (0.4 * sin (phase) + 0.15 * sin (2.0 * phase));
        }
        for (int off = 0; off < total; off += 512)
            ae_corrector_process (p, in + off, hl + off, hr + off, 512);

        /* 700 ms - 1000 ms after the change: landed, and staying there. */
        const double landed = peak_span (hl + hold + 33600, 14400,
                                         g0 * 0.97, g1 * 1.03, 48000.0);
        CHECK (fabs (1200.0 * log2 (landed / g1)) < 12.0,
               "glide 400 ms lands on the target (%.1f Hz, target %.1f)",
               landed, g1);

        ae_corrector_free (p);
        free (p); free (in); free (hl); free (hr);
    }
}

/* leadShiftSteps: applied after the snap (the detector still classifies the
   real note), moves the lead by an exact interval, and locked ghosts stack
   their intervals on the SHIFTED lead so the harmony stays a scale interval
   from the note the audience hears. */
/* The "super-bassy corrected lead" from the field. A guitar picked with a
   dominant 2nd harmonic makes YIN vote octave-high at the pluck and
   re-vote the true octave mid-note as the uppers decay; detected and
   target both step an equave in one hop, and the correction used to glide
   1200 cents -- swinging the lead's ratio through an octave for
   ~transition_ms after every re-vote. The rebase relabels instead, so the
   shift stays continuous. */
/* The field report: "pitch shifting pedals track your bends and vibrato
   and they audibly transfer to the output. That doesn't seem to be working
   in ours." It was not: at amount 1 the law shift = target - detected
   cancels a bend exactly as it happens, because target is a fixed degree.
   Correction now applies to the note's CENTRE and the deviation is added
   back, so the note lands on the degree and the playing survives. */
static void test_expression_transfer (void)
{
    const double bend_c = 120.0;  /* a whole-tone bend, over 400 ms */
    double out_end[2];            /* [0] expression off, [1] on */

    for (int c = 0; c < 2; ++c)
    {
        AeCorrector *p = calloc (1, sizeof (AeCorrector));
        ae_corrector_prepare (p, 48000.0, 512, 0.0, 0.0, AE_SHIFT_QUALITY_BALANCED);
        ae_corrector_set_edo (p, 12);
        ae_corrector_set_retune_ms (p, 20.0);
        ae_corrector_set_transition_ms (p, 50.0);
        ae_corrector_set_amount (p, 1.0);       /* full correction */
        ae_corrector_set_expression (p, c);     /* 0 = pin, 1 = pass */

        /* A3 held for 700 ms, then bent up 120 cents over 400 ms and held
           there. The bend crosses a degree boundary, so a centre-based
           snap will eventually re-target -- the assertion is taken while
           it is still mid-bend. */
        const int total = 98304;
        float *in = calloc ((size_t) total, sizeof (float));
        double ph = 0.0;
        for (int i = 0; i < total; ++i)
        {
            const double t = (double) i / 48000.0;
            double c_off = 0.0;
            if (t > 0.70) c_off = bend_c * ((t - 0.70) / 0.40 < 1.0
                                              ? (t - 0.70) / 0.40 : 1.0);
            ph += 2.0 * M_PI * (220.0 * pow (2.0, c_off / 1200.0)) / 48000.0;
            in[i] = (float) (0.4 * sin (ph) + 0.15 * sin (2.0 * ph));
        }
        /* Sample the OUTPUT pitch 250 ms into the bend: detected + shift. */
        double out_c = 0.0;
        for (int off = 0; off < total; off += 512)
        {
            const int n = total - off < 512 ? total - off : 512;
            ae_corrector_process (p, in + off, NULL, NULL, n);
            const double t = (double) off / 48000.0;
            if (t >= 0.94 && t < 0.96)
            {
                const double det = 1200.0 * log2 (ae_corrector_detected_hz (p) / 220.0);
                out_c = det + p->shift_semitones * 100.0;
            }
        }
        out_end[c] = out_c;
        ae_corrector_free (p);
        free (p); free (in);
    }

    /* Pinned: the output stays on the degree however hard you bend. */
    CHECK (fabs (out_end[0]) < 25.0,
           "expression 0 pins the output to the degree (%.0f cents off)",
           out_end[0]);
    /* Passed: the bend reaches the output. 250 ms in, the input is ~75
       cents up; the note's centre has taken some of it, so the output must
       be well clear of the degree and well short of a full re-snap. */
    CHECK (out_end[1] > 30.0,
           "expression 1 lets the bend through (%.0f cents vs pinned %.0f)",
           out_end[1], out_end[0]);
}

static void test_octave_revote_rebase (void)
{
    AeCorrector *p = calloc (1, sizeof (AeCorrector));
    ae_corrector_prepare (p, 48000.0, 512, 0.0, 0.0, AE_SHIFT_QUALITY_BALANCED);
    ae_corrector_set_edo (p, 12);
    /* Default-ish speeds: the bug lives in the transition glide. */
    ae_corrector_set_retune_ms (p, 20.0);
    ae_corrector_set_transition_ms (p, 50.0);

    /* 2 s of A2 (110 Hz): first second 2nd-harmonic dominant (YIN reads
       220), second second fundamental dominant (YIN reads 110). One
       physical note, one octave re-vote. */
    const int total = 96000;
    float *in = calloc ((size_t) total, sizeof (float));
    double ph = 0.0;
    for (int i = 0; i < total; ++i)
    {
        const double w = i < 45600 ? 0.0
                       : i > 50400 ? 1.0 : (i - 45600) / 4800.0;
        const double h1 = 0.10 + 0.40 * w;   /* fundamental fades in */
        const double h2 = 0.50 - 0.42 * w;   /* second harmonic fades out */
        ph += 2.0 * M_PI * 110.0 / 48000.0;
        in[i] = (float) (h1 * sin (ph) + h2 * sin (2.0 * ph)
                       + 0.06 * sin (3.0 * ph));
    }

    double max_shift = 0.0;
    for (int off = 0; off < total; off += 512)
    {
        const int n = total - off < 512 ? total - off : 512;
        ae_corrector_process (p, in + off, NULL, NULL, n);
        if (fabs (p->shift_semitones) > max_shift)
            max_shift = fabs (p->shift_semitones);
    }
    CHECK (max_shift < 3.0,
           "octave re-vote does not swing the lead (max |shift| %.2f st; "
           "the unrebased glide measured ~10)", max_shift);

    ae_corrector_free (p);
    free (p); free (in);
}

/* Toggling harmony off and back on clears the portamento memory: the first
   note after re-enable lands ON pitch instead of sliding in from wherever
   harmony last sang -- even when a long release kept the old voice's glide
   state alive across a quick toggle. */
static void test_harm_toggle_clears_glide (void)
{
    AeCorrector *p = calloc (1, sizeof (AeCorrector));
    ae_corrector_prepare (p, 48000.0, 512, 0.0, 0.0, AE_SHIFT_QUALITY_BALANCED);
    ae_corrector_set_edo (p, 12);
    ae_corrector_set_retune_ms (p, 0.0);
    ae_corrector_set_transition_ms (p, 0.0);
    ae_corrector_set_harm_glide_ms (p, 2000.0); /* a long, audible slide */

    AeHarmVoice voices[AE_HARM_VOICES];
    memset (voices, 0, sizeof (voices));
    for (int v = 0; v < AE_HARM_VOICES; ++v)
        voices[v].gain = 1.0;
    voices[0].interval = 7;
    ae_corrector_set_harmony (p, true, 0, voices);
    ae_corrector_set_synth (p, AE_HARM_SRC_SYNTH,
                            ae_synth_patch_find ("sine"), 5.0, 3000.0);

    const int seg = 24576;
    float *in = calloc ((size_t) seg, sizeof (float));
    float *hl = calloc ((size_t) seg, sizeof (float));
    float *hr = calloc ((size_t) seg, sizeof (float));

    /* 1. Sing A3: the ghost settles on E4 and the glide state points there. */
    double ph = 0.0;
    for (int i = 0; i < seg; ++i)
    {
        ph += 2.0 * M_PI * 220.0 / 48000.0;
        in[i] = (float) (0.4 * sin (ph) + 0.1 * sin (2.0 * ph));
    }
    for (int off = 0; off < seg; off += 512)
        ae_corrector_process (p, in + off, hl + off, hr + off, 512);

    /* 2. Harmony off, then ~200 ms of silence: the 3 s release keeps the
       old voice ringing (and its glide state alive) across the gap, which
       is exactly the leak -- without the edge-clear the next note would
       slide in from the tail's pitch. */
    ae_corrector_set_harmony (p, false, 0, voices);
    memset (in, 0, 9728 * sizeof (float));
    for (int off = 0; off < 9728; off += 512)
        ae_corrector_process (p, in + off, hl + off, hr + off, 512);
    ae_corrector_set_harmony (p, true, 0, voices);

    /* 3. Sing C4. Without the clear, the ghost slides from E4 toward G4
       over two seconds; with it, the first note lands on G4. */
    ph = 0.0;
    for (int i = 0; i < seg; ++i)
    {
        ph += 2.0 * M_PI * 261.6256 / 48000.0;
        in[i] = (float) (0.4 * sin (ph) + 0.1 * sin (2.0 * ph));
    }
    for (int off = 0; off < seg; off += 512)
        ae_corrector_process (p, in + off, hl + off, hr + off, 512);

    /* Measure 150-450 ms after re-enable: past the attack, far inside what
       a 2 s slide from E4 (329.6) to G4 (392.0) would still be climbing. */
    const double got = peak_span (hl + 7200, 14400, 329.63 * 0.97,
                                  392.0 * 1.03, 48000.0);
    CHECK (fabs (1200.0 * log2 (got / 392.0)) < 20.0,
           "harmony re-enable starts on pitch (%.1f Hz, target 392.0; a "
           "leaked glide reads ~340-360)", got);

    ae_corrector_free (p);
    free (p); free (in); free (hl); free (hr);
}

static void test_lead_shift (void)
{
    AeCorrector *p = calloc (1, sizeof (AeCorrector));
    ae_corrector_prepare (p, 48000.0, 512, 0.0, 0.0, AE_SHIFT_QUALITY_BALANCED);
    ae_corrector_set_edo (p, 12);
    ae_corrector_set_retune_ms (p, 0.0);
    ae_corrector_set_transition_ms (p, 0.0);
    ae_corrector_set_lead_shift (p, 12); /* one equave up */

    AeHarmVoice voices[AE_HARM_VOICES];
    memset (voices, 0, sizeof (voices));
    for (int v = 0; v < AE_HARM_VOICES; ++v)
        voices[v].gain = 1.0;
    voices[0].interval = 7; /* a fifth above the (shifted) lead */
    ae_corrector_set_harmony (p, true, 1, voices);
    ae_corrector_set_synth (p, AE_HARM_SRC_SYNTH,
                            ae_synth_patch_find ("sine"), 5.0, 200.0);

    const int total = 98304;
    float *in = calloc ((size_t) total, sizeof (float));
    float *hl = calloc ((size_t) total, sizeof (float));
    float *hr = calloc ((size_t) total, sizeof (float));
    float *lead = malloc ((size_t) total * sizeof (float));
    double phase = 0.0;
    for (int i = 0; i < total; ++i)
    {
        phase += 2.0 * M_PI * 220.0 / 48000.0;
        in[i] = (float) (0.4 * sin (phase) + 0.15 * sin (2.0 * phase));
    }
    memcpy (lead, in, (size_t) total * sizeof (float));
    for (int off = 0; off < total; off += 512)
        ae_corrector_process (p, lead + off, hl + off, hr + off, 512);

    /* The lead comes out an octave up... */
    const double l440 = goertzel (lead + total - 24000, 24000, 440.0, 48000.0);
    const double l220 = goertzel (lead + total - 24000, 24000, 220.0, 48000.0);
    CHECK (l440 > 4.0 * l220,
           "leadShiftSteps: lead is an equave up (440 %.3g vs 220 %.3g)",
           l440, l220);
    /* ...the published target says so (what a UI should draw)... */
    CHECK (fabs (ae_corrector_target_hz (p) - 440.0) < 2.0,
           "leadShiftSteps: published target is shifted (%.1f Hz)",
           ae_corrector_target_hz (p));
    /* ...and the ghost is the fifth above the SHIFTED lead: E5, not E4. */
    const double g659 = goertzel (hl + total - 24000, 24000, 659.26, 48000.0);
    const double g330 = goertzel (hl + total - 24000, 24000, 329.63, 48000.0);
    CHECK (g659 > 4.0 * g330,
           "leadShiftSteps: ghost stacks on the shifted lead (E5 %.3g vs E4 %.3g)",
           g659, g330);

    ae_corrector_free (p);
    free (p); free (in); free (hl); free (hr); free (lead);
}

/* Attack Sound: an onset fires a transient into the harmony bus BEFORE the
   synth ghost's envelope has risen -- the cover for a long synthAttackMs --
   at its own gain, outside every envelope. */
/* The field complaint, verbatim: "the same pitch that is totally stable
   while I'm holding the note just changes when I release it." A release is
   a bend: the mute pulls the string sharp/flat while the level collapses,
   the last voiced hops track it, and an unguarded ghost ends the note
   somewhere the player never played -- sometimes a whole re-snapped scale
   step away. The slope-freeze plus the rewind ring must make the ringing
   ghost keep the held note's pitch. */
/* midiOctaves: the held note names the PITCH CLASS, the player names the
   register (default "nearest"). Absolute mode ("held") retunes to the held
   note's own octave -- the standing-transpose "incredibly bassy" trap when
   chord voicings sit octaves below the lead line. */
static void test_midi_octave_fold (void)
{
    double shift[2];
    for (int c = 0; c < 2; ++c)
    {
        AeCorrector *p = calloc (1, sizeof (AeCorrector));
        ae_corrector_prepare (p, 48000.0, 512, 0.0, 0.0, AE_SHIFT_QUALITY_BALANCED);
        ae_corrector_set_edo (p, 12);
        ae_corrector_set_retune_ms (p, 0.0);
        ae_corrector_set_transition_ms (p, 0.0);
        ae_corrector_set_midi (p, true, 1ull << 48, 0); /* hold C3 */
        ae_corrector_set_midi_fold (p, c == 0);

        const int total = 49152;
        float *in = calloc ((size_t) total, sizeof (float));
        double ph = 0.0;
        for (int i = 0; i < total; ++i)
        {
            ph += 2.0 * M_PI * 523.25 / 48000.0; /* playing C5 */
            in[i] = (float) (0.4 * sin (ph) + 0.1 * sin (2.0 * ph));
        }
        for (int off = 0; off < total; off += 512)
            ae_corrector_process (p, in + off, NULL, NULL, 512);
        shift[c] = p->shift_semitones;
        ae_corrector_free (p);
        free (p); free (in);
    }
    CHECK (fabs (shift[0]) < 1.0,
           "midiOctaves nearest: C5 against held C3 stays put (%.2f st)",
           shift[0]);
    CHECK (shift[1] < -20.0,
           "midiOctaves held: absolute snap reaches down two octaves (%.2f st)",
           shift[1]);
}

/* Detection responsiveness: time from a pluck's onset to the first
   detection within 50 cents of truth. The analysis frame is sized at the
   textbook YIN minimum (two periods of the range bottom) rather than
   padded to a power of two -- the padding cost 2.3x the necessary window
   at the guitar range, and a fresh note only reads true once it fills
   the window, so lock time scales directly with it. Measured on the rig
   settings: 37.8 ms average lock before, 28.9 after (worst case 52 -> 31).
   The 34 ms bound here fails against the padded frame. */
/* FOLLOW link pieces that live below the HTTP layer. The degree encode
   must round-trip through the receiver's MIDI map (j = 4*edo + n - 60) in
   any EDO, folding by whole equaves when a degree sits outside MIDI range
   -- the fold is free because the receiver's "nearest" mode reads only the
   pitch class. And the envelope depth law must actually CUT: at depth 1 a
   linked source at level 0 silences this instance's wet output. */
/* POLY mode: a CHORD through the fixed-ratio lead shift arrives with all
   its notes intact, an octave up -- the pedal "poly" contract. The same
   chord through MONO mode is the negative control: one detected "pitch"
   drives a correction of the whole mix, and at least one chord tone is
   mangled or lost. Also: poly doubles the analysis block, so the latency
   the status reports must grow -- that is poly's documented price. */
static void test_poly_mode (void)
{
    const double notes[3] = { 130.81, 164.81, 196.0 }; /* C3 E3 G3 */
    double up[2][3], base[2][3];
    int lat[2] = { 0, 0 };
    for (int m = 0; m < 2; ++m) /* 0 = poly, 1 = mono control */
    {
        AeCorrector *p = calloc (1, sizeof (AeCorrector));
        ae_corrector_prepare (p, 48000.0, 512, 0.0, 0.0,
                              AE_SHIFT_QUALITY_BALANCED
                              | (m == 0 ? AE_SHIFT_QUALITY_POLY_FLAG : 0));
        ae_corrector_set_edo (p, 12);
        ae_corrector_set_retune_ms (p, 0.0);
        ae_corrector_set_transition_ms (p, 0.0);
        ae_corrector_set_formant_hold (p, false);
        ae_corrector_set_lead_shift (p, 12); /* +1 octave in 12-EDO */
        lat[m] = p->latency;

        const int total = 512 * 280; /* ~3 s */
        float *buf = calloc ((size_t) total, sizeof (float));
        double ph[3] = { 0.0, 0.3, 0.7 };
        for (int i = 0; i < total; ++i)
        {
            double v = 0.0;
            for (int k = 0; k < 3; ++k)
            {
                ph[k] += 2.0 * M_PI * notes[k] / 48000.0;
                v += sin (ph[k]) + 0.25 * sin (2.0 * ph[k]);
            }
            buf[i] = (float) (0.1 * v);
        }
        for (int off = 0; off < total; off += 512)
            ae_corrector_process (p, buf + off, NULL, NULL, 512);

        const float *tail = buf + total - 96000;
        for (int k = 0; k < 3; ++k)
        {
            up[m][k]   = goertzel (tail, 96000, notes[k] * 2.0, 48000.0);
            base[m][k] = goertzel (tail, 96000, notes[k], 48000.0);
        }
        ae_corrector_free (p); free (p); free (buf);
    }

    /* Poly: every chord tone present at the shifted position, and the
       shifted rendering dominates its unshifted residue. */
    double weakest = 1e9, strongest = 0.0;
    for (int k = 0; k < 3; ++k)
    {
        if (up[0][k] < weakest)   weakest   = up[0][k];
        if (up[0][k] > strongest) strongest = up[0][k];
        CHECK (up[0][k] > 3.0 * base[0][k],
               "poly: chord tone %d arrives SHIFTED (up %.4f vs residue %.4f)",
               k, up[0][k], base[0][k]);
    }
    CHECK (weakest > 0.12 * strongest,
           "poly: no chord tone is lost (weakest %.4f vs strongest %.4f)",
           weakest, strongest);

    /* Mono on the same chord loses or mangles at least one tone -- the
       reason poly mode exists. If this ever PASSES all three, mono has
       become polyphonic and this test should be rethought. */
    int mono_ok = 0;
    for (int k = 0; k < 3; ++k)
        if (up[1][k] > 3.0 * base[1][k] && up[1][k] > 0.12 * strongest)
            ++mono_ok;
    CHECK (mono_ok < 3,
           "poly control: MONO mode does not survive a chord (%d/3 tones "
           "clean); if it does, poly earned a rethink", mono_ok);

    CHECK (lat[0] >= 2 * lat[1] - 64,
           "poly: the doubled analysis block is visible in latency "
           "(poly %d vs mono %d samples) -- the documented price", lat[0], lat[1]);
}

/* The multi-f0 tracker alone: a harmonically rich triad must come back as
   exactly three notes at the right pitches (no ghosts at octaves or
   combination tones), and silence must kill them all. */
static void test_polyf0_tracker (void)
{
    AePolyF0 t;
    memset (&t, 0, sizeof (t));
    ae_polyf0_prepare (&t, 48000.0, 60.0, 1200.0);
    CHECK (t.win_size >= 4096, "polyf0: window resolves low chords (got %d)",
           t.win_size);
    float *frame = calloc ((size_t) t.win_size, sizeof (float));

    const double notes[3] = { 130.81, 164.81, 196.0 }; /* C3 E3 G3 */
    double ph[3] = { 0.0, 0.4, 0.9 };
    for (int f = 0; f < 10; ++f)
    {
        for (int i = 0; i < t.win_size; ++i)
        {
            double v = 0.0;
            for (int k = 0; k < 3; ++k)
            {
                ph[k] += 2.0 * M_PI * notes[k] / 48000.0;
                v += sin (ph[k]) + 0.30 * sin (2.0 * ph[k])
                                 + 0.15 * sin (3.0 * ph[k]);
            }
            frame[i] = (float) (0.1 * v);
        }
        ae_polyf0_process (&t, frame);
    }

    int active = 0;
    bool found[3] = { false, false, false };
    for (int k = 0; k < AE_POLY_MAX_NOTES; ++k)
    {
        if (! t.notes[k].active)
            continue;
        ++active;
        for (int m = 0; m < 3; ++m)
            if (fabs (1200.0 * log2 (t.notes[k].hz / notes[m])) < 20.0)
                found[m] = true;
    }
    CHECK (active == 3, "polyf0: a triad is three notes, no ghosts (got %d)",
           active);
    CHECK (found[0] && found[1] && found[2],
           "polyf0: every chord tone tracked within 20 cents (%d%d%d)",
           found[0], found[1], found[2]);

    memset (frame, 0, (size_t) t.win_size * sizeof (float));
    for (int f = 0; f < 6; ++f) /* death is 4 misses; 6 is decisive */
        ae_polyf0_process (&t, frame);
    active = 0;
    for (int k = 0; k < AE_POLY_MAX_NOTES; ++k)
        if (t.notes[k].active)
            ++active;
    CHECK (active == 0, "polyf0: silence kills every note (%d left)", active);

    ae_polyf0_free (&t);
    free (frame);
}

/* The detection EXPORT: in poly mode the tracker's chord is published
   per-note (packed words -> status polyDetected) whether or not a sample
   bank is striking -- that is what lets a host use the detection for its
   own purposes. No bank is loaded here on purpose: the reverted behaviour
   ran the tracker only for the chord sampler, and this test fails against
   it. */
static void test_poly_detect_export (void)
{
    AeCorrector *p = calloc (1, sizeof (AeCorrector));
    ae_corrector_prepare (p, 48000.0, 512, 0.0, 0.0,
                          AE_SHIFT_QUALITY_BALANCED
                          | AE_SHIFT_QUALITY_POLY_FLAG);
    ae_corrector_set_edo (p, 12);

    const double notes[3] = { 130.81, 164.81, 196.0 }; /* C3 E3 G3 */
    const int    degs[3]  = { 36, 40, 43 };            /* re engine C0 */
    const int total = 512 * 190; /* ~2 s */
    float *buf = calloc ((size_t) total, sizeof (float));
    double ph[3] = { 0.0, 0.3, 0.7 };
    for (int i = 0; i < total; ++i)
    {
        double v = 0.0;
        for (int k = 0; k < 3; ++k)
        {
            ph[k] += 2.0 * M_PI * notes[k] / 48000.0;
            v += sin (ph[k]) + 0.30 * sin (2.0 * ph[k]);
        }
        buf[i] = (float) (0.1 * v);
    }
    for (int off = 0; off < total; off += 512)
        ae_corrector_process (p, buf + off, NULL, NULL, 512);

    int published = 0, ids[AE_POLY_MAX_NOTES];
    bool found[3] = { false, false, false };
    for (int k = 0; k < AE_POLY_MAX_NOTES; ++k)
    {
        const uint64_t w = ae_corrector_poly_note (p, k);
        if (ae_poly_note_hz (w) <= 0.0f)
            continue;
        ids[published++] = ae_poly_note_id (w);
        CHECK (ae_poly_note_level (w) > 0.0,
               "poly export: a published note carries a level");
        for (int m = 0; m < 3; ++m)
            if (fabs (1200.0 * log2 ((double) ae_poly_note_hz (w)
                                     / notes[m])) < 20.0)
            {
                found[m] = true;
                CHECK (ae_poly_note_deg (w) == degs[m],
                       "poly export: hz %.1f snaps to degree %d (got %d)",
                       (double) ae_poly_note_hz (w), degs[m],
                       ae_poly_note_deg (w));
                CHECK (ae_follow_encode_midi (degs[m], 12) == 12 + degs[m],
                       "poly export: the FOLLOW note encoding holds in "
                       "12-EDO (deg %d)", degs[m]);
            }
    }
    CHECK (published == 3 && found[0] && found[1] && found[2],
           "poly export: the chord is published WITHOUT a bank "
           "(%d notes, %d%d%d)", published, found[0], found[1], found[2]);
    CHECK (ids[0] != ids[1] && ids[1] != ids[2] && ids[0] != ids[2],
           "poly export: note ids are distinct");
    CHECK (ae_corrector_poly_active (p) == 3,
           "poly export: the count matches the list (got %d)",
           ae_corrector_poly_active (p));

    ae_corrector_free (p);
    free (p); free (buf);
}

static void write_wav_pcm (const char *path, const float *pcm, int n);

/* POLY + leadSource "sample": the MEL9 move. Play a chord, hear the loaded
   LIBRARY play that chord. The recording is a pure sine, and the input
   carries strong 2nd/3rd harmonics -- so if the harmonics show up at the
   output the chord came through the shifter (the reverted integration),
   and if each chord tone comes back harmonic-free it was re-struck from
   the bank. Pitch alone could not tell those apart. */
static void test_chord_sampler (void)
{
    const char *root = "/tmp/ae-smp-poly";
    char dir[256], pth[512], cmd[512];
    snprintf (dir, sizeof (dir), "%s/piano", root);
    snprintf (cmd, sizeof (cmd), "rm -rf %s && mkdir -p %s", root, dir);
    if (system (cmd) != 0) { CHECK (false, "chord sampler: cannot stage"); return; }
    snprintf (pth, sizeof (pth), "%s/C4.wav", dir);
    {
        /* A SUSTAINED pure sine (no decay): rate-shifting a decaying
           recording stretches its decay differently per note, which would
           skew the balance this test measures. */
        const int n = 4 * 48000;
        float *pcm = calloc ((size_t) n, sizeof (float));
        double phr = 0.0;
        for (int i = 0; i < n; ++i)
        {
            phr += 2.0 * M_PI * 261.6256 / 48000.0;
            pcm[i] = (float) (0.5 * sin (phr));
        }
        write_wav_pcm (pth, pcm, n);
        free (pcm);
    }

    const double notes[3] = { 130.81, 164.81, 196.0 }; /* C3 E3 G3 */
    for (int cap = 3; cap >= 1; cap -= 2) /* full chord, then polyNotes 1 */
    {
        AeCorrector *p = calloc (1, sizeof (AeCorrector));
        ae_corrector_prepare (p, 48000.0, 512, 0.0, 0.0,
                              AE_SHIFT_QUALITY_BALANCED
                              | AE_SHIFT_QUALITY_POLY_FLAG);
        ae_corrector_set_edo (p, 12);
        ae_corrector_set_retune_ms (p, 0.0);
        ae_corrector_set_transition_ms (p, 0.0);
        ae_corrector_set_poly_notes (p, cap);
        char err[256] = "";
        CHECK (ae_corrector_load_samples (p, root, "piano", NULL, err, sizeof (err)),
               "chord sampler: bank loads (%s)", err);
        {
            int srcs[AE_HARM_VOICES];
            for (int v = 0; v < AE_HARM_VOICES; ++v) srcs[v] = AE_HARM_SRC_DEFAULT;
            ae_corrector_set_voice_sources (p, srcs, AE_HARM_SRC_SAMPLE);
        }
        ae_corrector_set_sample (p, 1.0, 0.9, true); /* library only, fixed strike */

        const int total = 512 * 280; /* ~3 s */
        float *buf = calloc ((size_t) total, sizeof (float));
        double ph[3] = { 0.0, 0.3, 0.7 };
        for (int i = 0; i < total; ++i)
        {
            double v = 0.0;
            for (int k = 0; k < 3; ++k)
            {
                ph[k] += 2.0 * M_PI * notes[k] / 48000.0;
                v += sin (ph[k]) + 0.35 * sin (2.0 * ph[k])
                                 + 0.20 * sin (3.0 * ph[k]);
            }
            buf[i] = (float) (0.12 * v);
        }
        for (int off = 0; off < total; off += 512)
            ae_corrector_process (p, buf + off, NULL, NULL, 512);

        const float *tail = buf + total - 48000;
        double fund[3], strongest = 0.0;
        int sounding = 0;
        for (int k = 0; k < 3; ++k)
        {
            fund[k] = goertzel (tail, 48000, notes[k], 48000.0);
            if (fund[k] > strongest) strongest = fund[k];
        }
        for (int k = 0; k < 3; ++k)
            if (fund[k] > 0.2 * strongest)
                ++sounding;

        if (cap == 3)
        {
            CHECK (sounding == 3,
                   "chord sampler: the library plays every chord tone "
                   "(%d of 3: %.3g %.3g %.3g)", sounding,
                   fund[0], fund[1], fund[2]);
            /* The discriminator: the input's harmonics must NOT arrive.
                2xE3 and 2xG3 are no chord tone's fundamental. */
            const double h2e = goertzel (tail, 48000, 2.0 * notes[1], 48000.0);
            const double h2g = goertzel (tail, 48000, 2.0 * notes[2], 48000.0);
            CHECK (h2e < 0.1 * fund[1] && h2g < 0.1 * fund[2],
                   "chord sampler: the output is the RECORDING, not the "
                   "shifted input (2nd harmonics %.3g/%.3g vs %.3g/%.3g)",
                   h2e, h2g, fund[1], fund[2]);
            CHECK (ae_corrector_poly_active (p) == 3,
                   "chord sampler: polyNotesActive reports the chord (got %d)",
                   ae_corrector_poly_active (p));
        }
        else
        {
            CHECK (sounding == 1,
                   "polyNotes 1: exactly one tone sounds "
                   "(%d: %.3g %.3g %.3g)", sounding,
                   fund[0], fund[1], fund[2]);
            CHECK (ae_corrector_poly_active (p) == 1,
                   "polyNotes 1: polyNotesActive honours the cap (got %d)",
                   ae_corrector_poly_active (p));
        }

        ae_corrector_free (p);
        free (p); free (buf);
    }
    snprintf (cmd, sizeof (cmd), "rm -rf %s", root);
    if (system (cmd) != 0) { /* best effort */ }
}

/* Onset-first staging in the chord sampler (research batch 2026-08):
   an onset doubles the tracker rate for 150 ms (fresh chords commit a
   frame sooner) and arms a re-strum window -- a still-tracked note whose
   raw salience rises past its onset-time baseline was re-plucked and
   strikes again (without this a re-strum of a ringing chord is silent).
   Early wrong guesses (born then corrected/died young) crossfade out in
   6 ms instead of ringing at a wrong degree through the release. */
static void test_poly_onset_response (void)
{
    const char *root = "/tmp/ae-smp-onset";
    char dir[256], pth[512], cmd[512];
    snprintf (dir, sizeof (dir), "%s/piano", root);
    snprintf (cmd, sizeof (cmd), "rm -rf %s && mkdir -p %s", root, dir);
    if (system (cmd) != 0) { CHECK (false, "onset response: cannot stage"); return; }
    snprintf (pth, sizeof (pth), "%s/C4.wav", dir);
    {
        const int n = 4 * 48000; /* sustained sine: no decay skew */
        float *pcm = calloc ((size_t) n, sizeof (float));
        double phr = 0.0;
        for (int i = 0; i < n; ++i)
        {
            phr += 2.0 * M_PI * 261.6256 / 48000.0;
            pcm[i] = (float) (0.5 * sin (phr));
        }
        write_wav_pcm (pth, pcm, n);
        free (pcm);
    }

    AeCorrector *p = calloc (1, sizeof (AeCorrector));
    ae_corrector_prepare (p, 48000.0, 512, 0.0, 0.0,
                          AE_SHIFT_QUALITY_BALANCED
                          | AE_SHIFT_QUALITY_POLY_FLAG);
    ae_corrector_set_edo (p, 12);
    ae_corrector_set_retune_ms (p, 0.0);
    ae_corrector_set_transition_ms (p, 0.0);
    char err[256] = "";
    CHECK (ae_corrector_load_samples (p, root, "piano", NULL, err, sizeof (err)),
           "onset response: bank loads (%s)", err);
    {
        int srcs[AE_HARM_VOICES];
        for (int v = 0; v < AE_HARM_VOICES; ++v) srcs[v] = AE_HARM_SRC_DEFAULT;
        ae_corrector_set_voice_sources (p, srcs, AE_HARM_SRC_SAMPLE);
    }
    ae_corrector_set_sample (p, 1.0, 0.9, true);
    ae_corrector_set_lead_env (p, 5.0, 400.0); /* audible ring for the count */

    /* Chord attacks at `quiet`, decays to ~0.3, is re-plucked at t1. */
    const double notes[3] = { 130.81, 164.81, 196.0 };
    const int quiet = 512 * 20, t1 = 512 * 120, total = 512 * 220;
    float *buf = calloc ((size_t) total, sizeof (float));
    double ph[3] = { 0.0, 0.3, 0.7 };
    for (int i = 0; i < total; ++i)
    {
        double v = 0.0;
        for (int k = 0; k < 3; ++k)
        {
            ph[k] += 2.0 * M_PI * notes[k] / 48000.0;
            v += sin (ph[k]) + 0.35 * sin (2.0 * ph[k]);
        }
        double env = 0.0;
        if (i >= quiet)
        {
            const int since = i - (i >= t1 ? t1 : quiet);
            env = exp (-(double) since / (0.45 * 48000.0));
        }
        buf[i] = (float) (0.15 * v * env);
    }

    int live_pre = 0, live_post = 0;
    for (int off = 0; off < total; off += 512)
    {
        ae_corrector_process (p, buf + off, NULL, NULL, 512);
        if (off == t1 - 512 * 4 || off == t1 + 512 * 28)
        {
            int live = 0;
            for (int k = 0; k < AE_POLY_MAX_NOTES; ++k)
                for (int sl = 0; sl < AE_SMP_SLOTS; ++sl)
                    if (p->smp[k][sl].rec != NULL)
                        ++live;
            if (off < t1) live_pre = live; else live_post = live;
        }
    }

    /* Onset burst: the library answers a fresh chord fast. Without the
       burst this measures 32 ms; the bound discriminates. */
    int first = -1;
    for (int i = quiet; i < total; ++i)
        if (fabsf (buf[i]) > 1e-4f) { first = i; break; }
    CHECK (first >= 0 && (first - quiet) <= (int) (0.027 * 48000.0),
           "onset burst: first sample out within 27 ms of the pluck "
           "(got %.1f ms)", first < 0 ? -1.0 : (first - quiet) / 48.0);

    /* One live slot per chord tone before the re-strum: no double
       strikes on the attack, no early wrong guesses left ringing. */
    CHECK (live_pre == 3,
           "onset response: a struck chord holds exactly one slot per "
           "tone (got %d)", live_pre);
    /* The re-strum strikes again on every row: old ring + fresh strike.
       Without restrike this stays at 3-4. */
    CHECK (live_post >= 5,
           "re-strum: an onset on a ringing chord strikes fresh slots "
           "(got %d live)", live_post);

    /* And it is audible: energy after the re-pluck beats the decayed
       ring before it. Reverted this measures ~1.2. */
    double pre = 0.0, post = 0.0;
    for (int i = t1 - 12000; i < t1 - 2400; ++i)
        pre += (double) buf[i] * buf[i];
    for (int i = t1 + 2400; i < t1 + 12000; ++i)
        post += (double) buf[i] * buf[i];
    CHECK (pre > 0.0 && post > 1.45 * pre,
           "re-strum: the fresh strike is audible over the ring "
           "(ratio %.2f)", pre > 0.0 ? post / pre : -1.0);

    ae_corrector_free (p);
    free (p); free (buf);
    snprintf (cmd, sizeof (cmd), "rm -rf %s", root);
    if (system (cmd) != 0) { /* best effort */ }
}

static void test_follow_link (void)
{
    /* encode: identity inside range, class-preserving fold outside */
    CHECK (ae_follow_encode_midi (4 * 22, 22) == 60,
           "follow encode: the pivot degree is middle C in any EDO");
    CHECK (ae_follow_encode_midi (4 * 22 + 9, 22) == 69,
           "follow encode: offsets ride steps");
    {
        const int deg = 4 * 72 - 100;          /* far below MIDI range raw */
        const int n = ae_follow_encode_midi (deg, 72);
        CHECK (n >= 0 && n <= 127
               && ((n - 60) % 72 + 72) % 72 == ((deg - 4 * 72) % 72 + 72) % 72,
               "follow encode: out-of-range degrees fold by equaves, class "
               "intact (deg %d -> n %d in 72-EDO)", deg, n);
    }

    /* envelope depth: level 0 at depth 1 cuts the wet lead */
    double rms_cut = 0.0, rms_open = 0.0;
    for (int c = 0; c < 2; ++c)
    {
        AeCorrector *p = calloc (1, sizeof (AeCorrector));
        ae_corrector_prepare (p, 48000.0, 512, 0.0, 0.0, AE_SHIFT_QUALITY_BALANCED);
        ae_corrector_set_edo (p, 12);
        ae_corrector_set_retune_ms (p, 0.0);
        ae_corrector_set_transition_ms (p, 0.0);
        ae_corrector_set_follow (p, 1.0);
        ae_corrector_set_follow_level (p, c == 0 ? 0.0 : 1.0);
        const int total = 512 * 188; /* block-aligned */
        float *buf = calloc ((size_t) total, sizeof (float));
        double ph = 0.0;
        for (int i = 0; i < total; ++i)
        {
            ph += 2.0 * M_PI * 220.0 / 48000.0;
            buf[i] = (float) (0.3 * sin (ph));
        }
        for (int off = 0; off < total; off += 512)
            ae_corrector_process (p, buf + off, NULL, NULL, 512);
        double sq = 0.0;
        for (int i = total - 24000; i < total; ++i)
            sq += (double) buf[i] * buf[i];
        if (c == 0) rms_cut = sqrt (sq / 24000.0);
        else        rms_open = sqrt (sq / 24000.0);
        ae_corrector_free (p); free (p); free (buf);
    }
    CHECK (rms_open > 0.05 && rms_cut < rms_open * 0.02,
           "followEnv 1: a source at level 0 CUTS the wet output "
           "(open %.4f vs cut %.4f); notes stopping must stop the voice",
           rms_open, rms_cut);

    /* the lead degree publishes, and silence withdraws it */
    {
        AeCorrector *p = calloc (1, sizeof (AeCorrector));
        ae_corrector_prepare (p, 48000.0, 512, 0.0, 0.0, AE_SHIFT_QUALITY_BALANCED);
        ae_corrector_set_edo (p, 12);
        const int total = 512 * 60;
        float *buf = calloc ((size_t) (total + 512 * 40), sizeof (float));
        double ph = 0.0;
        for (int i = 0; i < total; ++i)
        {
            ph += 2.0 * M_PI * 440.0 / 48000.0;
            buf[i] = (float) (0.3 * sin (ph));
        }
        for (int off = 0; off < total; off += 512)
            ae_corrector_process (p, buf + off, NULL, NULL, 512);
        const int deg = ae_corrector_lead_degree (p);
        CHECK (deg != AE_HARM_DEG_OFF && deg == 4 * 12 + 9,
               "follow: the lead's corrected degree publishes (%d, want %d "
               "= A4 in 12-EDO)", deg, 4 * 12 + 9);
        for (int off = 0; off < 512 * 40; off += 512)
            ae_corrector_process (p, buf + total + off, NULL, NULL, 512);
        CHECK (ae_corrector_lead_degree (p) == AE_HARM_DEG_OFF,
               "follow: silence withdraws the degree (note-stop transmits)");
        ae_corrector_free (p); free (p); free (buf);
    }
}

static void test_detection_lock_time (void)
{
    AeCorrector *p = calloc (1, sizeof (AeCorrector));
    ae_corrector_prepare (p, 48000.0, 512, 78.0, 1400.0, AE_SHIFT_QUALITY_BALANCED);
    ae_corrector_set_edo (p, 12);
    ae_corrector_set_retune_ms (p, 0.0);
    ae_corrector_set_transition_ms (p, 0.0);

    const double hz = 220.0 * pow (2.0, 15.0 / 1200.0);
    const int gap = 14400, note = 24000, total = gap + note;
    float *in = calloc ((size_t) total, sizeof (float));
    double ph = 0.0;
    for (int i = gap; i < total; ++i)
    {
        const double t = (i - gap) / 48000.0;
        ph += hz / 48000.0;
        double sig = 0.0;
        for (int h = 1; h <= 10; ++h)
            sig += sin (2.0 * M_PI * ph * h + 0.3 * h) / h;
        in[i] = (float) (0.12 * sig * exp (-1.2 * t) * (t < 0.004 ? t / 0.004 : 1.0));
    }
    double first_ms = -1.0;
    for (int off = 0; off < total; off += 512)
    {
        const int n = total - off < 512 ? total - off : 512;
        ae_corrector_process (p, in + off, NULL, NULL, n);
        const double d = (double) atomic_load_explicit (
            &p->detected_hz_out, memory_order_relaxed);
        if (first_ms < 0.0 && d > 0.0 && fabs (1200.0 * log2 (d / hz)) < 50.0)
            first_ms = (off + n - gap) * 1000.0 / 48000.0;
    }
    CHECK (first_ms > 0.0 && first_ms <= 34.0,
           "detection locks a fresh pluck within 34 ms (%.1f; the pow-2 "
           "padded frame took ~52 in the worst case)", first_ms);
    ae_corrector_free (p);
    free (p); free (in);
}

/* An energy onset clears the detector's octave-continuity claim: the
   raised bar for CHANGING octave is about the note that just ended, and
   carrying it across a legato boundary makes a leap's first frames fight
   the old note's octave. Staged as a voiced tone switching pitch with an
   amplitude edge and NO silent gap -- without the reset, last_best_tau
   rides through the boundary still holding the old note's lag. */
static void test_onset_clears_continuity (void)
{
    AeCorrector *p = calloc (1, sizeof (AeCorrector));
    ae_corrector_prepare (p, 48000.0, 512, 78.0, 1400.0, AE_SHIFT_QUALITY_BALANCED);
    ae_corrector_set_edo (p, 12);
    const int a = 512 * 94, total = a + 512; /* block-aligned boundary */
    float *in = calloc ((size_t) total, sizeof (float));
    double ph = 0.0;
    for (int i = 0; i < a; ++i)
    {
        ph += 2.0 * M_PI * 220.0 / 48000.0;
        in[i] = (float) (0.08 * sin (ph));
    }
    for (int i = a; i < total; ++i) /* leap + hard edge, no gap */
    {
        ph += 2.0 * M_PI * 660.0 / 48000.0;
        in[i] = (float) (0.4 * sin (ph));
    }
    for (int off = 0; off < a; off += 512)
        ae_corrector_process (p, in + off, NULL, NULL, 512);
    CHECK (p->detector.last_best_tau > 0,
           "continuity: a running voiced note holds its octave claim (%d)",
           p->detector.last_best_tau);
    ae_corrector_process (p, in + a, NULL, NULL, 512);
    CHECK (p->detector.last_best_tau == 0,
           "continuity: an energy onset CLEARS the claim (%d; a leap's "
           "first frames must not fight the old note's octave)",
           p->detector.last_best_tau);
    ae_corrector_free (p);
    free (p); free (in);
}

/* A plucked string STARTS SHARP (+20..40 cents, settling over ~50 ms) --
   and in 22-EDO the boundary is only 27.3 cents away, so with a few
   cents of ordinary intonation offset every RE-PLUCK of the same note
   flipped the target one step up (field report: "repeated notes of the
   same pitch trigger the next highest edostep"). The attack hold keeps
   the note that was already sounding through its own transient, via a
   remembered degree that survives the 1-2 unvoiced hops a pick costs. */
static void test_attack_hold (void)
{
    AeCorrector *p = calloc (1, sizeof (AeCorrector));
    ae_corrector_prepare (p, 48000.0, 512, 0.0, 0.0, AE_SHIFT_QUALITY_BALANCED);
    ae_corrector_set_edo (p, 22);
    ae_corrector_set_retune_ms (p, 20.0);
    ae_corrector_set_transition_ms (p, 50.0);

    /* a 22-EDO degree, played +12c sharp (real fretting), each pluck
       starting +25c sharper still and settling with tau 40 ms */
    const int deg = 4 * 22 + 7;
    const double true_hz = ae_degree_hz (deg, 22, AE_REFERENCE_C0_HZ, 1200.0);
    const double f0 = true_hz * pow (2.0, 12.0 / 1200.0);
    const int total = 512 * 188, pluck_every = 512 * 47;
    float *buf = calloc ((size_t) total, sizeof (float));
    double ph = 0.0;
    for (int i = 0; i < total; ++i)
    {
        const double t = (double) (i % pluck_every) / 48000.0;
        const double sharp = 1.0 + 0.0146 * exp (-t / 0.040);
        const double amp = 0.5 * exp (-t / 0.5);
        ph += 2.0 * M_PI * f0 * sharp / 48000.0;
        buf[i] = (float) (amp * (sin (ph) + 0.35 * sin (2.0 * ph)
                                 + 0.15 * sin (3.0 * ph)));
    }

    int bad = 0, seen = 0;
    for (int off = 0; off < total; off += 512)
    {
        ae_corrector_process (p, buf + off, NULL, NULL, 512);
        const float tgt = ae_corrector_target_hz (p);
        if (off >= pluck_every && tgt > 0.0f) /* from the 2nd pluck on */
        {
            ++seen;
            if (fabs (1200.0 * log2 ((double) tgt / true_hz)) > 27.0)
                ++bad;
        }
    }
    CHECK (seen > 100 && bad == 0,
           "attack hold: re-plucked notes never flip a degree "
           "(%d of %d hops off target)", bad, seen);

    /* Negative control: a REAL step up commits fast -- the hold must not
       tax legitimate moves. Same pluck shape, second half one degree up. */
    {
        AeCorrector *q = calloc (1, sizeof (AeCorrector));
        ae_corrector_prepare (q, 48000.0, 512, 0.0, 0.0, AE_SHIFT_QUALITY_BALANCED);
        ae_corrector_set_edo (q, 22);
        ae_corrector_set_retune_ms (q, 20.0);
        ae_corrector_set_transition_ms (q, 50.0);
        const double f1 = ae_degree_hz (deg + 1, 22, AE_REFERENCE_C0_HZ, 1200.0)
                          * pow (2.0, 12.0 / 1200.0);
        const int half = 512 * 94;
        double ph2 = 0.0;
        for (int i = 0; i < total; ++i)
        {
            const int since = i % pluck_every;
            const double t = (double) since / 48000.0;
            const double sharp = 1.0 + 0.0146 * exp (-t / 0.040);
            const double amp = 0.5 * exp (-t / 0.5);
            const double f = (i < half ? f0 : f1) * sharp;
            ph2 += 2.0 * M_PI * f / 48000.0;
            buf[i] = (float) (amp * (sin (ph2) + 0.35 * sin (2.0 * ph2)
                                     + 0.15 * sin (3.0 * ph2)));
        }
        const double t1 = ae_degree_hz (deg + 1, 22, AE_REFERENCE_C0_HZ, 1200.0);
        int settle_hops = -1, hops_after = 0;
        for (int off = 0; off < total; off += 512)
        {
            ae_corrector_process (q, buf + off, NULL, NULL, 512);
            if (off < half)
                continue;
            ++hops_after;
            const float tgt = ae_corrector_target_hz (q);
            if (settle_hops < 0 && tgt > 0.0f
                && fabs (1200.0 * log2 ((double) tgt / t1)) < 27.0)
                settle_hops = hops_after;
        }
        CHECK (settle_hops >= 0 && settle_hops <= 8,
               "attack hold: a real step up still commits promptly "
               "(%d blocks)", settle_hops);
        ae_corrector_free (q); free (q);
    }

    ae_corrector_free (p); free (p); free (buf);
}

static void test_release_pitch_stability (void)
{
    const int hold = 48000, rel = 3600, quiet = 48000;
    const int total = hold + rel + quiet;
    AeCorrector *p = calloc (1, sizeof (AeCorrector));
    ae_corrector_prepare (p, 48000.0, 512, 0.0, 0.0, AE_SHIFT_QUALITY_BALANCED);
    ae_corrector_set_edo (p, 12);
    ae_corrector_set_retune_ms (p, 20.0);
    ae_corrector_set_transition_ms (p, 50.0);

    AeHarmVoice voices[AE_HARM_VOICES];
    memset (voices, 0, sizeof (voices));
    for (int v = 0; v < AE_HARM_VOICES; ++v)
        voices[v].gain = 1.0;
    voices[0].interval = 7; /* P5 above A3: E4 */
    ae_corrector_set_harmony (p, true, 1, voices);
    ae_corrector_set_synth (p, AE_HARM_SRC_SYNTH,
                            ae_synth_patch_find ("sine"), 5.0, 800.0);

    /* A3 held; then a 75 ms release: pitch bends up ~80 cents while the
       level collapses (the mute), then silence. */
    float *in = calloc ((size_t) total, sizeof (float));
    float *hl = calloc ((size_t) total, sizeof (float));
    float *hr = calloc ((size_t) total, sizeof (float));
    double ph = 0.0;
    for (int i = 0; i < hold + rel; ++i)
    {
        double hz = 220.0, amp = 0.4;
        if (i >= hold)
        {
            const double w = (double) (i - hold) / rel;
            hz  = 220.0 * pow (2.0, 80.0 * w / 1200.0);
            amp = 0.4 * pow (10.0, -30.0 * w / 20.0); /* -30 dB across */
        }
        ph += 2.0 * M_PI * hz / 48000.0;
        in[i] = (float) (amp * (sin (ph) + 0.4 * sin (2.0 * ph)));
    }
    for (int off = 0; off < total; off += 512)
    {
        const int n = total - off < 512 ? total - off : 512;
        ae_corrector_process (p, in + off, hl + off, hr + off, n);
    }

    /* The ghost during the note... */
    const double held = peak_near (hl + hold - 24000, 20000, 329.63, 48000.0);
    /* ...and 150-450 ms into the ring-out, past the release artifact. */
    const double rung = peak_span (hl + hold + rel + 7200, 14400,
                                   329.63 * 0.93, 329.63 * 1.08, 48000.0);
    const double drift = 1200.0 * log2 (rung / held);
    CHECK (fabs (drift) < 15.0,
           "release keeps the held pitch (note %.2f Hz, tail %.2f Hz, "
           "drift %.1f cents)", held, rung, drift);

    ae_corrector_free (p);
    free (p); free (in); free (hl); free (hr);
}

static void test_attack_sound (void)
{
    const int quiet = 9600, sung = 48000, total = quiet + sung;
    double early[3]; /* off, pick, noise: harm-bus RMS in the first 60 ms */

    for (int c = 0; c < 3; ++c)
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
        /* 800 ms attack: the ghost itself is near-silent in the first 60 ms. */
        ae_corrector_set_synth (p, AE_HARM_SRC_SYNTH,
                                ae_synth_patch_find ("sine"), 800.0, 200.0);
        ae_corrector_set_attack (p, c == 0 ? AE_ATK_OFF
                                  : c == 1 ? AE_ATK_PICK : AE_ATK_NOISE, 1.0);

        float *in = calloc ((size_t) total, sizeof (float));
        float *hl = calloc ((size_t) total, sizeof (float));
        float *hr = calloc ((size_t) total, sizeof (float));
        double phase = 0.0;
        for (int i = quiet; i < total; ++i)
        {
            phase += 2.0 * M_PI * 220.0 / 48000.0;
            in[i] = (float) (0.4 * sin (phase) + 0.2 * sin (2.0 * phase));
        }
        for (int off = 0; off < total; off += 512)
        {
            const int n = total - off < 512 ? total - off : 512;
            ae_corrector_process (p, in + off, hl + off, hr + off, n);
        }

        double sq = 0.0;
        for (int i = quiet; i < quiet + 2880; ++i)
            sq += (double) hl[i] * hl[i];
        early[c] = sqrt (sq / 2880.0);

        ae_corrector_free (p);
        free (p); free (in); free (hl); free (hr);
    }

    CHECK (early[1] > 8.0 * early[0] + 1e-6,
           "attack pick covers the synth attack (pick %.4g vs off %.4g)",
           early[1], early[0]);
    CHECK (early[2] > 8.0 * early[0] + 1e-6,
           "attack noise covers the synth attack (noise %.4g vs off %.4g)",
           early[2], early[0]);
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

/* The local audio tap: the stub engine's processed output arrives through
   ae_audio_engine_tap_read with a monotonic, gapless sample counter and
   real signal in it -- the contract the UDP tap packets forward. */
static void test_audio_tap (void)
{
    AeEngineConfig cfg;
    memset (&cfg, 0, sizeof (cfg));
    cfg.buffer_frames     = 256;
    cfg.params.edo        = 12;
    cfg.params.amount     = 0.0;
    cfg.params.lead_on    = true;
    cfg.params.degrees_lo = ~0ull;
    cfg.params.degrees_hi = 0xffull;

    char err[256] = "";
    AeAudioEngine *e = ae_audio_engine_start (&cfg, err, sizeof (err));
    CHECK (e != NULL, "tap: engine starts (%s)", err);
    if (e == NULL)
        return;
    ae_audio_engine_set_tap (e, true, AE_SEND_WET);
    st_sleep_ms (300);

    float buf[4096];
    long long f1 = 0, f2 = 0;
    int n1 = ae_audio_engine_tap_read (e, buf, 4096, &f1);
    double energy = 0.0;
    for (int i = 0; i < n1; ++i)
        energy += (double) buf[i] * buf[i];
    st_sleep_ms (60);
    int n2 = ae_audio_engine_tap_read (e, buf, 4096, &f2);

    CHECK (n1 > 1000, "tap: audio arrives (%d samples in 300 ms)", n1);
    CHECK (energy > 1e-3, "tap: the wet stem carries signal (%.3g)", energy);
    CHECK (n2 > 0 && f2 == f1 + n1,
           "tap: the sample counter is gapless across reads "
           "(%lld + %d -> %lld)", f1, n1, f2);

    ae_audio_engine_stop (e);
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

/* The makeup gain on PERCUSSIVE input. Steady tones hid the defect: the
   shifter's output is the input from ~latency ago, so an unaligned level
   match sees every attack a whole latency early and every decay a latency
   late -- it pumped rail to rail on a pluck train (and the boosted lead
   drove the output soft clip: the field's "incredibly bassy" that survived
   every live flip and vanished on restart). Aligned and bounded, the
   makeup must stay near unity on plucks at ratio 1. */
/* The record send's "wet" tap: the corrected lead with the dry blend
   removed. Its defining property: while VOICED it carries the lead; while
   UNVOICED -- where the live output passes the dry input through -- it
   carries (near) silence. That is what makes the stem mixable against the
   dry track the interface records, instead of double-counting it. */
static void test_lead_wet_tap (void)
{
    AeCorrector *p = calloc (1, sizeof (AeCorrector));
    ae_corrector_prepare (p, 48000.0, 512, 0.0, 0.0, AE_SHIFT_QUALITY_BALANCED);
    ae_corrector_set_edo (p, 12);
    ae_corrector_set_retune_ms (p, 0.0);
    ae_corrector_set_transition_ms (p, 0.0);

    const int seg = 24576;
    float *in = calloc ((size_t) seg, sizeof (float));
    float *mono = malloc ((size_t) seg * sizeof (float));

    /* Voiced: a tone. wet ~ mono. */
    double ph = 0.0;
    for (int i = 0; i < seg; ++i)
    {
        ph += 2.0 * M_PI * 220.0 / 48000.0;
        in[i] = (float) (0.4 * sin (ph) + 0.1 * sin (2.0 * ph));
    }
    memcpy (mono, in, (size_t) seg * sizeof (float));
    double wet_sq = 0.0, mono_sq = 0.0;
    for (int off = 0; off < seg; off += 512)
    {
        ae_corrector_process (p, mono + off, NULL, NULL, 512);
        const float *w = ae_corrector_lead_wet (p);
        if (off >= seg / 2)
            for (int i = 0; i < 512; ++i)
            {
                wet_sq  += (double) w[i] * w[i];
                mono_sq += (double) mono[off + i] * mono[off + i];
            }
    }
    CHECK (wet_sq > 0.5 * mono_sq,
           "wet tap carries the voiced lead (wet %.3g vs mono %.3g)",
           wet_sq, mono_sq);

    /* Unvoiced: aperiodic noise. The live output passes dry; the tap must
       be near-silent -- that is the whole point of the stem. */
    unsigned rng = 1;
    for (int i = 0; i < seg; ++i)
    {
        rng = rng * 1664525u + 1013904223u;
        in[i] = (float) ((double) (rng >> 8) / 8388608.0 - 1.0) * 0.2f;
    }
    memcpy (mono, in, (size_t) seg * sizeof (float));
    wet_sq = mono_sq = 0.0;
    for (int off = 0; off < seg; off += 512)
    {
        ae_corrector_process (p, mono + off, NULL, NULL, 512);
        const float *w = ae_corrector_lead_wet (p);
        if (off >= seg / 2)
            for (int i = 0; i < 512; ++i)
            {
                wet_sq  += (double) w[i] * w[i];
                mono_sq += (double) mono[off + i] * mono[off + i];
            }
    }
    CHECK (mono_sq > 1e-4 && wet_sq < 0.05 * mono_sq,
           "wet tap drops the dry blend while unvoiced (wet %.3g vs mono %.3g)",
           wet_sq, mono_sq);

    ae_corrector_free (p);
    free (p); free (in); free (mono);
}

/* formantSemitones moves the spectral envelope without moving the pitch. */
static void test_formant_offset (void)
{
    const int total = 98304;
    float *in = calloc ((size_t) total, sizeof (float));
    float *out0 = calloc ((size_t) total, sizeof (float));
    float *out4 = calloc ((size_t) total, sizeof (float));
    double ph = 0.0;
    for (int i = 0; i < total; ++i)
    {
        ph += 2.0 * M_PI * 165.0 / 48000.0;
        in[i] = (float) (0.25 * sin (ph) + 0.25 * sin (2.0 * ph)
                       + 0.20 * sin (3.0 * ph) + 0.15 * sin (4.0 * ph)
                       + 0.10 * sin (5.0 * ph) + 0.08 * sin (6.0 * ph));
    }
    for (int c = 0; c < 2; ++c)
    {
        AeShifter *sh = ae_shifter_create (48000.0,
                            ae_shifter_block_samples (48000.0, AE_SHIFT_QUALITY_BALANCED));
        ae_shifter_set_semitones (sh, 0.0, 0.0);
        ae_shifter_set_formant_semitones (sh, c == 0 ? 0.0 : 5.0, true);
        ae_shifter_set_formant_base (sh, 165.0);
        float *out = c == 0 ? out0 : out4;
        for (int off = 0; off < total; off += 512)
            ae_shifter_process (sh, in + off, out + off, 512);
        ae_shifter_destroy (sh);
    }
    /* Pitch unchanged... */
    const double f0 = peak_near (out4 + total - 48000, 48000, 165.0, 48000.0);
    CHECK (fabs (1200.0 * log2 (f0 / 165.0)) < 15.0,
           "formant offset leaves the pitch alone (f0 %.2f Hz)", f0);
    /* ...but the harmonic balance moves: +5 st of tract tilts energy from
       the low partials toward the high ones relative to neutral. */
    const double lo0 = goertzel (out0 + total - 48000, 48000, 165.0, 48000.0)
                     + goertzel (out0 + total - 48000, 48000, 330.0, 48000.0);
    const double hi0 = goertzel (out0 + total - 48000, 48000, 660.0, 48000.0)
                     + goertzel (out0 + total - 48000, 48000, 825.0, 48000.0);
    const double lo4 = goertzel (out4 + total - 48000, 48000, 165.0, 48000.0)
                     + goertzel (out4 + total - 48000, 48000, 330.0, 48000.0);
    const double hi4 = goertzel (out4 + total - 48000, 48000, 660.0, 48000.0)
                     + goertzel (out4 + total - 48000, 48000, 825.0, 48000.0);
    const double tilt0 = hi0 / (lo0 > 1e-9 ? lo0 : 1e-9);
    const double tilt4 = hi4 / (lo4 > 1e-9 ? lo4 : 1e-9);
    CHECK (tilt4 > 1.3 * tilt0 || tilt4 < 0.77 * tilt0,
           "formant offset moves the envelope (tilt %.3g -> %.3g)",
           tilt0, tilt4);
    free (in); free (out0); free (out4);
}

/* ---- the sample source ------------------------------------------------- */

/* Write arbitrary mono float32 PCM as a WAV. */
static void write_wav_pcm (const char *path, const float *pcm, int n)
{
    FILE *f = fopen (path, "wb");
    if (f == NULL) return;
    const unsigned data = (unsigned) (n * 4), riff = 36 + data;
    fwrite ("RIFF", 1, 4, f); fwrite (&riff, 4, 1, f); fwrite ("WAVE", 1, 4, f);
    fwrite ("fmt ", 1, 4, f);
    const unsigned sz = 16; const unsigned short fmt = 3, ch = 1, bits = 32;
    const unsigned rate = 48000, byterate = 48000 * 4; const unsigned short align = 4;
    fwrite (&sz, 4, 1, f); fwrite (&fmt, 2, 1, f); fwrite (&ch, 2, 1, f);
    fwrite (&rate, 4, 1, f); fwrite (&byterate, 4, 1, f);
    fwrite (&align, 2, 1, f); fwrite (&bits, 2, 1, f);
    fwrite ("data", 1, 4, f); fwrite (&data, 4, 1, f);
    fwrite (pcm, 4, (size_t) n, f);
    fclose (f);
}

/* Write a mono float32 WAV whose content is a decaying sine at `hz`, so a
   test can read a bank back and hear which recording it got. */
static void write_wav (const char *path, double hz, double secs, double amp)
{
    const int n = (int) (secs * 48000.0);
    FILE *f = fopen (path, "wb");
    if (f == NULL) return;
    const unsigned data = (unsigned) (n * 4), riff = 36 + data;
    fwrite ("RIFF", 1, 4, f); fwrite (&riff, 4, 1, f); fwrite ("WAVE", 1, 4, f);
    fwrite ("fmt ", 1, 4, f);
    const unsigned sz = 16; const unsigned short fmt = 3, ch = 1, bits = 32;
    const unsigned rate = 48000, byterate = 48000 * 4; const unsigned short align = 4;
    fwrite (&sz, 4, 1, f); fwrite (&fmt, 2, 1, f); fwrite (&ch, 2, 1, f);
    fwrite (&rate, 4, 1, f); fwrite (&byterate, 4, 1, f);
    fwrite (&align, 2, 1, f); fwrite (&bits, 2, 1, f);
    fwrite ("data", 1, 4, f); fwrite (&data, 4, 1, f);
    double ph = 0.0;
    for (int i = 0; i < n; ++i)
    {
        ph += 2.0 * M_PI * hz / 48000.0;
        const float v = (float) (amp * sin (ph) * exp (-(double) i / (0.5 * 48000.0)));
        fwrite (&v, 4, 1, f);
    }
    fclose (f);
}

static void test_sampler_bank (void)
{
    const char *root = "/tmp/ae-smp-test";
    char dir[256];
    snprintf (dir, sizeof (dir), "%s/piano", root);
    char cmd[512];
    snprintf (cmd, sizeof (cmd), "rm -rf %s && mkdir -p %s", root, dir);
    if (system (cmd) != 0) { CHECK (false, "sampler: cannot stage fixtures"); return; }

    /* Two zones an octave apart; C4 has 2 main variants + a soft layer,
       C5 only a bare recording. Each recording sings its own zone pitch. */
    char pth[512];
    snprintf (pth, sizeof (pth), "%s/C4.wav",          dir); write_wav (pth, 261.6256, 1.0, 0.5);
    snprintf (pth, sizeof (pth), "%s/C4_rr2.wav",      dir); write_wav (pth, 261.6256, 1.0, 0.5);
    snprintf (pth, sizeof (pth), "%s/C4_soft.wav",     dir); write_wav (pth, 261.6256, 1.0, 0.5);
    snprintf (pth, sizeof (pth), "%s/C4_soft_rr2.wav", dir); write_wav (pth, 261.6256, 1.0, 0.5);
    snprintf (pth, sizeof (pth), "%s/C5.wav",          dir); write_wav (pth, 523.2511, 1.0, 0.5);
    snprintf (pth, sizeof (pth), "%s/notanote.wav",    dir); write_wav (pth, 100.0, 0.1, 0.5);

    AeSampleBank b;
    memset (&b, 0, sizeof (b));
    char err[256] = "";
    CHECK (ae_sampler_load (&b, root, "piano", NULL, 48000.0, AE_SMP_OCTAVE_AUTO, err, sizeof (err)),
           "sampler: bank loads (%s)", err);
    CHECK (b.n_zones == 2, "sampler: two zones (got %d)", b.n_zones);
    CHECK (b.n_recs == 5, "sampler: five recordings, junk name skipped (got %d)",
           b.n_recs);
    CHECK (b.zones[0] == 60 && b.zones[1] == 72,
           "sampler: C4=60 C5=72 (got %d, %d)", b.zones[0], b.zones[1]);

    /* Nearest zone by PITCH, and a FRACTIONAL rate -- an unquantised rate
       is what lands a 22-EDO degree exactly off a 12-per-octave map. */
    const double hz22 = 261.6256 * pow (2.0, 7.0 / 22.0); /* 7 steps of 22 */
    const int midi = (int) lround (12.0 * log2 (hz22 / 440.0) + 69.0);
    const int z = ae_sampler_zone (&b, midi);
    CHECK (z == 0, "sampler: nearest zone by pitch (got %d for midi %d)", z, midi);

    signed char rr[AE_SMP_MAX_ZONES * 2];
    memset (rr, -1, sizeof (rr));
    unsigned rng = 12345u;

    /* Soft layer is chosen on VELOCITY, and only where one exists. */
    const AeSampleRec *soft = ae_sampler_pick (&b, 0, 0.3, rr, &rng);
    const AeSampleRec *loud = ae_sampler_pick (&b, 0, 0.9, rr, &rng);
    CHECK (soft != NULL && soft->soft, "sampler: velocity 0.3 takes the soft pool");
    CHECK (loud != NULL && ! loud->soft, "sampler: velocity 0.9 takes the main pool");
    const AeSampleRec *nosoft = ae_sampler_pick (&b, 1, 0.1, rr, &rng);
    CHECK (nosoft != NULL && ! nosoft->soft,
           "sampler: a zone with no soft layer stays on the main pool");

    /* Round robin never repeats the immediately previous pick -- a repeated
       note reusing its recording machine-guns at once. */
    int repeats = 0;
    const AeSampleRec *prev = NULL;
    for (int i = 0; i < 200; ++i)
    {
        const AeSampleRec *r = ae_sampler_pick (&b, 0, 0.9, rr, &rng);
        if (r == prev) ++repeats;
        prev = r;
    }
    CHECK (repeats == 0, "sampler: no immediate round-robin repeat (%d in 200)",
           repeats);

    /* '#' is the other shipped spelling of a sharp (pizzicato uses it
       where the older sets use 's'); both must land on the same zone. */
    snprintf (pth, sizeof (pth), "%s/F#1.wav", dir); write_wav (pth, 46.25, 0.5, 0.4);
    snprintf (pth, sizeof (pth), "%s/Gs1.wav", dir); write_wav (pth, 51.91, 0.5, 0.4);
    AeSampleBank sh;
    memset (&sh, 0, sizeof (sh));
    CHECK (ae_sampler_load (&sh, root, "piano", NULL, 48000.0, AE_SMP_OCTAVE_AUTO, err, sizeof (err)),
           "sampler: sharps reload (%s)", err);
    bool got30 = false, got32 = false;
    for (int i = 0; i < sh.n_zones; ++i)
    {
        if (sh.zones[i] == 30) got30 = true;  /* F#1 */
        if (sh.zones[i] == 32) got32 = true;  /* G#1 */
    }
    CHECK (got30 && got32,
           "sampler: '#' and 's' sharps both parse (F#1=30 %d, Gs1=32 %d)",
           (int) got30, (int) got32);
    ae_sampler_free (&sh);

    /* Per-BANK level normalisation. The shipped sets are peak-normalised
       to targets ~20 dB apart, so without this, switching instrument moves
       the ghosts by that much. Measured on the MAIN layer and applied to
       the whole bank, which is what preserves the soft layer's deliberate
       peak-match (timbre swap, never a level change). */
    {
        char qdir[256], qp[512];
        snprintf (qdir, sizeof (qdir), "%s/quietset", root);
        snprintf (cmd, sizeof (cmd), "mkdir -p %s", qdir);
        if (system (cmd) == 0)
        {
            /* Same content as piano's C4, 20 dB down. */
            snprintf (qp, sizeof (qp), "%s/C4.wav", qdir);
            write_wav (qp, 261.6256, 1.0, 0.05);
            AeSampleBank q;
            memset (&q, 0, sizeof (q));
            if (ae_sampler_load (&q, root, "quietset", NULL, 48000.0,
                                 AE_SMP_OCTAVE_AUTO, err, sizeof (err)))
            {
                /* The loud bank was written at 0.5, this one at 0.05: the
                   normalisations must differ by ~20 dB in the other
                   direction, bringing both to the same reference. */
                const double d = 20.0 * log10 (q.norm / b.norm);
                CHECK (d > 15.0 && d < 25.0,
                       "sampler: bank level normalised (%.1f dB apart, "
                       "sources are 20 dB apart)", d);
                const double la = b.norm * b.meas_rms, lb = q.norm * q.meas_rms;
                CHECK (fabs (20.0 * log10 (la / lb)) < 1.0,
                       "sampler: both banks land on one reference (%.2f dB apart)",
                       20.0 * log10 (la / lb));
                ae_sampler_free (&q);
            }
        }
    }

    /* Filename pitch is not always sounding pitch: bass is named an octave
       ABOVE what it sounds, harpsichord an octave BELOW. A ghost asked for
       a pitch has to SOUND it, so the offset is folded in at index time. */
    {
        char bdir[256], bp[512];
        snprintf (bdir, sizeof (bdir), "%s/bass", root);
        snprintf (cmd, sizeof (cmd), "mkdir -p %s", bdir);
        if (system (cmd) == 0)
        {
            snprintf (bp, sizeof (bp), "%s/A2.wav", bdir);
            write_wav (bp, 110.0, 0.5, 0.4);
            AeSampleBank bb;
            memset (&bb, 0, sizeof (bb));
            if (ae_sampler_load (&bb, root, "bass", NULL, 48000.0,
                                 AE_SMP_OCTAVE_AUTO, err, sizeof (err)))
            {
                CHECK (bb.n_zones == 1 && bb.zones[0] == 33,
                       "sampler: bass A2 indexes as sounding A1=33 (got %d)",
                       bb.n_zones ? bb.zones[0] : -1);
                ae_sampler_free (&bb);
            }
            /* An explicit override beats the table, for any other set. */
            memset (&bb, 0, sizeof (bb));
            if (ae_sampler_load (&bb, root, "bass", NULL, 48000.0, 0,
                                 err, sizeof (err)))
            {
                CHECK (bb.n_zones == 1 && bb.zones[0] == 45,
                       "sampler: sampleOctave overrides the table (got %d)",
                       bb.n_zones ? bb.zones[0] : -1);
                ae_sampler_free (&bb);
            }
        }
    }

    /* Full-scale recordings are a DECODE FAULT, not a mix decision: a
       properly mastered set peaks below 0 dBFS. The shipped pizzicato set
       once had 32 files decoded 24-bit-as-16-bit, which loaded and played
       happily as full-scale noise -- silent corruption deserves a number. */
    {
        char cdir[256], cp[512];
        snprintf (cdir, sizeof (cdir), "%s/clipset", root);
        snprintf (cmd, sizeof (cmd), "mkdir -p %s", cdir);
        if (system (cmd) == 0)
        {
            snprintf (cp, sizeof (cp), "%s/C4.wav", cdir);
            write_wav (cp, 261.6256, 0.5, 1.6);   /* drives past full scale */
            snprintf (cp, sizeof (cp), "%s/E4.wav", cdir);
            write_wav (cp, 329.6276, 0.5, 0.4);   /* well mastered */
            AeSampleBank cb;
            memset (&cb, 0, sizeof (cb));
            if (ae_sampler_load (&cb, root, "clipset", NULL, 48000.0,
                                 AE_SMP_OCTAVE_AUTO, err, sizeof (err)))
            {
                CHECK (cb.clipped == 1,
                       "sampler: full-scale recordings are counted (%d of 2)",
                       cb.clipped);
                ae_sampler_free (&cb);
            }
        }
    }

    /* A SOFT-ONLY zone must sound at every velocity. Acoustic Instruments
       2.1's contrabass pizzicato ships eight notes with soft takes and no
       main take (rr:0 in its pack.json) -- upstream velocity coverage is
       simply uneven. The old pick fell back main-ward only, so a hard
       strike on such a zone returned NULL and more than half the
       instrument was silent above the layer threshold. */
    {
        char odir[300];
        snprintf (odir, sizeof (odir), "%s/softonly", root);
        snprintf (cmd, sizeof (cmd), "mkdir -p %s", odir);
        if (system (cmd) == 0)
        {
            char op[560];
            snprintf (op, sizeof (op), "%s/A4_soft.wav", odir);
            write_wav (op, 440.0, 0.4, 0.4);
            snprintf (op, sizeof (op), "%s/A4_soft_rr2.wav", odir);
            write_wav (op, 440.0, 0.4, 0.4);
            AeSampleBank ob;
            memset (&ob, 0, sizeof (ob));
            if (ae_sampler_load (&ob, root, "softonly", NULL, 48000.0,
                                 AE_SMP_OCTAVE_AUTO, err, sizeof (err)))
            {
                signed char rr[AE_SMP_MAX_ZONES * 2];
                memset (rr, -1, sizeof (rr));
                unsigned rng = 1;
                const AeSampleRec *hard =
                    ae_sampler_pick (&ob, 0, 0.9, rr, &rng);
                CHECK (hard != NULL && hard->soft,
                       "sampler: a HARD strike on a soft-only zone plays the "
                       "soft take instead of silence");
                ae_sampler_free (&ob);
            }
            else
                CHECK (false, "sampler: softonly load (%s)", err);
        }
    }

    /* A slow swell must not be normalised into the clip. The bank level
       is measured over each note's first 300 ms; a bowed swell has next to
       nothing there, so the measurement reads near-silence and the boost
       clamp alone allows +20 dB -- which, applied to files peak-normalised
       to -6 dBFS, parks every peak at +14. The ceiling caps the boost
       where the bank's loudest sample reaches -3 dBFS. Staged: a note
       whose first 300 ms is 1%% amplitude and whose body peaks at 0.5. */
    {
        char sdir[300];
        snprintf (sdir, sizeof (sdir), "%s/swell", root);
        snprintf (cmd, sizeof (cmd), "mkdir -p %s", sdir);
        if (system (cmd) == 0)
        {
            char sp2[560];
            snprintf (sp2, sizeof (sp2), "%s/A4.wav", sdir);
            /* hand-build: 300 ms near-silence then a loud body */
            {
                const int n = 48000;
                float *pcm = malloc ((size_t) n * sizeof (float));
                for (int i = 0; i < n; ++i)
                {
                    const double amp = i < 14400 ? 0.005 : 0.5;
                    pcm[i] = (float) (amp * sin (2.0 * M_PI * 440.0 * i / 48000.0));
                }
                write_wav_pcm (sp2, pcm, n);
                free (pcm);
            }
            AeSampleBank sb;
            memset (&sb, 0, sizeof (sb));
            if (ae_sampler_load (&sb, root, "swell", NULL, 48000.0,
                                 AE_SMP_OCTAVE_AUTO, err, sizeof (err)))
            {
                CHECK (sb.norm * 0.5 < 0.75,
                       "sampler: a swell's boost is capped at the -3 dBFS peak "
                       "ceiling (norm %.1f puts its peak at %.2f; the 300 ms "
                       "window alone would boost it 10x into the clip)",
                       sb.norm, sb.norm * 0.5);
                ae_sampler_free (&sb);
            }
            else
                CHECK (false, "sampler: swell load (%s)", err);
        }
    }

    /* Deep round-robin pools load COMPLETELY. The Plucked Acoustics banjo
       carries eleven takes on one note; the loader skips files past
       AE_SMP_MAX_RR with no error path, so an undersized cap is a silent
       drop of recordings that the zone/file counts in status would still
       appear to include -- the worst kind of missing. Stage six variants
       and demand the whole pool. */
    {
        char rdir[300];
        snprintf (rdir, sizeof (rdir), "%s/rrdeep", root);
        snprintf (cmd, sizeof (cmd), "mkdir -p %s", rdir);
        if (system (cmd) == 0)
        {
            char rp[560];
            snprintf (rp, sizeof (rp), "%s/A4.wav", rdir);
            write_wav (rp, 440.0, 0.4, 0.4);
            for (int r = 2; r <= 6; ++r)
            {
                snprintf (rp, sizeof (rp), "%s/A4_rr%d.wav", rdir, r);
                write_wav (rp, 440.0, 0.4, 0.4);
            }
            AeSampleBank rb;
            memset (&rb, 0, sizeof (rb));
            if (ae_sampler_load (&rb, root, "rrdeep", NULL, 48000.0,
                                 AE_SMP_OCTAVE_AUTO, err, sizeof (err)))
            {
                CHECK (rb.n_recs == 6 && rb.n_zones == 1 && rb.main_n[0] == 6,
                       "sampler: a six-deep round-robin pool loads whole "
                       "(%d recs, pool %d; short = the cap silently ate takes)",
                       rb.n_recs, rb.n_zones > 0 ? rb.main_n[0] : -1);
                ae_sampler_free (&rb);
            }
            else
                CHECK (false, "sampler: rrdeep load (%s)", err);
        }
    }

    /* Instrument discovery: the engine carries no instrument list, so a
       cache directory is the only source of truth. */
    char names[AE_SMP_MAX_INSTRUMENTS][32];
    const int ni = ae_sampler_list (root, names, AE_SMP_MAX_INSTRUMENTS);
    CHECK (ni == 7, "sampler: discovery finds every instrument folder "
           "(%d; rrdeep, swell and softonly are 5-7)", ni);
    snprintf (cmd, sizeof (cmd), "mkdir -p %s/emptyish && touch %s/emptyish/readme.txt",
              root, root);
    if (system (cmd) == 0)
        CHECK (ae_sampler_list (root, names, AE_SMP_MAX_INSTRUMENTS) == 7,
               "sampler: a folder with no recordings is not an instrument");

    ae_sampler_free (&b);
    CHECK (b.n_recs == 0, "sampler: free clears the bank");
    snprintf (cmd, sizeof (cmd), "rm -rf %s", root);
    if (system (cmd) != 0) { /* best effort */ }
}

/* A sample ghost is STRUCK at the lead's onset and RE-PITCHED after -- it
   must sound at the ghost's pitch, and a held note must not re-strike. */
static void test_sample_ghost (void)
{
    const char *root = "/tmp/ae-smp-ghost";
    char dir[256], pth[512], cmd[512];
    snprintf (dir, sizeof (dir), "%s/piano", root);
    snprintf (cmd, sizeof (cmd), "rm -rf %s && mkdir -p %s", root, dir);
    if (system (cmd) != 0) { CHECK (false, "sample ghost: cannot stage"); return; }
    /* A PURE SINE at C4. The input below carries a strong 2nd harmonic, so
       a shifted ghost would bring that harmonic along with it and a sample
       ghost cannot -- which is what tells the two sources apart. A pitch
       test alone could not: a shifted fifth of A3 is also E4. */
    snprintf (pth, sizeof (pth), "%s/C4.wav", dir); write_wav (pth, 261.6256, 3.0, 0.5);

    AeCorrector *p = calloc (1, sizeof (AeCorrector));
    ae_corrector_prepare (p, 48000.0, 512, 0.0, 0.0, AE_SHIFT_QUALITY_BALANCED);
    ae_corrector_set_edo (p, 12);
    ae_corrector_set_retune_ms (p, 0.0);
    ae_corrector_set_transition_ms (p, 0.0);
    char err[256] = "";
    CHECK (ae_corrector_load_samples (p, root, "piano", NULL, err, sizeof (err)),
           "sample ghost: bank loads into the corrector (%s)", err);

    AeHarmVoice voices[AE_HARM_VOICES];
    memset (voices, 0, sizeof (voices));
    for (int v = 0; v < AE_HARM_VOICES; ++v)
        voices[v].gain = 1.0;
    voices[0].interval = 7;                     /* a fifth above A3 -> E4 */
    ae_corrector_set_harmony (p, true, 0, voices);
    ae_corrector_set_sample (p, 1.0, 0.9, false);      /* sample only, fixed strike */
    {
        int srcs[AE_HARM_VOICES];
        for (int v = 0; v < AE_HARM_VOICES; ++v) srcs[v] = AE_HARM_SRC_DEFAULT;
        ae_corrector_set_voice_sources (p, srcs, AE_HARM_SRC_VOICE);
    }
    ae_corrector_set_synth (p, AE_HARM_SRC_SAMPLE, 0, 5.0, 400.0);

    const int quiet = 9600, sung = 72704, total = quiet + sung;
    float *in = calloc ((size_t) total, sizeof (float));
    float *hl = calloc ((size_t) total, sizeof (float));
    float *hr = calloc ((size_t) total, sizeof (float));
    double ph = 0.0;
    for (int i = quiet; i < total; ++i)
    {
        ph += 2.0 * M_PI * 220.0 / 48000.0;
        in[i] = (float) (0.4 * sin (ph) + 0.35 * sin (2.0 * ph));
    }
    for (int off = 0; off < total; off += 512)
    {
        const int n = total - off < 512 ? total - off : 512;
        ae_corrector_process (p, in + off, hl + off, hr + off, n);
    }

    /* Sings E4 (329.63), not the recording's C4 -- the rate transposed it. */
    const double p_fifth = goertzel (hl + quiet + 9600, 24000, 329.63, 48000.0);
    const double p_zone  = goertzel (hl + quiet + 9600, 24000, 261.63, 48000.0);
    CHECK (p_fifth > 8.0 * p_zone,
           "sample ghost sings its own pitch, not the zone's (E4 %.3g vs C4 %.3g)",
           p_fifth, p_zone);
    /* ...and it is really the SAMPLE: the recording is a pure sine, so the
       input's strong 2nd harmonic must NOT come along. A shifted ghost
       would carry it up to 659 Hz. */
    const double p_h2 = goertzel (hl + quiet + 9600, 24000, 659.26, 48000.0);
    CHECK (p_h2 < 0.05 * p_fifth,
           "sample ghost is the recording, not a shifted copy "
           "(2nd harmonic %.3g vs fundamental %.3g)", p_h2, p_fifth);
    CHECK (ae_corrector_voice_source (p, 0) == AE_HARM_SRC_SAMPLE,
           "sample ghost: the source switch actually took");

    ae_corrector_free (p);
    free (p); free (in); free (hl); free (hr);
    snprintf (cmd, sizeof (cmd), "rm -rf %s", root);
    if (system (cmd) != 0) { /* best effort */ }
}

/* Strike level tracks THIS note, not the last one. Measuring takes 30 ms
   and the strike cannot wait for it, so a voice is struck at the fast
   follower's estimate and the window's verdict refines it (ramped). The
   version this replaced read the LAST completed measurement at strike
   time, which struck every note at the previous note's level -- exactly
   wrong the moment the dynamics change. */
static void test_sample_velocity (void)
{
    const char *root = "/tmp/ae-smp-vel";
    char dir[256], pth[512], cmd[512];
    snprintf (dir, sizeof (dir), "%s/piano", root);
    snprintf (cmd, sizeof (cmd), "rm -rf %s && mkdir -p %s", root, dir);
    if (system (cmd) != 0) { CHECK (false, "sample velocity: cannot stage"); return; }
    snprintf (pth, sizeof (pth), "%s/C4.wav", dir); write_wav (pth, 261.6256, 3.0, 0.5);

    AeCorrector *p = calloc (1, sizeof (AeCorrector));
    ae_corrector_prepare (p, 48000.0, 512, 0.0, 0.0, AE_SHIFT_QUALITY_BALANCED);
    ae_corrector_set_edo (p, 12);
    ae_corrector_set_retune_ms (p, 0.0);
    ae_corrector_set_transition_ms (p, 0.0);
    char err[256] = "";
    if (! ae_corrector_load_samples (p, root, "piano", NULL, err, sizeof (err)))
    { CHECK (false, "sample velocity: bank load (%s)", err); free (p); return; }

    AeHarmVoice voices[AE_HARM_VOICES];
    memset (voices, 0, sizeof (voices));
    for (int v = 0; v < AE_HARM_VOICES; ++v)
        voices[v].gain = 1.0;
    voices[0].interval = 7;
    ae_corrector_set_harmony (p, true, 0, voices);
    ae_corrector_set_sample (p, 1.0, -1.0, false);      /* measure the velocity */
    ae_corrector_set_synth (p, AE_HARM_SRC_SAMPLE, 0, 5.0, 200.0);
    CHECK (ae_corrector_voice_source (p, 0) == AE_HARM_SRC_SAMPLE,
           "sample velocity: voice 0 really is on the sample source");

    /* A LOUD note, a long silence, then a QUIETER one. The gap has to
       outlast the onset detector's slow follower (150 ms) by enough that
       the second note still reads as an EDGE against it -- a much quieter
       note too soon after a loud one is inside the first one's decay and
       is deliberately not an onset. */
    const int gap = 36864, note = 36864;
    const int total = gap + note + gap + note;
    float *in = calloc ((size_t) total, sizeof (float));
    float *hl = calloc ((size_t) total, sizeof (float));
    float *hr = calloc ((size_t) total, sizeof (float));
    double ph = 0.0;
    for (int i = 0; i < total; ++i)
    {
        const bool n1 = i >= gap && i < gap + note;
        const bool n2 = i >= gap + note + gap;
        if (! n1 && ! n2) continue;
        ph += 2.0 * M_PI * 220.0 / 48000.0;
        const double amp = n1 ? 0.50 : 0.12;      /* ~12 dB apart */
        in[i] = (float) (amp * (sin (ph) + 0.3 * sin (2.0 * ph)));
    }
    /* Sample the STRIKE LEVEL each note actually got, once its 30 ms
       measuring window has closed and the refinement has settled. The
       radiated level is the wrong probe here: the voiced gate shapes it
       too, and would mask the very thing under test. */
    double loud = -1.0, quiet = -1.0;
    for (int off = 0; off < total; off += 512)
    {
        const int n = total - off < 512 ? total - off : 512;
        ae_corrector_process (p, in + off, hl + off, hr + off, n);
        const double g = p->smp[0][p->smp_cur[0]].gain;
        if (off >= gap + 9600 && off < gap + 12000)                 loud  = g;
        if (off >= gap + note + gap + 9600
            && off < gap + note + gap + 12000)                      quiet = g;
    }

    CHECK (loud > 0.5, "strike level: the loud note strikes hard (%.3f)", loud);
    CHECK (quiet > 0.0 && quiet < loud * 0.85,
           "strike level follows THIS note, not the last (loud %.3f vs quiet "
           "%.3f; striking at the previous note's verdict inverts these)",
           loud, quiet);

    ae_corrector_free (p);
    free (p); free (in); free (hl); free (hr);
    snprintf (cmd, sizeof (cmd), "rm -rf %s", root);
    if (system (cmd) != 0) { /* best effort */ }
}

/* The strike map is RELATIVE, and this is the case that proved it had to
   be. Play at a REALISTIC gain stage -- an interface with ~16 dB of
   headroom, so "as hard as this player plays" peaks nowhere near full
   scale -- and the hardest note must still strike at the top of the range.
   The absolute map this replaced scored the same playing on how close it
   came to 0 dBFS, so it read the headroom as timidity: every velocity on
   the rig sat in the bottom half and quiet notes fell off the bottom
   entirely. */
static void test_velocity_relative (void)
{
    const char *root = "/tmp/ae-smp-rel";
    char dir[256], pth[512], cmd[512];
    snprintf (dir, sizeof (dir), "%s/piano", root);
    snprintf (cmd, sizeof (cmd), "rm -rf %s && mkdir -p %s", root, dir);
    if (system (cmd) != 0) { CHECK (false, "velocity map: cannot stage"); return; }
    snprintf (pth, sizeof (pth), "%s/C4.wav", dir); write_wav (pth, 261.6256, 3.0, 0.5);

    /* Peaks: hard 0.15 (-16.5 dBFS), then 12 dB under it. Both are what a
       correctly gain-staged interface actually delivers. */
    const double hard = 0.15, soft = hard * 0.25; /* -12 dB */
    double got[2] = { -1.0, -1.0 };

    for (int pass = 0; pass < 2; ++pass)
    {
        AeCorrector *p = calloc (1, sizeof (AeCorrector));
        ae_corrector_prepare (p, 48000.0, 512, 0.0, 0.0, AE_SHIFT_QUALITY_BALANCED);
        ae_corrector_set_edo (p, 12);
        ae_corrector_set_retune_ms (p, 0.0);
        ae_corrector_set_transition_ms (p, 0.0);
        char err[256] = "";
        if (! ae_corrector_load_samples (p, root, "piano", NULL, err, sizeof (err)))
        { CHECK (false, "velocity map: bank load (%s)", err); free (p); return; }

        AeHarmVoice voices[AE_HARM_VOICES];
        memset (voices, 0, sizeof (voices));
        for (int v = 0; v < AE_HARM_VOICES; ++v) voices[v].gain = 1.0;
        voices[0].interval = 7;
        ae_corrector_set_harmony (p, true, 0, voices);
        ae_corrector_set_sample (p, 1.0, -1.0, false);
        ae_corrector_set_synth (p, AE_HARM_SRC_SAMPLE, 0, 5.0, 200.0);

        /* Pass 0 plays only the hard note, so the reference is set by that
           note itself. Pass 1 plays hard, then the same soft note after a
           gap long enough to read as a fresh onset. */
        const int gap = 36864, note = 36864;
        const int total = pass == 0 ? gap + note : gap + note + gap + note;
        float *in = calloc ((size_t) total, sizeof (float));
        float *hl = calloc ((size_t) total, sizeof (float));
        float *hr = calloc ((size_t) total, sizeof (float));
        double ph = 0.0;
        for (int i = 0; i < total; ++i)
        {
            const bool n1 = i >= gap && i < gap + note;
            const bool n2 = i >= gap + note + gap;
            if (! n1 && ! n2) continue;
            ph += 2.0 * M_PI * 220.0 / 48000.0;
            /* sin + 0.3*sin(2x) peaks at ~1.13x the amplitude; scale so the
               PEAK is what the comment claims. */
            const double amp = (n1 ? hard : soft) / 1.13;
            in[i] = (float) (amp * (sin (ph) + 0.3 * sin (2.0 * ph)));
        }
        const int probe = pass == 0 ? gap : gap + note + gap;
        for (int off = 0; off < total; off += 512)
        {
            const int n = total - off < 512 ? total - off : 512;
            ae_corrector_process (p, in + off, hl + off, hr + off, n);
            if (off >= probe + 9600 && off < probe + 12000)
                got[pass] = p->smp[0][p->smp_cur[0]].gain;
        }
        ae_corrector_free (p);
        free (p); free (in); free (hl); free (hr);
    }

    CHECK (got[0] > 0.95,
           "velocity map: hard playing at -16.5 dBFS strikes at the top "
           "(%.3f; the absolute map scored this 0.59 because it measured "
           "the headroom, not the playing)", got[0]);
    /* 12 dB below the reference is half of a 24 dB window: 0.2 + 0.8*0.5. */
    CHECK (got[1] > 0.50 && got[1] < 0.70,
           "velocity map: 12 dB down lands mid-window (%.3f, want ~0.60)",
           got[1]);
    CHECK (got[1] > AE_VEL_FLOOR - 1e-9,
           "velocity map: a confirmed note never strikes below the floor "
           "(%.3f < %.2f)", got[1], AE_VEL_FLOOR);

    snprintf (cmd, sizeof (cmd), "rm -rf %s", root);
    if (system (cmd) != 0) { /* best effort */ }
}

/* Let-ring: a struck voice finishes on its own decay THROUGH the next
   strike, instead of being damped as the new note is repitched onto it.
   The probe is the slot table rather than the radiated audio, because the
   voiced gate and the harmony envelope both shape the output and would
   mask exactly the thing under test. */
/* A SUPPLIED velocity reference. The map's reference is the caller's by
   definition -- "how hard this player plays when playing hard" is a fact
   about the performance, not about this engine -- so a host that already
   knows it (TENDRIL's loudest onset of the capture) can assert it instead
   of waiting for the engine to rediscover it note by note. The probe is
   identical playing under two different references: under observation both
   would read 1.0, because the note IS the loudest heard so far. */
static void test_velocity_ref_supplied (void)
{
    const char *root = "/tmp/ae-smp-vref";
    char dir[256], pth[512], cmd[512];
    snprintf (dir, sizeof (dir), "%s/piano", root);
    snprintf (cmd, sizeof (cmd), "rm -rf %s && mkdir -p %s", root, dir);
    if (system (cmd) != 0) { CHECK (false, "vel ref: cannot stage"); return; }
    snprintf (pth, sizeof (pth), "%s/C4.wav", dir); write_wav (pth, 261.6256, 3.0, 0.5);

    const double hard = 0.15;                 /* -16.5 dBFS peak */
    /* At the note's own level, 12 dB above it, and 12 dB BELOW it. The
       third case is the one that pins down "held, not raised": an observed
       reference would climb to meet the note, a supplied one must not. */
    const double refs[3] = { hard, hard * 3.98107, hard / 3.98107 };
    double got[3] = { -1.0, -1.0, -1.0 }, seen[3] = { 0.0, 0.0, 0.0 };

    for (int c = 0; c < 3; ++c)
    {
        AeCorrector *p = calloc (1, sizeof (AeCorrector));
        ae_corrector_prepare (p, 48000.0, 512, 0.0, 0.0, AE_SHIFT_QUALITY_BALANCED);
        ae_corrector_set_edo (p, 12);
        ae_corrector_set_retune_ms (p, 0.0);
        ae_corrector_set_transition_ms (p, 0.0);
        char err[256] = "";
        if (! ae_corrector_load_samples (p, root, "piano", NULL, err, sizeof (err)))
        { CHECK (false, "vel ref: bank load (%s)", err); free (p); return; }

        AeHarmVoice voices[AE_HARM_VOICES];
        memset (voices, 0, sizeof (voices));
        for (int v = 0; v < AE_HARM_VOICES; ++v) voices[v].gain = 1.0;
        voices[0].interval = 7;
        ae_corrector_set_harmony (p, true, 0, voices);
        ae_corrector_set_sample (p, 1.0, -1.0, false);
        ae_corrector_set_synth (p, AE_HARM_SRC_SAMPLE, 0, 5.0, 200.0);
        ae_corrector_set_vel_ref (p, refs[c]);

        const int gap = 36864, note = 36864, total = gap + note;
        float *in = calloc ((size_t) total, sizeof (float));
        float *hl = calloc ((size_t) total, sizeof (float));
        float *hr = calloc ((size_t) total, sizeof (float));
        double ph = 0.0;
        for (int i = gap; i < total; ++i)
        {
            ph += 2.0 * M_PI * 220.0 / 48000.0;
            in[i] = (float) ((hard / 1.13) * (sin (ph) + 0.3 * sin (2.0 * ph)));
        }
        for (int off = 0; off < total; off += 512)
        {
            const int n = total - off < 512 ? total - off : 512;
            ae_corrector_process (p, in + off, hl + off, hr + off, n);
            if (off >= gap + 9600 && off < gap + 12000)
                got[c] = p->smp[0][p->smp_cur[0]].gain;
        }
        seen[c] = (double) ae_corrector_sample_vel_ref (p);
        ae_corrector_free (p);
        free (p); free (in); free (hl); free (hr);
    }

    CHECK (got[0] > 0.95,
           "supplied reference at the note's own level strikes full (%.3f)",
           got[0]);
    CHECK (got[1] > 0.50 && got[1] < 0.70,
           "a reference 12 dB ABOVE the playing scores it mid-window (%.3f, "
           "want ~0.60); under observation this same note reads 1.0, which "
           "is what makes the supplied reference do anything at all", got[1]);
    /* A reference BELOW the playing: the map's max() still keeps the note
       at unity rather than above it, and -- the point of this case -- the
       reference itself does not climb to meet the note the way an observed
       one would. */
    CHECK (got[2] > 0.95,
           "a reference below the playing still lands the note at unity, "
           "never past it (%.3f)", got[2]);
    CHECK (fabs (seen[2] - refs[2]) < 1e-4,
           "a supplied reference is HELD, not raised by a louder note "
           "(%.4f vs %.4f asked for; an observed reference would have "
           "climbed to %.4f)", seen[2], refs[2], hard);
    CHECK (fabs (seen[1] - refs[1]) < 1e-4,
           "a supplied reference is not decayed either (%.4f vs %.4f)",
           seen[1], refs[1]);

    /* Negative hands the reference back to observation. */
    {
        AeCorrector *p = calloc (1, sizeof (AeCorrector));
        ae_corrector_prepare (p, 48000.0, 512, 0.0, 0.0, AE_SHIFT_QUALITY_BALANCED);
        ae_corrector_set_vel_ref (p, 0.5);
        ae_corrector_set_vel_ref (p, -1.0);
        CHECK (p->vel_ref_fixed < 0.0,
               "sampleVelRefDb 'auto' returns the reference to observation");
        ae_corrector_free (p); free (p);
    }

    snprintf (cmd, sizeof (cmd), "rm -rf %s", root);
    if (system (cmd) != 0) { /* best effort */ }
}

static void test_sample_ring (void)
{
    const char *root = "/tmp/ae-smp-ring";
    char dir[256], pth[512], cmd[512];
    snprintf (dir, sizeof (dir), "%s/piano", root);
    snprintf (cmd, sizeof (cmd), "rm -rf %s && mkdir -p %s", root, dir);
    if (system (cmd) != 0) { CHECK (false, "let-ring: cannot stage"); return; }
    /* Long recordings, so a slot only falls silent because it was damped. */
    snprintf (pth, sizeof (pth), "%s/C4.wav", dir); write_wav (pth, 261.6256, 8.0, 0.5);
    snprintf (pth, sizeof (pth), "%s/G4.wav", dir); write_wav (pth, 391.9954, 8.0, 0.5);

    int sounding[2] = { -1, -1 };
    for (int ring = 0; ring < 2; ++ring)
    {
        AeCorrector *p = calloc (1, sizeof (AeCorrector));
        ae_corrector_prepare (p, 48000.0, 512, 0.0, 0.0, AE_SHIFT_QUALITY_BALANCED);
        ae_corrector_set_edo (p, 12);
        ae_corrector_set_retune_ms (p, 0.0);
        ae_corrector_set_transition_ms (p, 0.0);
        char err[256] = "";
        if (! ae_corrector_load_samples (p, root, "piano", NULL, err, sizeof (err)))
        { CHECK (false, "let-ring: bank load (%s)", err); free (p); return; }

        AeHarmVoice voices[AE_HARM_VOICES];
        memset (voices, 0, sizeof (voices));
        for (int v = 0; v < AE_HARM_VOICES; ++v) voices[v].gain = 1.0;
        voices[0].interval = 7;
        ae_corrector_set_harmony (p, true, 0, voices);
        ae_corrector_set_sample (p, 1.0, 0.8, ring != 0);
        ae_corrector_set_synth (p, AE_HARM_SRC_SAMPLE, 0, 5.0, 200.0);

        /* Two notes, each a clear onset, the second far enough after the
           first that it registers as an edge rather than as decay. */
        const int gap = 36864, note = 36864;
        const int total = gap + note + gap + note;
        float *in = calloc ((size_t) total, sizeof (float));
        float *hl = calloc ((size_t) total, sizeof (float));
        float *hr = calloc ((size_t) total, sizeof (float));
        double ph = 0.0;
        for (int i = 0; i < total; ++i)
        {
            const bool n1 = i >= gap && i < gap + note;
            const bool n2 = i >= gap + note + gap;
            if (! n1 && ! n2) continue;
            ph += 2.0 * M_PI * (n1 ? 220.0 : 246.94) / 48000.0;
            in[i] = (float) (0.35 * (sin (ph) + 0.3 * sin (2.0 * ph)));
        }
        const int probe = gap + note + gap + 14400; /* 300 ms into note 2 */
        for (int off = 0; off < total; off += 512)
        {
            const int n = total - off < 512 ? total - off : 512;
            ae_corrector_process (p, in + off, hl + off, hr + off, n);
            if (off >= probe && off < probe + 512)
            {
                int live = 0;
                for (int k = 0; k < AE_SMP_SLOTS; ++k)
                    if (p->smp[0][k].rec != NULL) ++live;
                sounding[ring] = live;
            }
        }
        ae_corrector_free (p);
        free (p); free (in); free (hl); free (hr);
    }

    CHECK (sounding[0] == 1,
           "let-ring off: the previous note is damped as the new one is "
           "repitched (%d slots sounding, want 1)", sounding[0]);
    CHECK (sounding[1] >= 2,
           "let-ring on: the previous note is still ringing under the new "
           "one (%d slots sounding, want >= 2)", sounding[1]);

    /* The RING IS BOUNDED: from the moment it is superseded a note decays
       under the release ceiling, so with a short release an old note is
       GONE by the time two more have landed. Unbounded ring is not
       sustain, it is a wash of old notes under every new one -- on the
       8-second recordings staged here, three ringing slots at once -- and
       the shipped default is ring ON, so this is the case the field hears.
       (Found the hard way: the ceiling was documented and not implemented,
       and the report was "the bassy corrected note is back".) */
    {
        AeCorrector *p = calloc (1, sizeof (AeCorrector));
        ae_corrector_prepare (p, 48000.0, 512, 0.0, 0.0, AE_SHIFT_QUALITY_BALANCED);
        ae_corrector_set_edo (p, 12);
        ae_corrector_set_retune_ms (p, 0.0);
        ae_corrector_set_transition_ms (p, 0.0);
        char err2[256] = "";
        if (! ae_corrector_load_samples (p, root, "piano", NULL, err2, sizeof (err2)))
        { CHECK (false, "ring ceiling: bank load (%s)", err2); free (p); return; }
        AeHarmVoice voices[AE_HARM_VOICES];
        memset (voices, 0, sizeof (voices));
        for (int v = 0; v < AE_HARM_VOICES; ++v) voices[v].gain = 1.0;
        voices[0].interval = 7;
        ae_corrector_set_harmony (p, true, 0, voices);
        ae_corrector_set_sample (p, 1.0, 0.8, true);          /* let-ring */
        ae_corrector_set_synth (p, AE_HARM_SRC_SAMPLE, 0, 5.0, 120.0);
        /* 120 ms ceiling: a superseded slot frees ~0.84 s later (-60 dB) */

        const int gap = 36864, note = 36864, leg = gap + note;
        const int total = 3 * leg;
        float *in = calloc ((size_t) total, sizeof (float));
        float *hl = calloc ((size_t) total, sizeof (float));
        float *hr = calloc ((size_t) total, sizeof (float));
        double ph = 0.0;
        for (int i = 0; i < total; ++i)
        {
            const int k = i / leg;
            if (i - k * leg < gap) continue;
            const double hz = k == 0 ? 220.0 : k == 1 ? 246.94 : 293.66;
            ph += 2.0 * M_PI * hz / 48000.0;
            in[i] = (float) (0.35 * (sin (ph) + 0.3 * sin (2.0 * ph)));
        }
        int live = -1;
        const int probe = 2 * leg + gap + 9600; /* 200 ms into note 3 */
        for (int off = 0; off < total; off += 512)
        {
            const int n = total - off < 512 ? total - off : 512;
            ae_corrector_process (p, in + off, hl + off, hr + off, n);
            if (off >= probe && off < probe + 512)
            {
                live = 0;
                for (int k = 0; k < AE_SMP_SLOTS; ++k)
                    if (p->smp[0][k].rec != NULL) ++live;
            }
        }
        CHECK (live >= 1 && live <= 2,
               "let-ring is BOUNDED by the release ceiling: two notes on, "
               "the first is gone (%d slots; 3 = unbounded ring, the wash)",
               live);
        ae_corrector_free (p);
        free (p); free (in); free (hl); free (hr);
    }

    snprintf (cmd, sizeof (cmd), "rm -rf %s", root);
    if (system (cmd) != 0) { /* best effort */ }
}

/* The LEAD's own release. A synth lead keeps sounding after the input
   stops -- it is an oscillator, not a copy of the input -- so leadReleaseMs
   is a real tail, and a long one must outlast a short one. The harmony's
   synthReleaseMs is held identical across the two runs, which is the whole
   point of the key: the lead's envelope is no longer the harmony's. */
/* Field report: "the threshold to trigger notes is too high, I'm losing
   my quiet notes" and "it loses repeated/successive notes a lot". Three
   fixes under test: gateDb scales every trigger floor; a re-pluck under
   a still-ringing string strikes via the attack-hold's arming (the
   energy Schmitt cannot see it); a legato note change strikes on the
   degree change (no energy edge exists at all). */
static void test_sample_triggering (void)
{
    const char *root = "/tmp/ae-smp-trig";
    char dir[256], pth[512], cmd[512];
    snprintf (dir, sizeof (dir), "%s/piano", root);
    snprintf (cmd, sizeof (cmd), "rm -rf %s && mkdir -p %s", root, dir);
    if (system (cmd) != 0) { CHECK (false, "triggering: cannot stage"); return; }
    snprintf (pth, sizeof (pth), "%s/C4.wav", dir);
    {
        const int n = 4 * 48000; /* sustained sine */
        float *pcm = calloc ((size_t) n, sizeof (float));
        double phr = 0.0;
        for (int i = 0; i < n; ++i)
        {
            phr += 2.0 * M_PI * 261.6256 / 48000.0;
            pcm[i] = (float) (0.5 * sin (phr));
        }
        write_wav_pcm (pth, pcm, n);
        free (pcm);
    }

    const int total = 512 * 188, t1 = 512 * 94;
    float *buf = calloc ((size_t) total, sizeof (float));

    /* 1. QUIET NOTES: a pluck at ~-52 dBFS RMS. The default gate's onset
       floor sits just above it -- no strike; gateDb -65 hears it. */
    for (int pass = 0; pass < 2; ++pass)
    {
        AeCorrector *p = calloc (1, sizeof (AeCorrector));
        ae_corrector_prepare (p, 48000.0, 512, 0.0, 0.0, AE_SHIFT_QUALITY_BALANCED);
        ae_corrector_set_edo (p, 12);
        ae_corrector_set_retune_ms (p, 0.0);
        ae_corrector_set_transition_ms (p, 0.0);
        ae_corrector_set_gate (p, pass == 0 ? 0.0 /* default */
                                            : pow (10.0, -65.0 / 20.0));
        char err[256] = "";
        CHECK (ae_corrector_load_samples (p, root, "piano", NULL, err, sizeof (err)),
               "triggering: bank loads (%s)", err);
        int srcs[AE_HARM_VOICES];
        for (int v = 0; v < AE_HARM_VOICES; ++v) srcs[v] = AE_HARM_SRC_DEFAULT;
        ae_corrector_set_voice_sources (p, srcs, AE_HARM_SRC_SAMPLE);
        ae_corrector_set_sample (p, 1.0, 0.9, true);
        ae_corrector_set_lead_env (p, 5.0, 400.0);

        double ph = 0.0;
        for (int i = 0; i < total; ++i)
        {
            const double t = i < 512 * 20 ? -1.0
                            : (double) (i - 512 * 20) / 48000.0;
            const double amp = t < 0.0 ? 0.0 : 0.003 * exp (-t / 0.5);
            ph += 2.0 * M_PI * 220.0 / 48000.0;
            buf[i] = (float) (amp * (sin (ph) + 0.3 * sin (2.0 * ph)));
        }
        for (int off = 0; off < total; off += 512)
            ae_corrector_process (p, buf + off, NULL, NULL, 512);
        double e = 0.0;
        for (int i = 512 * 30; i < total; ++i)
            e += (double) buf[i] * buf[i];
        if (pass == 0)
            CHECK (e < 1e-6,
                   "gateDb: below the default floor stays silent (%.3g) -- "
                   "if this fires, lower the fixture level", e);
        else
            CHECK (e > 1e-4,
                   "gateDb: -65 dB hears the quiet pluck (energy %.3g)", e);
        ae_corrector_free (p); free (p);
    }

    /* 2. RE-PLUCK UNDER RING + 3. LEGATO DEGREE CHANGE, one corrector:
       first half re-plucks A3 (sharp attack, ring between); second half
       steps to B3 with NO amplitude edge at all. Count strikes by live
       slots (400 ms release keeps every struck slot alive to the end). */
    {
        AeCorrector *p = calloc (1, sizeof (AeCorrector));
        ae_corrector_prepare (p, 48000.0, 512, 0.0, 0.0, AE_SHIFT_QUALITY_BALANCED);
        ae_corrector_set_edo (p, 12);
        ae_corrector_set_retune_ms (p, 0.0);
        ae_corrector_set_transition_ms (p, 0.0);
        char err[256] = "";
        CHECK (ae_corrector_load_samples (p, root, "piano", NULL, err, sizeof (err)),
               "triggering: bank loads again (%s)", err);
        int srcs[AE_HARM_VOICES];
        for (int v = 0; v < AE_HARM_VOICES; ++v) srcs[v] = AE_HARM_SRC_DEFAULT;
        ae_corrector_set_voice_sources (p, srcs, AE_HARM_SRC_SAMPLE);
        ae_corrector_set_sample (p, 1.0, 0.9, true);
        ae_corrector_set_lead_env (p, 5.0, 2000.0); /* slots outlive the test */

        const int pluck2 = 512 * 47; /* second pluck of A3, under ring */
        double ph = 0.0;
        for (int i = 0; i < total; ++i)
        {
            double f, amp;
            if (i < t1)
            {
                const int since = i % pluck2;
                const double t = (double) since / 48000.0;
                f   = 220.0 * (1.0 + 0.0146 * exp (-t / 0.040));
                amp = 0.4 * exp (-t / 0.5);
            }
            else
            {   /* legato: pitch steps a whole tone, amplitude steady */
                f   = 246.94;
                amp = 0.4;
            }
            ph += 2.0 * M_PI * f / 48000.0;
            buf[i] = (float) (amp * (sin (ph) + 0.3 * sin (2.0 * ph)));
        }
        int live_mid = 0, live_end = 0;
        for (int off = 0; off < total; off += 512)
        {
            ae_corrector_process (p, buf + off, NULL, NULL, 512);
            if (off == t1 - 512 || off == total - 512)
            {
                int live = 0;
                for (int sl = 0; sl < AE_SMP_SLOTS; ++sl)
                    if (p->smp[AE_HARM_VOICES][sl].rec != NULL)
                        ++live;
                if (off < t1) live_mid = live; else live_end = live;
            }
        }
        CHECK (live_mid >= 2,
               "re-pluck: the second pluck under ring STRIKES "
               "(%d live slots; the Schmitt alone leaves 1)", live_mid);
        CHECK (live_end >= live_mid + 1,
               "legato: a degree change with no energy edge strikes "
               "(%d -> %d live slots)", live_mid, live_end);
        ae_corrector_free (p); free (p);
    }

    free (buf);
    snprintf (cmd, sizeof (cmd), "rm -rf %s", root);
    if (system (cmd) != 0) { /* best effort */ }
}

/* Field report: leadReleaseMs 5 but a sampled choir faded far longer and
   then CUT at a quiet volume. Two properties pinned here: a superseded
   let-ring slot decays to -80 dB before it is freed (at -60 the cut was
   audible on dense sustained textures after bank makeup gain), and the
   lead's own release really silences the output at the pace set. */
static void test_sample_release (void)
{
    const char *root = "/tmp/ae-smp-rel";
    char dir[256], pth[512], cmd[512];
    snprintf (dir, sizeof (dir), "%s/piano", root);
    snprintf (cmd, sizeof (cmd), "rm -rf %s && mkdir -p %s", root, dir);
    if (system (cmd) != 0) { CHECK (false, "release: cannot stage"); return; }
    snprintf (pth, sizeof (pth), "%s/C4.wav", dir);
    {
        const int n = 6 * 48000;
        float *pcm = calloc ((size_t) n, sizeof (float));
        double phr = 0.0;
        for (int i = 0; i < n; ++i)
        {
            phr += 2.0 * M_PI * 261.6256 / 48000.0;
            pcm[i] = (float) (0.5 * sin (phr));
        }
        write_wav_pcm (pth, pcm, n);
        free (pcm);
    }

    /* 1. Release-ceiling slot lifetime: superseded under a 200 ms
       ceiling, the old slot must survive past 1.6 s (freed at -80 dB =
       9.2 time constants) -- the old -60 dB threshold freed it by 1.4 s. */
    {
        AeCorrector *p = calloc (1, sizeof (AeCorrector));
        ae_corrector_prepare (p, 48000.0, 512, 0.0, 0.0, AE_SHIFT_QUALITY_BALANCED);
        ae_corrector_set_edo (p, 12);
        ae_corrector_set_retune_ms (p, 0.0);
        ae_corrector_set_transition_ms (p, 0.0);
        char err[256] = "";
        CHECK (ae_corrector_load_samples (p, root, "piano", NULL, err, sizeof (err)),
               "release: bank loads (%s)", err);
        int srcs[AE_HARM_VOICES];
        for (int v = 0; v < AE_HARM_VOICES; ++v) srcs[v] = AE_HARM_SRC_DEFAULT;
        ae_corrector_set_voice_sources (p, srcs, AE_HARM_SRC_SAMPLE);
        ae_corrector_set_sample (p, 1.0, 0.9, true);
        ae_corrector_set_lead_env (p, 5.0, 200.0);

        const int t1 = 512 * 94, total = 512 * 400; /* step at ~1 s, run 4.3 s */
        float *buf = calloc ((size_t) total, sizeof (float));
        double ph = 0.0;
        for (int i = 0; i < total; ++i)
        {
            const double f = i < t1 ? 220.0 : 246.94; /* legato step strikes */
            ph += 2.0 * M_PI * f / 48000.0;
            buf[i] = (float) (0.4 * (sin (ph) + 0.3 * sin (2.0 * ph)));
        }
        int live_16 = 0, live_22 = 0;
        const int c16 = t1 + (int) (1.6 * 48000.0) / 512 * 512;
        const int c22 = t1 + (int) (2.2 * 48000.0) / 512 * 512;
        for (int off = 0; off < total; off += 512)
        {
            ae_corrector_process (p, buf + off, NULL, NULL, 512);
            if (off == c16 || off == c22)
            {
                int live = 0;
                for (int sl = 0; sl < AE_SMP_SLOTS; ++sl)
                    if (p->smp[AE_HARM_VOICES][sl].rec != NULL)
                        ++live;
                if (off == c16) live_16 = live; else live_22 = live;
            }
        }
        CHECK (live_16 >= 2,
               "release ceiling: the superseded slot decays to -80 dB "
               "before it is freed (%d live at +1.6 s; -60 dB freed it "
               "by 1.4 s)", live_16);
        CHECK (live_22 <= live_16 - 1,
               "release ceiling: ...and it IS freed once finished "
               "(%d live at +2.2 s)", live_22);
        ae_corrector_free (p); free (p); free (buf);
    }

    /* 2. The lead's release is honest: leadReleaseMs 5, input cut dead --
       the sampled output must be silent within 60 ms. */
    {
        AeCorrector *p = calloc (1, sizeof (AeCorrector));
        ae_corrector_prepare (p, 48000.0, 512, 0.0, 0.0, AE_SHIFT_QUALITY_BALANCED);
        ae_corrector_set_edo (p, 12);
        ae_corrector_set_retune_ms (p, 0.0);
        ae_corrector_set_transition_ms (p, 0.0);
        char err[256] = "";
        CHECK (ae_corrector_load_samples (p, root, "piano", NULL, err, sizeof (err)),
               "release: bank loads twice (%s)", err);
        int srcs[AE_HARM_VOICES];
        for (int v = 0; v < AE_HARM_VOICES; ++v) srcs[v] = AE_HARM_SRC_DEFAULT;
        ae_corrector_set_voice_sources (p, srcs, AE_HARM_SRC_SAMPLE);
        ae_corrector_set_sample (p, 1.0, 0.9, true);
        ae_corrector_set_lead_env (p, 5.0, 5.0);

        const int t1 = 512 * 94, total = 512 * 141;
        float *buf = calloc ((size_t) total, sizeof (float));
        double ph = 0.0;
        for (int i = 0; i < t1; ++i)
        {
            ph += 2.0 * M_PI * 220.0 / 48000.0;
            buf[i] = (float) (0.4 * (sin (ph) + 0.3 * sin (2.0 * ph)));
        }
        for (int off = 0; off < total; off += 512)
            ae_corrector_process (p, buf + off, NULL, NULL, 512);
        double tail = 0.0;
        for (int i = t1 + (int) (0.060 * 48000.0); i < total; ++i)
            tail += (double) buf[i] * buf[i];
        CHECK (tail < 1e-6,
               "lead release 5 ms: the sampled lead is SILENT 60 ms after "
               "the note ends (tail energy %.3g)", tail);
        ae_corrector_free (p); free (p); free (buf);
    }

    snprintf (cmd, sizeof (cmd), "rm -rf %s", root);
    if (system (cmd) != 0) { /* best effort */ }
}

static void test_lead_envelope (void)
{
    double tail[2] = { 0.0, 0.0 };
    for (int slow = 0; slow < 2; ++slow)
    {
        AeCorrector *p = calloc (1, sizeof (AeCorrector));
        ae_corrector_prepare (p, 48000.0, 512, 0.0, 0.0, AE_SHIFT_QUALITY_BALANCED);
        ae_corrector_set_edo (p, 12);
        ae_corrector_set_retune_ms (p, 0.0);
        ae_corrector_set_transition_ms (p, 0.0);
        /* Synth LEAD; the harmony envelope is the same in both runs. */
        int srcs[AE_HARM_VOICES];
        for (int v = 0; v < AE_HARM_VOICES; ++v) srcs[v] = AE_HARM_SRC_DEFAULT;
        ae_corrector_set_voice_sources (p, srcs, AE_HARM_SRC_SYNTH);
        ae_corrector_set_synth (p, AE_HARM_SRC_SYNTH, 0, 80.0, 500.0);
        ae_corrector_set_lead_env (p, 5.0, slow ? 800.0 : 20.0);

        const int note = 48000, after = 24000;
        const int total = note + after;
        float *in  = calloc ((size_t) total, sizeof (float));
        float *out = calloc ((size_t) total, sizeof (float));
        double ph = 0.0;
        for (int i = 0; i < note; ++i)
        {
            ph += 2.0 * M_PI * 220.0 / 48000.0;
            in[i] = (float) (0.3 * sin (ph));
        }
        memcpy (out, in, (size_t) total * sizeof (float));
        for (int off = 0; off < total; off += 512)
        {
            const int n = total - off < 512 ? total - off : 512;
            ae_corrector_process (p, out + off, NULL, NULL, n);
        }
        /* RMS of the 100 ms starting 150 ms after the input stopped. */
        double sq = 0.0;
        const int a = note + 7200, b = a + 4800;
        for (int i = a; i < b && i < total; ++i) sq += (double) out[i] * out[i];
        tail[slow] = sqrt (sq / (double) (b - a));
        ae_corrector_free (p); free (p); free (in); free (out);
    }
    CHECK (tail[1] > tail[0] * 3.0,
           "leadReleaseMs shapes the lead's own tail (800 ms %.5f vs 20 ms "
           "%.5f); identical harmony release in both runs", tail[1], tail[0]);
}

/* bypassOutput decides what bypass PUTS on the output, and the decision is
   shared by the backends so it can be checked without a device. */
static void test_bypass_output (void)
{
    AeLiveParams lp;
    memset (&lp, 0, sizeof (lp));
    lp.edo = 12; lp.ref_hz = 261.6256; lp.period_cents = 1200.0;
    AeAtomicParams ap;
    memset (&ap, 0, sizeof (ap));
    AeCorrector *p = calloc (1, sizeof (AeCorrector));
    ae_corrector_prepare (p, 48000.0, 512, 0.0, 0.0, AE_SHIFT_QUALITY_BALANCED);
    AeMixParams mix;

    lp.bypass = true; lp.bypass_mute = false;
    ae_atomic_params_store (&ap, &lp);
    ae_atomic_params_apply (&ap, p, 0, 0, &mix);
    CHECK (mix.bypass && ae_bypass_gain (&mix) == 1.0f,
           "bypassOutput 'dry': bypass passes the input through");

    lp.bypass_mute = true;
    ae_atomic_params_store (&ap, &lp);
    ae_atomic_params_apply (&ap, p, 0, 0, &mix);
    CHECK (ae_bypass_gain (&mix) == 0.0f,
           "bypassOutput 'mute': bypass puts silence on the output");

    /* Not bypassed, the switch must not touch the live path -- it decides
       what bypass does, not what the engine does. */
    lp.bypass = false;
    ae_atomic_params_store (&ap, &lp);
    ae_atomic_params_apply (&ap, p, 0, 0, &mix);
    CHECK (ae_bypass_gain (&mix) == 1.0f,
           "bypassOutput only applies while bypass is engaged");

    ae_corrector_free (p); free (p);
}

static void test_makeup_on_plucks (void)
{
    AeShifter *sh = ae_shifter_create (48000.0,
                        ae_shifter_block_samples (48000.0, AE_SHIFT_QUALITY_BALANCED));
    ae_shifter_set_semitones (sh, 0.0, 0.0);
    ae_shifter_set_formant_semitones (sh, 0.0, true);
    ae_shifter_set_formant_base (sh, 220.0);

    const int total = 4 * 48000;
    float *in  = calloc ((size_t) total, sizeof (float));
    float *out = calloc ((size_t) total, sizeof (float));
    double ph = 0.0;
    for (int i = 0; i < total; ++i)
    {
        const int    k = i % 28800;              /* a pluck every 600 ms */
        const double env = exp (-k / (0.35 * 48000.0)) * (k < 24000 ? 1.0 : 0.0);
        ph += 2.0 * M_PI * 220.0 / 48000.0;
        in[i] = (float) (0.5 * env * (sin (ph) + 0.5 * sin (2.0 * ph)
                                    + 0.25 * sin (3.0 * ph)));
    }
    double mk_min = 10.0, mk_max = 0.0;
    for (int off = 0; off < total; off += 512)
    {
        ae_shifter_process (sh, in + off, out + off, 512);
        if (off > 48000) /* past warm-up */
        {
            const double m = ae_shifter_makeup (sh);
            if (m < mk_min) mk_min = m;
            if (m > mk_max) mk_max = m;
        }
    }
    CHECK (mk_min > 0.66 && mk_max < 1.5,
           "makeup stays near unity on a pluck train (%.2f .. %.2f; "
           "the unaligned version swung 0.25 .. 4.0)", mk_min, mk_max);

    /* Post-reset gap: loud input, no output yet. The old rule boosted into
       the hole (toward +12 dB); the new one must not exceed unity. */
    ae_shifter_reset (sh);
    ae_shifter_set_semitones (sh, 0.0, 0.0);
    double mk_gap = 0.0;
    for (int off = 0; off < 9600; off += 512) /* first 200 ms after reset */
    {
        ae_shifter_process (sh, in + off, out + off, 512);
        const double m = ae_shifter_makeup (sh);
        if (m > mk_gap) mk_gap = m;
    }
    CHECK (mk_gap <= 1.01,
           "makeup never boosts into the post-reset gap (max %.2f)", mk_gap);

    ae_shifter_destroy (sh);
    free (in); free (out);
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
    test_harmony_glide();
    test_harm_toggle_clears_glide();
    test_lead_shift();
    test_octave_revote_rebase();
    test_expression_transfer();
    test_attack_sound();
    test_poly_mode();
    test_polyf0_tracker();
    test_poly_detect_export();
    test_poly_onset_response();
    test_chord_sampler();
    test_follow_link();
    test_detection_lock_time();
    test_onset_clears_continuity();
    test_attack_hold();
    test_release_pitch_stability();
    test_midi_octave_fold();
    test_synth_envelope();
    test_walk();
    test_walk_tiebreak();
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
    test_audio_tap();
    test_engine_channels();
    test_96k();
    test_soft_clip();
    test_makeup_on_plucks();
    test_sampler_bank();
    test_sample_ghost();
    test_sample_velocity();
    test_velocity_relative();
    test_velocity_ref_supplied();
    test_sample_ring();
    test_sample_triggering();
    test_sample_release();
    test_lead_envelope();
    test_bypass_output();
    test_lead_wet_tap();
    test_formant_offset();

    if (failures == 0)
    {
        printf ("All self-tests passed.\n");
        return 0;
    }
    printf ("%d failure(s).\n", failures);
    return 1;
}
