/* IR file loading for the convolution points (v0.4-delta B4/B7).
   ===============================================================
   Treebrain is the LIBRARIAN: it owns <rig-data>/irs/, resamples at import
   and hands this engine a {path, hash} pair per point. This side only
   loads and VERIFIES -- the hash proves the file on disk is the one the
   scene meant, and a sample-rate mismatch is refused outright (the
   real-time path never resamples; the librarian's per-rate caches exist
   precisely so it never has to).

   Hash convention (shared with the manifest): FNV-1a 64-bit over the raw
   file bytes, written as 16 lowercase hex digits. */

#include "ir_load.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint64_t ae_ir_hash_bytes (const void *data, size_t len, uint64_t seed)
{
    const unsigned char *b = (const unsigned char *) data;
    uint64_t h = seed != 0 ? seed : 1469598103934665603ULL;
    for (size_t i = 0; i < len; ++i)
    {
        h ^= b[i];
        h *= 1099511628211ULL;
    }
    return h;
}

/* Minimal RIFF/WAVE reader: PCM 16/24-bit and float32, mono or stereo.
   Returns malloc'd interleaved floats. */
static float *wav_read (const unsigned char *buf, size_t len, int *out_frames,
                        int *out_channels, int *out_rate, char *err, size_t err_len)
{
    if (len < 44 || memcmp (buf, "RIFF", 4) != 0 || memcmp (buf + 8, "WAVE", 4) != 0)
    {
        snprintf (err, err_len, "not a RIFF/WAVE file");
        return NULL;
    }
    const unsigned char *fmt = NULL, *data = NULL;
    size_t data_len = 0;
    size_t off = 12;
    while (off + 8 <= len)
    {
        const unsigned char *ck = buf + off;
        const size_t ck_len = (size_t) ck[4] | ((size_t) ck[5] << 8)
                            | ((size_t) ck[6] << 16) | ((size_t) ck[7] << 24);
        if (off + 8 + ck_len > len)
            break;
        if (memcmp (ck, "fmt ", 4) == 0)
            fmt = ck + 8;
        else if (memcmp (ck, "data", 4) == 0)
        {
            data = ck + 8;
            data_len = ck_len;
        }
        off += 8 + ck_len + (ck_len & 1);
    }
    if (fmt == NULL || data == NULL)
    {
        snprintf (err, err_len, "missing fmt/data chunk");
        return NULL;
    }
    const int format   = fmt[0] | (fmt[1] << 8);
    const int channels = fmt[2] | (fmt[3] << 8);
    const int rate     = fmt[4] | (fmt[5] << 8) | (fmt[6] << 16) | (fmt[7] << 24);
    const int bits     = fmt[14] | (fmt[15] << 8);
    if (channels < 1 || channels > 2)
    {
        snprintf (err, err_len, "%d channels (mono or stereo only)", channels);
        return NULL;
    }
    const int bytes = bits / 8;
    const bool pcm = format == 1 && (bits == 16 || bits == 24);
    const bool f32 = format == 3 && bits == 32;
    if (! pcm && ! f32)
    {
        snprintf (err, err_len, "format %d/%d-bit (PCM 16/24 or float32 only)",
                  format, bits);
        return NULL;
    }
    const int frames = (int) (data_len / (size_t) (bytes * channels));
    if (frames <= 0)
    {
        snprintf (err, err_len, "empty data chunk");
        return NULL;
    }
    float *out = malloc ((size_t) frames * (size_t) channels * sizeof (float));
    if (out == NULL)
    {
        snprintf (err, err_len, "out of memory");
        return NULL;
    }
    for (int i = 0; i < frames * channels; ++i)
    {
        const unsigned char *s = data + (size_t) i * (size_t) bytes;
        if (f32)
        {
            float v;
            memcpy (&v, s, 4);
            out[i] = v;
        }
        else if (bits == 16)
        {
            const int16_t v = (int16_t) (s[0] | (s[1] << 8));
            out[i] = (float) v / 32768.0f;
        }
        else /* 24 */
        {
            int32_t v = s[0] | (s[1] << 8) | (s[2] << 16);
            if (v & 0x800000)
                v |= (int32_t) 0xFF000000;
            out[i] = (float) v / 8388608.0f;
        }
    }
    *out_frames = frames;
    *out_channels = channels;
    *out_rate = rate;
    return out;
}

bool ae_ir_load_point (AeCorrector *corr, int point, const char *path,
                       const char *hash, double predelay_ms, double engine_rate,
                       char *err, size_t err_len)
{
    err[0] = '\0';
    if (path == NULL || path[0] == '\0')
    {
        /* An empty path clears the point (fade to bypass). */
        if (! ae_corrector_load_ir (corr, point, NULL, NULL, 0, 0.0))
        {
            snprintf (err, err_len, "a previous IR swap is still fading");
            return false;
        }
        return true;
    }

    FILE *f = fopen (path, "rb");
    if (f == NULL)
    {
        snprintf (err, err_len, "cannot open %s", path);
        return false;
    }
    fseek (f, 0, SEEK_END);
    const long fl = ftell (f);
    fseek (f, 0, SEEK_SET);
    if (fl <= 0 || fl > 64 * 1024 * 1024)
    {
        fclose (f);
        snprintf (err, err_len, "unreasonable file size");
        return false;
    }
    unsigned char *bytes = malloc ((size_t) fl);
    if (bytes == NULL || fread (bytes, 1, (size_t) fl, f) != (size_t) fl)
    {
        fclose (f);
        free (bytes);
        snprintf (err, err_len, "read failed on %s", path);
        return false;
    }
    fclose (f);

    /* Verify the librarian's hash before trusting a byte of it. */
    if (hash != NULL && hash[0] != '\0')
    {
        char got[17];
        snprintf (got, sizeof (got), "%016llx",
                  (unsigned long long) ae_ir_hash_bytes (bytes, (size_t) fl, 0));
        if (strcmp (got, hash) != 0)
        {
            free (bytes);
            snprintf (err, err_len, "hash mismatch (file %s, expected %s, got %s)",
                      path, hash, got);
            return false;
        }
    }

    int frames = 0, channels = 0, rate = 0;
    float *pcm = wav_read (bytes, (size_t) fl, &frames, &channels, &rate,
                           err, err_len);
    free (bytes);
    if (pcm == NULL)
        return false;
    if (engine_rate > 0 && rate != (int) engine_rate)
    {
        free (pcm);
        snprintf (err, err_len,
                  "IR is %d Hz but the engine runs %d Hz -- the librarian's "
                  "per-rate cache should have matched (re-import the IR)",
                  rate, (int) engine_rate);
        return false;
    }
    const int cap = (int) (2.0 * engine_rate);
    if (cap > 0 && frames > cap)
        frames = cap; /* the 2 s ceiling; the librarian warns at import */

    bool ok;
    if (channels == 1)
    {
        ok = ae_corrector_load_ir (corr, point, pcm, NULL, frames, predelay_ms);
    }
    else
    {
        /* Deinterleave. A stereo IR into the MONO lead point folds to the
           mid signal -- the librarian warned at assign time. */
        float *l = malloc ((size_t) frames * sizeof (float));
        float *r = malloc ((size_t) frames * sizeof (float));
        if (l == NULL || r == NULL)
        {
            free (pcm);
            free (l);
            free (r);
            snprintf (err, err_len, "out of memory");
            return false;
        }
        for (int i = 0; i < frames; ++i)
        {
            l[i] = pcm[2 * i];
            r[i] = pcm[2 * i + 1];
        }
        if (point == 0)
            for (int i = 0; i < frames; ++i)
                l[i] = 0.5f * (l[i] + r[i]);
        ok = ae_corrector_load_ir (corr, point, l, point == 0 ? NULL : r,
                                   frames, predelay_ms);
        free (l);
        free (r);
    }
    free (pcm);
    if (! ok)
        snprintf (err, err_len, "a previous IR swap is still fading");
    return ok;
}
