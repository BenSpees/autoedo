#include "json.h"

#include <stdlib.h>
#include <string.h>

static const char *skip_ws (const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        ++p;
    return p;
}

static const char *skip_string (const char *p) /* p points at opening quote */
{
    ++p;
    while (*p && *p != '"')
    {
        if (*p == '\\' && p[1])
            ++p;
        ++p;
    }
    return *p == '"' ? p + 1 : p;
}

static const char *skip_value (const char *p)
{
    p = skip_ws (p);
    if (*p == '"')
        return skip_string (p);
    if (*p == '{' || *p == '[')
    {
        const char open = *p, close = (open == '{') ? '}' : ']';
        int depth = 0;
        while (*p)
        {
            if (*p == '"')
            {
                p = skip_string (p);
                continue;
            }
            if (*p == open)
                ++depth;
            else if (*p == close && --depth == 0)
                return p + 1;
            ++p;
        }
        return p;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' '
           && *p != '\t' && *p != '\n' && *p != '\r')
        ++p;
    return p;
}

/* Returns a pointer to the value of `key` in the top-level object, or NULL. */
static const char *find_key (const char *json, const char *key)
{
    const size_t klen = strlen (key);
    const char *p = skip_ws (json);
    if (*p != '{')
        return NULL;
    ++p;

    while (1)
    {
        p = skip_ws (p);
        if (*p != '"')
            return NULL;

        const char *kstart = p + 1;
        const char *kend   = skip_string (p) - 1; /* points at closing quote */
        const size_t len   = (size_t) (kend - kstart);

        p = skip_ws (kend + 1);
        if (*p != ':')
            return NULL;
        p = skip_ws (p + 1);

        if (len == klen && strncmp (kstart, key, klen) == 0)
            return p;

        p = skip_value (p);
        p = skip_ws (p);
        if (*p == ',')
            ++p;
        else
            return NULL;
    }
}

bool ae_json_get_number (const char *json, const char *key, double *out)
{
    const char *v = find_key (json, key);
    if (v == NULL || ! (*v == '-' || *v == '+' || (*v >= '0' && *v <= '9')))
        return false;
    char *end = NULL;
    const double d = strtod (v, &end);
    if (end == v)
        return false;
    *out = d;
    return true;
}

bool ae_json_get_bool (const char *json, const char *key, bool *out)
{
    const char *v = find_key (json, key);
    if (v == NULL)
        return false;
    if (strncmp (v, "true", 4) == 0)  { *out = true;  return true; }
    if (strncmp (v, "false", 5) == 0) { *out = false; return true; }
    return false;
}

bool ae_json_get_string (const char *json, const char *key, char *out, size_t cap)
{
    const char *v = find_key (json, key);
    if (v == NULL || *v != '"' || cap == 0)
        return false;

    ++v;
    size_t n = 0;
    while (*v && *v != '"' && n + 1 < cap)
    {
        char c = *v++;
        if (c == '\\' && *v)
        {
            const char e = *v++;
            switch (e)
            {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case 'u': /* keep it simple: skip the 4 hex digits, emit '?' */
                    for (int i = 0; i < 4 && *v; ++i) ++v;
                    c = '?';
                    break;
                default: c = e; break; /* \" \\ \/ */
            }
        }
        out[n++] = c;
    }
    out[n] = '\0';
    return true;
}

int ae_json_get_flag_array (const char *json, const char *key, unsigned char *out, int max)
{
    const char *v = find_key (json, key);
    if (v == NULL || *v != '[')
        return -1;

    int n = 0;
    const char *p = skip_ws (v + 1);
    while (*p && *p != ']')
    {
        unsigned char flag = 0;
        if (strncmp (p, "true", 4) == 0)
            flag = 1;
        else if (*p == '-' || *p == '+' || (*p >= '0' && *p <= '9'))
            flag = (strtod (p, NULL) != 0.0) ? 1 : 0;

        if (n < max)
            out[n] = flag;
        ++n;

        p = skip_value (p);
        p = skip_ws (p);
        if (*p == ',')
            p = skip_ws (p + 1);
    }
    return n < max ? n : max;
}

char *ae_json_escape_append (char *dst, size_t cap, const char *src)
{
    size_t n = strlen (dst);
    for (const char *s = src; *s && n + 7 < cap; ++s)
    {
        const unsigned char c = (unsigned char) *s;
        if (c == '"' || c == '\\')
        {
            dst[n++] = '\\';
            dst[n++] = (char) c;
        }
        else if (c < 0x20)
        {
            static const char hex[] = "0123456789abcdef";
            dst[n++] = '\\'; dst[n++] = 'u'; dst[n++] = '0'; dst[n++] = '0';
            dst[n++] = hex[c >> 4]; dst[n++] = hex[c & 15];
        }
        else
        {
            dst[n++] = (char) c;
        }
    }
    dst[n] = '\0';
    return dst;
}
