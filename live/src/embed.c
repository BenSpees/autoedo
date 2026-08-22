/* The embedded library face (embed.h): a thin shell over the application
   core (app.h) and the embed audio backend (audio_embed.c). Everything of
   substance lives in those two -- this file only adapts the surface. */

#include "embed.h"
#include "app.h"
#include "audio.h"

#include <stdlib.h>
#include <string.h>

struct AeEmbed
{
    AeApp *app;
};

AeEmbed *ae_embed_create (const AeEmbedOptions *opt, char *err, size_t err_len)
{
    AeEmbed *inst = calloc (1, sizeof (*inst));
    if (inst == NULL)
    {
        if (err_len > 0)
        {
            strncpy (err, "out of memory", err_len - 1);
            err[err_len - 1] = '\0';
        }
        return NULL;
    }
    AeAppOptions ao;
    memset (&ao, 0, sizeof (ao));
    ao.http_port   = opt->http_port;
    ao.config_file = opt->config_file;
    ao.embed_rate  = opt->sample_rate;
    ao.embed_block = opt->max_block;
    inst->app = ae_app_create (&ao, err, err_len);
    if (inst->app == NULL)
    {
        free (inst);
        return NULL;
    }
    return inst;
}

void ae_embed_destroy (AeEmbed *inst)
{
    if (inst == NULL)
        return;
    ae_app_destroy (inst->app);
    free (inst);
}

int ae_embed_process (AeEmbed *inst, const float *in, float *out_l,
                      float *out_r, float *chain, int n)
{
    return ae_embed_process_det (inst, in, NULL, out_l, out_r, chain, n);
}

int ae_embed_process_det (AeEmbed *inst, const float *in, const float *det,
                          float *out_l, float *out_r, float *chain, int n)
{
    if (inst == NULL || in == NULL || n <= 0)
        return 0;
    AeAudioEngine *e = ae_app_engine_live (inst->app);
    if (e != NULL)
        return ae_embed_engine_process_det (e, in, det, out_l, out_r, chain, n);
    /* No engine (failed start, mid-restart): the PA feed goes silent, the
       chain keeps its instrument. */
    if (out_l != NULL) memset (out_l, 0, (size_t) n * sizeof (float));
    if (out_r != NULL) memset (out_r, 0, (size_t) n * sizeof (float));
    if (chain != NULL) memcpy (chain, in, (size_t) n * sizeof (float));
    return n;
}

void ae_embed_config (AeEmbed *inst, const char *json)
{
    if (inst != NULL && json != NULL)
        ae_app_post_config (inst->app, json);
}

int ae_embed_status (AeEmbed *inst, char *out, size_t cap)
{
    if (inst == NULL)
        return 0;
    return ae_app_status (inst->app, out, cap);
}

int ae_embed_output_channel (AeEmbed *inst)
{
    return inst == NULL ? 0 : ae_app_output_channel (inst->app);
}

int ae_embed_http_running (AeEmbed *inst)
{
    return inst != NULL && ae_app_http_running (inst->app) ? 1 : 0;
}
