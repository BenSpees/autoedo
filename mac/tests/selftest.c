/* Self-tests for the C port: tuning math, YIN detection, PSOLA correction
   and the JSON helpers. Pure POSIX — runs anywhere (`make test`). */

#include "../src/json.h"
#include "../src/psola.h"
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

static void test_psola (void)
{
    AePsola *p = calloc (1, sizeof (AePsola));
    ae_psola_prepare (p, 48000.0, 512);
    ae_psola_set_edo (p, 12);
    ae_psola_set_retune_ms (p, 0.0); /* hard snap */

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
        ae_psola_process (p, buf + off, n);
    }

    CHECK (ae_psola_voiced (p), "psola voiced after 1 s of tone");
    const float det = ae_psola_detected_hz (p);
    const float tgt = ae_psola_target_hz (p);
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

    ae_psola_free (p);
    free (p);
    free (buf);
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

    char esc[64] = "";
    ae_json_escape_append (esc, sizeof (esc), "a\"b\\c\nd");
    CHECK (strcmp (esc, "a\\\"b\\\\c\\u000ad") == 0, "escape: '%s'", esc);
}

int main (void)
{
    test_tuning();
    test_yin();
    test_psola();
    test_json();

    if (failures == 0)
    {
        printf ("All self-tests passed.\n");
        return 0;
    }
    printf ("%d failure(s).\n", failures);
    return 1;
}
