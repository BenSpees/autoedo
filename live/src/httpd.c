#include "httpd.h"

#include <stdbool.h>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <pthread.h> /* winpthreads (mingw-w64) */
  typedef SOCKET ae_sock_t;
  #define AE_BAD_SOCK   INVALID_SOCKET
  #define sock_close    closesocket
  #define sock_read(fd, buf, n)  recv ((fd), (char *) (buf), (int) (n), 0)
  #define sock_write(fd, buf, n) send ((fd), (const char *) (buf), (int) (n), 0)
  #define SHUT_RDWR     SD_BOTH
  #ifndef MSG_NOSIGNAL
  #define MSG_NOSIGNAL 0
  #endif
  #ifndef strncasecmp
  #define strncasecmp _strnicmp
  #endif
  static void sock_set_nonblock (ae_sock_t fd)
  {
      u_long one = 1;
      ioctlsocket (fd, FIONBIO, &one);
  }
  static void sock_startup (void)
  {
      static bool done = false;
      if (! done)
      {
          WSADATA wsa;
          WSAStartup (MAKEWORD (2, 2), &wsa);
          done = true;
      }
  }
#else
  #include <arpa/inet.h>
  #include <fcntl.h>
  #include <netdb.h>
  #include <netinet/in.h>
  #include <pthread.h>
  #include <signal.h>
  #include <sys/socket.h>
  #include <unistd.h>
  typedef int ae_sock_t;
  #define AE_BAD_SOCK   (-1)
  #define sock_close    close
  #define sock_read(fd, buf, n)  read ((fd), (buf), (n))
  #define sock_write(fd, buf, n) write ((fd), (buf), (n))
  #ifndef MSG_NOSIGNAL /* macOS: SIGPIPE is globally ignored instead */
  #define MSG_NOSIGNAL 0
  #endif
  static void sock_set_nonblock (ae_sock_t fd)
  {
      fcntl (fd, F_SETFL, fcntl (fd, F_GETFL, 0) | O_NONBLOCK);
  }
  static void sock_startup (void) {}
#endif

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_HEADER 8192
#define MAX_BODY   (1 << 20)
#define MAX_WS     16

struct AeHttpServer
{
    ae_sock_t     listen_fd;
    pthread_t     thread;
    AeHttpHandler handler;
    void         *user;
    volatile bool stopping;

    /* WebSocket clients (GET /ws connections hijacked from the HTTP path). */
    pthread_mutex_t ws_lock;
    ae_sock_t       ws_fds[MAX_WS];
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

static bool write_all (ae_sock_t fd, const void *data, size_t len)
{
    const char *p = data;
    while (len > 0)
    {
        const long n = (long) sock_write (fd, p, len);
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
static bool ws_upgrade (struct AeHttpServer *s, ae_sock_t fd,
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
        sock_close (fd);
        return true;
    }

    /* Non-blocking so a stalled client can never wedge the broadcaster. */
    sock_set_nonblock (fd);

    pthread_mutex_lock (&s->ws_lock);
    if (s->ws_count < MAX_WS)
        s->ws_fds[s->ws_count++] = fd;
    else
        sock_close (fd);
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
        const ae_sock_t fd = s->ws_fds[i];
        /* Localhost + small frames: a short or failed write means the tab is
           gone or wedged either way — drop it. */
        const bool ok =
            send (fd, (const char *) hdr, (int) hn, MSG_NOSIGNAL) == (long) hn
            && send (fd, (const char *) data, (int) len, MSG_NOSIGNAL) == (long) len;
        if (! ok)
        {
            sock_close (fd);
            s->ws_fds[i] = s->ws_fds[--s->ws_count];
        }
        else
            ++i;
    }
    pthread_mutex_unlock (&s->ws_lock);
}

/* Returns true if the fd was hijacked (WebSocket) and must not be closed. */
static bool handle_connection (struct AeHttpServer *s, ae_sock_t fd)
{
    char   header[MAX_HEADER + 1];
    size_t got = 0;
    char  *body_start = NULL;

    /* Read until end of headers. */
    while (got < MAX_HEADER)
    {
        const long n = (long) sock_read (fd, header + got, MAX_HEADER - got);
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

    /* CORS preflight: the server is single-controller by design and binds
       to loopback only; external control UIs on other origins are allowed. */
    if (strcmp (method, "OPTIONS") == 0)
    {
        const char *pre = "HTTP/1.1 204 No Content\r\n"
                          "Access-Control-Allow-Origin: *\r\n"
                          "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
                          "Access-Control-Allow-Headers: Content-Type\r\n"
                          "Access-Control-Allow-Private-Network: true\r\n"
                          "Access-Control-Max-Age: 86400\r\n"
                          "Connection: close\r\n\r\n";
        write_all (fd, pre, strlen (pre));
        return false;
    }

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
        const long n = (long) sock_read (fd, body + have, content_len - have);
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
                             "Access-Control-Allow-Origin: *\r\n"
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
        const ae_sock_t fd = accept (s->listen_fd, (struct sockaddr *) &addr, &alen);
        if (fd == AE_BAD_SOCK)
        {
            if (errno == EINTR)
                continue;
            break; /* listen socket closed (shutdown) or fatal error */
        }
        if (! handle_connection (s, fd))
            sock_close (fd);
    }
    return NULL;
}

/* ------------------------------------------------------------- client ----
   Minimal HTTP POST for the FOLLOW link: one engine posting a virtual MIDI
   note to another on the same machine. Blocking with a hard 100 ms budget
   (connect + send + first response bytes), called from a dedicated control
   thread -- never the audio thread. Returns true if the request was sent
   and any response line came back. */
bool ae_http_post_json (const char *host, int port, const char *path,
                        const char *json)
{
#ifdef _WIN32
    sock_startup ();
#endif
    char req[512];
    const int blen = (int) strlen (json);
    const int rlen = snprintf (req, sizeof (req),
        "POST %s HTTP/1.1\r\nHost: %s\r\nContent-Type: application/json\r\n"
        "Content-Length: %d\r\nConnection: close\r\n\r\n%s",
        path, host, blen, json);
    if (rlen <= 0 || rlen >= (int) sizeof (req))
        return false;

    struct addrinfo hints, *ai = NULL;
    memset (&hints, 0, sizeof (hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char portstr[16];
    snprintf (portstr, sizeof (portstr), "%d", port);
    if (getaddrinfo (host, portstr, &hints, &ai) != 0 || ai == NULL)
        return false;

    ae_sock_t fd = socket (ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd == AE_BAD_SOCK) { freeaddrinfo (ai); return false; }

#ifdef _WIN32
    DWORD tmo = 100;
    setsockopt (fd, SOL_SOCKET, SO_RCVTIMEO, (const char *) &tmo, sizeof (tmo));
    setsockopt (fd, SOL_SOCKET, SO_SNDTIMEO, (const char *) &tmo, sizeof (tmo));
#else
    struct timeval tv = { 0, 100000 };
    setsockopt (fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof (tv));
    setsockopt (fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof (tv));
#endif

    bool ok = false;
    if (connect (fd, ai->ai_addr, (int) ai->ai_addrlen) == 0
        && send (fd, req, (size_t) rlen, 0) == (long) rlen)
    {
        char resp[64];
        ok = recv (fd, resp, sizeof (resp), 0) > 0;
    }
    sock_close (fd);
    freeaddrinfo (ai);
    return ok;
}


AeHttpServer *ae_http_start (int port, AeHttpHandler handler, void *user,
                             char *err, size_t err_len)
{
    sock_startup();
#ifndef _WIN32
    signal (SIGPIPE, SIG_IGN); /* a dropped browser connection must not kill us */
#endif

    const ae_sock_t fd = socket (AF_INET, SOCK_STREAM, 0);
    if (fd == AE_BAD_SOCK)
    {
        snprintf (err, err_len, "socket: %s", strerror (errno));
        return NULL;
    }

    const int one = 1;
    setsockopt (fd, SOL_SOCKET, SO_REUSEADDR, (const char *) &one, sizeof (one));

    struct sockaddr_in addr;
    memset (&addr, 0, sizeof (addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons ((uint16_t) port);
    addr.sin_addr.s_addr = htonl (INADDR_LOOPBACK); /* local-only by design */

    if (bind (fd, (struct sockaddr *) &addr, sizeof (addr)) < 0)
    {
        snprintf (err, err_len, "bind 127.0.0.1:%d: %s", port, strerror (errno));
        sock_close (fd);
        return NULL;
    }
    if (listen (fd, 16) < 0)
    {
        snprintf (err, err_len, "listen: %s", strerror (errno));
        sock_close (fd);
        return NULL;
    }

    AeHttpServer *s = calloc (1, sizeof (*s));
    if (s == NULL)
    {
        snprintf (err, err_len, "out of memory");
        sock_close (fd);
        return NULL;
    }
    s->listen_fd = fd;
    s->handler   = handler;
    s->user      = user;
    pthread_mutex_init (&s->ws_lock, NULL);

    if (pthread_create (&s->thread, NULL, server_thread, s) != 0)
    {
        snprintf (err, err_len, "pthread_create failed");
        sock_close (fd);
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
    sock_close (s->listen_fd);
    pthread_join (s->thread, NULL);
    pthread_mutex_lock (&s->ws_lock);
    for (int i = 0; i < s->ws_count; ++i)
        sock_close (s->ws_fds[i]);
    s->ws_count = 0;
    pthread_mutex_unlock (&s->ws_lock);
    pthread_mutex_destroy (&s->ws_lock);
    free (s);
}
