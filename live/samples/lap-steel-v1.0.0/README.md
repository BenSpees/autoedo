# Lap Steel v1.0.0 — private package

Thinned from **Indiginus THE STEEL**, a commercially licensed Kontakt library.

> **This package is licence-restricted and is NOT part of the CC0/CC-BY acoustic pack.**
> Keep it out of any public repo, release artifact or downloadable build. See `NOTICE.txt`.
> It is packaged separately for exactly this reason — do not merge it into
> `acoustic-instruments`.

## Contents

* **44 files, 12 MB.** Mono, 44.1 kHz FLAC.
* **22 notes, C3–F♯6**, sampled every **whole step** (max gap 2 semitones).
* **2 round robins per note**, one down-strike and one up-strike.
* Long steady sustains, median 11.7 s. No loop points.

Naming matches the acoustic pack: `<Note>[_rrN].flac`, sharps as `s`, middle C = C4.

## What was selected, and why

The source library is ~3,900 `.ncw` files and 3.9 GB. Almost none of that is pitch
information — it is variation this engine cannot use:

| Axis | Library | Kept |
|---|---|---|
| Articulations | 8 | 1 |
| Takes per note | up to 10 | 2 |
| Strings | same pitch on ST1–ST6 | best one per pitch |
| Format | NCW (~uncompressed) | mono FLAC |
| Channels | stereo | mono |

The kept articulation is **ST-Both**: the long steady sustains, with up-strikes and
down-strikes merged as round robins. The library's manual states the instrument
"automatically alternates between down strikes and up strikes with each note played", so
those are two halves of one articulation, not two voices.

Rejected, with reasons measured rather than assumed:

- **US / DS** are *slides*, not strikes. A note arrives ~110 cents off pitch and takes
  ~350 ms to settle — unusable in a microtonal host that retunes by playback rate.
- **PL** (120 ms attack, steady pitch) is the viable alternative if a softer baked-in
  attack is wanted. It costs the ability to adjust that attack later.
- **HRM** harmonics sound an octave above their names, per the vendor's own manual.

## Octave

The source names octaves with middle C as **C3** (Kontakt convention); this package is
relabelled to C4 = middle C. Every file was verified against its measured fundamental
after relabelling — worst deviation 21 cents, no octave outliers.

If you re-run `steel-thin.py` yourself, this correction is already built in
(`SOURCE_OCTAVE_OFFSET`).

## Mellow attack

There is no slow-swell articulation to extract: the volume-pedal swell is Kontakt's
"Smooth Attack" envelope, not recorded audio. Apply an attack envelope in the engine.
