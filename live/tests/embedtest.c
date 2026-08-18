/* Embed-library tests: drive the public face (src/embed.h) end to end the
   way a host process would -- create at the host's rate/block, config via
   the JSON grammar, mono blocks through, status echo back, destroy. Run by
   `make test` alongside the DSP self-tests. */

#include "../src/embed.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int g_fail = 0;

#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { printf ("FAIL %s (%s:%d)\n", msg, __FILE__, __LINE__); \
                       ++g_fail; } \
        else           printf ("ok   %s\n", msg); \
    } while (0)

#define RATE  48000.0
#define BLOCK 256

/* A tone with harmonics a fixed number of cents off A3, phase-continuous
   across calls -- the same signal the stub backend feeds itself. */
static void tone_block (float *out, int n, double detune_cents, double *phase)
{
    const double f = 220.0 * pow (2.0, detune_cents / 1200.0);
    for (int i = 0; i < n; ++i)
    {
        *phase += 2.0 * M_PI * f / RATE;
        out[i] = (float) (0.4 * sin (*phase) + 0.2 * sin (2.0 * *phase)
                        + 0.1 * sin (3.0 * *phase));
    }
}

static float peak (const float *x, int n)
{
    float p = 0.0f;
    for (int i = 0; i < n; ++i)
        if (fabsf (x[i]) > p)
            p = fabsf (x[i]);
    return p;
}

int main (void)
{
    char status[16384];
    char err[256] = "";

    /* A private config file so the test never reads or writes a rig's. */
    const char *cfg_path = "build/embedtest-config.json";
    remove (cfg_path);

    AeEmbedOptions opt;
    memset (&opt, 0, sizeof (opt));
    opt.sample_rate = RATE;
    opt.max_block   = BLOCK;
    opt.http_port   = 0; /* audio only; the server is the standalone's test */
    opt.config_file = cfg_path;

    AeEmbed *inst = ae_embed_create (&opt, err, sizeof (err));
    CHECK (inst != NULL, "create");
    if (inst == NULL)
    {
        printf ("  (%s)\n", err);
        return 1;
    }
    CHECK (! ae_embed_http_running (inst), "no http server when port 0");

    ae_embed_status (inst, status, sizeof (status));
    CHECK (strstr (status, "\"running\":true") != NULL, "status: running");
    CHECK (strstr (status, "\"embedded\":true") != NULL, "status: embedded flag");
    CHECK (strstr (status, "\"inputRate\":48000") != NULL, "status: host rate");
    CHECK (strstr (status, "\"config\":{") != NULL, "status: config echo");

    /* Config via the same grammar as POST /api/config. */
    ae_embed_config (inst,
        "{\"edo\":22,\"retuneMs\":0,\"transitionMs\":0,\"amount\":1,"
        "\"outputChannel\":4,\"label\":\"embedtest\"}");
    ae_embed_status (inst, status, sizeof (status));
    CHECK (strstr (status, "\"edo\":22") != NULL, "config echo: edo 22");
    CHECK (strstr (status, "\"sampleMatch\":1") != NULL,
           "config echo: sampleMatch defaults to source parity");
    CHECK (strstr (status, "\"strikeTilt\":3") != NULL,
           "config echo: strikeTilt defaults to +3 dB/oct");
    ae_embed_config (inst, "{\"strikeTilt\":0}");
    ae_embed_status (inst, status, sizeof (status));
    CHECK (strstr (status, "\"strikeTilt\":0") != NULL,
           "config echo: strikeTilt follows a write");
    ae_embed_config (inst, "{\"strikeTilt\":3}");
    ae_embed_config (inst, "{\"sampleMatch\":0.4}");
    ae_embed_status (inst, status, sizeof (status));
    CHECK (strstr (status, "\"sampleMatch\":0.4") != NULL,
           "config echo: sampleMatch follows a write");
    ae_embed_config (inst, "{\"sampleMatch\":1}");
    CHECK (strstr (status, "\"label\":\"embedtest\"") != NULL,
           "config echo: label");
    CHECK (ae_embed_output_channel (inst) == 4, "output channel routing fact");

    /* Blocks through: a detuned tone must come out corrected and audible on
       the PA feed and on the chain feed alike. */
    float in[BLOCK], out_l[BLOCK], out_r[BLOCK], chain[BLOCK];
    double phase = 0.0;
    float pa_peak = 0.0f, chain_peak = 0.0f;
    int frames_ok = 0;
    for (int b = 0; b < 100; ++b) /* ~0.5 s: past detector warm-up */
    {
        tone_block (in, BLOCK, 35.0, &phase);
        if (ae_embed_process (inst, in, out_l, out_r, chain, BLOCK) == BLOCK)
            ++frames_ok;
        if (b >= 50)
        {
            const float pl = peak (out_l, BLOCK);
            if (pl > pa_peak) pa_peak = pl;
            const float pc = peak (chain, BLOCK);
            if (pc > chain_peak) chain_peak = pc;
        }
    }
    CHECK (frames_ok == 100, "process returns n every block");
    CHECK (pa_peak > 0.05f, "PA feed carries the corrected signal");
    CHECK (chain_peak > 0.05f, "chain feed carries the corrected signal");

    /* The status cache refreshes on the pump tick (100 ms) or on any config
       post; the loop above ran faster than real time, so nudge one. */
    ae_embed_config (inst, "{}");
    ae_embed_status (inst, status, sizeof (status));
    CHECK (strstr (status, "\"voiced\":true") != NULL, "status: voiced");
    CHECK (strstr (status, "\"detectedHz\":0.0000") == NULL,
           "status: a pitch was detected");

    /* An oversized host buffer must be chunked, not truncated. */
    {
        float big_in[BLOCK * 3], big_l[BLOCK * 3], big_r[BLOCK * 3],
              big_c[BLOCK * 3];
        tone_block (big_in, BLOCK * 3, 35.0, &phase);
        const int n = ae_embed_process (inst, big_in, big_l, big_r, big_c,
                                        BLOCK * 3);
        CHECK (n == BLOCK * 3, "oversized block is chunked through");
    }

    /* Bypass: the PA feed goes dry; bypassOutput:"mute" silences the PA
       feed but must NEVER silence the chain feed (the host looper's
       source). The chain's source flip crossfades (~10 ms), so run the
       fade out before comparing. */
    ae_embed_config (inst, "{\"bypass\":true}");
    for (int b = 0; b < 40; ++b) /* ~0.2 s: the crossfade is long gone */
    {
        tone_block (in, BLOCK, 35.0, &phase);
        ae_embed_process (inst, in, out_l, out_r, chain, BLOCK);
    }
    CHECK (peak (out_l, BLOCK) > 0.05f, "bypass dry reaches the PA feed");
    {
        float dmax = 0.0f;
        for (int i = 0; i < BLOCK; ++i)
            if (fabsf (chain[i] - in[i]) > dmax)
                dmax = fabsf (chain[i] - in[i]);
        CHECK (dmax < 1e-3f, "bypass chain feed is the dry input");
    }
    ae_embed_config (inst, "{\"bypassOutput\":\"mute\"}");
    tone_block (in, BLOCK, 35.0, &phase);
    ae_embed_process (inst, in, out_l, out_r, chain, BLOCK);
    CHECK (peak (out_l, BLOCK) == 0.0f, "bypass mute silences the PA feed");
    CHECK (peak (chain, BLOCK) > 0.05f, "bypass mute keeps the chain feed dry");
    ae_embed_config (inst, "{\"bypass\":false,\"bypassOutput\":\"dry\"}");

    /* Every voice off -- lead muted, harmony off, no drone: the engine's
       mix is deliberate silence, and the chain must carry the DRY input
       instead (field: loops recorded silence through a lead-muted engine,
       and 'my dry guitar is no longer going into the loops'). The PA feed
       honestly goes quiet; the chain must not. */
    ae_embed_config (inst, "{\"leadOn\":false,\"harmOn\":false,\"droneOn\":false}");
    for (int b = 0; b < 40; ++b)
    {
        tone_block (in, BLOCK, 35.0, &phase);
        ae_embed_process (inst, in, out_l, out_r, chain, BLOCK);
    }
    CHECK (peak (out_l, BLOCK) < 1e-3f, "all voices off: PA feed is quiet");
    {
        float dmax = 0.0f;
        for (int i = 0; i < BLOCK; ++i)
            if (fabsf (chain[i] - in[i]) > dmax)
                dmax = fabsf (chain[i] - in[i]);
        CHECK (dmax < 1e-3f, "all voices off: chain feed is the dry input");
    }
    ae_embed_config (inst, "{\"leadOn\":true}");

    /* The consolidated sample section. No bank is loaded here, so a
       "sample" lead has nothing to sound -- but the mix's DRY side is the
       LATENCY-FREE input and must carry the instrument at 50% (a library
       that fails to load must never silence the player), and at 100% the
       samples stand alone (none loaded = honest silence). */
    ae_embed_config (inst,
        "{\"leadSource\":\"sample\",\"sampleMix\":0.5}");
    for (int b2 = 0; b2 < 40; ++b2)
    {
        tone_block (in, BLOCK, 35.0, &phase);
        ae_embed_process (inst, in, out_l, out_r, chain, BLOCK);
    }
    CHECK (peak (out_l, BLOCK) > 0.05f,
           "sample lead at 50% mix: the dry side carries the instrument");
    ae_embed_config (inst, "{\"sampleMix\":1}");
    for (int b2 = 0; b2 < 40; ++b2)
    {
        tone_block (in, BLOCK, 35.0, &phase);
        ae_embed_process (inst, in, out_l, out_r, chain, BLOCK);
    }
    CHECK (peak (out_l, BLOCK) < 1e-3f,
           "sample lead at 100% mix: samples alone (none loaded = silence)");
    ae_embed_status (inst, status, sizeof (status));
    CHECK (strstr (status, "\"sampleLevelDb\":0") != NULL,
           "config echo: sampleLevelDb");
    CHECK (strstr (status, "\"sampleToneDb\":0") != NULL,
           "config echo: sampleToneDb");
    ae_embed_config (inst, "{\"sampleLevelDb\":-6,\"sampleToneDb\":4}");
    ae_embed_status (inst, status, sizeof (status));
    CHECK (strstr (status, "\"sampleLevelDb\":-6") != NULL
           && strstr (status, "\"sampleToneDb\":4") != NULL,
           "config echo: sample section follows writes");
    CHECK (strstr (status, "\"melMix\":0,") != NULL,
           "config echo: MEL layer defaults off");
    CHECK (strstr (status, "\"melPreset\":\"footage\"") != NULL,
           "config echo: MEL preset defaults to the plain footage stack");
    ae_embed_config (inst, "{\"melMix\":0.4,\"melOctDb\":-3,"
                           "\"melAttackMs\":250,\"melReleaseMs\":600,"
                           "\"melPreset\":\"strings\"}");
    ae_embed_status (inst, status, sizeof (status));
    CHECK (strstr (status, "\"melMix\":0.4") != NULL
           && strstr (status, "\"melOctDb\":-3") != NULL
           && strstr (status, "\"melAttackMs\":250") != NULL
           && strstr (status, "\"melReleaseMs\":600") != NULL
           && strstr (status, "\"melPreset\":\"strings\"") != NULL,
           "config echo: MEL section follows writes");
    ae_embed_config (inst, "{\"melMix\":0,\"melPreset\":\"footage\"}");
    ae_embed_config (inst,
        "{\"leadSource\":\"voice\",\"sampleMix\":1,"
        "\"sampleLevelDb\":0,\"sampleToneDb\":0}");

    /* The pitch standard must survive its own echo BYTE-EXACTLY: the echo
       is also the config file and the body rig layers re-assert, and at
       %.6g it crept (440 -> 440.001 over 20 cycles) until the anchor
       residue warning fired on an untouched rig. */
    ae_embed_config (inst, "{\"refNote\":0,\"refNoteHz\":261.6256}");
    for (int rt = 0; rt < 20; ++rt)
    {
        char body[128];
        double hz = 0.0;
        ae_embed_status (inst, status, sizeof (status));
        const char *rn = strstr (status, "\"refNoteHz\":");
        if (rn == NULL || sscanf (rn, "\"refNoteHz\":%lf", &hz) != 1)
            break; /* the final CHECK will say so */
        snprintf (body, sizeof (body), "{\"refNote\":0,\"refNoteHz\":%.10g}", hz);
        ae_embed_config (inst, body);
    }
    ae_embed_status (inst, status, sizeof (status));
    CHECK (strstr (status, "\"refNoteHz\":261.6256") != NULL
           && strstr (status, "\"refA4\":440") != NULL,
           "pitch standard survives 20 echo round-trips exactly");

    /* A restart-scoped key rebuilds the engine under the host's feet; the
       next process call must survive it (silence or audio, never a crash),
       and the engine must come back. */
    ae_embed_config (inst, "{\"quality\":\"low\"}");
    for (int b = 0; b < 20; ++b)
    {
        tone_block (in, BLOCK, 35.0, &phase);
        ae_embed_process (inst, in, out_l, out_r, chain, BLOCK);
    }
    ae_embed_status (inst, status, sizeof (status));
    CHECK (strstr (status, "\"running\":true") != NULL,
           "engine back after restart-scoped config");
    CHECK (strstr (status, "\"quality\":\"low\"") != NULL,
           "config echo: quality low");

    /* The config survived to disk for the next launch. */
    {
        FILE *f = fopen (cfg_path, "r");
        CHECK (f != NULL, "config file written");
        if (f != NULL)
        {
            char buf[8192];
            const size_t n = fread (buf, 1, sizeof (buf) - 1, f);
            buf[n] = '\0';
            fclose (f);
            CHECK (strstr (buf, "\"edo\":22") != NULL, "config file carries edo");
        }
    }

    ae_embed_destroy (inst);
    printf ("embedtest: %s\n", g_fail == 0 ? "ALL OK" : "FAILURES");
    remove (cfg_path);
    return g_fail == 0 ? 0 : 1;
}
