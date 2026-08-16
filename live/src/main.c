/* AutoEDO Live — self-contained realtime EDO pitch corrector.

   Wires the audio engine (audio.h backend) to a local web UI: the embedded
   page is served at http://127.0.0.1:<port>/ and every device / processing
   setting is read and changed through the JSON API below.

     GET  /            the control page (embedded at build time)
     GET  /api/status  engine state, live pitch read-out, current config
     GET  /api/devices audio device list
     POST /api/config  partial config update; restarts the engine if the
                       device selection or buffer size changed
     POST /api/restart force an engine restart

   Config persists to ~/.autoedo.json (or the --config PATH; each instance
   of a multi-instance rig gets its own file). */

#include "audio.h"
#include "corrector.h" /* synth patch table names */
#include "sampler.h"   /* instrument discovery */
#include "httpd.h"
#include "json.h"
#include "tuning.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  static void ae_sleep_ms (int ms) { Sleep ((DWORD) ms); }
#else
  #include <unistd.h>
  static void ae_sleep_ms (int ms) { usleep ((useconds_t) ms * 1000); }
#endif

#include "shifter.h"    /* pitch-shifter quality presets + version */
#include "web_index.h"  /* generated: web_index_html[], web_index_html_len */
#include "web_scales.h" /* generated: web_scales_json[], web_scales_json_len */

#define DEFAULT_PORT 8017

/* Git short hash stamped by the Makefile; "which binary am I actually
   running" should never again be a field question. */
#ifndef AE_BUILD_ID
#define AE_BUILD_ID "unknown"
#endif

static long long mono_ms (void)
{
    struct timespec t;
    clock_gettime (CLOCK_MONOTONIC, &t);
    return (long long) t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

typedef struct
{
    AeEngineConfig  engine_cfg;
    /* UI-facing tuning-reference fields; params.ref_hz / period_cents and
       the detection range are derived from these in config_sync(). */
    int             root_note;     /* 0..11, 0 = C (degree 0 of the grid) */
    double          root_cents;    /* fine offset, -50..+50 */
    double          ref_a4;        /* the absolute pitch anchor, carried as
                                      the A4 of a 12-EDO grid on the same
                                      root -- NOT a claim about where A sits
                                      in the tuning actually in use */
    int             ref_note;      /* which note refNoteHz names, 0..11,
                                      0 = C (the default anchor) */
    double          stretch_cents; /* octave stretch, cents per octave */
    char            range_name[16];/* detection range preset */
    double          det_min_hz;    /* explicit range override, 0 = use the */
    double          det_max_hz;    /* preset named by range_name           */
    char            quality_name[16];/* pitch-shifter quality/latency preset */
    char            scale_cat[64]; /* last-loaded catalog scale (cosmetic; */
    char            scale_name[64];/* the mask itself is the real state)   */
    char            label[64];     /* instance name ("Voice", "Guitar"); the
                                      UI shows it so two instances read apart */
    char            config_file[1024]; /* "" => ~/.autoedo.json */
    int             port;
    pthread_mutex_t lock;      /* guards engine + engine_cfg */
    AeAudioEngine  *engine;
    char            engine_err[256];

    /* IR points (v0.4-delta B7): the librarian's {path, hash} plus the
       predelay, per point (0 lead, 1 harmony). Persisted with the config
       so a relaunch reloads the same spaces; ir_err carries the last load
       failure into status (cleared by a success). */
    struct
    {
        char   path[1024];
        char   hash[24];
        double predelay_ms;
    } ir_cfg[2];
    bool            ir_dirty[2]; /* changed since last (re)load */
    char            ir_err[256];
    char            send_err[128]; /* last refused send write ("" = none) */
    /* FOLLOW link (sender side): this instance posts its lead's corrected
       degree as a virtual MIDI note to another instance's /api/midi, where
       stock MIDI Harmony (midiOctaves:"nearest") retunes that instance's
       lead to the pitch class. Voice-follows-guitar with zero new receiver
       machinery. */
    char            follow_host[128]; /* "" = follow off */
    int             follow_port;
    bool            follow_hold;   /* keep the last note held through the
                                      source's silence (else note-off) */
    char            follow_err[128];
    _Atomic int     follow_note;   /* last note posted, -1 = none */
    _Atomic long long follow_in_ms;/* monotonic ms of the last /api/follow
                                      received; 0 = never. The pump enforces
                                      a 400 ms TTL so a dead sender cannot
                                      pin this instance's pitch or leave it
                                      cut: stale link returns to neutral. */
    pthread_t       follow_thread;
    bool            follow_thread_up;
    _Atomic bool    follow_quit;
    char            sample_err[256];
    char            sample_insts[AE_SMP_MAX_INSTRUMENTS][32];
    int             sample_inst_n;
    char            sample_hash[80];
    bool            sample_dirty;  /* the bank needs a (re)load */

    /* Status is serialized once per pump tick (or config change) and every
       consumer — each WebSocket push and each /api/status GET — is handed
       the same cached string. Sized for the config echo plus the pitch
       trace (~48 pairs). */
    pthread_mutex_t status_lock;
    char            status_json[16384];
} App;

/* Detection-range presets (min/max Hz of the tracking window). */
static const struct { const char *name; double min_hz, max_hz; } k_ranges[] = {
    { "bass",       55.0,  400.0 },
    { "baritone",   65.0,  450.0 },
    { "tenor",      80.0,  600.0 },
    { "alto",      100.0,  800.0 },
    { "soprano",   130.0, 1200.0 },
    /* Guitar in standard tuning: low E is 82.4 Hz and the top string's
       12th fret is 659, so 78..1400 covers the instrument with a little
       room either side. The tight bottom is the point -- every subharmonic
       of a note the guitar can actually play falls outside the window, so
       the detector cannot chase one even before the octave guard votes. */
    { "guitar",     78.0, 1400.0 },
    { "instrument", 65.0, 1600.0 },
    { "wide",       40.0, 2000.0 },
};

static int quality_lookup (const char *name)
{
    if (strcmp (name, "low") == 0)  return AE_SHIFT_QUALITY_LOW;
    if (strcmp (name, "high") == 0) return AE_SHIFT_QUALITY_HIGH;
    return AE_SHIFT_QUALITY_BALANCED;
}

static void range_lookup (const char *name, double *min_hz, double *max_hz)
{
    for (size_t i = 0; i < sizeof (k_ranges) / sizeof (k_ranges[0]); ++i)
        if (strcmp (name, k_ranges[i].name) == 0)
        {
            *min_hz = k_ranges[i].min_hz;
            *max_hz = k_ranges[i].max_hz;
            return;
        }
    *min_hz = 65.0; /* "instrument" */
    *max_hz = 1600.0;
}

static volatile sig_atomic_t g_stop = 0;

static void on_signal (int sig)
{
    (void) sig;
    g_stop = 1;
}

/* ------------------------------------------------------------------ config */

static void config_path (const App *app, char *out, size_t cap)
{
    if (app->config_file[0] != '\0')
    {
        snprintf (out, cap, "%s", app->config_file);
        return;
    }
    const char *home = getenv ("HOME");
#ifdef _WIN32
    if (home == NULL || home[0] == '\0')
        home = getenv ("USERPROFILE");
#endif
    if (home != NULL && home[0] != '\0')
        snprintf (out, cap, "%s/.autoedo.json", home);
    else
        snprintf (out, cap, ".autoedo.json");
}

static void config_defaults (App *app)
{
    AeEngineConfig *c = &app->engine_cfg;
    memset (c, 0, sizeof (*c));
    c->buffer_frames          = 256;
    c->params.edo             = 12;
    c->params.retune_ms       = 20.0;
    c->params.transition_ms   = 50.0;
    c->params.amount          = 1.0;
    c->params.tolerance_cents = 0.0;
    c->params.stickiness      = 0.0;
    c->params.humanize        = 0.0;
    c->params.expression      = 1.0;  /* the playing survives by default */
    c->params.degrees_lo      = ~0ull;   /* every degree enabled */
    c->params.degrees_hi      = 0xffull;
    c->params.bypass          = false;
    c->params.bypass_mute     = false; /* bypass passes the dry through */
    /* follow link defaults live on App, not AeEngineConfig: set at startup */
    c->params.lead_on         = true;
    c->params.lead_gain_db    = 0.0;
    c->params.lead_shift_steps = 0;
    c->params.output_gain_db  = 0.0;

    app->root_note     = 0;      /* C */
    app->root_cents    = 0.0;
    app->ref_a4        = 440.0;
    app->ref_note      = 0;      /* state the standard as C, not as A */
    app->stretch_cents = 0.0;
    snprintf (app->range_name, sizeof (app->range_name), "instrument");
    app->det_min_hz = app->det_max_hz = 0.0; /* 0 = use the named preset */
    snprintf (app->quality_name, sizeof (app->quality_name), "balanced");
    app->scale_cat[0] = app->scale_name[0] = '\0';

    c->params.harm_on        = false;
    c->params.harm_lock      = 1; /* Mask — the headline behavior */
    c->params.harm_master_db = 0.0;
    for (int v = 0; v < 5; ++v)
    {
        c->params.harm_interval[v] = 0;
        c->params.harm_ext[v]      = 0;
        c->params.harm_gain_db[v]  = 0.0;
        c->params.harm_pan[v]      = 0.0;
        c->params.harm_detune_cents[v] = 0.0;
    }
    c->params.harm_mute = 0;
    c->params.harm_solo = 0;
    c->params.harm_glide_ms = 0.0;
    c->params.midi_fold      = true;  /* retune to the held class in the
                                          PLAYED register; "held" = absolute */
    c->params.formant_hold   = true;  /* voice default; guitars want off */
    c->params.formant_st     = 0.0;   /* neutral tract */
    c->send_channel          = 0;     /* no record send */
    c->params.send_content   = 1;     /* "wet": the stem, not a mix copy */
    c->params.send_gain_db   = 0.0;
    c->params.send_on        = false; /* the safety: off until asserted */
    app->send_err[0]         = '\0';
    c->params.sample_mix      = 1.0;  /* the sample alone until layered */
    c->params.sample_velocity = -1.0; /* measure it from the lead's attack */
    c->params.sample_ring     = true; /* let-ring: struck notes finish */
    c->params.sample_vel_ref  = -1.0; /* observe the reference, "auto" */
    c->params.poly_notes      = 6;    /* chord sampler polyphony cap */
    c->sample_root[0] = c->sample_manifest[0] = '\0';
    snprintf (c->sample_instrument, sizeof (c->sample_instrument), "piano");
    c->sample_octave = AE_SMP_OCTAVE_AUTO;
    app->sample_err[0] = '\0';
    app->sample_dirty  = false;
    c->params.attack_sound   = 0;     /* off */
    c->params.attack_gain_db = -26.0; /* Xentar's shipped pick level */  /* jump, the classic harmonizer */
    c->params.harm_sustain = true;  /* the release means nothing without it */
    c->params.harm_hold    = false; /* momentary; never a saved state */
    c->params.harm_source      = 0;     /* pitch-shifted live audio */
    c->params.lead_source      = 0;     /* the shifter's corrected voice */
    for (int v = 0; v < 5; ++v)
        c->params.harm_voice_source[v] = -1; /* follow harmSource */
    c->params.synth_patch      = 0;     /* "pad" */
    c->params.ensemble_depth   = 1.0;
    c->params.synth_vowel      = 0.0;
    c->params.harm_tilt_db     = 0.0;
    c->params.vowel_mode       = 0;   /* channel vocoder */
    c->params.synth_attack_ms  = 80.0;
    c->params.synth_release_ms = 500.0;
    c->params.lead_attack_ms   = 5.0;   /* the click guard: no shaping */
    c->params.lead_release_ms  = 500.0;
    c->params.drone_on  = false;
    c->params.drone_deg = 0;
    c->params.ir_lead_mix     = 0.25; /* a space, not a wash, when enabled */
    c->params.ir_lead_gain_db = 0.0;
    c->params.ir_lead_on      = false;
    c->params.ir_harm_mix     = 0.25;
    c->params.ir_harm_gain_db = 0.0;
    c->params.ir_harm_on      = false;
    memset (app->ir_cfg, 0, sizeof (app->ir_cfg));
    app->ir_err[0] = '\0';
    c->params.midi_mode = false;
    c->midi_source[0]   = '\0'; /* all MIDI inputs */
    c->input_channel    = 0;    /* backend default channel handling */
    c->output_channel   = 0;    /* stereo on the device's first two channels */
    app->label[0]       = '\0';
}

/* Derive the engine-facing reference/period/detection-range values from the
   UI-facing fields. Degree 0 sits on the root note in octave 0 (e.g. root C
   at A4 = 440 gives the classic 16.3516 Hz C0 anchor). */
static void config_sync (App *app)
{
    AeEngineConfig *c = &app->engine_cfg;
    c->params.ref_hz = app->ref_a4
                     * pow (2.0, ((double) app->root_note - 9.0) / 12.0) / 16.0
                     * pow (2.0, app->root_cents / 1200.0);
    c->params.period_cents = 1200.0 + app->stretch_cents;
    range_lookup (app->range_name, &c->det_min_hz, &c->det_max_hz);
    /* An explicit window wins over the preset. A named voice type is a fine
       default, but nothing beats telling the detector the actual bottom of
       the instrument in front of it -- a period longer than the lowest note
       the source can play is, by definition, not that source's pitch. */
    if (app->det_min_hz > 0.0) c->det_min_hz = app->det_min_hz;
    if (app->det_max_hz > 0.0) c->det_max_hz = app->det_max_hz;
    if (c->det_max_hz < c->det_min_hz * 2.0)
        c->det_max_hz = c->det_min_hz * 2.0;
    c->quality = quality_lookup (app->quality_name);
}

/* Serialise just the config (shared between the save file and /api/status). */
static void config_json (const App *app, char *out, size_t cap)
{
    const AeEngineConfig *c = &app->engine_cfg;
    char sample_oct_str[16];
    if (c->sample_octave == AE_SMP_OCTAVE_AUTO)
        snprintf (sample_oct_str, sizeof (sample_oct_str), "\"auto\"");
    else
        snprintf (sample_oct_str, sizeof (sample_oct_str), "%d", c->sample_octave);
    /* Echoed in the units it is written in: "auto" or dBFS. status carries
       the reference actually IN FORCE, always a number -- so a pinned rig
       sees the two agree, and an observing one sees what was found. */
    char vel_ref_str[16];
    if (c->params.sample_vel_ref < 0.0)
        snprintf (vel_ref_str, sizeof (vel_ref_str), "\"auto\"");
    else
        snprintf (vel_ref_str, sizeof (vel_ref_str), "%.1f",
                  c->params.sample_vel_ref > 1e-6
                      ? 20.0 * log10 (c->params.sample_vel_ref) : -60.0);
    snprintf (out, cap,
              "{\"edo\":%d,\"retuneMs\":%.6g,\"transitionMs\":%.6g,"
              "\"amount\":%.6g,\"toleranceCents\":%.6g,\"stickiness\":%.6g,"
              "\"humanize\":%.6g,\"expression\":%.6g,"
              "\"rootNote\":%d,\"rootCents\":%.6g,\"refA4\":%.6g,"
              "\"refNote\":%d,\"refNoteHz\":%.6g,"
              "\"stretchCents\":%.6g,\"range\":\"%s\",\"quality\":\"%s\","
              "\"detectMinHz\":%.6g,\"detectMaxHz\":%.6g,"
              "\"bypass\":%s,\"bypassOutput\":\"%s\",\"leadOn\":%s,\"leadGainDb\":%.4g,\"leadShiftSteps\":%d,"
              "\"outputGainDb\":%.6g,"
              "\"bufferFrames\":%d,\"inputChannel\":%d,\"outputChannel\":%d,\"inputUid\":\"",
              c->params.edo, c->params.retune_ms, c->params.transition_ms,
              c->params.amount, c->params.tolerance_cents, c->params.stickiness,
              c->params.humanize, c->params.expression,
              app->root_note, app->root_cents, app->ref_a4,
              app->ref_note,
              app->ref_a4 * pow (2.0, ((double) app->ref_note - 9.0) / 12.0),
              app->stretch_cents, app->range_name, app->quality_name,
              app->det_min_hz, app->det_max_hz,
              c->params.bypass ? "true" : "false",
              c->params.bypass_mute ? "mute" : "dry",
              c->params.lead_on ? "true" : "false",
              c->params.lead_gain_db,
              c->params.lead_shift_steps,
              c->params.output_gain_db, c->buffer_frames, c->input_channel,
              c->output_channel);
    ae_json_escape_append (out, cap, c->input_uid);
    strncat (out, "\",\"outputUid\":\"", cap - strlen (out) - 1);
    ae_json_escape_append (out, cap, c->output_uid);
    strncat (out, "\",\"label\":\"", cap - strlen (out) - 1);
    ae_json_escape_append (out, cap, app->label);
    strncat (out, "\",\"samplePath\":\"", cap - strlen (out) - 1);
    ae_json_escape_append (out, cap, c->sample_root);
    strncat (out, "\",\"sampleManifest\":\"", cap - strlen (out) - 1);
    ae_json_escape_append (out, cap, c->sample_manifest);
    strncat (out, "\",\"sampleHash\":\"", cap - strlen (out) - 1);
    ae_json_escape_append (out, cap, app->sample_hash);
    strncat (out, "\",\"scaleCat\":\"", cap - strlen (out) - 1);
    ae_json_escape_append (out, cap, app->scale_cat);
    strncat (out, "\",\"scaleName\":\"", cap - strlen (out) - 1);
    ae_json_escape_append (out, cap, app->scale_name);
    strncat (out, "\",\"degrees\":[", cap - strlen (out) - 1);
    for (int d = 0; d < AE_MAX_EDO; ++d)
    {
        const bool on = d < 64 ? ((c->params.degrees_lo >> d) & 1u) != 0
                               : ((c->params.degrees_hi >> (d - 64)) & 1u) != 0;
        char item[4];
        snprintf (item, sizeof (item), "%s%d", d > 0 ? "," : "", on ? 1 : 0);
        strncat (out, item, cap - strlen (out) - 1);
    }
    strncat (out, "]", cap - strlen (out) - 1);

    /* Harmony (Xentar hm/hx field packing, plus gains/pans/mute/solo). */
    /* Sized for the LONGEST of the chunks written through it (the harmony
       and sample block below), not the average: a truncation here is a
       config echo that parses but is missing keys, which is worse than a
       hard failure because it looks like a feature-detect miss. */
    char harm[1024];
    static const char *lock_names[] = { "off", "mask", "ji" };
    snprintf (harm, sizeof (harm),
              ",\"harmSource\":\"%s\",\"leadSource\":\"%s\",\"synthPatch\":\"%s\","
              "\"synthAttackMs\":%.6g,\"synthReleaseMs\":%.6g,"
              "\"leadAttackMs\":%.6g,\"leadReleaseMs\":%.6g,"
              "\"ensembleDepth\":%.6g,\"synthVowel\":%.6g,\"harmTiltDb\":%.6g,"
              "\"vowelMode\":\"%s\",\"droneOn\":%s,\"droneDeg\":%d,\"hSrc\":[",
              c->params.harm_source == 1 ? "synth"
                : c->params.harm_source == 2 ? "sample" : "voice",
              c->params.lead_source == 1 ? "synth"
                : c->params.lead_source == 2 ? "sample" : "voice",
              ae_synth_patch_name (c->params.synth_patch),
              c->params.synth_attack_ms, c->params.synth_release_ms,
              c->params.lead_attack_ms, c->params.lead_release_ms,
              c->params.ensemble_depth, c->params.synth_vowel,
              c->params.harm_tilt_db,
              c->params.vowel_mode == 1 ? "lpc" : "vocoder",
              c->params.drone_on ? "true" : "false", c->params.drone_deg);
    strncat (out, harm, cap - strlen (out) - 1);
    for (int v = 0; v < 5; ++v)
    {
        const int s = c->params.harm_voice_source[v];
        snprintf (harm, sizeof (harm), "%s\"%s\"", v ? "," : "",
                  s == 0 ? "voice" : s == 1 ? "synth"
                : s == 2 ? "sample" : "default");
        strncat (out, harm, cap - strlen (out) - 1);
    }
    strncat (out, "]", cap - strlen (out) - 1);
    snprintf (harm, sizeof (harm),
              ",\"harmOn\":%s,\"harmLock\":\"%s\",\"harmGainDb\":%.4g,"
              "\"harmSustain\":%s,\"harmHold\":%s,\"harmGlideMs\":%.4g,"
              "\"attackSound\":\"%s\",\"attackGainDb\":%.4g,"
              "\"midiOctaves\":\"%s\",\"formantHold\":%s,\"formantSemitones\":%.4g,"
              "\"sampleMix\":%.4g,\"sampleVelocity\":%.4g,\"sampleRing\":%s,"
              "\"followUrl\":\"%s\",\"followHold\":%s,\"followEnv\":%.4g,"
              "\"polyMode\":%s,\"polyNotes\":%d,"
              "\"sampleVelRefDb\":%s,"
              "\"sampleInstrument\":\"%s\",\"sampleOctave\":%s,"
              "\"sendChannel\":%d,\"sendContent\":\"%s\",\"sendGainDb\":%.4g,\"sendOn\":%s,"
              "\"midiMode\":%s,\"midiSource\":\"",
              c->params.harm_on ? "true" : "false",
              lock_names[c->params.harm_lock >= 0 && c->params.harm_lock <= 2
                           ? c->params.harm_lock : 0],
              c->params.harm_master_db,
              c->params.harm_sustain ? "true" : "false",
              c->params.harm_hold ? "true" : "false",
              c->params.harm_glide_ms,
              (const char *[]){ "off", "noise", "pick", "click" }
                  [c->params.attack_sound >= 0 && c->params.attack_sound <= 3
                       ? c->params.attack_sound : 0],
              c->params.attack_gain_db,
              c->params.midi_fold ? "nearest" : "held",
              c->params.formant_hold ? "true" : "false",
              c->params.formant_st,
              c->params.sample_mix, c->params.sample_velocity,
              c->params.sample_ring ? "true" : "false",
              c->follow_url, app->follow_hold ? "true" : "false",
              c->params.follow_env,
              c->poly_mode ? "true" : "false",
              c->params.poly_notes,
              vel_ref_str,
              c->sample_instrument, sample_oct_str,
              c->send_channel,
              (const char *[]){ "full", "wet", "lead", "harm" }
                  [c->params.send_content >= 0 && c->params.send_content <= 3
                       ? c->params.send_content : 0],
              c->params.send_gain_db,
              c->params.send_on ? "true" : "false",
              c->params.midi_mode ? "true" : "false");
    strncat (out, harm, cap - strlen (out) - 1);
    ae_json_escape_append (out, cap, c->midi_source);
    strncat (out, "\"", cap - strlen (out) - 1);
    const char *keys[5] = { "hm", "hx", "hg", "hp", "hd" };
    for (int k = 0; k < 5; ++k)
    {
        size_t n = 0;
        n += (size_t) snprintf (harm + n, sizeof (harm) - n, ",\"%s\":[", keys[k]);
        for (int v = 0; v < 5; ++v)
        {
            if (k == 0)      n += (size_t) snprintf (harm + n, sizeof (harm) - n, "%s%d",  v ? "," : "", c->params.harm_interval[v]);
            else if (k == 1) n += (size_t) snprintf (harm + n, sizeof (harm) - n, "%s%d",  v ? "," : "", c->params.harm_ext[v]);
            else if (k == 2) n += (size_t) snprintf (harm + n, sizeof (harm) - n, "%s%.4g", v ? "," : "", c->params.harm_gain_db[v]);
            else if (k == 3) n += (size_t) snprintf (harm + n, sizeof (harm) - n, "%s%.4g", v ? "," : "", c->params.harm_pan[v]);
            else             n += (size_t) snprintf (harm + n, sizeof (harm) - n, "%s%.4g", v ? "," : "", c->params.harm_detune_cents[v]);
        }
        snprintf (harm + n, sizeof (harm) - n, "]");
        strncat (out, harm, cap - strlen (out) - 1);
    }
    snprintf (harm, sizeof (harm),
              ",\"hMute\":[%d,%d,%d,%d,%d],\"hSolo\":[%d,%d,%d,%d,%d]",
              (c->params.harm_mute >> 0) & 1, (c->params.harm_mute >> 1) & 1,
              (c->params.harm_mute >> 2) & 1, (c->params.harm_mute >> 3) & 1,
              (c->params.harm_mute >> 4) & 1,
              (c->params.harm_solo >> 0) & 1, (c->params.harm_solo >> 1) & 1,
              (c->params.harm_solo >> 2) & 1, (c->params.harm_solo >> 3) & 1,
              (c->params.harm_solo >> 4) & 1);
    strncat (out, harm, cap - strlen (out) - 1);

    /* IR points -- the spec's irLead{}/irHarm{} objects as flat keys (the
       shape this parser speaks; documented in CONTROL.md §IR). */
    static const char *ir_pfx[2] = { "irLead", "irHarm" };
    for (int pt = 0; pt < 2; ++pt)
    {
        const double mix  = pt == 0 ? c->params.ir_lead_mix : c->params.ir_harm_mix;
        const double gain = pt == 0 ? c->params.ir_lead_gain_db : c->params.ir_harm_gain_db;
        const bool   on   = pt == 0 ? c->params.ir_lead_on : c->params.ir_harm_on;
        snprintf (harm, sizeof (harm), ",\"%sPath\":\"", ir_pfx[pt]);
        strncat (out, harm, cap - strlen (out) - 1);
        ae_json_escape_append (out, cap, app->ir_cfg[pt].path);
        snprintf (harm, sizeof (harm),
                  "\",\"%sHash\":\"%s\",\"%sPredelayMs\":%.4g,"
                  "\"%sMix\":%.4g,\"%sGainDb\":%.4g,\"%sOn\":%s",
                  ir_pfx[pt], app->ir_cfg[pt].hash,
                  ir_pfx[pt], app->ir_cfg[pt].predelay_ms,
                  ir_pfx[pt], mix, ir_pfx[pt], gain,
                  ir_pfx[pt], on ? "true" : "false");
        strncat (out, harm, cap - strlen (out) - 1);
    }
    strncat (out, "}", cap - strlen (out) - 1);
}

static void config_save (const App *app)
{
    char path[1024];
    config_path (app, path, sizeof (path));
    FILE *f = fopen (path, "w");
    if (f == NULL)
    {
        fprintf (stderr, "autoedo: cannot write %s: %s\n", path, strerror (errno));
        return;
    }
    char body[8192] = "";
    config_json (app, body, sizeof (body));
    fprintf (f, "%s\n", body);
    fclose (f);
}

static double num_clamp (double v, double lo, double hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Apply any known keys found in `json` onto the app config. Returns true if
   a setting that needs an engine restart (devices / buffer / range) changed. */
static bool config_apply_json (App *app, const char *json)
{
    AeEngineConfig *c = &app->engine_cfg;
    bool restart = false;
    double num;
    bool   b;
    char   str[AE_UID_MAX];
    char   str2[1024];

    if (ae_json_get_number (json, "edo", &num))
    {
        int edo = (int) num;
        if (edo < AE_MIN_EDO) edo = AE_MIN_EDO;
        if (edo > AE_MAX_EDO) edo = AE_MAX_EDO;
        c->params.edo = edo;
    }
    if (ae_json_get_number (json, "retuneMs", &num))
        c->params.retune_ms = num_clamp (num, 0.0, 400.0);
    if (ae_json_get_number (json, "transitionMs", &num))
        c->params.transition_ms = num_clamp (num, 0.0, 200.0);
    if (ae_json_get_number (json, "amount", &num))
        c->params.amount = num_clamp (num, 0.0, 1.0);
    if (ae_json_get_number (json, "toleranceCents", &num))
        c->params.tolerance_cents = num_clamp (num, 0.0, 50.0);
    if (ae_json_get_number (json, "stickiness", &num))
        c->params.stickiness = num_clamp (num, 0.0, 1.0);
    if (ae_json_get_number (json, "expression", &num))
        c->params.expression = num_clamp (num, 0.0, 1.0);
    if (ae_json_get_number (json, "humanize", &num))
        c->params.humanize = num_clamp (num, 0.0, 1.0);
    if (ae_json_get_number (json, "rootNote", &num))
        app->root_note = (int) num_clamp (num, 0.0, 11.0);
    if (ae_json_get_number (json, "rootCents", &num))
        app->root_cents = num_clamp (num, -50.0, 50.0);
    if (ae_json_get_number (json, "refA4", &num))
        app->ref_a4 = num_clamp (num, 400.0, 480.0);
    /* State the pitch standard in whatever terms the rig actually uses.
       `refNote` names the note, `refNoteHz` gives its frequency in octave 4;
       between them they set the same single anchor `refA4` carries, and the
       echo always shows all three.

       This matters for anything that is not 12-EDO. The EDO grid is built
       off DEGREE 0, which `rootNote` places -- so with rootNote = C, C is
       the anchor and is the SAME frequency in every EDO, while the degree
       you would call A falls wherever that EDO puts it (in 22-EDO, 433.12
       or 446.99 Hz, never 440). `refA4` is only the arithmetic that gets
       there: it is the A of a 12-EDO grid on the same root, not a claim
       about where A sits in the tuning in use. Nudging it because "our A
       isn't 440" moves C, and moves the whole rig off the band. Set
       refNote/refNoteHz instead and the trap does not exist. Parsed after
       refA4, so a POST carrying both lands on the more specific one. */
    if (ae_json_get_number (json, "refNote", &num))
        app->ref_note = (int) num_clamp (num, 0.0, 11.0);
    if (ae_json_get_number (json, "refNoteHz", &num) && num > 0.0)
        app->ref_a4 = num_clamp (num * pow (2.0, (9.0 - (double) app->ref_note) / 12.0),
                                 400.0, 480.0);
    if (ae_json_get_number (json, "stretchCents", &num))
        app->stretch_cents = num_clamp (num, -30.0, 30.0);
    if (ae_json_get_string (json, "range", str, sizeof (str)))
    {
        double lo1, hi1, lo2, hi2;
        range_lookup (app->range_name, &lo1, &hi1);
        range_lookup (str, &lo2, &hi2);
        if (lo1 != lo2 || hi1 != hi2)
            restart = true; /* detection window is baked in at prepare time */
        snprintf (app->range_name, sizeof (app->range_name), "%.15s", str);
    }
    /* Explicit detection window, 0 = fall back to the preset. Restart-scoped
       like `range`: the analysis buffers are sized from it at prepare. */
    if (ae_json_get_number (json, "detectMinHz", &num))
    {
        const double lo = num == 0.0 ? 0.0 : num_clamp (num, 20.0, 500.0);
        if (lo != app->det_min_hz) restart = true;
        app->det_min_hz = lo;
    }
    if (ae_json_get_number (json, "detectMaxHz", &num))
    {
        const double hi = num == 0.0 ? 0.0 : num_clamp (num, 100.0, 4000.0);
        if (hi != app->det_max_hz) restart = true;
        app->det_max_hz = hi;
    }
    if (ae_json_get_bool (json, "polyMode", &b))
    {
        if (b != c->poly_mode)
            restart = true; /* rebuilds the shifters at the doubled block */
        c->poly_mode = b;
    }
    if (ae_json_get_number (json, "polyNotes", &num))
        c->params.poly_notes = (int) num_clamp (num, 1.0, 6.0);
    if (ae_json_get_string (json, "quality", str, sizeof (str)))
    {
        if (quality_lookup (str) != quality_lookup (app->quality_name))
            restart = true; /* the shifter block size is fixed at prepare time */
        snprintf (app->quality_name, sizeof (app->quality_name), "%.15s", str);
    }
    if (ae_json_get_bool (json, "bypass", &b))
        c->params.bypass = b;
    /* What bypass PUTS on the output. "mute" retires the stateful
       outputGainDb:-60 dance for a rig whose dry already reaches the desk
       on its own row -- one switch, no level to restore afterwards. */
    if (ae_json_get_string (json, "bypassOutput", str, sizeof (str)))
        c->params.bypass_mute = strcmp (str, "mute") == 0;
    if (ae_json_get_bool (json, "leadOn", &b))
        c->params.lead_on = b;
    if (ae_json_get_number (json, "outputGainDb", &num))
        c->params.output_gain_db = num_clamp (num, -60.0, 12.0);

    /* Harmony. */
    if (ae_json_get_number (json, "leadGainDb", &num))
        c->params.lead_gain_db = num_clamp (num, -60.0, 12.0);
    if (ae_json_get_number (json, "leadShiftSteps", &num))
        c->params.lead_shift_steps = (int) num_clamp (num, -72.0, 72.0);
    if (ae_json_get_bool (json, "harmOn", &b))
        c->params.harm_on = b;
    if (ae_json_get_number (json, "harmGainDb", &num))
        c->params.harm_master_db = num_clamp (num, -24.0, 12.0);
    if (ae_json_get_string (json, "midiOctaves", str, sizeof (str)))
        c->params.midi_fold = strcmp (str, "held") != 0;
    if (ae_json_get_bool (json, "formantHold", &b))
        c->params.formant_hold = b;
    if (ae_json_get_number (json, "formantSemitones", &num))
        c->params.formant_st = num_clamp (num, -12.0, 12.0);

    /* The record send. Content/gain/mute are live; the channel needs the
       device channel map, so it is restart-scoped like outputChannel. A
       collision with the live output is REFUSED, not summed: a silent sum
       is a feedback loop with a friendly face. The refusal lands in
       status.sendError. */
    if (ae_json_get_number (json, "sendChannel", &num))
    {
        const int ch = (int) num_clamp (num, 0.0, 32.0);
        const bool collide = ch > 0
            && (ch == c->output_channel
                || (c->output_channel == 0 && ch <= 2));
        if (collide)
            snprintf (app->send_err, sizeof (app->send_err),
                      "send channel %d collides with the live output", ch);
        else
        {
            if (ch != c->send_channel) restart = true;
            c->send_channel = ch;
            app->send_err[0] = '\0';
        }
    }
    if (ae_json_get_string (json, "sendContent", str, sizeof (str)))
        c->params.send_content = strcmp (str, "wet") == 0 ? 1
                               : strcmp (str, "lead") == 0 ? 2
                               : strcmp (str, "harm") == 0 ? 3 : 0;
    if (ae_json_get_number (json, "sendGainDb", &num))
        c->params.send_gain_db = num_clamp (num, -60.0, 12.0);
    if (ae_json_get_bool (json, "sendOn", &b))
        c->params.send_on = b;
    if (ae_json_get_string (json, "attackSound", str, sizeof (str)))
        c->params.attack_sound = strcmp (str, "noise") == 0 ? 1
                               : strcmp (str, "pick")  == 0 ? 2
                               : strcmp (str, "click") == 0 ? 3 : 0;
    if (ae_json_get_number (json, "attackGainDb", &num))
        c->params.attack_gain_db = num_clamp (num, -60.0, 12.0);
    if (ae_json_get_number (json, "sampleMix", &num))
        c->params.sample_mix = num_clamp (num, 0.0, 1.0);
    if (ae_json_get_number (json, "sampleVelocity", &num))
        c->params.sample_velocity = num < 0.0 ? -1.0 : num_clamp (num, 0.0, 1.0);
    if (ae_json_get_bool (json, "sampleRing", &b))
        c->params.sample_ring = b;
    /* FOLLOW link. followUrl aims THIS instance's corrected degree and
       envelope at another instance ("" = off); followHold keeps the last
       note held through this instance's silence; followEnv is the
       RECEIVER-side depth: how much of the incoming envelope shapes this
       instance's output (1 = the source's silence cuts it). */
    if (ae_json_get_string (json, "followUrl", str2, sizeof (str2)))
    {
        char host[128] = "";
        int  fport = 0;
        const char *u = str2;
        if (strncmp (u, "http://", 7) == 0) u += 7;
        const char *colon = strrchr (u, ':');
        if (str2[0] == '\0')
        {
            app->follow_host[0] = '\0';
            app->follow_port = 0;
            snprintf (c->follow_url, sizeof (c->follow_url), "%s", "");
            app->follow_err[0] = '\0';
        }
        else if (colon != NULL && colon != u
                 && (size_t) (colon - u) < sizeof (host)
                 && atoi (colon + 1) > 0 && atoi (colon + 1) < 65536)
        {
            memcpy (host, u, (size_t) (colon - u));
            host[colon - u] = '\0';
            fport = atoi (colon + 1);
            snprintf (app->follow_host, sizeof (app->follow_host), "%s", host);
            app->follow_port = fport;
            snprintf (c->follow_url, sizeof (c->follow_url), "%.127s", str2);
            app->follow_err[0] = '\0';
        }
        else
            snprintf (app->follow_err, sizeof (app->follow_err),
                      "follow: bad followUrl (want host:port): %.80s", str2);
    }
    if (ae_json_get_bool (json, "followHold", &b))
        app->follow_hold = b;
    if (ae_json_get_number (json, "followEnv", &num))
        c->params.follow_env = num_clamp (num, 0.0, 1.0);
    /* The strike map's reference, in dBFS. "auto" (default) observes one;
       a number asserts it, for a host that already knows how hard this
       player plays -- TENDRIL's loudest onset of the capture, an FX
       layer's own rolling peak. Stored linear because that is what the
       map wants; -1 is the "observe" sentinel, which a dB value can never
       collide with because every legal dB here is <= 0 and stored as a
       POSITIVE amplitude. */
    if (ae_json_get_string (json, "sampleVelRefDb", str, sizeof (str))
        && strcmp (str, "auto") == 0)
        c->params.sample_vel_ref = -1.0;
    else if (ae_json_get_number (json, "sampleVelRefDb", &num))
        c->params.sample_vel_ref = pow (10.0, num_clamp (num, -60.0, 0.0) / 20.0);
    if (ae_json_get_string (json, "samplePath", str2, sizeof (str2)))
    {
        snprintf (c->sample_root, sizeof (c->sample_root), "%s", str2);
        app->sample_dirty = true;
    }
    if (ae_json_get_string (json, "sampleManifest", str2, sizeof (str2)))
    {
        snprintf (c->sample_manifest, sizeof (c->sample_manifest), "%s", str2);
        app->sample_dirty = true;
    }
    if (ae_json_get_string (json, "sampleHash", str2, sizeof (str2)))
    {
        /* The librarian's token for "the library changed". Re-read only
           when it moves -- 120 WAVs is not a per-POST operation. */
        if (strcmp (str2, app->sample_hash) != 0)
        {
            snprintf (app->sample_hash, sizeof (app->sample_hash), "%.79s", str2);
            app->sample_dirty = true;
        }
    }
    /* "auto" = the built-in table for the sets whose filenames are not
       their sounding pitch; a number overrides it for any other set. */
    if (ae_json_get_string (json, "sampleOctave", str, sizeof (str))
        && strcmp (str, "auto") == 0)
    {
        if (c->sample_octave != AE_SMP_OCTAVE_AUTO) app->sample_dirty = true;
        c->sample_octave = AE_SMP_OCTAVE_AUTO;
    }
    else if (ae_json_get_number (json, "sampleOctave", &num))
    {
        const int oct = (int) num_clamp (num, -24.0, 24.0);
        if (oct != c->sample_octave) app->sample_dirty = true;
        c->sample_octave = oct;
    }
    if (ae_json_get_string (json, "sampleInstrument", str, sizeof (str)))
    {
        if (strcmp (str, c->sample_instrument) != 0)
        {
            snprintf (c->sample_instrument, sizeof (c->sample_instrument), "%.31s", str);
            app->sample_dirty = true;
        }
    }
    if (ae_json_get_number (json, "harmGlideMs", &num))
        c->params.harm_glide_ms = num_clamp (num, 0.0, 5000.0);
    if (ae_json_get_bool (json, "harmSustain", &b))
        c->params.harm_sustain = b;
    if (ae_json_get_bool (json, "harmHold", &b))
        c->params.harm_hold = b;
    if (ae_json_get_string (json, "harmSource", str, sizeof (str)))
        c->params.harm_source = strcmp (str, "synth") == 0 ? 1
                              : strcmp (str, "sample") == 0 ? 2 : 0;
    if (ae_json_get_string (json, "leadSource", str, sizeof (str)))
        c->params.lead_source = strcmp (str, "synth") == 0 ? 1
                              : strcmp (str, "sample") == 0 ? 2 : 0;
    if (ae_json_get_number (json, "ensembleDepth", &num))
        c->params.ensemble_depth = num_clamp (num, 0.0, 1.0);
    if (ae_json_get_number (json, "synthVowel", &num))
        c->params.synth_vowel = num_clamp (num, 0.0, 1.0);
    if (ae_json_get_number (json, "harmTiltDb", &num))
        c->params.harm_tilt_db = num_clamp (num, -12.0, 12.0);
    if (ae_json_get_string (json, "vowelMode", str, sizeof (str)))
        c->params.vowel_mode = strcmp (str, "lpc") == 0 ? 1 : 0;
    if (ae_json_get_bool (json, "droneOn", &b))
        c->params.drone_on = b;
    if (ae_json_get_number (json, "droneDeg", &num))
    {
        const int deg = (int) num;
        c->params.drone_deg = deg < 0 ? 0
                            : deg > 8 * AE_MAX_EDO ? 8 * AE_MAX_EDO : deg;
    }

    /* IR points: the live three go to the params; path/hash/predelay mark
       the point dirty and the caller reloads it (file + FFT work). */
    if (ae_json_get_number (json, "irLeadMix", &num))
        c->params.ir_lead_mix = num_clamp (num, 0.0, 1.0);
    if (ae_json_get_number (json, "irLeadGainDb", &num))
        c->params.ir_lead_gain_db = num_clamp (num, -24.0, 12.0);
    if (ae_json_get_bool (json, "irLeadOn", &b))
        c->params.ir_lead_on = b;
    if (ae_json_get_number (json, "irHarmMix", &num))
        c->params.ir_harm_mix = num_clamp (num, 0.0, 1.0);
    if (ae_json_get_number (json, "irHarmGainDb", &num))
        c->params.ir_harm_gain_db = num_clamp (num, -24.0, 12.0);
    if (ae_json_get_bool (json, "irHarmOn", &b))
        c->params.ir_harm_on = b;
    {
        static const char *pfx[2] = { "irLead", "irHarm" };
        char key[32], val[1024];
        for (int pt = 0; pt < 2; ++pt)
        {
            snprintf (key, sizeof (key), "%sPath", pfx[pt]);
            if (ae_json_get_string (json, key, val, sizeof (val))
                && strcmp (val, app->ir_cfg[pt].path) != 0)
            {
                snprintf (app->ir_cfg[pt].path, sizeof (app->ir_cfg[pt].path),
                          "%s", val);
                app->ir_dirty[pt] = true;
            }
            snprintf (key, sizeof (key), "%sHash", pfx[pt]);
            if (ae_json_get_string (json, key, val, sizeof (val))
                && strcmp (val, app->ir_cfg[pt].hash) != 0)
            {
                snprintf (app->ir_cfg[pt].hash, sizeof (app->ir_cfg[pt].hash),
                          "%.23s", val); /* 16 hex digits + headroom */
                app->ir_dirty[pt] = true;
            }
            snprintf (key, sizeof (key), "%sPredelayMs", pfx[pt]);
            if (ae_json_get_number (json, key, &num)
                && num_clamp (num, 0.0, 50.0) != app->ir_cfg[pt].predelay_ms)
            {
                app->ir_cfg[pt].predelay_ms = num_clamp (num, 0.0, 50.0);
                app->ir_dirty[pt] = true;
            }
        }
    }
    /* Per-voice sources: "voice" / "synth", anything else (including
       "default" and an empty slot) means follow harmSource. */
    {
        char srcs[5][AE_JSON_STR_MAX];
        const int ns = ae_json_get_str_array (json, "hSrc", srcs, 5);
        for (int v = 0; v < ns; ++v)
            c->params.harm_voice_source[v] =
                strcmp (srcs[v], "synth")  == 0 ? 1
              : strcmp (srcs[v], "sample") == 0 ? 2
              : strcmp (srcs[v], "voice")  == 0 ? 0 : -1;
    }
    if (ae_json_get_string (json, "synthPatch", str, sizeof (str)))
    {
        const int idx = ae_synth_patch_find (str);
        if (idx >= 0) /* unknown names keep the current patch */
            c->params.synth_patch = idx;
    }
    if (ae_json_get_number (json, "leadAttackMs", &num))
        c->params.lead_attack_ms = num_clamp (num, 0.0, 5000.0);
    if (ae_json_get_number (json, "leadReleaseMs", &num))
        c->params.lead_release_ms = num_clamp (num, 5.0, 10000.0);
    if (ae_json_get_number (json, "synthAttackMs", &num))
        c->params.synth_attack_ms = num_clamp (num, 0.0, 5000.0);
    if (ae_json_get_number (json, "synthReleaseMs", &num))
        c->params.synth_release_ms = num_clamp (num, 0.0, 10000.0);
    if (ae_json_get_string (json, "harmLock", str, sizeof (str)))
        c->params.harm_lock = strcmp (str, "mask") == 0 ? 1
                            : strcmp (str, "ji") == 0 ? 2 : 0;
    double arr[5];
    int n5;
    if ((n5 = ae_json_get_num_array (json, "hm", arr, 5)) >= 0)
        for (int v = 0; v < n5; ++v)
            c->params.harm_interval[v] =
                (int) num_clamp (arr[v], -(double) AE_MAX_EDO, (double) AE_MAX_EDO);
    if ((n5 = ae_json_get_num_array (json, "hx", arr, 5)) >= 0)
        for (int v = 0; v < n5; ++v)
            c->params.harm_ext[v] = (int) num_clamp (arr[v], 0.0, 2.0);
    if ((n5 = ae_json_get_num_array (json, "hg", arr, 5)) >= 0)
        for (int v = 0; v < n5; ++v)
            /* Ceiling raised from +6: with 0 dB now meaning "at the lead's
               level", the whole +12 is real headroom above the lead rather
               than half of it being spent climbing back to parity. */
            c->params.harm_gain_db[v] = num_clamp (arr[v], -60.0, 12.0);
    if ((n5 = ae_json_get_num_array (json, "hp", arr, 5)) >= 0)
        for (int v = 0; v < n5; ++v)
            c->params.harm_pan[v] = num_clamp (arr[v], -1.0, 1.0);
    if ((n5 = ae_json_get_num_array (json, "hd", arr, 5)) >= 0)
        for (int v = 0; v < n5; ++v)
            c->params.harm_detune_cents[v] = num_clamp (arr[v], -100.0, 100.0);
    if ((n5 = ae_json_get_num_array (json, "hMute", arr, 5)) >= 0)
    {
        uint32_t m = c->params.harm_mute;
        for (int v = 0; v < n5; ++v)
            m = arr[v] != 0.0 ? (m | (1u << v)) : (m & ~(1u << v));
        c->params.harm_mute = m;
    }
    if ((n5 = ae_json_get_num_array (json, "hSolo", arr, 5)) >= 0)
    {
        uint32_t m = c->params.harm_solo;
        for (int v = 0; v < n5; ++v)
            m = arr[v] != 0.0 ? (m | (1u << v)) : (m & ~(1u << v));
        c->params.harm_solo = m;
    }
    if (ae_json_get_string (json, "scaleCat", str, sizeof (str)))
        snprintf (app->scale_cat, sizeof (app->scale_cat), "%.63s", str);
    if (ae_json_get_string (json, "scaleName", str, sizeof (str)))
        snprintf (app->scale_name, sizeof (app->scale_name), "%.63s", str);
    if (ae_json_get_string (json, "label", str, sizeof (str)))
        snprintf (app->label, sizeof (app->label), "%.63s", str);

    /* MIDI Harmony: the mode is live; the source binding needs a restart. */
    if (ae_json_get_bool (json, "midiMode", &b))
        c->params.midi_mode = b;
    if (ae_json_get_string (json, "midiSource", str, sizeof (str))
        && strcmp (str, c->midi_source) != 0)
    {
        snprintf (c->midi_source, sizeof (c->midi_source), "%s", str);
        restart = true;
    }

    unsigned char flags[AE_MAX_EDO];
    const int n = ae_json_get_flag_array (json, "degrees", flags, AE_MAX_EDO);
    if (n >= 0)
    {
        uint64_t lo = c->params.degrees_lo, hi = c->params.degrees_hi;
        for (int d = 0; d < n; ++d)
        {
            if (d < 64)
                lo = flags[d] ? (lo | (1ull << d)) : (lo & ~(1ull << d));
            else
                hi = flags[d] ? (hi | (1ull << (d - 64))) : (hi & ~(1ull << (d - 64)));
        }
        c->params.degrees_lo = lo;
        c->params.degrees_hi = hi;
    }

    if (ae_json_get_number (json, "bufferFrames", &num))
    {
        int bf = (int) num;
        if (bf < 32)   bf = 32;
        if (bf > 2048) bf = 2048;
        if (bf != c->buffer_frames)
        {
            c->buffer_frames = bf;
            restart = true;
        }
    }
    if (ae_json_get_number (json, "inputChannel", &num))
    {
        const int ch = (int) num_clamp (num, 0.0, 32.0);
        if (ch != c->input_channel)
        {
            c->input_channel = ch;
            restart = true; /* the capture binding is fixed at engine start */
        }
    }
    if (ae_json_get_number (json, "outputChannel", &num))
    {
        const int ch = (int) num_clamp (num, 0.0, 32.0);
        /* The same collision rule from the other side: moving the live
           output ONTO the record send is refused too. */
        if (c->send_channel > 0
            && (ch == c->send_channel || (ch == 0 && c->send_channel <= 2)))
        {
            snprintf (app->send_err, sizeof (app->send_err),
                      "output channel %d collides with the record send", ch);
        }
        else if (ch != c->output_channel)
        {
            c->output_channel = ch;
            restart = true; /* the playback binding is fixed at engine start */
        }
    }
    if (ae_json_get_string (json, "inputUid", str, sizeof (str))
        && strcmp (str, c->input_uid) != 0)
    {
        snprintf (c->input_uid, sizeof (c->input_uid), "%s", str);
        restart = true;
    }
    if (ae_json_get_string (json, "outputUid", str, sizeof (str))
        && strcmp (str, c->output_uid) != 0)
    {
        snprintf (c->output_uid, sizeof (c->output_uid), "%s", str);
        restart = true;
    }
    return restart;
}

static void config_load (App *app)
{
    char path[1024];
    config_path (app, path, sizeof (path));
    FILE *f = fopen (path, "r");
    if (f == NULL)
        return;
    char buf[8192];
    const size_t n = fread (buf, 1, sizeof (buf) - 1, f);
    buf[n] = '\0';
    fclose (f);
    config_apply_json (app, buf);

    /* Harmony is PERFORMANCE STATE, not a setting, so it does not survive a
       relaunch. The file still carries harmOn/droneOn (the config echo has
       one shape, always), but a fresh engine comes up with the ghosts
       silent. Otherwise ending a set with harmony up means the next launch
       -- often mid-soundcheck, or at the top of a song, before any chart
       has said anything about harmony -- starts singing on its own, which
       is exactly the surprise a controller then has to race to undo. */
    app->engine_cfg.params.harm_on  = false;
    app->engine_cfg.params.drone_on = false;
    app->engine_cfg.params.harm_hold = false; /* momentary: never a start state */
}

/* ------------------------------------------------------------------ engine */

/* Caller must hold app->lock. (Re)load the sample bank when the librarian's
   binding changed -- path, manifest, instrument, or the library hash. A
   failed load leaves the running bank playing and puts the reason in
   status.sampleError; a cleared path unloads. */
static void samples_reload_locked (App *app)
{
    if (app->engine == NULL || ! app->sample_dirty)
        return;
    app->sample_dirty = false;
    AeEngineConfig *c = &app->engine_cfg;
    /* Re-read what the cache actually holds, so a UI builds its picker from
       the engine's own table rather than a list compiled into it. */
    app->sample_inst_n = ae_sampler_list (c->sample_root, app->sample_insts,
                                          AE_SMP_MAX_INSTRUMENTS);
    if (c->sample_root[0] == '\0')
    {
        app->sample_err[0] = '\0';
        return;
    }
    char err[256] = "";
    if (! ae_audio_engine_load_samples (app->engine, c->sample_root,
                                        c->sample_instrument, c->sample_manifest,
                                        c->sample_octave, err, sizeof (err)))
        snprintf (app->sample_err, sizeof (app->sample_err), "%s", err);
    else
        app->sample_err[0] = '\0';
}

/* Caller must hold app->lock. */
/* (Re)load configured IR points into the running engine. A fresh engine
   needs everything; a config edit only what it dirtied. Failures land in
   ir_err for the status echo -- an IR that cannot load must be a visible
   fact, not a silently dry point. */
static void ir_reload_locked (App *app, bool only_dirty)
{
    if (app->engine == NULL)
        return;
    for (int pt = 0; pt < 2; ++pt)
    {
        if (only_dirty && ! app->ir_dirty[pt])
            continue;
        const bool had = app->ir_dirty[pt];
        app->ir_dirty[pt] = false;
        if (app->ir_cfg[pt].path[0] == '\0' && ! had)
            continue; /* nothing configured; a fresh corrector is clear */
        char err[256];
        if (ae_audio_engine_load_ir (app->engine, pt, app->ir_cfg[pt].path,
                                     app->ir_cfg[pt].hash,
                                     app->ir_cfg[pt].predelay_ms,
                                     err, sizeof (err)))
            app->ir_err[0] = '\0';
        else
        {
            snprintf (app->ir_err, sizeof (app->ir_err), "%s", err);
            fprintf (stderr, "autoedo: IR load: %s\n", err);
        }
    }
}

static void engine_restart_locked (App *app)
{
    if (app->engine != NULL)
    {
        ae_audio_engine_stop (app->engine);
        app->engine = NULL;
    }
    app->engine_err[0] = '\0';
    app->engine = ae_audio_engine_start (&app->engine_cfg, app->engine_err,
                                         sizeof (app->engine_err));
    if (app->engine == NULL)
        fprintf (stderr, "autoedo: engine start failed: %s\n", app->engine_err);
    ir_reload_locked (app, false); /* a fresh corrector needs its spaces back */
    app->sample_dirty = true;      /* ...and its sample bank */
    samples_reload_locked (app);
}

/* --------------------------------------------------------------------- api */

/* Serialize the full status once into the cache (Rule 2 of the smooth-UI
   pattern: one serialization per tick, shared by all consumers). */
static void status_refresh (App *app)
{
    char buf[16384];

    pthread_mutex_lock (&app->lock);

    AeEngineStatus st;
    memset (&st, 0, sizeof (st));
    if (app->engine != NULL)
        ae_audio_engine_get_status (app->engine, &st);

    char cfg[8192] = "";
    config_json (app, cfg, sizeof (cfg));

    char in_name[2 * AE_NAME_MAX] = "", out_name[2 * AE_NAME_MAX] = "", err[2 * 256] = "";
    char ir_err[2 * 256] = "";
    char send_err_esc[2 * 128] = "";
    char follow_err_esc[2 * 128] = "";
    double follow_lvl = 1.0; /* app->lock is already held here */
    if (app->engine != NULL)
        follow_lvl = ae_audio_engine_follow_level (app->engine);
    char sample_err_esc[2 * 256] = "";
    ae_json_escape_append (in_name,  sizeof (in_name),  st.input_name);
    ae_json_escape_append (send_err_esc, sizeof (send_err_esc), app->send_err);
    ae_json_escape_append (follow_err_esc, sizeof (follow_err_esc), app->follow_err);
    ae_json_escape_append (sample_err_esc, sizeof (sample_err_esc), app->sample_err);
    ae_json_escape_append (out_name, sizeof (out_name), st.output_name);
    ae_json_escape_append (err,      sizeof (err),      app->engine_err);
    ae_json_escape_append (ir_err,   sizeof (ir_err),   app->ir_err);

    const double lat_ms = st.output_rate > 0.0
                            ? 1000.0 * st.latency_samples / st.output_rate : 0.0;
    /* Device overhead rides OUTSIDE processLatencyMs: that key is what
       the record send is aligned by (an in-engine tap, no hardware in
       its path), so it must not absorb hardware numbers. The panel's
       honest mic-to-speaker figure is totalLatencyMs. */
    const double dev_ms = st.output_rate > 0.0
                            ? 1000.0 * st.device_latency_samples / st.output_rate
                            : 0.0;

    /* live ghost degrees: JSON null when a voice is silent */
    char hdeg[128];
    size_t hn = 0;
    hn += (size_t) snprintf (hdeg + hn, sizeof (hdeg) - hn, "[");
    for (int v = 0; v < 5; ++v)
    {
        if (st.harm_deg[v] == INT_MIN || ! st.running)
            hn += (size_t) snprintf (hdeg + hn, sizeof (hdeg) - hn, "%snull", v ? "," : "");
        else
            hn += (size_t) snprintf (hdeg + hn, sizeof (hdeg) - hn, "%s%d", v ? "," : "", st.harm_deg[v]);
    }
    snprintf (hdeg + hn, sizeof (hdeg) - hn, "]");

    /* pitch trace: [[detected, target], ...] oldest first, with the absolute
       detection count so a consumer can stitch frames without duplicates. */
    char trace[AE_TRACE_MAX * 24 + 32];
    size_t tn = 0;
    tn += (size_t) snprintf (trace + tn, sizeof (trace) - tn, "[");
    for (int i = 0; i < st.trace_len && tn < sizeof (trace) - 26; ++i)
        tn += (size_t) snprintf (trace + tn, sizeof (trace) - tn, "%s[%.1f,%.1f]",
                                 i ? "," : "",
                                 (double) st.trace_det[i], (double) st.trace_tgt[i]);
    snprintf (trace + tn, sizeof (trace) - tn, "]");

    /* held MIDI notes as note numbers */
    char midi[512];
    size_t mn = 0;
    mn += (size_t) snprintf (midi + mn, sizeof (midi) - mn, "[");
    int mcount = 0;
    for (int n = 0; n < 128 && mn < sizeof (midi) - 8; ++n)
    {
        const bool on = n < 64 ? ((st.midi_held_lo >> n) & 1u) != 0
                               : ((st.midi_held_hi >> (n - 64)) & 1u) != 0;
        if (on)
            mn += (size_t) snprintf (midi + mn, sizeof (midi) - mn, "%s%d",
                                     mcount++ ? "," : "", n);
    }
    snprintf (midi + mn, sizeof (midi) - mn, "]");

    /* Instruments actually present in the cache -- same contract as the
       synth patch table: the picker is built from what is there. */
    char insts[AE_SMP_MAX_INSTRUMENTS * 34 + 4];
    size_t in_ = 0;
    in_ += (size_t) snprintf (insts + in_, sizeof (insts) - in_, "[");
    for (int i = 0; i < app->sample_inst_n && in_ < sizeof (insts) - 40; ++i)
        in_ += (size_t) snprintf (insts + in_, sizeof (insts) - in_, "%s\"%s\"",
                                  i ? "," : "", app->sample_insts[i]);
    snprintf (insts + in_, sizeof (insts) - in_, "]");

    /* Poly detection export: the tracker's live chord, one object per
       note. `note` is the same MIDI-style encoding the FOLLOW link uses
       (60 = engine degree 4*edo, one per degree, equave-folded), `hz` the
       raw tracked pitch, `cents` its offset from the snapped degree (a
       polyphonic tuner for free), `level` relative 0..1 (loudest ~ 1),
       `id` stable across the note's life so a reader can tell re-struck
       from continuing. Empty array outside poly mode. */
    _Static_assert (AE_POLY_STATUS_MAX == AE_POLY_MAX_NOTES,
                    "status export mirrors the tracker slot for slot");
    char polyd[AE_POLY_STATUS_MAX * 72 + 4];
    size_t pd = 0;
    pd += (size_t) snprintf (polyd + pd, sizeof (polyd) - pd, "[");
    {
        const AeLiveParams *lp = &app->engine_cfg.params;
        const double period = lp->period_cents > 0.0 ? lp->period_cents
                                                     : 1200.0;
        int pcount = 0;
        for (int k = 0; k < AE_POLY_STATUS_MAX && pd < sizeof (polyd) - 76; ++k)
        {
            const uint64_t w = st.poly_note[k];
            const double hz = (double) ae_poly_note_hz (w);
            if (hz <= 0.0)
                continue;
            const int deg = ae_poly_note_deg (w);
            const double snap_hz = ae_degree_hz (deg, lp->edo, lp->ref_hz,
                                                 period);
            pd += (size_t) snprintf (polyd + pd, sizeof (polyd) - pd,
                     "%s{\"note\":%d,\"hz\":%.2f,\"cents\":%.1f,"
                     "\"level\":%.2f,\"id\":%d}",
                     pcount++ ? "," : "",
                     ae_follow_encode_midi (deg, lp->edo), hz,
                     snap_hz > 0.0 ? 1200.0 * log2 (hz / snap_hz) : 0.0,
                     ae_poly_note_level (w), ae_poly_note_id (w));
        }
    }
    snprintf (polyd + pd, sizeof (polyd) - pd, "]");

    /* Patch names, so UIs build their picker from the engine's own table. */
    char patches[256];
    size_t pn = 0;
    pn += (size_t) snprintf (patches + pn, sizeof (patches) - pn, "[");
    for (int i = 0; i < ae_synth_patch_count() && pn < sizeof (patches) - 8; ++i)
        pn += (size_t) snprintf (patches + pn, sizeof (patches) - pn, "%s\"%s\"",
                                 i ? "," : "", ae_synth_patch_name (i));
    snprintf (patches + pn, sizeof (patches) - pn, "]");

    snprintf (buf, sizeof (buf),
        "{\"running\":%s,\"engineBuild\":\"%s\",\"error\":\"%s\","
        "\"inputRate\":%.6g,\"outputRate\":%.6g,"
        "\"latencySamples\":%d,\"latencyMs\":%.1f,\"processLatencyMs\":%.1f,"
        "\"deviceLatencyMs\":%.1f,\"totalLatencyMs\":%.1f,"
        "\"detectedHz\":%.4f,\"targetHz\":%.4f,\"shiftSt\":%.2f,\"shiftStMin\":%.2f,\"shiftStMax\":%.2f,"
        "\"leadMakeupDb\":%.2f,\"outPeakDb\":%.1f,\"voiced\":%s,"
        "\"traceSeq\":%u,\"trace\":%s,"
        "\"harmDeg\":%s,\"midiNotes\":%s,"
        "\"inputName\":\"%s\",\"outputName\":\"%s\","
        "\"shifter\":\"Signalsmith Stretch %s\",\"formantSupport\":%s,"
        "\"synthPatches\":%s,\"irError\":\"%s\",\"sendError\":\"%s\",\"sampleError\":\"%s\","
        "\"followError\":\"%s\",\"followNote\":%d,\"followLevelIn\":%.2f,"
        "\"sampleVelLast\":%.3f,\"sampleVelRefDb\":%.1f,\"sampleZones\":%d,\"sampleFiles\":%d,"
        "\"polyNotesActive\":%d,\"polyDetected\":%s,"
        "\"sampleInstruments\":%s,"
        "\"sampleNormDb\":%.1f,\"sampleOctaveApplied\":%d,\"sampleClipped\":%d,"
        "\"stepCents\":%.4f,\"config\":%s}",
        st.running ? "true" : "false", AE_BUILD_ID, err,
        st.input_rate, st.output_rate,
        st.latency_samples, lat_ms, lat_ms,
        dev_ms, lat_ms + dev_ms,
        (double) st.detected_hz, (double) st.target_hz, (double) st.shift_st,
        (double) st.shift_st_min, (double) st.shift_st_max,
        st.lead_makeup > 1e-6f ? 20.0 * log10 ((double) st.lead_makeup) : -120.0,
        st.out_peak    > 1e-6f ? 20.0 * log10 ((double) st.out_peak)    : -120.0,
        st.voiced ? "true" : "false",
        st.trace_seq, trace,
        hdeg, midi,
        in_name, out_name,
        ae_shifter_version(), ae_shifter_has_formant_support() ? "true" : "false",
        patches, ir_err, send_err_esc, sample_err_esc,
        follow_err_esc, atomic_load_explicit (&app->follow_note, memory_order_relaxed),
        follow_lvl,
        (double) st.sample_vel,
        st.sample_vel_ref > 1e-6f ? 20.0 * log10 ((double) st.sample_vel_ref) : -120.0,
        st.sample_zones, st.sample_files,
        st.poly_notes_live, polyd, insts,
        (double) st.sample_norm_db, st.sample_octave, st.sample_clipped,
        ae_edo_step_cents_ex (app->engine_cfg.params.edo,
                              app->engine_cfg.params.period_cents > 0.0
                                ? app->engine_cfg.params.period_cents : 1200.0),
        cfg);

    pthread_mutex_unlock (&app->lock);

    pthread_mutex_lock (&app->status_lock);
    snprintf (app->status_json, sizeof (app->status_json), "%s", buf);
    pthread_mutex_unlock (&app->status_lock);
}

static void api_status (App *app, AeHttpResponse *resp)
{
    resp->status       = 200;
    resp->content_type = "application/json";
    pthread_mutex_lock (&app->status_lock);
    ae_http_resp_printf (resp, "%s", app->status_json);
    pthread_mutex_unlock (&app->status_lock);
}

static void api_devices (App *app, AeHttpResponse *resp)
{
    (void) app;
    AeDeviceInfo *devs = NULL;
    int n = 0;
    if (ae_audio_list_devices (&devs, &n) != 0)
    {
        resp->status       = 500;
        resp->content_type = "application/json";
        ae_http_resp_printf (resp, "{\"error\":\"device enumeration failed\"}");
        return;
    }

    const size_t cap = 1024 + (size_t) n * (2 * AE_NAME_MAX + 2 * AE_UID_MAX + 128);
    char *body = malloc (cap);
    if (body == NULL)
    {
        free (devs);
        resp->status = 500;
        return;
    }
    body[0] = '\0';
    strncat (body, "{\"devices\":[", cap - 1);
    for (int i = 0; i < n; ++i)
    {
        char item[256];
        strncat (body, i > 0 ? ",{\"uid\":\"" : "{\"uid\":\"", cap - strlen (body) - 1);
        ae_json_escape_append (body, cap, devs[i].uid);
        strncat (body, "\",\"name\":\"", cap - strlen (body) - 1);
        ae_json_escape_append (body, cap, devs[i].name);
        snprintf (item, sizeof (item),
                  "\",\"inputs\":%d,\"outputs\":%d,\"rate\":%.6g,"
                  "\"defaultInput\":%s,\"defaultOutput\":%s}",
                  devs[i].input_channels, devs[i].output_channels, devs[i].nominal_rate,
                  devs[i].is_default_input ? "true" : "false",
                  devs[i].is_default_output ? "true" : "false");
        strncat (body, item, cap - strlen (body) - 1);
    }
    strncat (body, "],\"midiSources\":[", cap - strlen (body) - 1);
    char midi_names[16][AE_NAME_MAX];
    const int mn = ae_audio_list_midi_sources (midi_names, 16);
    for (int i = 0; i < mn; ++i)
    {
        strncat (body, i > 0 ? ",\"" : "\"", cap - strlen (body) - 1);
        ae_json_escape_append (body, cap, midi_names[i]);
        strncat (body, "\"", cap - strlen (body) - 1);
    }
    strncat (body, "]}", cap - strlen (body) - 1);
    free (devs);

    resp->status       = 200;
    resp->content_type = "application/json";
    resp->body         = body;
    resp->body_len     = strlen (body);
}

static void api_config_post (App *app, const char *body, AeHttpResponse *resp)
{
    pthread_mutex_lock (&app->lock);

    const bool restart = config_apply_json (app, body);
    config_sync (app);
    if (restart || app->engine == NULL)
        engine_restart_locked (app); /* reloads every configured IR too */
    else
    {
        ae_audio_engine_set_params (app->engine, &app->engine_cfg.params);
        ir_reload_locked (app, true); /* only what this edit dirtied */
        samples_reload_locked (app);
    }

    config_save (app);
    pthread_mutex_unlock (&app->lock);

    status_refresh (app); /* the POST echo must reflect the change now */
    api_status (app, resp);
}

static void handle_request (void *user, const char *method, const char *path,
                            const char *body, size_t body_len, AeHttpResponse *resp)
{
    (void) body_len;
    App *app = user;

    if (strcmp (method, "GET") == 0
        && (strcmp (path, "/") == 0 || strcmp (path, "/index.html") == 0))
    {
        resp->status       = 200;
        resp->content_type = "text/html; charset=utf-8";
        ae_http_resp_set (resp, web_index_html, web_index_html_len);
        return;
    }
    if (strcmp (method, "GET") == 0 && strcmp (path, "/api/status") == 0)
    {
        api_status (app, resp);
        return;
    }
    if (strcmp (method, "GET") == 0 && strcmp (path, "/api/devices") == 0)
    {
        api_devices (app, resp);
        return;
    }
    if (strcmp (method, "GET") == 0 && strcmp (path, "/api/scales") == 0)
    {
        resp->status       = 200;
        resp->content_type = "application/json";
        ae_http_resp_set (resp, web_scales_json, web_scales_json_len);
        return;
    }
    if (strcmp (method, "POST") == 0 && strcmp (path, "/api/config") == 0)
    {
        api_config_post (app, body, resp);
        return;
    }
    if (strcmp (method, "POST") == 0 && strcmp (path, "/api/midi") == 0)
    {
        /* Virtual held notes: {"notes":[60,64,67]}. Merged (OR) with any
           hardware MIDI input; also how tests drive MIDI Harmony. */
        double notes[32];
        const int n = ae_json_get_num_array (body, "notes", notes, 32);
        uint64_t lo = 0, hi = 0;
        for (int i = 0; i < n; ++i)
        {
            const int note = (int) notes[i];
            if (note >= 0 && note < 64)        lo |= 1ull << note;
            else if (note >= 64 && note < 128) hi |= 1ull << (note - 64);
        }
        pthread_mutex_lock (&app->lock);
        if (app->engine != NULL)
            ae_audio_engine_set_midi_notes (app->engine, lo, hi);
        pthread_mutex_unlock (&app->lock);
        status_refresh (app);
        api_status (app, resp);
        return;
    }
    if (strcmp (method, "POST") == 0 && strcmp (path, "/api/follow") == 0)
    {
        /* FOLLOW link input: {"note": n|-1, "level": 0..1}. Deliberately
           NOT /api/midi -- the virtual chord set belongs to the controller
           and a link must not fight it -- and deliberately no status
           rebuild: this arrives ~40x/s from another engine, and the reply
           is a receipt, not a snapshot. */
        double num = 0.0;
        int note = -1;
        double level = 1.0;
        if (ae_json_get_number (body, "note", &num))
            note = (int) num;
        if (ae_json_get_number (body, "level", &num))
            level = num < 0.0 ? 0.0 : (num > 1.0 ? 1.0 : num);
        pthread_mutex_lock (&app->lock);
        if (app->engine != NULL)
            ae_audio_engine_set_follow (app->engine, note, level);
        pthread_mutex_unlock (&app->lock);
        atomic_store_explicit (&app->follow_in_ms, mono_ms (),
                               memory_order_relaxed);
        ae_http_resp_printf (resp, "{\"ok\":true}");
        return;
    }
    if (strcmp (method, "POST") == 0 && strcmp (path, "/api/restart") == 0)
    {
        pthread_mutex_lock (&app->lock);
        engine_restart_locked (app);
        pthread_mutex_unlock (&app->lock);
        status_refresh (app);
        api_status (app, resp);
        return;
    }

    resp->status = 404;
    ae_http_resp_printf (resp, "not found");
}

/* -------------------------------------------------------------------- main */

/* ---------------------------------------------------------- follow link */



/* Sender thread: watch the lead's corrected degree and post changes to the
   followed instance's /api/midi as a one-note virtual chord. Note-grained
   (a few posts per second while playing), loopback-latency cheap, and
   entirely outside the audio thread. The receiver needs nothing new:
   midiMode:true + midiOctaves:"nearest" is the whole contract, and the
   engine that plays the note stays in charge of its own register. */
static void *follow_thread_fn (void *arg)
{
    App *app = arg;
    int       sent_note  = -2;   /* -2 = nothing ever sent, -1 = note-off */
    double    sent_level = -1.0;
    long long last_post  = 0;
    int       err_squelch = 0;

    while (! atomic_load_explicit (&app->follow_quit, memory_order_relaxed))
    {
        ae_engine_sleep_ms (3);

        char host[128];
        int  port, deg = AE_HARM_DEG_OFF, edo = 12;
        bool hold;
        double level = 0.0;
        pthread_mutex_lock (&app->lock);
        snprintf (host, sizeof (host), "%s", app->follow_host);
        port = app->follow_port;
        hold = app->follow_hold;
        if (app->engine != NULL)
        {
            deg   = ae_audio_engine_lead_degree (app->engine);
            edo   = app->engine_cfg.params.edo;
            level = ae_audio_engine_env (app->engine);
        }
        pthread_mutex_unlock (&app->lock);

        if (host[0] == '\0')
        {
            sent_note = -2; sent_level = -1.0;
            atomic_store_explicit (&app->follow_note, -1, memory_order_relaxed);
            continue;
        }

        int want_note;
        if (deg == AE_HARM_DEG_OFF)
            want_note = hold && sent_note >= 0 ? sent_note : -1;
        else
            want_note = ae_follow_encode_midi (deg, edo);

        /* Post on a note change, on the level moving audibly, and at
           least every 150 ms as the keepalive the receiver's TTL rides
           on. Level is quantized to 1% so the wire carries changes, not
           jitter. */
        const long long now = mono_ms ();
        const double    ql  = (double) ((int) (level * 100.0)) / 100.0;
        const bool note_moved  = want_note != sent_note;
        const bool level_moved = fabs (ql - sent_level) >= 0.01
                                 && now - last_post >= 20;
        const bool keepalive   = now - last_post >= 150;
        if (! note_moved && ! level_moved && ! keepalive)
            continue;

        char body[96];
        if (want_note < 0)
            snprintf (body, sizeof (body), "{\"note\":-1,\"level\":%.2f}", ql);
        else
            snprintf (body, sizeof (body), "{\"note\":%d,\"level\":%.2f}",
                      want_note, ql);

        if (ae_http_post_json (host, port, "/api/follow", body))
        {
            sent_note = want_note; sent_level = ql; last_post = now;
            atomic_store_explicit (&app->follow_note, want_note,
                                   memory_order_relaxed);
            if (app->follow_err[0] != '\0')
            {
                pthread_mutex_lock (&app->lock);
                app->follow_err[0] = '\0';
                pthread_mutex_unlock (&app->lock);
            }
            err_squelch = 0;
        }
        else if (err_squelch <= 0)
        {
            pthread_mutex_lock (&app->lock);
            snprintf (app->follow_err, sizeof (app->follow_err),
                      "follow: no engine answering at %.90s:%d", host, port);
            pthread_mutex_unlock (&app->lock);
            err_squelch = 700;
        }
        else
            --err_squelch;
    }
    /* A dead sender must not leave the follower pinned or cut: the
       receiver's TTL handles a crash, and this handles a clean exit. */
    if (app->follow_host[0] != '\0' && sent_note != -2)
        ae_http_post_json (app->follow_host, app->follow_port,
                           "/api/follow", "{\"note\":-1,\"level\":1}");
    return NULL;
}

int main (int argc, char **argv)
{
    App app;
    memset (&app, 0, sizeof (app));
    pthread_mutex_init (&app.lock, NULL);
    pthread_mutex_init (&app.status_lock, NULL);
    app.port = DEFAULT_PORT;
    config_defaults (&app);

    /* Flags before config_load: --config decides where to load from. */
    for (int i = 1; i < argc; ++i)
    {
        if (strcmp (argv[i], "--port") == 0 && i + 1 < argc)
            app.port = atoi (argv[++i]);
        else if (strcmp (argv[i], "--config") == 0 && i + 1 < argc)
            snprintf (app.config_file, sizeof (app.config_file), "%s", argv[++i]);
        else
        {
            fprintf (stderr, "usage: %s [--port N] [--config PATH]\n", argv[0]);
            return 2;
        }
    }

    config_load (&app);
    config_sync (&app);

    signal (SIGINT,  on_signal);
    signal (SIGTERM, on_signal);

    pthread_mutex_lock (&app.lock);
    engine_restart_locked (&app);
    pthread_mutex_unlock (&app.lock);
    /* An engine failure is not fatal: the web UI still comes up so the user
       can pick a working device. */

    app.follow_hold = true;
    atomic_store_explicit (&app.follow_note, -1, memory_order_relaxed);
    /* Re-arm the persisted followUrl (parse via the config path). */
    if (app.engine_cfg.follow_url[0] != '\0')
    {
        char buf2[192];
        snprintf (buf2, sizeof (buf2), "{\"followUrl\":\"%s\"}",
                  app.engine_cfg.follow_url);
        config_apply_json (&app, buf2);
    }
    atomic_store_explicit (&app.follow_quit, false, memory_order_relaxed);
    app.follow_thread_up =
        pthread_create (&app.follow_thread, NULL, follow_thread_fn, &app) == 0;

    status_refresh (&app); /* the cache must exist before the first GET */

    char err[256];
    AeHttpServer *server = ae_http_start (app.port, handle_request, &app,
                                          err, sizeof (err));
    if (server == NULL)
    {
        fprintf (stderr, "autoedo: web server failed: %s\n", err);
        pthread_mutex_lock (&app.lock);
        if (app.engine != NULL)
            ae_audio_engine_stop (app.engine);
        pthread_mutex_unlock (&app.lock);
        return 1;
    }

    printf ("AutoEDO Live — control UI at http://127.0.0.1:%d/\n", app.port);
    fflush (stdout);

    /* Pump: rebuild the status string and push it to every WebSocket client
       ~10x a second. The UI renders from this stream; it polls nothing. */
    while (! g_stop)
    {
        /* FOLLOW receiver TTL: a sender that died mid-note must not pin
           this instance's pitch or leave it cut. 400 ms of silence on the
           link returns it to neutral (the sender keeps alive at 150 ms). */
        {
            const long long seen = atomic_load_explicit (&app.follow_in_ms,
                                                         memory_order_relaxed);
            if (seen > 0 && mono_ms () - seen > 400)
            {
                pthread_mutex_lock (&app.lock);
                if (app.engine != NULL)
                    ae_audio_engine_set_follow (app.engine, -1, 1.0);
                pthread_mutex_unlock (&app.lock);
                atomic_store_explicit (&app.follow_in_ms, 0,
                                       memory_order_relaxed);
            }
        }
        status_refresh (&app);
        char frame[sizeof (app.status_json)];
        pthread_mutex_lock (&app.status_lock);
        const size_t n = strlen (app.status_json);
        memcpy (frame, app.status_json, n + 1);
        pthread_mutex_unlock (&app.status_lock);
        ae_http_ws_broadcast (server, frame, n);
        ae_sleep_ms (100);
    }

    printf ("autoedo: shutting down\n");
    ae_http_stop (server);
    pthread_mutex_lock (&app.lock);
    if (app.engine != NULL)
        ae_audio_engine_stop (app.engine);
    app.engine = NULL;
    pthread_mutex_unlock (&app.lock);
    return 0;
}
