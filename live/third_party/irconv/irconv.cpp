/* irconv implementation. See irconv.h for the scheme and the contract.
   C++11, no dependencies; C ABI. Internally double-precision FFTs over
   float-stored spectra (a 2 s reverb tail does not need double storage,
   and the spectra rings are the memory that matters). */

#include "irconv.h"

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <new>
#include <utility> /* std::swap — libstdc++ leaks it in, libc++ does not */

namespace {

/* M_PI is a POSIX extension, absent from <cmath> under strict ISO C++ (the
   mingw-w64 Windows build). Carry our own, as src/corrector.c does. */
constexpr double kPi = 3.14159265358979323846;

constexpr int kB0   = 64;   /* direct head length == zero-latency guarantee */
constexpr int kLmax = 4096; /* largest FFT partition; bounds the burst */

/* ---- tiny iterative radix-2 complex FFT with cached twiddles ---------- */

struct Fft {
    int n = 0;
    double* tw = nullptr;   /* n/2 twiddles, (re, im) interleaved */
    int* rev = nullptr;     /* bit-reversal table */

    bool init(int size) {
        n = size;
        tw = static_cast<double*>(std::malloc(sizeof(double) * static_cast<size_t>(n)));
        rev = static_cast<int*>(std::malloc(sizeof(int) * static_cast<size_t>(n)));
        if (!tw || !rev)
            return false;
        for (int k = 0; k < n / 2; ++k) {
            const double a = -2.0 * kPi * k / n;
            tw[2 * k] = std::cos(a);
            tw[2 * k + 1] = std::sin(a);
        }
        int bits = 0;
        while ((1 << bits) < n)
            ++bits;
        for (int i = 0; i < n; ++i) {
            int r = 0;
            for (int b = 0; b < bits; ++b)
                if (i & (1 << b))
                    r |= 1 << (bits - 1 - b);
            rev[i] = r;
        }
        return true;
    }
    void free() {
        std::free(tw);
        std::free(rev);
        tw = nullptr;
        rev = nullptr;
    }
    /* In-place on interleaved complex doubles; inverse leaves the caller to
       scale by 1/n (folded into the spectrum fill instead, see below). */
    void run(double* x, bool inverse) const {
        for (int i = 0; i < n; ++i) {
            const int r = rev[i];
            if (r > i) {
                std::swap(x[2 * i], x[2 * r]);
                std::swap(x[2 * i + 1], x[2 * r + 1]);
            }
        }
        for (int len = 2; len <= n; len <<= 1) {
            const int half = len / 2, step = n / len;
            for (int base = 0; base < n; base += len) {
                for (int k = 0; k < half; ++k) {
                    const double wr = tw[2 * k * step];
                    const double wi = inverse ? -tw[2 * k * step + 1]
                                              : tw[2 * k * step + 1];
                    double* a = x + 2 * (base + k);
                    double* b = x + 2 * (base + k + half);
                    const double tr = b[0] * wr - b[1] * wi;
                    const double ti = b[0] * wi + b[1] * wr;
                    b[0] = a[0] - tr;
                    b[1] = a[1] - ti;
                    a[0] += tr;
                    a[1] += ti;
                }
            }
        }
    }
};

/* One doubling partition: length L at offset O == L in the IR. */
struct DblPart {
    int L = 0;
    float* spec = nullptr;  /* Hseg spectrum, 2L complex floats (4L floats) */
    float* stash = nullptr; /* the L output samples being consumed */
    bool active = false;    /* the IR actually reaches this partition */
};

} // namespace

struct IrcConvolver {
    int maxIr = 0;
    int maxBlock = 0;

    float h0[kB0];              /* direct taps */
    float* hist = nullptr;      /* input history ring */
    int histMask = 0;
    long long pos = 0;          /* absolute samples processed */

    DblPart dbl[16];
    int numDbl = 0;

    /* Uniform tail: partitions of kLmax at offsets 2*kLmax + j*kLmax. */
    int numTail = 0;            /* sized for maxIr */
    int tailActive = 0;         /* how many the CURRENT IR reaches */
    float* tailSpec = nullptr;  /* numTail spectra, each 2*kLmax complex */
    float* specRing = nullptr;  /* input spectra ring, ringCap entries */
    int ringCap = 0;
    float* tailStash = nullptr; /* kLmax samples being consumed */
    bool tailOn = false;

    /* FFT workspaces (largest size), shared by every partition. */
    double* work = nullptr;     /* 2 * 2*kLmax doubles (complex) */
    double* acc = nullptr;      /* tail accumulator, 2 * 2*kLmax doubles */
    Fft ffts[8];                /* sizes 2*kB0 .. 2*kLmax */
    int numFft = 0;

    const Fft* fftFor(int size) const {
        for (int i = 0; i < numFft; ++i)
            if (ffts[i].n == size)
                return &ffts[i];
        return nullptr;
    }
};

namespace {

/* Forward FFT of `n` real samples zero-padded to `size`, into out
   (interleaved complex doubles). */
void fftReal(const Fft* f, const float* x, int n, double* out) {
    const int size = f->n;
    for (int i = 0; i < size; ++i) {
        out[2 * i] = i < n ? static_cast<double>(x[i]) : 0.0;
        out[2 * i + 1] = 0.0;
    }
    f->run(out, false);
}

/* Complex multiply-accumulate: acc += a * b (spectra stored as floats). */
inline void specMac(double* acc, const float* a, const float* b, int cplx) {
    for (int i = 0; i < cplx; ++i) {
        const double ar = a[2 * i], ai = a[2 * i + 1];
        const double br = b[2 * i], bi = b[2 * i + 1];
        acc[2 * i] += ar * br - ai * bi;
        acc[2 * i + 1] += ar * bi + ai * br;
    }
}

} // namespace

extern "C" {

IrcConvolver* irc_create(int max_ir_len, int max_block) {
    if (max_ir_len < 1 || max_block < 1)
        return nullptr;
    IrcConvolver* c = new (std::nothrow) IrcConvolver();
    if (!c)
        return nullptr;
    c->maxIr = max_ir_len;
    c->maxBlock = max_block;

    /* History ring: enough for the largest FFT's 2L look-back plus a whole
       process call. */
    int need = 4 * kLmax + max_block;
    int histLen = 1;
    while (histLen < need)
        histLen <<= 1;
    c->hist = static_cast<float*>(std::calloc(static_cast<size_t>(histLen), sizeof(float)));
    c->histMask = histLen - 1;

    /* Doubling partitions while they fit under both the cap and the IR. */
    for (int L = kB0; L <= kLmax && L < max_ir_len; L <<= 1) {
        DblPart& d = c->dbl[c->numDbl++];
        d.L = L;
        d.spec = static_cast<float*>(std::calloc(static_cast<size_t>(4 * L), sizeof(float)));
        d.stash = static_cast<float*>(std::calloc(static_cast<size_t>(L), sizeof(float)));
    }

    /* Tail partitions to cover the rest. */
    if (max_ir_len > 2 * kLmax) {
        c->numTail = (max_ir_len - 2 * kLmax + kLmax - 1) / kLmax;
        c->tailSpec = static_cast<float*>(
            std::calloc(static_cast<size_t>(c->numTail) * 4 * kLmax, sizeof(float)));
        c->ringCap = c->numTail + 3;
        c->specRing = static_cast<float*>(
            std::calloc(static_cast<size_t>(c->ringCap) * 4 * kLmax, sizeof(float)));
        c->tailStash = static_cast<float*>(std::calloc(kLmax, sizeof(float)));
    }

    c->work = static_cast<double*>(std::calloc(4 * kLmax, sizeof(double)));
    c->acc = static_cast<double*>(std::calloc(4 * kLmax, sizeof(double)));
    for (int size = 2 * kB0; size <= 2 * kLmax; size <<= 1)
        if (!c->ffts[c->numFft].init(size) || ++c->numFft > 7)
            break;

    bool ok = c->hist && c->work && c->acc;
    for (int i = 0; i < c->numDbl; ++i)
        ok = ok && c->dbl[i].spec && c->dbl[i].stash;
    if (c->numTail)
        ok = ok && c->tailSpec && c->specRing && c->tailStash;
    if (!ok) {
        irc_destroy(c);
        return nullptr;
    }
    std::memset(c->h0, 0, sizeof(c->h0));
    return c;
}

void irc_destroy(IrcConvolver* c) {
    if (!c)
        return;
    std::free(c->hist);
    for (int i = 0; i < 16; ++i) {
        std::free(c->dbl[i].spec);
        std::free(c->dbl[i].stash);
    }
    std::free(c->tailSpec);
    std::free(c->specRing);
    std::free(c->tailStash);
    std::free(c->work);
    std::free(c->acc);
    for (int i = 0; i < c->numFft; ++i)
        c->ffts[i].free();
    delete c;
}

void irc_set_ir(IrcConvolver* c, const float* ir, int len) {
    if (!c)
        return;
    if (!ir || len < 0)
        len = 0;
    if (len > c->maxIr)
        len = c->maxIr;

    for (int k = 0; k < kB0; ++k)
        c->h0[k] = k < len ? ir[k] : 0.0f;

    /* Doubling partitions: spectrum of h[L .. 2L), scaled by 1/(2L) here so
       the inverse FFT needs no pass of its own. */
    for (int i = 0; i < c->numDbl; ++i) {
        DblPart& d = c->dbl[i];
        const int L = d.L, size = 2 * L;
        const int off = L;
        const int have = len > off ? (len - off < L ? len - off : L) : 0;
        d.active = have > 0;
        if (!d.active) {
            std::memset(d.spec, 0, sizeof(float) * static_cast<size_t>(4 * L));
            continue;
        }
        const Fft* f = c->fftFor(size);
        fftReal(f, ir + off, have, c->work);
        const double s = 1.0 / size;
        for (int k = 0; k < size; ++k) {
            d.spec[2 * k] = static_cast<float>(c->work[2 * k] * s);
            d.spec[2 * k + 1] = static_cast<float>(c->work[2 * k + 1] * s);
        }
    }

    /* Tail: spectrum per partition, same folded 1/(2L) scale. */
    c->tailActive = 0;
    if (c->numTail) {
        const Fft* f = c->fftFor(2 * kLmax);
        for (int j = 0; j < c->numTail; ++j) {
            const int off = 2 * kLmax + j * kLmax;
            const int have = len > off ? (len - off < kLmax ? len - off : kLmax) : 0;
            float* spec = c->tailSpec + static_cast<size_t>(j) * 4 * kLmax;
            if (have <= 0) {
                std::memset(spec, 0, sizeof(float) * 4 * kLmax);
                continue;
            }
            c->tailActive = j + 1;
            fftReal(f, ir + off, have, c->work);
            const double s = 1.0 / (2 * kLmax);
            for (int k = 0; k < 2 * kLmax; ++k) {
                spec[2 * k] = static_cast<float>(c->work[2 * k] * s);
                spec[2 * k + 1] = static_cast<float>(c->work[2 * k + 1] * s);
            }
        }
    }
    c->tailOn = c->tailActive > 0;
}

void irc_reset(IrcConvolver* c) {
    if (!c)
        return;
    std::memset(c->hist, 0, sizeof(float) * static_cast<size_t>(c->histMask + 1));
    for (int i = 0; i < c->numDbl; ++i)
        std::memset(c->dbl[i].stash, 0, sizeof(float) * static_cast<size_t>(c->dbl[i].L));
    if (c->numTail) {
        std::memset(c->specRing, 0,
                    sizeof(float) * static_cast<size_t>(c->ringCap) * 4 * kLmax);
        std::memset(c->tailStash, 0, sizeof(float) * kLmax);
    }
    c->pos = 0;
}

/* Fire the FFT work due when `pos` sits exactly on a partition boundary. */
static void ircBoundaries(IrcConvolver* c) {
    const long long pos = c->pos;
    /* Doubling partitions: at pos == m*L (m >= 1), compute the overlap-save
       block for input [pos-2L, pos) -> output consumed over [pos, pos+L). */
    for (int i = 0; i < c->numDbl; ++i) {
        DblPart& d = c->dbl[i];
        const int L = d.L, size = 2 * L;
        if (pos == 0 || (pos & (L - 1)) != 0)
            continue;
        if (!d.active) {
            /* Nothing of the IR here; the stash stays zero. */
            continue;
        }
        const Fft* f = c->fftFor(size);
        for (int k = 0; k < size; ++k) {
            const long long t = pos - size + k;
            c->work[2 * k] = t >= 0 ? c->hist[t & c->histMask] : 0.0;
            c->work[2 * k + 1] = 0.0;
        }
        f->run(c->work, false);
        /* Multiply by the (pre-scaled) segment spectrum, in place. */
        for (int k = 0; k < size; ++k) {
            const double xr = c->work[2 * k], xi = c->work[2 * k + 1];
            const double hr = d.spec[2 * k], hi = d.spec[2 * k + 1];
            c->work[2 * k] = xr * hr - xi * hi;
            c->work[2 * k + 1] = xr * hi + xi * hr;
        }
        f->run(c->work, true);
        for (int k = 0; k < L; ++k)
            d.stash[k] = static_cast<float>(c->work[2 * (L + k)]);
    }

    /* Tail FDL: at pos == b*Lmax, push the spectrum of [pos-2Lmax, pos) and
       synthesize the block consumed over [pos, pos+Lmax). */
    if (c->tailOn && pos != 0 && (pos & (kLmax - 1)) == 0) {
        const Fft* f = c->fftFor(2 * kLmax);
        const long long b = pos / kLmax;
        float* slot = c->specRing
                    + static_cast<size_t>(b % c->ringCap) * 4 * kLmax;
        for (int k = 0; k < 2 * kLmax; ++k) {
            const long long t = pos - 2 * kLmax + k;
            c->work[2 * k] = t >= 0 ? c->hist[t & c->histMask] : 0.0;
            c->work[2 * k + 1] = 0.0;
        }
        f->run(c->work, false);
        for (int k = 0; k < 4 * kLmax; ++k)
            slot[k] = static_cast<float>(c->work[k]);

        /* y block b needs partition j against input spectrum block b-2-j
           (spectrum block m holds input [(m-2)Lmax, m*Lmax) -- i.e. the
           spectrum pushed at pos == m*Lmax covers OLS output block m-1, so
           partition j at offset (2+j)Lmax reads the spectrum pushed at
           block b-1-j). */
        std::memset(c->acc, 0, sizeof(double) * 4 * kLmax);
        for (int j = 0; j < c->tailActive; ++j) {
            const long long m = b - 1 - j;
            if (m <= 0)
                continue;
            const float* xs = c->specRing
                            + static_cast<size_t>(m % c->ringCap) * 4 * kLmax;
            const float* hs = c->tailSpec + static_cast<size_t>(j) * 4 * kLmax;
            specMac(c->acc, xs, hs, 2 * kLmax);
        }
        f->run(c->acc, true);
        for (int k = 0; k < kLmax; ++k)
            c->tailStash[k] = static_cast<float>(c->acc[2 * (kLmax + k)]);
    }
}

void irc_process(IrcConvolver* c, const float* in, float* out, int n) {
    if (!c || n <= 0)
        return;
    int done = 0;
    while (done < n) {
        /* Boundaries fire when pos LANDS on them; every partition length
           is a multiple of the smallest, so stopping the run there catches
           them all. */
        ircBoundaries(c);
        /* Run until the next partition boundary (or the end of the call). */
        int run = n - done;
        const int smallest = c->numDbl ? c->dbl[0].L : kLmax;
        const long long next = ((c->pos / smallest) + 1) * smallest;
        if (next - c->pos < run)
            run = static_cast<int>(next - c->pos);

        for (int i = 0; i < run; ++i) {
            const long long t = c->pos + i;
            c->hist[t & c->histMask] = in[done + i];
            /* Direct head. */
            double y = 0.0;
            const int taps = kB0 <= t + 1 ? kB0 : static_cast<int>(t + 1);
            for (int k = 0; k < taps; ++k)
                y += static_cast<double>(c->h0[k]) * c->hist[(t - k) & c->histMask];
            /* Doubling stashes: partition (L) block floor(t/L)-1 lives in
               stash[t mod L]. */
            for (int d = 0; d < c->numDbl; ++d)
                if (c->dbl[d].active && t >= c->dbl[d].L)
                    y += c->dbl[d].stash[t & (c->dbl[d].L - 1)];
            if (c->tailOn && t >= 2 * kLmax)
                y += c->tailStash[t & (kLmax - 1)];
            out[done + i] = static_cast<float>(y);
        }
        c->pos += run;
        done += run;
    }
}

/* ---- the point unit ---------------------------------------------------- */

struct IrcPoint {
    IrcConvolver* inst[2] = {nullptr, nullptr};
    int active = 0;
    int predelay[2] = {0, 0};

    double fs = 48000.0;
    int maxBlock = 0;
    int maxPre = 0;

    float* preRing = nullptr; /* shared input ring for the predelays */
    int preMask = 0;
    long long prePos = 0;

    float* wetA = nullptr; /* per-call scratch */
    float* wetB = nullptr;
    float* delayed = nullptr;

    /* Crossfade: armed by the control thread, run by the audio thread. */
    std::atomic<int> pendingSwap{0}; /* 1 = new IR ready in inst[!active] */
    int fadeLen = 0;
    int fadePos = -1; /* -1 = idle */

    std::atomic<double> mixT{1.0};
    std::atomic<double> gainT{1.0}; /* linear */
    std::atomic<bool> onT{false};
    double mixCur = 0.0; /* effective wet amount, smoothed */
    double smooth = 0.0; /* per-sample one-pole coefficient */
    bool bypassed = true; /* fast path: off + faded out = free */
};

IrcPoint* irc_point_create(int max_ir_len, int max_block, double sample_rate) {
    IrcPoint* p = new (std::nothrow) IrcPoint();
    if (!p)
        return nullptr;
    p->fs = sample_rate > 0 ? sample_rate : 48000.0;
    p->maxBlock = max_block;
    p->maxPre = static_cast<int>(0.05 * p->fs) + 1; /* the 50 ms ceiling */
    p->fadeLen = static_cast<int>(0.03 * p->fs);    /* ~30 ms swap fade */
    p->smooth = 1.0 - std::exp(-1.0 / (0.010 * p->fs));
    p->inst[0] = irc_create(max_ir_len, max_block);
    p->inst[1] = irc_create(max_ir_len, max_block);
    int need = p->maxPre + max_block;
    int len = 1;
    while (len < need)
        len <<= 1;
    p->preRing = static_cast<float*>(std::calloc(static_cast<size_t>(len), sizeof(float)));
    p->preMask = len - 1;
    p->wetA = static_cast<float*>(std::calloc(static_cast<size_t>(max_block), sizeof(float)));
    p->wetB = static_cast<float*>(std::calloc(static_cast<size_t>(max_block), sizeof(float)));
    p->delayed = static_cast<float*>(std::calloc(static_cast<size_t>(max_block), sizeof(float)));
    if (!p->inst[0] || !p->inst[1] || !p->preRing || !p->wetA || !p->wetB || !p->delayed) {
        irc_point_destroy(p);
        return nullptr;
    }
    return p;
}

void irc_point_destroy(IrcPoint* p) {
    if (!p)
        return;
    irc_destroy(p->inst[0]);
    irc_destroy(p->inst[1]);
    std::free(p->preRing);
    std::free(p->wetA);
    std::free(p->wetB);
    std::free(p->delayed);
    delete p;
}

bool irc_point_busy(const IrcPoint* p) {
    return p && (p->pendingSwap.load(std::memory_order_acquire) != 0 || p->fadePos >= 0);
}

bool irc_point_load(IrcPoint* p, const float* ir, int len, double predelay_ms) {
    if (!p || irc_point_busy(p))
        return false;
    const int other = 1 - p->active;
    irc_set_ir(p->inst[other], ir, len);
    irc_reset(p->inst[other]);
    int pre = static_cast<int>(predelay_ms * 0.001 * p->fs + 0.5);
    if (pre < 0)
        pre = 0;
    if (pre > p->maxPre - 1)
        pre = p->maxPre - 1;
    p->predelay[other] = pre;
    p->pendingSwap.store(1, std::memory_order_release);
    return true;
}

void irc_point_set(IrcPoint* p, double mix, double gain_db, bool on) {
    if (!p)
        return;
    if (mix < 0)
        mix = 0;
    if (mix > 1)
        mix = 1;
    if (gain_db < -24)
        gain_db = -24;
    if (gain_db > 12)
        gain_db = 12;
    p->mixT.store(mix, std::memory_order_relaxed);
    p->gainT.store(std::pow(10.0, gain_db / 20.0), std::memory_order_relaxed);
    p->onT.store(on, std::memory_order_relaxed);
}

void irc_point_reset(IrcPoint* p) {
    if (!p)
        return;
    irc_reset(p->inst[0]);
    irc_reset(p->inst[1]);
    std::memset(p->preRing, 0, sizeof(float) * static_cast<size_t>(p->preMask + 1));
    p->prePos = 0;
    p->mixCur = 0.0;
    p->fadePos = -1;
    p->pendingSwap.store(0, std::memory_order_release);
}

void irc_point_process(IrcPoint* p, const float* in, float* out, int n) {
    if (!p || n <= 0)
        return;

    /* Fast path: an OFF point that has fully faded out (and has no swap in
       flight) costs a copy, not a convolution. Entering it clears every
       signal buffer, so the next enable swells the space in from a clean
       slate instead of replaying a frozen tail. */
    {
        const bool wantWet = p->onT.load(std::memory_order_relaxed)
            && p->mixT.load(std::memory_order_relaxed) > 0.0;
        if (!wantWet && p->mixCur < 1e-6 && p->fadePos < 0
            && !p->pendingSwap.load(std::memory_order_acquire)) {
            if (!p->bypassed) {
                p->bypassed = true;
                irc_reset(p->inst[0]);
                irc_reset(p->inst[1]);
                std::memset(p->preRing, 0,
                            sizeof(float) * static_cast<size_t>(p->preMask + 1));
                p->prePos = 0;
                p->mixCur = 0.0;
            }
            if (out != in)
                std::memmove(out, in, sizeof(float) * static_cast<size_t>(n));
            return;
        }
        p->bypassed = false;
    }

    /* Feed the shared predelay ring first, so both instances can read
       their own delays out of one history. */
    for (int i = 0; i < n; ++i)
        p->preRing[(p->prePos + i) & p->preMask] = in[i];

    /* A prepared swap starts its fade on a call boundary. */
    if (p->fadePos < 0 && p->pendingSwap.load(std::memory_order_acquire))
        p->fadePos = 0;

    const int a = p->active;
    const auto delayedInto = [&](int inst, float* dst) {
        const int d = p->predelay[inst];
        for (int i = 0; i < n; ++i) {
            const long long t = p->prePos + i - d;
            dst[i] = t >= 0 ? p->preRing[t & p->preMask] : 0.0f;
        }
    };
    delayedInto(a, p->delayed);
    irc_process(p->inst[a], p->delayed, p->wetA, n);

    const bool fading = p->fadePos >= 0;
    if (fading) {
        delayedInto(1 - a, p->delayed);
        irc_process(p->inst[1 - a], p->delayed, p->wetB, n);
    }

    const double mixTarget = p->onT.load(std::memory_order_relaxed)
        ? p->mixT.load(std::memory_order_relaxed) : 0.0;
    const double gain = p->gainT.load(std::memory_order_relaxed);

    double mix = p->mixCur;
    int fp = p->fadePos;
    for (int i = 0; i < n; ++i) {
        mix += (mixTarget - mix) * p->smooth;
        double wet;
        if (fading && fp < p->fadeLen) {
            /* Equal-power: the two tails are decorrelated spaces. */
            const double x = (fp + 0.5) / p->fadeLen;
            wet = p->wetA[i] * std::cos(0.5 * kPi * x)
                + p->wetB[i] * std::sin(0.5 * kPi * x);
            ++fp;
        } else if (fading) {
            wet = p->wetB[i];
        } else {
            wet = p->wetA[i];
        }
        const double dry = in[i];
        out[i] = static_cast<float>(dry * (1.0 - mix) + wet * gain * mix);
    }
    p->mixCur = mix;
    p->prePos += n;

    if (fading && fp >= p->fadeLen) {
        /* Fade complete: the new instance takes over. */
        p->active = 1 - a;
        p->fadePos = -1;
        p->pendingSwap.store(0, std::memory_order_release);
    } else if (fading) {
        p->fadePos = fp;
    }
}

} /* extern "C" */
