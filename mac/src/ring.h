/* Lock-free single-producer / single-consumer float ring buffer with a
   linear-interpolating reader, used to bridge the input and output audio
   callbacks (which may run at different sample rates on different devices). */

#ifndef AUTOEDO_RING_H
#define AUTOEDO_RING_H

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
    float   *buf;
    uint64_t mask;            /* capacity - 1 (capacity is a power of two) */
    _Atomic uint64_t wpos;    /* total samples written (producer-owned) */
    _Atomic uint64_t rpos;    /* total samples consumed (consumer-owned) */
} AeRing;

static inline int ae_ring_init (AeRing *r, uint64_t capacity_pow2)
{
    r->buf = calloc (capacity_pow2, sizeof (float));
    r->mask = capacity_pow2 - 1;
    atomic_store (&r->wpos, 0);
    atomic_store (&r->rpos, 0);
    return r->buf != NULL ? 0 : -1;
}

static inline void ae_ring_free (AeRing *r)
{
    free (r->buf);
    r->buf = NULL;
}

static inline uint64_t ae_ring_fill (const AeRing *r)
{
    return atomic_load_explicit (&((AeRing *) r)->wpos, memory_order_acquire)
         - atomic_load_explicit (&((AeRing *) r)->rpos, memory_order_acquire);
}

/* Producer: append n samples. Samples that would overrun the reader are
   dropped (an audible glitch beats memory corruption). */
static inline void ae_ring_write (AeRing *r, const float *src, uint64_t n)
{
    const uint64_t w    = atomic_load_explicit (&r->wpos, memory_order_relaxed);
    const uint64_t fill = w - atomic_load_explicit (&r->rpos, memory_order_acquire);
    uint64_t space = (r->mask + 1) - fill;
    if (n > space)
        n = space;
    for (uint64_t i = 0; i < n; ++i)
        r->buf[(w + i) & r->mask] = src[i];
    atomic_store_explicit (&r->wpos, w + n, memory_order_release);
}

/* Consumer: read n output samples, resampling by `ratio` input samples per
   output sample with linear interpolation. `frac` carries the fractional
   read position between calls. Returns the number of output samples
   actually produced (may be < n on underrun). */
static inline int ae_ring_read_lerp (AeRing *r, float *dst, int n, double ratio, double *frac)
{
    const uint64_t w     = atomic_load_explicit (&r->wpos, memory_order_acquire);
    const uint64_t rp    = atomic_load_explicit (&r->rpos, memory_order_relaxed);
    const uint64_t avail = w - rp;
    double pos = *frac;

    int i = 0;
    for (; i < n; ++i)
    {
        const uint64_t i0 = (uint64_t) pos;
        if (i0 + 1 >= avail)
            break;
        const double t = pos - (double) i0;
        const float  a = r->buf[(rp + i0) & r->mask];
        const float  b = r->buf[(rp + i0 + 1) & r->mask];
        dst[i] = (float) (a + t * (b - a));
        pos += ratio;
    }

    const uint64_t advance = (uint64_t) pos;
    atomic_store_explicit (&r->rpos, rp + advance, memory_order_release);
    *frac = pos - (double) advance;
    return i;
}

#endif /* AUTOEDO_RING_H */
