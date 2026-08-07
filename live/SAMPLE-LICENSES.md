# Sample licences

AutoEDO ships no samples. Its sampler is handed a directory by the librarian —
Treebrain, via `samplePath` — and plays whatever instrument folders it finds
there. So the licence question here is not "what do we distribute" but "what
must we say while we are playing it".

For all but four instruments, nothing. The Xentar library's original nine sets
and the eight multisampled sets of the plucked-acoustics pack are CC0: public
domain, no attribution, no conditions.

## The four that need a line

`dulcimer`, `koto`, `shamisen` and `sitar` are rendered from the **Fluid (R3)
General MIDI SoundFont by Frank Wen** and are licensed **CC BY 3.0**, which
requires attribution in any application that presents them to a user.

These four are also the widest sets in the library — every semitone from A0 to
C8, 88 zones each — which is why `AE_SMP_MAX_ZONES` is 128 rather than the 64
that covered everything before them.

The line, which appears beside the instrument picker whenever one of the four
is the selected sample instrument (`showSampleCredit` in `web/index.html`):

> Derived from the Fluid (R3) General MIDI SoundFont by Frank Wen, licensed
> CC BY 3.0.

Full text: <https://creativecommons.org/licenses/by/3.0/legalcode>

## Why the names are hard-coded here

The engine deliberately has no instrument list — `ae_sampler_list` discovers
folders, so adding an instrument is dropping a directory in the cache and
nothing else. That is the right design for playback and the wrong one for
licensing, because a discovered folder carries no provenance. Matching the
four names in the UI is the smallest thing that keeps the obligation attached
to the instrument rather than to a page someone has to remember to update.

If the librarian ever starts sending per-instrument metadata (Treebrain
already computes it — `sampleInstrumentCredit()` in `core/src/SampleLibrary.cpp`,
served in the `/api/status` samples block as each set's `credit`), prefer that
over this table and delete it.

Provenance for every set, including the ones rejected on licence grounds, is
in Treebrain's `wav/LICENSES-plucked.md`.
