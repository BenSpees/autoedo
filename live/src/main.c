/* AutoEDO Live — the standalone process: argument parsing and a signal
   loop around the application core (app.h). Everything real -- the JSON
   API, the web UI, the engine lifecycle -- lives in app.c, shared with
   the embedded library build (embed.h). */

#include "app.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  static void main_sleep_ms (int ms) { Sleep ((DWORD) ms); }
#else
  #include <unistd.h>
  static void main_sleep_ms (int ms) { usleep ((useconds_t) ms * 1000); }
#endif

#define DEFAULT_PORT 8017

static volatile sig_atomic_t g_stop = 0;

static void on_signal (int sig)
{
    (void) sig;
    g_stop = 1;
}

int main (int argc, char **argv)
{
    AeAppOptions opt;
    memset (&opt, 0, sizeof (opt));
    opt.http_port = DEFAULT_PORT;

    for (int i = 1; i < argc; ++i)
    {
        if (strcmp (argv[i], "--port") == 0 && i + 1 < argc)
            opt.http_port = atoi (argv[++i]);
        else if (strcmp (argv[i], "--config") == 0 && i + 1 < argc)
            opt.config_file = argv[++i];
        else
        {
            fprintf (stderr, "usage: %s [--port N] [--config PATH]\n", argv[0]);
            return 2;
        }
    }

    signal (SIGINT,  on_signal);
    signal (SIGTERM, on_signal);

    char err[256];
    AeApp *app = ae_app_create (&opt, err, sizeof (err));
    if (app == NULL)
    {
        fprintf (stderr, "autoedo: %s\n", err);
        return 1;
    }
    if (! ae_app_http_running (app))
    {
        /* Standalone, the web UI IS the product: a port that cannot bind is
           a launch failure, not a degraded mode. */
        ae_app_destroy (app);
        return 1;
    }

    printf ("AutoEDO Live — control UI at http://127.0.0.1:%d/\n", opt.http_port);
    fflush (stdout);

    while (! g_stop)
        main_sleep_ms (100);

    printf ("autoedo: shutting down\n");
    ae_app_destroy (app);
    return 0;
}
