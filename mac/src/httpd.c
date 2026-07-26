#include "httpd.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL /* macOS: SIGPIPE is globally ignored instead */
#define MSG_NOSIGNAL 0
#endif

#define MAX_HEADER 8192
#define MAX_BODY   (1 << 20)
#define MAX_WS     16

struct AeHttpServer
{
    int           listen_fd;
    pthread_t     thread;
    AeHttpHandler handler;
    void         *user;
    volatile bool stopping;

    /* WebSocket clients (GET /ws connections hijacked from the HTTP path). */
    pthread_mutex_t ws_lock;
    int             ws_fds[MAX_WS];
    int             ws_count;
};

/* ---- SHA-1 + base64, just enough for the WebSocket handshake ---------- */

static void sha1_digest (const unsigned char *msg, size_t len, unsigned char out[20])
{
    uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE,
             h3 = 0x10325476, h4 = 0xC3D2E1F0;

    const size_t total = ((len + 8) / 64 + 1) * 64;
    unsigned char *buf = calloc (total, 1);
    if (buf == NULL)
        return;
    memcpy (buf, msg, len);
    buf[len] = 0x80;
    const uint64_t bits = (uint64_t) len * 8;
    for (int i = 0; i < 8; ++i)
        buf[total - 1 - i] = (unsigned char) (bits >> (8 * i));

    for (size_t off = 0; off < total; off += 64)
    {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = ((uint32_t) buf[off + 4 * i] << 24)
                 | ((uint32_t) buf[off + 4 * i + 1] << 16)
                 | ((uint32_t) buf[off + 4 * i + 2] << 8)
                 |  (uint32_t) buf[off + 4 * i + 3];
        for (int i = 16; i < 80; ++i)
        {
            const uint32_t v = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
            w[i] = (v << 1) | (v >> 31);
        }
        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; ++i)
        {
            uint32_t f, k;
            if (i < 20)      { f = (b & c) | (~b & d);           k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d;                    k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d);  k = 0x8F1BBCDC; }
            else             { f = b ^ c ^ d;                    k = 0xCA62C1D6; }
            const uint32_t t = ((a << 5) | (a >> 27)) + f + e + k + w[i];
            e = d; d = c; c = (b << 30) | (b >> 2); b = a; a = t;
        }
        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }
    free (buf);

    const uint32_t hs[5] = { h0, h1, h2, h3, h4 };
    for (int i = 0; i < 5; ++i)
    {
        out[4 * i]     = (unsigned char) (hs[i] >> 24);
        out[4 * i + 1] = (unsigned char) (hs[i] >> 16);
        out[4 * i + 2] = (unsigned char) (hs[i] >> 8);
        out[4 * i + 3] = (unsigned char)  hs[i];
    }
}

static void b64_encode (const unsigned char *in, size_t n, char *out)
{
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3)
    {
        const unsigned b0 = in[i];
        const unsigned b1 = i + 1 < n ? in[i + 1] : 0;
        const unsigned b2 = i + 2 < n ? in[i + 2] : 0;
        out[o++] = tbl[b0 >> 2];
        out[o++] = tbl[((b0 & 3) << 4) | (b1 >> 4)];
        out[o++] = i + 1 < n ? tbl[((b1 & 15) << 2) | (b2 >> 6)] : '=';
        out[o++] = i + 2 < n ? tbl[b2 & 63] : '=';
    }
    out[o] = '\0';
}

/* Case-insensitive header lookup within [hdrs, end). */
static bool header_value (const char *hdrs, const char *end, const char *name,
                          char *out, size_t cap)
{
    const size_t nlen = strlen (name);
    for (const char *p = hdrs; p + nlen + 1 < end; )
    {
        if (strncasecmp (p, name, nlen) == 0 && p[nlen] == ':')
        {
            const char *v = p + nlen + 1;
            while (v < end && (*v == ' ' || *v == '\t'))
                ++v;
            size_t n = 0;
            while (v < end && *v != '\r' && *v != '\n' && n + 1 < cap)
                out[n++] = *v++;
            out[n] = '\0';
            return true;
        }
        while (p < end && *p != '\n')
            ++p;
        ++p;
    }
    return false;
}

void ae_http_resp_printf (AeHttpResponse *resp, const char *fmt, ...)
{
    va_list ap;
    va_start (ap, fmt);
    va_list ap2;
    va_copy (ap2, ap);
    const int n = vsnprintf (NULL, 0, fmt, ap);
    va_end (ap);

    free (resp->body);
    resp->body = NULL;
    resp->body_len = 0;
    if (n < 0)
    {
        va_end (ap2);
        return;
    }
    resp->body = malloc ((size_t) n + 1);
    if (resp->body != NULL)
    {
        vsnprintf (resp->body, (size_t) n + 1, fmt, ap2);
        resp->body_len = (size_t) n;
    }
    va_end (ap2);
}

void ae_http_resp_set (AeHttpResponse *resp, const void *data, size_t len)
{
    free (resp->body);
    resp->body = malloc (len > 0 ? len : 1);
    if (resp->body != NULL)
    {
        memcpy (resp->body, data, len);
        resp->body_len = len;
    }
    else
    {
        resp->body_len = 0;
    }
}

static const char *status_text (int status)
{
    switch (status)
    {
        case 200: return "OK";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Payload Too Large";
        default:  return "Internal Server Error";
    }
}

static bool write_all (int fd, const void *data, size_t len)
{
    const char *p = data;
    while (len > 0)
    {
        const ssize_t n = write (fd, p, len);
        if (n <= 0)
        {
            if (n < 0 && errno == EINTR)
                continue;
            return false;
        }
        p += n;
        len -= (size_t) n;
    }
    return true;
}

/* Upgrade an accepted socket into a /ws push client. Returns true if the
   fd was taken over (registered or closed here). */
static bool ws_upgrade (struct AeHttpServer *s, int fd,
                        const char *header, const char *body_start)
{
    char key[128];
    if (! header_value (header, body_start, "Sec-WebSocket-Key", key, sizeof (key)))
        return false;

    static const char guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    char cat[192];
    snprintf (cat, sizeof (cat), "%s%s", key, guid);
    unsigned char digest[20];
    sha1_digest ((const unsigned char *) cat, strlen (cat), digest);
    char accept[32];
    b64_encode (digest, 20, accept);

    char resp[256];
    const int n = snprintf (resp, sizeof (resp),
                            "HTTP/1.1 101 Switching Protocols\r\n"
                            "Upgrade: websocket\r\n"
                            "Connection: Upgrade\r\n"
                            "Sec-WebSocket-Accept: %s\r\n\r\n", accept);
    if (! write_all (fd, resp, (size_t) n))
    {
        close (fd);
        return true;
    }

    /* Non-blocking so a stalled client can never wedge the broadcaster. */
    fcntl (fd, F_SETFL, fcntl (fd, F_GETFL, 0) | O_NONBLOCK);

    pthread_mutex_lock (&s->ws_lock);
    if (s->ws_count < MAX_WS)
        s->ws_fds[s->ws_count++] = fd;
    else
        close (fd);
    pthread_mutex_unlock (&s->ws_lock);
    return true;
}

void ae_http_ws_broadcast (AeHttpServer *s, const char *data, size_t len)
{
    if (s == NULL)
        return;

    unsigned char hdr[10];
    size_t hn = 0;
    hdr[hn++] = 0x81; /* FIN + text */
    if (len < 126)
        hdr[hn++] = (unsigned char) len;
    else if (len <= 0xffff)
    {
        hdr[hn++] = 126;
        hdr[hn++] = (unsigned char) (len >> 8);
        hdr[hn++] = (unsigned char) len;
    }
    else
    {
        hdr[hn++] = 127;
        for (int i = 7; i >= 0; --i)
            hdr[hn++] = (unsigned char) ((uint64_t) len >> (8 * i));
    }

    pthread_mutex_lock (&s->ws_lock);
    for (int i = 0; i < s->ws_count; )
    {
        const int fd = s->ws_fds[i];
        /* Localhost + small frames: a short or failed write means the tab is
           gone or wedged either way — drop it. */
        const bool ok =
            send (fd, hdr, hn, MSG_NOSIGNAL) == (ssize_t) hn
            && send (fd, data, len, MSG_NOSIGNAL) == (ssize_t) len;
        if (! ok)
        {
            close (fd);
            s->ws_fds[i] = s->ws_fds[--s->ws_count];
        }
        else
            ++i;
    }
    pthread_mutex_unlock (&s->ws_lock);
}

/* Returns true if the fd was hijacked (WebSocket) and must not be closed. */
static bool handle_connection (struct AeHttpServer *s, int fd)
{
    char   header[MAX_HEADER + 1];
    size_t got = 0;
    char  *body_start = NULL;

    /* Read until end of headers. */
    while (got < MAX_HEADER)
    {
        const ssize_t n = read (fd, header + got, MAX_HEADER - got);
        if (n <= 0)
        {
            if (n < 0 && errno == EINTR)
                continue;
            return false;
        }
        got += (size_t) n;
        header[got] = '\0';
        if ((body_start = strstr (header, "\r\n\r\n")) != NULL)
        {
            body_start += 4;
            break;
        }
        if ((body_start = strstr (header, "\n\n")) != NULL)
        {
            body_start += 2;
            break;
        }
    }
    if (body_start == NULL)
        return false;

    /* Parse the request line. */
    char method[16] = "";
    char path[1024] = "";
    if (sscanf (header, "%15s %1023s", method, path) != 2)
        return false;
    char *q = strchr (path, '?'); /* ignore query strings */
    if (q != NULL)
        *q = '\0';

    if (strcmp (method, "GET") == 0 && strcmp (path, "/ws") == 0)
        return ws_upgrade (s, fd, header, body_start);

    /* Content-Length (case-insensitive search). */
    size_t content_len = 0;
    for (char *h = header; h < body_start; ++h)
    {
        if (strncasecmp (h, "Content-Length:", 15) == 0)
        {
            content_len = (size_t) strtoul (h + 15, NULL, 10);
            break;
        }
    }
    if (content_len > MAX_BODY)
    {
        const char *msg = "HTTP/1.1 413 Payload Too Large\r\nConnection: close\r\n\r\n";
        write_all (fd, msg, strlen (msg));
        return false;
    }

    /* Assemble the body (part may already be in the header buffer). */
    char *body = malloc (content_len + 1);
    if (body == NULL)
        return false;
    size_t have = got - (size_t) (body_start - header);
    if (have > content_len)
        have = content_len;
    memcpy (body, body_start, have);
    while (have < content_len)
    {
        const ssize_t n = read (fd, body + have, content_len - have);
        if (n <= 0)
        {
            if (n < 0 && errno == EINTR)
                continue;
            free (body);
            return false;
        }
        have += (size_t) n;
    }
    body[content_len] = '\0';

    AeHttpResponse resp = { 404, "text/plain", NULL, 0 };
    s->handler (s->user, method, path, body, content_len, &resp);
    free (body);

    if (resp.body == NULL)
        ae_http_resp_printf (&resp, "%s", status_text (resp.status));

    char head[512];
    const int hn = snprintf (head, sizeof (head),
                             "HTTP/1.1 %d %s\r\n"
                             "Content-Type: %s\r\n"
                             "Content-Length: %zu\r\n"
                             "Cache-Control: no-store\r\n"
                             "Connection: close\r\n\r\n",
                             resp.status, status_text (resp.status),
                             resp.content_type != NULL ? resp.content_type : "text/plain",
                             resp.body_len);
    if (write_all (fd, head, (size_t) hn))
        write_all (fd, resp.body, resp.body_len);
    free (resp.body);
    return false;
}

static void *server_thread (void *arg)
{
    struct AeHttpServer *s = arg;
    while (! s->stopping)
    {
        struct sockaddr_in addr;
        socklen_t alen = sizeof (addr);
        const int fd = accept (s->listen_fd, (struct sockaddr *) &addr, &alen);
        if (fd < 0)
        {
            if (errno == EINTR)
                continue;
            break; /* listen socket closed (shutdown) or fatal error */
        }
        if (! handle_connection (s, fd))
            close (fd);
    }
    return NULL;
}

AeHttpServer *ae_http_start (int port, AeHttpHandler handler, void *user,
                             char *err, size_t err_len)
{
    signal (SIGPIPE, SIG_IGN); /* a dropped browser connection must not kill us */

    const int fd = socket (AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        snprintf (err, err_len, "socket: %s", strerror (errno));
        return NULL;
    }

    const int one = 1;
    setsockopt (fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof (one));

    struct sockaddr_in addr;
    memset (&addr, 0, sizeof (addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons ((uint16_t) port);
    addr.sin_addr.s_addr = htonl (INADDR_LOOPBACK); /* local-only by design */

    if (bind (fd, (struct sockaddr *) &addr, sizeof (addr)) < 0)
    {
        snprintf (err, err_len, "bind 127.0.0.1:%d: %s", port, strerror (errno));
        close (fd);
        return NULL;
    }
    if (listen (fd, 16) < 0)
    {
        snprintf (err, err_len, "listen: %s", strerror (errno));
        close (fd);
        return NULL;
    }

    AeHttpServer *s = calloc (1, sizeof (*s));
    if (s == NULL)
    {
        snprintf (err, err_len, "out of memory");
        close (fd);
        return NULL;
    }
    s->listen_fd = fd;
    s->handler   = handler;
    s->user      = user;
    pthread_mutex_init (&s->ws_lock, NULL);

    if (pthread_create (&s->thread, NULL, server_thread, s) != 0)
    {
        snprintf (err, err_len, "pthread_create failed");
        close (fd);
        free (s);
        return NULL;
    }
    return s;
}

void ae_http_stop (AeHttpServer *s)
{
    if (s == NULL)
        return;
    s->stopping = true;
    shutdown (s->listen_fd, SHUT_RDWR);
    close (s->listen_fd);
    pthread_join (s->thread, NULL);
    pthread_mutex_lock (&s->ws_lock);
    for (int i = 0; i < s->ws_count; ++i)
        close (s->ws_fds[i]);
    s->ws_count = 0;
    pthread_mutex_unlock (&s->ws_lock);
    pthread_mutex_destroy (&s->ws_lock);
    free (s);
}
