# MPE output with a rule-based string allocator

Steps 1 and 3 of the hand-off. Step 1 (below, to "Measurements") gave MPE
output with a rule-based allocator and no model change. Step 3 (from
"Model-driven MPE") reads `.g2m` format version 3: the allocator then
also takes the model's string probabilities and onset type at each
note-on, and legato notes overlap the note they continue. Format version
4 ("Pitch bend") adds the pitch-offset head: each note's bend, every
frame, on its channel.

Step 1 of the player's part of the MPE plan (kept with the training
code), done 2026-10-29 in commit `0cd6a43`. The
model is unchanged: notes are still note-on (with velocity) and note-off
per pitch. What's new is where they go.

## Output modes

The plugin has an **Output Mode** port (index 16, symbol `output_mode`):

- **Plain** (0, the default): as before, every note on the **MIDI
  Channel**. The output is byte-identical to the player before this
  change: the plugin test compares it with the core `Transcriber`'s
  events, and `g2m-transcribe` without `--mpe` still writes
  `reference.py`'s file byte for byte.
- **MPE** (1): an MPE lower zone. Channel 1 is the manager channel (unused
  for now), and string *s* (1 = high E … 6 = low E) is member channel
  *s* + 1, so channels 2–7, highest string first, as gp2midi's `--mpe`
  writes. The MIDI Channel port is unused in this mode.

and an **MPE Bend Range** port (renamed Bend Range 2026-11-27, for plain output too: `docs/plain-bends.md`; index 17, `bend_range`, 1–96 semitones,
default 12): the pitch-bend range sent as RPN 0, and the range the
model's bends are scaled to (format version 4, "Pitch bend"), whatever
the file's `bend_range`. A synth that ignores RPN 0 assumes MPE's default
of ±48, so set this to 48 for it and the bends are scaled to match. The ports are appended
after the existing ones, so saved sessions keep their port indices; the
URI is unchanged and the LV2 minor version went from 1 to 3 (odd:
development).

### Messages

All controller messages are sent at the start of the run() buffer
(offset 0), before any note:

- **Configuration** (the first run after activation, and on switching to
  MPE): the MPE Configuration Message on channel 1 with 6 member
  channels, then RPN 0 = the bend range on channels 2–7. The MCM resets
  every member's range to ±48, so RPN 0 must follow it. Each RPN is
  CC 101 / 100, data entry CC 6 (and CC 38 = 0 for RPN 0), then the null
  RPN (CC 101 / 100 = 127), exactly as gp2midi writes them:
  - MCM: `B0 65 00`, `B0 64 06`, `B0 06 06`, `B0 65 7F`, `B0 64 7F`;
  - RPN 0 on channel *c*: `Bc 65 00`, `Bc 64 00`, `Bc 06 range`,
    `Bc 26 00`, `Bc 65 7F`, `Bc 64 7F`.
- **Bend range change** in MPE mode: RPN 0 alone on every member; the
  notes keep sounding.
- **Leaving MPE**: the note-offs, then the MCM with 0 member channels,
  which turns the zone off so the synth stops treating channel 1 as a
  manager.
- **Ending notes**: a change of output mode ends the notes sounding, as
  they were sent (plain notes on the old channel, MPE notes on their
  strings' channels), at offset 0, before the configuration. So do a
  model swap and re-activation, in either mode, and a change of MIDI
  Channel in plain mode, as before.

Notes keep the timing of plain mode: an event decided in frame *k* is on
that frame's last sample. When the allocator steals a string (below),
the stolen note's note-off goes out at the same offset just before the
new note-on.

## The string allocator

`core/include/g2m/string_allocator.h` (`StringAllocator`) chooses a
string for each note at its note-on, and the note keeps it until its
note-off: a note never changes channel (agreed in the plan). It is
portable, keeps fixed-size state and never allocates. `mpe.h`
(`MpeOutput`) turns its choices into MIDI.

The rule, precisely enough to port to the training code's reference:

**Strings.** String *s* = 1 … 6 has open pitch
`open = [64, 59, 55, 50, 45, 40]` (standard tuning, MIDI numbers, string
1 first) and plays the pitches `low(s) … high(s)` with
`high(s) = open(s) + 24` (24 frets) and `low(s) = open(s)`, except
`low(6) = 38`: the model covers 38–88, and a guitarist plays 38–39 in
drop D. So string 6 has 38–64, 5 has 45–69, 4 has 50–74, 3 has 55–79,
2 has 59–83 and 1 has 64–88.

**Hand span.** The fret of pitch *p* on string *s*, for the span, is
`max(p - open(s), 0)`: the drop-D notes 38–39 count as open (fret 0),
as if the string were retuned. Fret 0 is an open string and needs no
finger. A set of notes, each on its string, *fits the hand* when the
largest and smallest of their frets **other than 0** differ by at most
**4** (index finger on fret *n*, little finger on *n* + 4). A set with
no fretted note, or one, always fits.

**State.** The notes sounding, each with its string, and the order in
which they started.

**Note-on of pitch *p*** (with optional finite weights `w[s]`, one per
string, and an onset type: picked, legato or slide; see "Model-driven
MPE" for where they come from):

0. If *p* is already sounding (the decoder doesn't do this, but a caller
   might), it is ended first, with a note-off on its string.

   **Legato** (format version 3): if the onset type is legato or slide,
   the candidates are the strings sounding a note *q* with
   `1 <= |p - q| <= reach` (reach 5 for legato, 12 for a slide) that can
   play *p*. If there are any, the string with the highest weight wins,
   then the smallest `|p - q|`, then the note that started latest. *p*
   takes that string and *q* is ended: in MPE output the note-on of *p*
   goes first, then *q*'s note-off on the same channel. *q*'s own
   note-off from the decoder later is ignored. Otherwise the note is
   placed as a picked one, below.
1. *Free* strings are those with no note sounding. A string *can play*
   *p* if `low(s) <= p <= high(s)`.
2. Candidates, in order of preference, the first non-empty set winning:
   1. **shape**: free strings that can play *p* and where *p*, together
      with all the notes sounding, fits the hand;
   2. **range**: free strings that can play *p*;
   3. **free**: any free string;
   4. **steal**: none is free (6 notes are sounding). The note that
      started earliest is ended (note-off on its string) and its string
      is taken. Its own note-off from the decoder later is ignored.
3. Within the winning set (steps 1–3), the string with the highest
   weight; with no weights, or equal weights, the **lowest string**: the
   highest string number, 6 before 5 … before 1.

**Note-off of pitch *p*:** frees its string; nothing is sent if *p* isn't
sounding (it was stolen).

In plain words: each new note goes on the lowest free string that can
play it while the hand can still hold the notes ringing; if no string
lets the hand hold them, on the lowest free string that can play it; if
none can, on the lowest free string at all; if all six are busy, on the
string of the oldest note, which is cut. Notes in the same block arrive
lowest pitch first (the `Transcriber`'s order), so a chord fills the
strings upwards from the bass.

Why steal rather than drop the new note: a new onset is what the player
heard last, and the oldest note has usually decayed most; this is what
synths do when they run out of voices. Why "free" before "steal": no
note is cut while a channel is idle, at the cost of an unrealistic
string. In plain mode none of this applies: there is one channel and
every note gets through.

**Weights** are the model's string probabilities at the onset (step 3
of the hand-off, below). They only rank the strings within the step that
wins, so playability still comes first.

The tests (`core/test/test_string_allocator.cpp`) reach every step and
branch: each condition in `string_allocator.cpp` and `mpe.cpp` was
broken on purpose (the span's `<=`, open strings in the span, the drop-D
range and fret, the tie order, the weight comparison, the steal order,
each fallback step, the range bounds, the repeated note-on) and each
change fails at least one test.

## Measurements

With `data5-e120` (the current best model) on real recordings, using
`g2m-transcribe --mpe`, which prints how many notes each step placed:

| File | Notes | Shape | Range | Free | Steal |
|---|---|---|---|---|---|
| GuitarSet Jazz2 p05 solo | 143 | 123 | 20 | 0 | 0 |
| GuitarSet Funk1 p05 solo | 75 | 64 | 11 | 0 | 0 |
| GuitarSet BN1 p00 comp | 187 | 138 | 43 | 6 | 0 |
| GuitarSet Rock2 p00 comp | 541 | 373 | 120 | 47 | 1 |
| Here Comes the Sun v10 (render) | 691 | 563 | 101 | 27 | 0 |
| Sultans of Swing lead (render) | 2,011 | 1,572 | 405 | 34 | 0 |
| Sultans of Swing rhythm (render) | 9,352 | 3,677 | 3,478 | 2,155 | 42 |

- Solo lines are almost all placed within the hand span; dense rhythm
  parts much less so. The rule's weak point is visible there: a high note
  alone takes the lowest string that can play it, at a high fret (E4
  goes to string 6 at fret 24), and a bass note arriving while it rings
  then finds its only string busy ("free" step).
- Steals mean more than six notes at once; with the rhythm part these
  are mostly the model's short false notes during strummed chords.
- The time per block is unchanged (360–410 µs per 64-sample block on an
  i9-12900F, as before): the allocator is a few loops over six strings
  per note-on.

For comparison only (not kept), the same files with "the free string
whose open pitch is nearest the note" (roughly the lowest fret) instead
of "the lowest string" in every step: the rhythm part gave 5,392 shape /
3,691 range / 227 free / 42 steal, the lead 1,618 / 385 / 8 / 0, but
Rock2 comp got worse (266 / 262 / 12 / 1). Neither rule knows the true
string. GuitarSet's per-string annotations could measure how often each
rule picks the string actually played, once the dataset's MPE labels
(`mpe_midi`, being added in dataset-generation) exist.

In phase 1 the string choice changes nothing audible on most MPE synths
(no bends yet): any valid assignment works, and the stream is valid
whatever the rule. It starts to matter with per-note bends and with
synths whose timbre depends on the channel.

## Tests

- `core/test/test_string_allocator.cpp`: the rule on hand-made sequences.
- `core/test/test_mpe.cpp`: the configuration bytes; notes on their
  strings' channels; a steal's note-off before the note-on; random
  sequences and the test vectors' events giving a valid stream (only
  member channels, at most one note per channel, note-offs matching)
  with every note-on through at its sample; no allocation.
- `lv2-plugin/test/test_lv2_plugin.cpp`: the plugin in MPE mode against
  the core `Transcriber` + `MpeOutput` on the test audio in random
  buffer sizes; the configuration at activation and re-activation; RPN 0
  alone on a range change; the MIDI Channel ignored in MPE mode; plain →
  MPE → plain with notes held; a model swap in MPE mode; and no
  allocation inside the plugin's `run()` in either mode (the test host
  counts `operator new`, leaving out its own worker queue). The existing
  plain-mode tests are unchanged and pass.

## Model-driven MPE (format version 3)

Done 2026-10-31 on branch `claude/mpe-player-v3` (from training's
`claude/mpe-g2m-v3`, commit `dd63ea5`, which defines the format;
the format spec), not yet merged.

### Loading and the heads

- The loader reads versions 1–3. Version 3 adds `heads` to the header (the
  `string` head over strings 1–6 and the `onset_type` head over picked,
  legato, slide; either, both or none), their tensors after `head.bias`,
  and the decoder's `legato_threshold` and `slide_threshold`. A head of
  another name, or with other classes, is refused: a new head is a new
  format version. Versions 1–2 load as version 3 with no heads and both
  thresholds 0.5; their notes are unchanged and every note-on is picked.
- The network computes a pitch's heads only when the decoder asks: their
  rows of the head matrices, on the features of the block just run, then a
  softmax per head (`Network::heads`). The decoder asks in frames with an
  onset edge, with the first frame at or above the fallback threshold
  (fallback on), or with a note start, and keeps each note-on's heads from
  the frame its velocity comes from, as `reference.py` does: the edge for
  a gated onset, the first high frame for a late note, else the start
  frame. On the version 3 test vectors that is 503 of 16,000 pitch-frames
  (3 %).
- Each note-on (`NoteEvent`) carries the six string probabilities and the
  onset type: slide if its probability is at least `slide_threshold`,
  else legato if at least `legato_threshold`, else picked.
- `Transcriber::last_heads` computes every pitch's heads for the last
  block, for tools (`g2m-transcribe --outputs`) and tests.

### Output

- **MPE:** the allocator gets the string probabilities as weights and the
  onset type; legato and slide notes continue a note on its string as an
  overlap (the rule above). The channel is still chosen at the note-on
  and never moves.
- **Plain:** unchanged. Legato and slide notes are ordinary note-ons, and
  the note they would continue ends at its own note-off.

### The legato rule: why

Neither the spec nor the reference says which note a legato note-on
continues; it is the allocator's choice (`export-format.md`, "MPE heads
at the note-on"). The rule above (step 0) was proposed here and accepted
on 2026-10-31; training mirrors it in its allocator on
`claude/mpe-g2m-v3`. The reasons:

- A hammer-on, pull-off or slide stays on one string, so the note it
  continues is on a string that can play the new pitch.
- Reach: a hammer-on or pull-off is within the hand (from an open string
  to fret 5 at most, so 5 semitones, the hand span plus one); a slide
  goes as far as the pitch-bend range (12), which a later glide must fit.
- Of several candidates, the string head knows best (it was trained on
  where the note is played); without weights, or with equal ones, the
  nearer pitch, then the newer note, which is what a player usually
  continues.
- The continued note is ended at once (its note-off follows the new
  note-on at the same sample) because a string sounds one pitch. A synth
  in mono-legato mode sees a note-on while a note is held on that
  channel and slurs.
- With no candidate (nothing sounding in reach) the note is placed as a
  picked one, rather than dropped or forced: a false legato costs nothing.

It doesn't change what `reference.py` produces (string probabilities and
onset type per note-on).

### Checking against the reference

- Test vectors: `make_vectors.py` writes `mpe`, a small random version 3
  model with both heads, with the reference's outputs (`[blocks, P, 12]`)
  and events with their heads; `small` and `odd` stay version 2. The
  decoder fed the reference's outputs gives its events, string
  probabilities and onset types exactly. The network's notes and heads
  are within 1.8e-7 and 2.1e-7.
- `g2m-transcribe --outputs --events` against `reference.py --outputs
  --events` on `small.wav` with the `mpe` model: identical events, onset
  types and MIDI; string probabilities within 8.9e-8.
- A real model: `mpe-on-ow` (best epoch 113, both heads, exported at
  `dd63ea5` with `fallback_frames` 2 and `fallback_threshold` 0.7), with
  `reference.py --outputs --events` against `g2m-transcribe` on six
  recordings:

  | File | s | Note-ons | Legato / slide | Outputs max diff | String probs max diff | Events, MIDI |
  |---|---|---|---|---|---|---|
  | GuitarSet Jazz2 p05 solo | 34.9 | 141 | 7 / 15 | 8.9e-6 | 1.1e-6 | identical |
  | GuitarSet Rock2 p00 comp | 27.0 | 539 | 0 / 4 | 4.3e-6 | 2.1e-6 | identical |
  | GuitarSet BN1 p00 comp | 22.3 | 151 | 0 / 2 | 1.9e-6 | 8.3e-7 | identical |
  | GuitarSet Funk1 p05 solo | 25.3 | 71 | 1 / 10 | 8.0e-6 | 9.5e-7 | identical |
  | Here Comes the Sun v10 (render) | 77.8 | 664 | 0 / 14 | 1.4e-5 | 3.5e-6 | identical |
  | Sultans of Swing lead (render) | 332.9 | 1,936 | 50 / 56 | 1.2e-5 | 5.0e-6 | identical |

  Every event, velocity and onset type is identical (3,570 note-ons); the
  outputs (notes and all heads, every pitch and block) agree to 1.4e-5,
  as the version 2 models did (1.0e-5).
- Each new branch of the decoder (the heads kept at edges, first high
  frames, gated and late starts; slide before legato) and of step 0 (the
  reach per type, the range, the weight, distance and age order, ending
  the continued note, the legato note counting as new) was broken on
  purpose, and each change fails a test.

### Measurements with `mpe-on-ow`

How the strings were chosen (`g2m-transcribe --mpe`), against the
rule-based allocator alone with `data5-e120` from step 1 (the models
differ, so the note counts do too):

| File | Note-ons | Legato | Shape | Range | Free | Steal | Step 1: free / steal |
|---|---|---|---|---|---|---|---|
| GuitarSet Jazz2 p05 solo | 141 | 11 | 122 | 8 | 0 | 0 | 0 / 0 |
| GuitarSet Funk1 p05 solo | 71 | 7 | 63 | 1 | 0 | 0 | 0 / 0 |
| GuitarSet BN1 p00 comp | 151 | 2 | 137 | 12 | 0 | 0 | 6 / 0 |
| GuitarSet Rock2 p00 comp | 539 | 3 | 364 | 157 | 9 | 6 | 47 / 1 |
| Here Comes the Sun v10 | 664 | 9 | 628 | 25 | 1 | 1 | 27 / 0 |
| Sultans of Swing lead | 1,936 | 81 | 1,577 | 261 | 13 | 4 | 34 / 0 |
| Sultans of Swing rhythm | 8,700 | 9 | 5,521 | 2,859 | 261 | 50 | 2,155 / 42 |

- With the string head's weights, the "any free string" fallback that
  the lowest-string rule hit on dense parts (a high note on a low string
  blocking a later bass note) almost disappears: 261 instead of 2,155 on
  the rhythm part.
- Legato continuations are a few percent of the notes, mostly in solo and
  lead lines. Legato and slide note-ons with no note to continue in reach
  are placed as picked (e.g. Sultans lead: 106 legato / slide note-ons, 81
  continuations). The onset-type head says "slide" more often than
  "legato" on this model.
- Time per block (`g2m-bench`, single thread, i9-12900F, with a GPU
  training run going): on the Sultans lead recording (249,659 blocks),
  `mpe-on-ow` 371 µs mean, 367 median, 407 p99; `data5-e120` 367 / 364 /
  402. On noise, 382 µs median for both. The heads cost about 1 %
  (they're computed for a few pitch-frames per note), and the player stays
  at 28 % of the 1,333 µs budget.

## Pitch bend (format version 4)

Done 2026-10-31 on branch `claude/mpe-g2m-v4` (training's `7a9a0bc`
defines the format: the format spec's "Pitch bend" and
"MPE output", and the reference).

### Loading

- Version 4 adds **curve heads** to `heads`: `{name, curve: {unit, min,
  max}}`, one value per pitch per frame, clamped to `[min, max]`. The
  reader takes them generically (`HeadSpec::curve`, `ModelConfig::
  curve_heads`), so a damping curve needs only its name accepted; the one
  known now is `pitch_offset` in semitones (−12 to 12). An unknown name or
  unit is refused, as is a curve head before version 4.
- The decoder's `bend_dead_band` and `bend_range` (1–96) are read from a
  version 4 file; earlier versions get the defaults (0.25, 12) and no
  bends. Their notes, heads and MPE output are exactly as before (checked
  below).

### Decoding

As the spec and `reference.py`'s `Decoder` do it:

- A note's bend is read at **its own pitch** in **every frame** from its
  note-on to the frame before its note-off. `Network::curves` computes
  only the curve heads' rows, and the decoder asks only for pitches with
  a note starting or held (on the v4 test vectors: 9,266 of 16,000
  pitch-frames, as that model's notes are long).
- The value (`bend_value` in `decoder.h`): `s = 0 if |o| < dead band else
  o`; `value = clamp(8192 + lrintf(s × (8192 / bend_range)), 0, 16383)`,
  the division and the product in `float`.
- A note-on carries its frame's value and `s`; a note held from an
  earlier frame gets a bend event when its value differs from the last it
  carried or sent. Bend events come after the frame's note-ons, lowest
  pitch first.
- API: `NoteEvent` has `has_bend`, `bend`, `semitones`; bend events go to
  a new `EventSink::bend(BendEvent)`, which does nothing unless
  overridden. Bends are not a kind of `NoteEvent`: every consumer reading
  velocity 0 as a note-off stays correct, and plain output ignores bends
  with no code.

### Output

- **MPE** (`MpeOutput`): the message order of the spec's "MPE output",
  byte for byte: on a note-on, the repeated note's or the stolen note's
  note-off, then the note-on's bend `Ec lsb msb` if it differs from the
  channel's last, then `9c pitch velocity`, then a legato continuation's
  note-off. A bend event goes to the note's channel if the allocator has
  the note on a string and the value differs from the channel's last;
  otherwise nothing (a note continued by a legato note keeps its
  decoder note, and its bends, until its note-off, which is ignored too).
  Each channel's last bend starts at 8192 and is forgotten again whenever
  the configuration is sent (`reset_bends`).
- **The MPE Bend Range port** overrides the file's `bend_range`: it is
  sent in RPN 0 and the decoder scales the bends to it, so a synth fixed
  at ±48 bends by the right amount.
- **Plain**: no bends, as the spec says (following one note's bend in
  plain MIDI is step 2 of the hand-off, not specified yet).
- `g2m-transcribe`: `--events` lists the note-ons' `bend` and `semitones`
  and the bend events (as `reference.py --events`); `--mpe` writes the
  bends; `--bend-range N` (or `--set bend_range=N`) sets the decoder's
  range and the RPN 0 sent (default: the model's); `--decode REF.f32`
  skips the network and decodes `reference.py --outputs`' outputs, for
  exact comparisons.

### Checking against the reference

- **The decoder and the MPE writer are exact.** On the test model of the
  training session (`test-pitch-offset-short.v4.g2m`, 10 s of a bent lead
  line, `paranoid-lead-10s.wav`), `g2m-transcribe --decode` on the
  reference's outputs gives its 12,432 events (5 notes, 12,422 bend
  events) identically, its plain MIDI and its MPE MIDI byte for byte.
  The same holds on the player's own v4 test vectors (`bend`: 7,283 bend
  events, a non-default `bend_range` 24 and dead band 0.3; a CTest
  compares the MPE file with `reference.py --mpe`'s).
- **With the network**, on that model and WAV: outputs within 8.6e-6, the
  plain MIDI identical, the five notes identical, but 7 of 30,014
  note-frames (0.023 %) get a different bend value, so the bend events
  and the MPE MIDI differ there. These are threshold flips the spec
  allows ("Checking a port"): a pitch offset 7e-7 from the reference's
  that sits on a rounding boundary (`s × 682.67` = −793.4998 against
  −793.5003, so 7398 against 7399), or on the dead band's edge
  (|o| ≈ 0.25: 8192 against 8363). A 14-bit bend steps every 0.0015
  semitones at ±12, so float32 differences of 1e-6 in the head's output
  cross a step now and then; no tolerance in the format can avoid it. On
  the `bend` test vectors the whole `Transcriber` matches exactly.
- **Version 3 is unchanged:** `strat-heads.g2m` on `strat10.wav` and
  `mpe-on-ow` on GuitarSet Jazz2 give events, onset types and MIDI
  identical to the reference (as in the v3 checks above), and their MPE
  output is byte-identical to `reference.py --mpe` (which uses training's
  Python port of the allocator) on three recordings.
- Each new branch (the dead band's `<`, the range, round half to even,
  the clamp, a bend event only on a change, the note-on's value kept,
  held notes read and ending ones not, the curve's clamp, the channel's
  memory, the note-on's bend, bends for notes not on a string, the reset
  on reconfiguration, the port's range) was broken on purpose and fails a
  test. One guard does not: the plugin's `MidiSink::bend` ignores bends in
  plain mode, but the MPE allocator holds no notes then, so nothing could
  be sent anyway.

### Time per block

`g2m-bench` (single thread, i9-12900F, a GPU training run going), the
same network with and without the pitch-offset head, two rounds each:

| Model | Recording | Notes | Mean | Median | p99 (µs) |
|---|---|---|---|---|---|
| `test-pitch-offset-short.v4` (string, onset type, pitch offset) | Sultans lead, 249,659 blocks | 5 | 368–370 | 365–368 | 401–404 |
| `strat-heads` (v3: string, onset type) | Sultans lead | 836 | 379–380 | 376–377 | 416–418 |
| `test-pitch-offset-short.v4` | paranoid-lead-10s, 7,500 blocks | 5 | 370 | 370 | 402 |
| `strat-heads` | paranoid-lead-10s | 48 | 382 | 376 | 419 |

Both stay at 28 % of the 1,333 µs budget. The v4 test model was trained
only briefly and plays few notes (its five notes on paranoid-lead-10s
carry 12,422 bend events), so its bend path runs little on these files;
the difference between the rows is mostly the class heads at note
starts and run-to-run noise. The pitch-offset head costs 384
multiply-adds per sounding note per block: at six notes ringing, 2,304
against the network's 1,082,752 (0.2 %), and 19,584 (1.8 %, about 7 µs)
even if every pitch were on.

## Open

- **Lowest string or lowest fret.** The agreed rule is "the lowest free
  string"; the measurements above suggest a lower-fret preference fits
  dense parts better. With a string head the weights now decide within
  each step, so this matters only for models without one.
- **The onset-type thresholds** (0.5 by default) are untuned. Legato
  precision on `mpe-on-ow` was about 0.21 (training's `docs/mpe-heads.md`),
  so live, many picked notes may come out as legato overlaps. There is no
  plugin control for them yet; `g2m-transcribe --set legato_threshold=...`
  sets them offline.
- **True strings.** The dataset's MPE labels (`mpe_midi`) now have true
  strings for GuitarSet: scoring the allocator with the heads' weights at
  the decoded onsets against them is the natural next measurement
  (training's `allocator.py` scores it on label notes).
- **Plain mode** has no projection yet (step 2 of the hand-off): plain
  output is the decoder's notes, with no bends.
- **Bend smoothing**: none, as specified; a bend can change every 1.33 ms
  frame. Whether synths need a rate limit or smoothing is to be heard
  live.
- **The manager channel** carries nothing yet. A later global message
  (e.g. sustain) would go there.
- Live check with an MPE synth (e.g. Surge XT or Vital in MPE mode, and
  mono legato for the overlaps) in REAPER or jalv: not done.
