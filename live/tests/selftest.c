/* Self-tests for the C port: tuning math, YIN detection, pitch correction
   and the JSON helpers. Pure POSIX — runs anywhere (`make test`). */

#include "../src/json.h"
#include "../src/corrector.h"
#include "../src/tuning.h"
#include "../src/yin.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

int main (void)
{
    test_tuning();
    test_yin();
    test_correction();
    test_walk();
    test_harmony();
    test_midi_harmony();
    test_json();

    if (failures == 0)
    {
        printf ("All self-tests passed.\n");
        return 0;
    }
    printf ("%d failure(s).\n", failures);
    return 1;
}
