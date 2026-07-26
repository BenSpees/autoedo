/* Tiny embedded HTTP server (POSIX sockets, one worker thread) that hosts
   the configuration web UI and its JSON API. Binds to 127.0.0.1 only. */

#ifndef AUTOEDO_HTTPD_H
#define AUTOEDO_HTTPD_H

#include <stddef.h>

typedef struct
{
    int         status;        /* e.g. 200, 404 */
    const char *content_type;  /* e.g. "application/json" */
    char       *body;          /* malloc'd; the server frees it after sending */
    size_t      body_len;
} AeHttpResponse;

/* Convenience: printf into resp->body (replacing any previous body). */
void ae_http_resp_printf (AeHttpResponse *resp, const char *fmt, ...);

/* Convenience: set a static body (copied). */
void ae_http_resp_set (AeHttpResponse *resp, const void *data, size_t len);

/* Handler fills `resp`. `body` is NUL-terminated (bodies are text/JSON). */
typedef void (*AeHttpHandler) (void *user, const char *method, const char *path,
                               const char *body, size_t body_len,
                               AeHttpResponse *resp);

typedef struct AeHttpServer AeHttpServer;

/* Start serving on 127.0.0.1:port. Returns NULL on failure (err filled). */
AeHttpServer *ae_http_start (int port, AeHttpHandler handler, void *user,
                             char *err, size_t err_len);

void ae_http_stop (AeHttpServer *s);

#endif /* AUTOEDO_HTTPD_H */
