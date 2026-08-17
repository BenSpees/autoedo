/* AutoEDO Live as a library (Treebrain record-send ask B2): the whole
   engine -- corrector, harmony, sampler, IRs, the JSON config grammar and
   optionally its own HTTP/WS control server -- hosted inside another
   process, fed float32 blocks instead of a device.

   One instance = one engine = one mono channel (a voice/guitar rig makes
   two, exactly like two standalone processes). The host owns the audio
   device; the instance owns everything the standalone engine owned except
   the jacks.

   Threading:
     - create/destroy/config/status: control thread(s).
     - process: exactly one audio thread. Lock-free against config writes;
       a config that forces an engine rebuild swaps engines under a
       one-block grace, during which process passes the input through dry.

   Config is the SAME JSON grammar as POST /api/config, persisted to the
   same per-instance file, and when http_port > 0 the same web UI and API
   are served from inside the host -- so an existing controller (or a
   browser) cannot tell the engine moved in. Feature-detect: the status
   echo carries "embedded":true. */

#ifndef AUTOEDO_EMBED_H
#define AUTOEDO_EMBED_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AeEmbed AeEmbed;

typedef struct
{
    double      sample_rate; /* host rate; 0 = 48000 */
    int         max_block;   /* host max block frames; 0 = 512 */
    int         http_port;   /* > 0 = serve the control UI/API on
                                127.0.0.1:<port> (8017/8018 by rig
                                convention); 0 = audio only */
    const char *config_file; /* persisted config path for THIS instance;
                                NULL = ~/.autoedo.json */
} AeEmbedOptions;

/* Bring an instance up: config load, engine start, worker threads, and the
   HTTP server when asked for. NULL only on allocation failure (err filled);
   an engine or server that cannot start is reported through the status
   JSON instead, exactly like the standalone process. */
AeEmbed *ae_embed_create (const AeEmbedOptions *opt, char *err, size_t err_len);

void ae_embed_destroy (AeEmbed *inst);

/* AUDIO THREAD. Run `n` mono input frames through the instance.
     out_l/out_r  the live PA feed (what the standalone engine's output
                  jack carried), bypassOutput semantics included; route it
                  per ae_embed_output_channel().
     chain        the feed for the host's own downstream chain (looper /
                  FX / spaces): the live mono mix, except bypass ALWAYS
                  passes the dry input at unity -- a PA-side mute must
                  never silence the looper's source.
   Any output may be NULL. While no engine runs (failed start, mid-restart)
   the outputs go silent and `chain` carries the dry input, so the host's
   chain never loses its instrument. Returns n. */
int ae_embed_process (AeEmbed *inst, const float *in, float *out_l,
                      float *out_r, float *chain, int n);

/* CONTROL THREAD. Apply a config body -- the same JSON grammar, clamping
   and restart rules as POST /api/config. */
void ae_embed_config (AeEmbed *inst, const char *json);

/* CONTROL THREAD. Copy the current status JSON (the GET /api/status body,
   config echo included). Returns the copied length. */
int ae_embed_status (AeEmbed *inst, char *out, size_t cap);

/* The instance's configured 1-based live output channel (0 = stereo on
   host channels 1-2): where the host should land out_l/out_r, same
   semantics as the standalone engine's outputChannel device routing (a
   single named channel takes the mono fold). */
int ae_embed_output_channel (AeEmbed *inst);

/* Whether the in-process control server is actually listening (false when
   http_port was 0, or the bind failed -- e.g. a standalone engine already
   owns the port). */
int ae_embed_http_running (AeEmbed *inst);

#ifdef __cplusplus
}
#endif

#endif /* AUTOEDO_EMBED_H */
