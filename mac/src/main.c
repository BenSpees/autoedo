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

   Config persists to ~/.autoedo.json. */

#include "audio.h"
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
#include <unistd.h>

#include "web_index.h"  /* generated: web_index_html[], web_index_html_len */
#include "web_scales.h" /* generated: web_scales_json[], web_scales_json_len */

#define DEFAULT_PORT 8017

typedef struct
{
    AeEngineConfig  engine_cfg;
    /* UI-facing tuning-reference fields; params.ref_hz / period_cents and
       the detection range are derived from these in config_sync(). */
    int             root_note;     /* 0..11, 0 = C (degree 0 of the grid) */
    double          root_cents;    /* fine offset, -50..+50 */
    double          ref_a4;        /* reference A4, default 440.0 */
    double          stretch_cents; /* octave stretch, cents per octave */
    char            range_name[16];/* detection range preset */
    char            scale_cat[64]; /* last-loaded catalog scale (cosmetic; */
    char            scale_name[64];/* the mask itself is the real state)   */
    int             port;
    pthread_mutex_t lock;      /* guards engine + engine_cfg */
    AeAudioEngine  *engine;
    char            engine_err[256];

    /* Status is serialized once per pump tick (or config change) and every
       consumer — each WebSocket push and each /api/status GET — is handed
       the same cached string. */
    pthread_mutex_t status_lock;
    char            status_json[8192];
} App;

/* Detection-range presets (min/max Hz of the tracking window). */
static const struct { const char *name; double min_hz, max_hz; } k_ranges[] = {
    { "bass",       55.0,  400.0 },
    { "baritone",   65.0,  450.0 },
    { "tenor",      80.0,  600.0 },
    { "alto",      100.0,  800.0 },
    { "soprano",   130.0, 1200.0 },
    { "instrument", 65.0, 1600.0 },
    { "wide",       40.0, 2000.0 },
};

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

static void config_path (char *out, size_t cap)
{
    const char *home = getenv ("HOME");
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
    c->params.degrees_lo      = ~0ull;   /* every degree enabled */
    c->params.degrees_hi      = 0xffull;
    c->params.bypass          = false;
    c->params.output_gain_db  = 0.0;

    app->root_note     = 0;      /* C */
    app->root_cents    = 0.0;
    app->ref_a4        = 440.0;
    app->stretch_cents = 0.0;
    snprintf (app->range_name, sizeof (app->range_name), "instrument");
    app->scale_cat[0] = app->scale_name[0] = '\0';

    c->params.harm_on   = false;
    c->params.harm_lock = 1; /* Mask — the headline behavior */
    for (int v = 0; v < 5; ++v)
    {
        c->params.harm_interval[v] = 0;
        c->params.harm_ext[v]      = 0;
        c->params.harm_gain_db[v]  = 0.0;
        c->params.harm_pan[v]      = 0.0;
    }
    c->params.harm_mute = 0;
    c->params.harm_solo = 0;
    c->params.midi_mode = false;
    c->midi_source[0]   = '\0'; /* all MIDI inputs */
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
}

/* Serialise just the config (shared between the save file and /api/status). */
static void config_json (const App *app, char *out, size_t cap)
{
    const AeEngineConfig *c = &app->engine_cfg;
    snprintf (out, cap,
              "{\"edo\":%d,\"retuneMs\":%.6g,\"transitionMs\":%.6g,"
              "\"amount\":%.6g,\"toleranceCents\":%.6g,\"stickiness\":%.6g,"
              "\"humanize\":%.6g,"
              "\"rootNote\":%d,\"rootCents\":%.6g,\"refA4\":%.6g,"
              "\"stretchCents\":%.6g,\"range\":\"%s\","
              "\"bypass\":%s,\"outputGainDb\":%.6g,"
              "\"bufferFrames\":%d,\"inputUid\":\"",
              c->params.edo, c->params.retune_ms, c->params.transition_ms,
              c->params.amount, c->params.tolerance_cents, c->params.stickiness,
              c->params.humanize,
              app->root_note, app->root_cents, app->ref_a4,
              app->stretch_cents, app->range_name,
              c->params.bypass ? "true" : "false",
              c->params.output_gain_db, c->buffer_frames);
    ae_json_escape_append (out, cap, c->input_uid);
    strncat (out, "\",\"outputUid\":\"", cap - strlen (out) - 1);
    ae_json_escape_append (out, cap, c->output_uid);
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
    char harm[512];
    static const char *lock_names[] = { "off", "mask", "ji" };
    snprintf (harm, sizeof (harm), ",\"harmOn\":%s,\"harmLock\":\"%s\",\"midiMode\":%s,\"midiSource\":\"",
              c->params.harm_on ? "true" : "false",
              lock_names[c->params.harm_lock >= 0 && c->params.harm_lock <= 2
                           ? c->params.harm_lock : 0],
              c->params.midi_mode ? "true" : "false");
    strncat (out, harm, cap - strlen (out) - 1);
    ae_json_escape_append (out, cap, c->midi_source);
    strncat (out, "\"", cap - strlen (out) - 1);
    const char *keys[4] = { "hm", "hx", "hg", "hp" };
    for (int k = 0; k < 4; ++k)
    {
        size_t n = 0;
        n += (size_t) snprintf (harm + n, sizeof (harm) - n, ",\"%s\":[", keys[k]);
        for (int v = 0; v < 5; ++v)
        {
            if (k == 0)      n += (size_t) snprintf (harm + n, sizeof (harm) - n, "%s%d",  v ? "," : "", c->params.harm_interval[v]);
            else if (k == 1) n += (size_t) snprintf (harm + n, sizeof (harm) - n, "%s%d",  v ? "," : "", c->params.harm_ext[v]);
            else if (k == 2) n += (size_t) snprintf (harm + n, sizeof (harm) - n, "%s%.4g", v ? "," : "", c->params.harm_gain_db[v]);
            else             n += (size_t) snprintf (harm + n, sizeof (harm) - n, "%s%.4g", v ? "," : "", c->params.harm_pan[v]);
        }
        snprintf (harm + n, sizeof (harm) - n, "]");
        strncat (out, harm, cap - strlen (out) - 1);
    }
    snprintf (harm, sizeof (harm),
              ",\"hMute\":[%d,%d,%d,%d,%d],\"hSolo\":[%d,%d,%d,%d,%d]}",
              (c->params.harm_mute >> 0) & 1, (c->params.harm_mute >> 1) & 1,
              (c->params.harm_mute >> 2) & 1, (c->params.harm_mute >> 3) & 1,
              (c->params.harm_mute >> 4) & 1,
              (c->params.harm_solo >> 0) & 1, (c->params.harm_solo >> 1) & 1,
              (c->params.harm_solo >> 2) & 1, (c->params.harm_solo >> 3) & 1,
              (c->params.harm_solo >> 4) & 1);
    strncat (out, harm, cap - strlen (out) - 1);
}

static void config_save (const App *app)
{
    char path[1024];
    config_path (path, sizeof (path));
    FILE *f = fopen (path, "w");
    if (f == NULL)
    {
        fprintf (stderr, "autoedo: cannot write %s: %s\n", path, strerror (errno));
        return;
    }
    char body[4096] = "";
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
    if (ae_json_get_number (json, "humanize", &num))
        c->params.humanize = num_clamp (num, 0.0, 1.0);
    if (ae_json_get_number (json, "rootNote", &num))
        app->root_note = (int) num_clamp (num, 0.0, 11.0);
    if (ae_json_get_number (json, "rootCents", &num))
        app->root_cents = num_clamp (num, -50.0, 50.0);
    if (ae_json_get_number (json, "refA4", &num))
        app->ref_a4 = num_clamp (num, 400.0, 480.0);
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
    if (ae_json_get_bool (json, "bypass", &b))
        c->params.bypass = b;
    if (ae_json_get_number (json, "outputGainDb", &num))
        c->params.output_gain_db = num_clamp (num, -60.0, 12.0);

    /* Harmony. */
    if (ae_json_get_bool (json, "harmOn", &b))
        c->params.harm_on = b;
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
            c->params.harm_gain_db[v] = num_clamp (arr[v], -60.0, 6.0);
    if ((n5 = ae_json_get_num_array (json, "hp", arr, 5)) >= 0)
        for (int v = 0; v < n5; ++v)
            c->params.harm_pan[v] = num_clamp (arr[v], -1.0, 1.0);
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
    config_path (path, sizeof (path));
    FILE *f = fopen (path, "r");
    if (f == NULL)
        return;
    char buf[8192];
    const size_t n = fread (buf, 1, sizeof (buf) - 1, f);
    buf[n] = '\0';
    fclose (f);
    config_apply_json (app, buf);
}

/* ------------------------------------------------------------------ engine */

/* Caller must hold app->lock. */
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
}

/* --------------------------------------------------------------------- api */

/* Serialize the full status once into the cache (Rule 2 of the smooth-UI
   pattern: one serialization per tick, shared by all consumers). */
static void status_refresh (App *app)
{
    char buf[8192];

    pthread_mutex_lock (&app->lock);

    AeEngineStatus st;
    memset (&st, 0, sizeof (st));
    if (app->engine != NULL)
        ae_audio_engine_get_status (app->engine, &st);

    char cfg[4096] = "";
    config_json (app, cfg, sizeof (cfg));

    char in_name[2 * AE_NAME_MAX] = "", out_name[2 * AE_NAME_MAX] = "", err[2 * 256] = "";
    ae_json_escape_append (in_name,  sizeof (in_name),  st.input_name);
    ae_json_escape_append (out_name, sizeof (out_name), st.output_name);
    ae_json_escape_append (err,      sizeof (err),      app->engine_err);

    const double lat_ms = st.output_rate > 0.0
                            ? 1000.0 * st.latency_samples / st.output_rate : 0.0;

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

    snprintf (buf, sizeof (buf),
        "{\"running\":%s,\"error\":\"%s\","
        "\"inputRate\":%.6g,\"outputRate\":%.6g,"
        "\"latencySamples\":%d,\"latencyMs\":%.1f,"
        "\"detectedHz\":%.4f,\"targetHz\":%.4f,\"voiced\":%s,"
        "\"harmDeg\":%s,\"midiNotes\":%s,"
        "\"inputName\":\"%s\",\"outputName\":\"%s\","
        "\"stepCents\":%.4f,\"config\":%s}",
        st.running ? "true" : "false", err,
        st.input_rate, st.output_rate,
        st.latency_samples, lat_ms,
        (double) st.detected_hz, (double) st.target_hz, st.voiced ? "true" : "false",
        hdeg, midi,
        in_name, out_name,
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
        engine_restart_locked (app);
    else if (app->engine != NULL)
        ae_audio_engine_set_params (app->engine, &app->engine_cfg.params);

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

int main (int argc, char **argv)
{
    App app;
    memset (&app, 0, sizeof (app));
    pthread_mutex_init (&app.lock, NULL);
    pthread_mutex_init (&app.status_lock, NULL);
    app.port = DEFAULT_PORT;
    config_defaults (&app);
    config_load (&app);
    config_sync (&app);

    for (int i = 1; i < argc; ++i)
    {
        if (strcmp (argv[i], "--port") == 0 && i + 1 < argc)
            app.port = atoi (argv[++i]);
        else
        {
            fprintf (stderr, "usage: %s [--port N]\n", argv[0]);
            return 2;
        }
    }

    signal (SIGINT,  on_signal);
    signal (SIGTERM, on_signal);

    pthread_mutex_lock (&app.lock);
    engine_restart_locked (&app);
    pthread_mutex_unlock (&app.lock);
    /* An engine failure is not fatal: the web UI still comes up so the user
       can pick a working device. */

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
        status_refresh (&app);
        char frame[sizeof (app.status_json)];
        pthread_mutex_lock (&app.status_lock);
        const size_t n = strlen (app.status_json);
        memcpy (frame, app.status_json, n + 1);
        pthread_mutex_unlock (&app.status_lock);
        ae_http_ws_broadcast (server, frame, n);
        usleep (100 * 1000);
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
