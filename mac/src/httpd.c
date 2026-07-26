#include "httpd.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_HEADER 8192
#define MAX_BODY   (1 << 20)

struct AeHttpServer
{
    int           listen_fd;
    pthread_t     thread;
    AeHttpHandler handler;
    void         *user;
    volatile bool stopping;
};

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

static void handle_connection (struct AeHttpServer *s, int fd)
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
            return;
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
        return;

    /* Parse the request line. */
    char method[16] = "";
    char path[1024] = "";
    if (sscanf (header, "%15s %1023s", method, path) != 2)
        return;
    char *q = strchr (path, '?'); /* ignore query strings */
    if (q != NULL)
        *q = '\0';

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
        return;
    }

    /* Assemble the body (part may already be in the header buffer). */
    char *body = malloc (content_len + 1);
    if (body == NULL)
        return;
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
            return;
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
        handle_connection (s, fd);
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
    free (s);
}
