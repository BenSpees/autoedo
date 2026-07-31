/* irconv -- non-uniform partitioned convolution with a direct head.
   =================================================================
   The ONE convolver library of the Treebrain/AutoEDO rig (v0.4-delta B3):
   vendored byte-identical into both codebases, tested once, so the voice
   lead in AutoEDO and the loop buses in Treebrain run the same verified
   code.

   Scheme (Gardner-style):
     - h[0..B0)   : direct time-domain FIR  => ZERO added latency. This is
                    not an optimization but the point: the AutoEDO lead and
                    harmony are live monitored paths.
     - doubling   : FFT partitions of length L at offset O == L, sizes
                    B0, 2*B0, ... up to Lmax -- each one's output block
                    completes exactly when its first sample is due.
     - uniform    : the tail beyond 2*Lmax as a frequency-domain delay line
                    (one shared forward FFT per Lmax block, one inverse for
                    the summed spectra).

   Real-time contract: irc_create allocates EVERYTHING for max_ir_len;
   irc_process never allocates, locks, or touches the filesystem.
   irc_set_ir re-fills partition spectra (FFT work) and is meant for a
   CONTROL thread -- the IrcPoint layer below swaps instances under a
   crossfade so the audio thread never waits on it.

   The point unit (IrcPoint) is the per-processing-point object the rig
   uses: two convolver instances (IR or predelay changes crossfade between
   them over ~30 ms -- no zipper, no dropout), an integer predelay ring,
   and smoothed mix/gain. C API so the C11 engine links it like the
   shifter. */

#ifndef IRCONV_H
#define IRCONV_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct IrcConvolver IrcConvolver;

/* One mono convolver sized for IRs up to max_ir_len samples and process
   calls up to max_block samples. Returns NULL on allocation failure. */
IrcConvolver *irc_create (int max_ir_len, int max_block);
void          irc_destroy (IrcConvolver *c);

/* Install an impulse response (CONTROL thread: does FFT fills). len is
   clamped to max_ir_len. A NULL ir or len <= 0 installs silence. */
void irc_set_ir (IrcConvolver *c, const float *ir, int len);

/* Clear signal history (audio thread safe; no allocation). */
void irc_reset (IrcConvolver *c);

/* out[0..n) = (in convolved with the IR)[0..n), continuing the stream.
   Zero added latency: out[0] of the first call after reset already carries
   in[0] * ir[0]. n may vary call to call up to max_block. */
void irc_process (IrcConvolver *c, const float *in, float *out, int n);

/* --- the per-point unit -------------------------------------------------- */

typedef struct IrcPoint IrcPoint;

IrcPoint *irc_point_create (int max_ir_len, int max_block, double sample_rate);
void      irc_point_destroy (IrcPoint *p);

/* Load an IR (and its predelay) into the INACTIVE instance and arm a ~30 ms
   crossfade to it (control thread; the FFT fills happen here). Returns
   false -- and changes nothing -- while a previous swap's crossfade is
   still running. A NULL ir with len 0 crossfades to silence (bypass with
   a tail). */
bool irc_point_load (IrcPoint *p, const float *ir, int len, double predelay_ms);

/* Live parameters (any thread; lock-free): wet/dry mix 0..1, wet gain in
   dB (-24..+12), and the on switch (off = dry passthrough, wet tail rides
   out through the mix smoothing). */
void irc_point_set (IrcPoint *p, double mix, double gain_db, bool on);

/* Audio thread: out[0..n) = dry/wet blend, zero added latency on the dry
   AND the wet path. out may alias in. */
void irc_point_process (IrcPoint *p, const float *in, float *out, int n);

/* True while the last irc_point_load's crossfade is still in flight. */
bool irc_point_busy (const IrcPoint *p);

/* Clear all signal history (both instances, predelay, smoothers). */
void irc_point_reset (IrcPoint *p);

#ifdef __cplusplus
}
#endif

#endif /* IRCONV_H */
