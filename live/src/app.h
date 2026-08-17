/* The application core behind both faces of AutoEDO Live: the standalone
   process (main.c + a device backend) and the embedded library a host mixes
   in-process (embed.c + the embed backend). Everything that is not main()
   lives here -- config persistence and the JSON grammar, the HTTP/WS control
   server, the status cache, the FOLLOW link and the tap sender, and the
   engine lifecycle -- so the two faces cannot drift apart. */

#ifndef AUTOEDO_APP_H
#define AUTOEDO_APP_H

#include <stdbool.h>
#include <stddef.h>

typedef struct AeApp AeApp;
typedef struct AeAudioEngine AeAudioEngine;

typedef struct
{
    int         http_port;   /* > 0 = serve the control UI/API on 127.0.0.1;
                                0 = no server (a host that only wants audio) */
    const char *config_file; /* per-instance persisted config; NULL or "" =
                                ~/.autoedo.json */
    double      embed_rate;  /* embed backend only: the host's sample rate
                                (0 = the backend default). Device backends
                                ignore both of these. */
    int         embed_block; /* embed backend only: the host's max block */
} AeAppOptions;

/* Bring the whole app up: config load, engine start, FOLLOW/tap/pump
   threads, and (port > 0) the HTTP server. An engine that cannot start is
   NOT fatal -- the control surface still comes up so the user can fix the
   device -- but an HTTP port that cannot bind is reported through
   ae_app_http_running() and the caller decides. NULL only on allocation
   failure, with a reason in err. */
AeApp *ae_app_create (const AeAppOptions *opt, char *err, size_t err_len);

/* Stop the server, the worker threads and the engine, then free. */
void ae_app_destroy (AeApp *app);

bool ae_app_http_running (const AeApp *app);

/* Apply a config JSON body -- exactly POST /api/config without the HTTP.
   Control thread; may restart the engine. */
void ae_app_post_config (AeApp *app, const char *json);

/* Copy the cached status JSON (the GET /api/status body). Returns its
   length, truncated to cap - 1. */
int ae_app_status (AeApp *app, char *out, size_t cap);

/* The running engine, lock-free -- what an embedding host's AUDIO thread
   processes through. NULL while stopped or mid-restart; the app clears it
   and lets a block turn over before tearing an engine down, so a pointer
   read here stays valid for the duration of one process call. */
AeAudioEngine *ae_app_engine_live (AeApp *app);

/* The configured 1-based live output channel (0 = stereo on 1-2): where an
   embedding host should land this instance's PA feed, same semantics as the
   standalone engine's own device routing. */
int ae_app_output_channel (AeApp *app);

#endif /* AUTOEDO_APP_H */
