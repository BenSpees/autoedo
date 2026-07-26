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
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "web_index.h" /* generated: web_index_html[], web_index_html_len */

#define DEFAULT_PORT 8017

typedef struct
{
    AeEngineConfig  engine_cfg;
    int             port;
    pthread_mutex_t lock;      /* guards engine + engine_cfg */
    AeAudioEngine  *engine;
    char            engine_err[256];
} App;

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

static void config_defaults (AeEngineConfig *c)
{
    memset (c, 0, sizeof (*c));
    c->buffer_frames          = 256;
    c->params.edo             = 12;
    c->params.retune_ms       = 20.0;
    c->params.degrees_lo      = ~0ull;   /* every degree enabled */
    c->params.degrees_hi      = 0xffull;
    c->params.bypass          = false;
    c->params.output_gain_db  = 0.0;
}

/* Serialise just the config (shared between the save file and /api/status). */
static void config_json (const AeEngineConfig *c, char *out, size_t cap)
{
    snprintf (out, cap,
              "{\"edo\":%d,\"retuneMs\":%.6g,\"bypass\":%s,\"outputGainDb\":%.6g,"
              "\"bufferFrames\":%d,\"inputUid\":\"",
              c->params.edo, c->params.retune_ms,
              c->params.bypass ? "true" : "false",
              c->params.output_gain_db, c->buffer_frames);
    ae_json_escape_append (out, cap, c->input_uid);
    strncat (out, "\",\"outputUid\":\"", cap - strlen (out) - 1);
    ae_json_escape_append (out, cap, c->output_uid);
    strncat (out, "\",\"degrees\":[", cap - strlen (out) - 1);
    for (int d = 0; d < AE_MAX_EDO; ++d)
    {
        const bool on = d < 64 ? ((c->params.degrees_lo >> d) & 1u) != 0
                               : ((c->params.degrees_hi >> (d - 64)) & 1u) != 0;
        char item[4];
        snprintf (item, sizeof (item), "%s%d", d > 0 ? "," : "", on ? 1 : 0);
        strncat (out, item, cap - strlen (out) - 1);
    }
    strncat (out, "]}", cap - strlen (out) - 1);
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
    config_json (&app->engine_cfg, body, sizeof (body));
    fprintf (f, "%s\n", body);
    fclose (f);
}

/* Apply any known keys found in `json` onto `c`. Returns true if a setting
   that needs an engine restart (devices / buffer size) changed. */
static bool config_apply_json (AeEngineConfig *c, const char *json)
{
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
        c->params.retune_ms = num < 0.0 ? 0.0 : (num > 500.0 ? 500.0 : num);
    if (ae_json_get_bool (json, "bypass", &b))
        c->params.bypass = b;
    if (ae_json_get_number (json, "outputGainDb", &num))
        c->params.output_gain_db = num < -60.0 ? -60.0 : (num > 12.0 ? 12.0 : num);

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
    config_apply_json (&app->engine_cfg, buf);
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

static void api_status (App *app, AeHttpResponse *resp)
{
    pthread_mutex_lock (&app->lock);

    AeEngineStatus st;
    memset (&st, 0, sizeof (st));
    if (app->engine != NULL)
        ae_audio_engine_get_status (app->engine, &st);

    char cfg[4096] = "";
    config_json (&app->engine_cfg, cfg, sizeof (cfg));

    char in_name[2 * AE_NAME_MAX] = "", out_name[2 * AE_NAME_MAX] = "", err[2 * 256] = "";
    ae_json_escape_append (in_name,  sizeof (in_name),  st.input_name);
    ae_json_escape_append (out_name, sizeof (out_name), st.output_name);
    ae_json_escape_append (err,      sizeof (err),      app->engine_err);

    const double lat_ms = st.output_rate > 0.0
                            ? 1000.0 * st.latency_samples / st.output_rate : 0.0;

    resp->status       = 200;
    resp->content_type = "application/json";
    ae_http_resp_printf (resp,
        "{\"running\":%s,\"error\":\"%s\","
        "\"inputRate\":%.6g,\"outputRate\":%.6g,"
        "\"latencySamples\":%d,\"latencyMs\":%.1f,"
        "\"detectedHz\":%.4f,\"targetHz\":%.4f,\"voiced\":%s,"
        "\"inputName\":\"%s\",\"outputName\":\"%s\","
        "\"stepCents\":%.4f,\"config\":%s}",
        st.running ? "true" : "false", err,
        st.input_rate, st.output_rate,
        st.latency_samples, lat_ms,
        (double) st.detected_hz, (double) st.target_hz, st.voiced ? "true" : "false",
        in_name, out_name,
        ae_edo_step_cents (app->engine_cfg.params.edo), cfg);

    pthread_mutex_unlock (&app->lock);
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

    const bool restart = config_apply_json (&app->engine_cfg, body);
    if (restart || app->engine == NULL)
        engine_restart_locked (app);
    else if (app->engine != NULL)
        ae_audio_engine_set_params (app->engine, &app->engine_cfg.params);

    config_save (app);
    pthread_mutex_unlock (&app->lock);

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
    if (strcmp (method, "POST") == 0 && strcmp (path, "/api/config") == 0)
    {
        api_config_post (app, body, resp);
        return;
    }
    if (strcmp (method, "POST") == 0 && strcmp (path, "/api/restart") == 0)
    {
        pthread_mutex_lock (&app->lock);
        engine_restart_locked (app);
        pthread_mutex_unlock (&app->lock);
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
    app.port = DEFAULT_PORT;
    config_defaults (&app.engine_cfg);
    config_load (&app);

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

    while (! g_stop)
        usleep (200 * 1000);

    printf ("autoedo: shutting down\n");
    ae_http_stop (server);
    pthread_mutex_lock (&app.lock);
    if (app.engine != NULL)
        ae_audio_engine_stop (app.engine);
    app.engine = NULL;
    pthread_mutex_unlock (&app.lock);
    return 0;
}
