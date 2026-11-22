# Format version 2: the decoder's late fallback

Branch `decoder-fallback`, started 2026-10-26.

The training code added a late
fallback to the decoder and made it `.g2m` format version 2. If the
model misses a note's onset, the note still starts once the model's
frame output stays high. The format spec has it in
"Decoder" and "Versions". In brief:
- Two new decoder settings: `fallback_frames` (0 = off) and
  `fallback_threshold`, which must be at least `frame_threshold` when
  the fallback is on.
- New state per pitch:
  - `high`, the frames in a row at or above `fallback_threshold`;
  - `high_velocity`, the velocity output in the first of those frames.
- A pitch with no note on, no onset starting this frame and no gated
  onset waiting starts a note in the frame where `high` reaches
  `fallback_frames`. The note takes `high_velocity`.
- Version 2 headers carry every decoder setting. A version 1 file is
  read as version 2 with the fallback off, which is exactly version 1's
  decoder.
- The weights and the network are unchanged.

Why it matters here: in the REAPER sessions (see
`initial-implementation.md`, step 6), new notes after silence triggered
patchily. The training code's evaluation traced this to missed onsets on pitches
not just heard, and measured the fallback on `data5-e120`'s validation
set (2 frames at 0.7):
- recall within 50 ms rises from 0.48 to 0.80;
- recall for pitches not struck in the previous 3 s rises from 0.15 to
  0.83;
- precision within 50 ms rises from 0.50 to 0.58;
- median latency rises from 3.0 to 7.5 ms, and the 90th percentile
  from 8.6 to 27 ms.

## Changes

- `core/include/g2m/model.h`:
  - `kFormatVersion` 2 and `kMinFormatVersion` 1;
  - `DecoderSettings::fallback_frames` and `fallback_threshold`, off by
    default;
  - `Model::format_version`.
- `core/src/model_file.cpp`:
  - reads versions 1 and 2;
  - in version 2, both fallback settings are required;
  - in version 1 the fallback is off;
  - rejects a file whose `fallback_threshold` is below its
    `frame_threshold` while the fallback is on, as `reference.py` does.
- `core/src/decoder.h`: the fallback, ported line by line from the spec.
  It runs after `end` is decided and before the note-on is emitted, so a
  late start updates `risen`, `age` and `note_on` like any other start.
- `g2m-transcribe --set` accepts `fallback_frames` and
  `fallback_threshold`.
- The plugin needed no change. It runs with the model's decoder
  settings, and its Onset Threshold override keeps the fallback as the
  model sets it.

## Test vectors

`core/test/make_vectors.py` now exports version 2 files with the
fallback on: `small` uses 2 frames at 0.53, and `odd` 3 frames at 0.51.

With random weights, a pitch's frame probability rarely stays high
without its onset firing too, and it stays near 0.5. At first the
fallback never fired. So the vectors now:
- lower the onset logits of a few pitches (`quiet_onsets`: pitch 3 of
  `small`, pitches 1, 4 and 6 of `odd`), so those pitches often sound
  with no onset;
- use fallback thresholds that the frame probability reaches (around
  its 90th percentile).

The fallback now adds 15 note-ons in `small` (42 in all) and 6 in `odd`
(169). `small.wav` gives 29 note-ons, enough for the plugin tests.
`small`'s gate went from 6 to 3 frames, because with the new onsets only
a 3-frame gate had onsets expiring exactly at the gate.

Tests added or changed:
- the loader reads version 2's fallback settings and a version 1 file
  (fallback off);
- it rejects version 3 and version 0, a version 2 file with a fallback
  setting missing, and a threshold below the frame threshold;
- `late_fallback_adds_notes`: turning the fallback off gives fewer
  note-ons on both vectors;
- `decoder_settings` turns the fallback off too when it checks that an
  impossible onset threshold gives no notes.

Mutation check of `decoder.h`, every condition broken in turn:
- All 11 non-equivalent mutations make tests fail. These are the six
  from the first plan plus five for the fallback: no `start` block, no
  `note_on` block, `ends` ignored, the gated velocity used, and
  `high_velocity` taken every frame.
- Two mutations can't change behaviour, so no test can catch them:
  - **Dropping `waiting` from the block.** The fallback threshold is at
    least the frame threshold, so a frame probability that high makes
    the pitch active, and an active pitch confirms or starts a gated
    onset at once. `waiting` is never true while `high` > 0.
  - **`high >= fallback_frames` instead of `==`.** While `high` keeps
    counting, a note that is on can't end except by a re-strike, which
    is itself a start. So once a late start has been blocked, the
    fallback stays blocked until `high` resets.

All tests pass, including under the sanitizers.

## Results (2026-10-26)

Model `data5-e120` (format 2,
fallback 2 frames at 0.7), copied to `models/data5-e120.g2m`. Compared
with `reference.py --outputs` on the seven files from step 4 of the first
plan:

| file | s | notes | largest output diff | MIDI |
|---|---|---|---|---|
| guitarset Jazz2 p00 solo | 20.5 | 131 | 2.6e-6 | identical |
| guitarset Rock2 p05 comp | 45.2 | 389 | 4.2e-6 | identical |
| idmt Lick1 LP_KN | 11.7 | 18 | 2.7e-6 | identical |
| idmt Lick2 AR_KN | 14.4 | 33 | 2.3e-6 | identical |
| Sultans of Swing track 3 | 203.6 | 507 | 1.8e-5 | identical |
| Here Comes the Sun v10 | 78.8 | 691 | 1.6e-5 | identical |
| Design for Life DI | 100.4 | 460 | 5.2e-6 | identical |

- The MIDI is byte-identical on all seven files: 2,229 notes, with the
  fallback on.
- The mean output difference is 3–8e-8. The largest is 1.8e-5, against
  1.0e-5 for `data4-big-wd03-e120`: different weights, still float
  rounding, and no event differs.
- Time per block is unchanged: 355–426 µs.

What the fallback does on these files. `g2m-transcribe --set
fallback_frames=0` gives the notes without it. Scoring uses the training
project's matching (`metrics.py`): same pitch, at most 5.2 ms early, and
at most 10 or 50 ms late. The Design for Life DI has no labels.

| file | labels | notes off → on | R@10 off / on | R@50 off / on | P@50 off / on |
|---|---|---|---|---|---|
| guitarset Jazz2 | 100 | 84 → 131 | 0.34 / 0.35 | 0.36 / 0.63 | 0.43 / 0.48 |
| guitarset Rock2 | 325 | 260 → 389 | 0.27 / 0.29 | 0.30 / 0.46 | 0.38 / 0.38 |
| idmt Lick1 | 13 | 7 → 18 | 0.15 / 0.15 | 0.15 / 1.00 | 0.29 / 0.72 |
| idmt Lick2 | 21 | 17 → 33 | 0.29 / 0.29 | 0.29 / 1.00 | 0.35 / 0.64 |
| Sultans of Swing | 364 | 363 → 507 | 0.60 / 0.61 | 0.60 / 0.99 | 0.61 / 0.71 |
| Here Comes the Sun | 412 | 473 → 691 | 0.49 / 0.50 | 0.51 / 0.99 | 0.44 / 0.59 |
| Design for Life DI | – | 221 → 460 | | | |

- Within 50 ms, recall rises on every file: to 0.99–1.00 on the
  rendered tabs and the IDMT licks, and to 0.46–0.63 on GuitarSet.
  Precision rises or holds, so most added notes are real ones,
  recovered late.
- Within 10 ms nothing changes, because the fallback never starts a
  note early.
- MIDI from both settings is in `tmp/listen/data5-e120/`
  (`NAME.fallback.mid`, `NAME.no-fallback.mid`; sources in
  `sources.tsv`) for listening.

Verdict (2026-10-26, from the user): "much better" than without the
fallback.

Still to find out: how the extra 5–30 ms on recovered notes feels live
in the plugin. If it's wanted, a plugin control could turn the fallback
off or change its wait. It isn't added yet, because the model's setting
is the tuned one.
