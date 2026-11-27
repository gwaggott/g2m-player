# Pitch bend in plain output

Plan, 2026-11-27; the decisions are at the end. Steps 1–5 done; step 6 measured, listening open.

Plain output puts every note on one MIDI channel, and a pitch-bend message
bends the whole channel, so plain output has sent no bends: Bend Mode
Natural drops them, Retrigger turns each semitone of a bend into a new
note, Off drops them. MPE output is unchanged by this plan: each note
already has its own channel and its own bend (`docs/mpe-output.md`, "Pitch
bend").

This plan gives plain output a channel pitch bend, two ways:

- **Natural** (the default, now in plain output too): the channel bend
  follows one note, the lead: the newest note sounding. Right for single
  lines and solos, where nearly all bends are; in a chord the other notes
  move with the lead.
- **Auto** (new): the channel bend follows a note only while it is the only
  note sounding; while other notes sound, bends go by semitone steps as in
  Retrigger. A double-stop bend doesn't drag the held note with it. In MPE
  output Auto is Natural.

Retrigger and Off are unchanged in both outputs. Nothing changes in the
model, the format or the decoder: the decoder already gives each note's
bend (`BendEvent::semitones`, after the dead band) every frame.

## What "sounding" means

A note is sounding from the note-on the Bend Mode sends until its
note-off: after the sustain pedal, so a note the pedal holds counts, and a
frozen note (Sustain Mode Freeze) sounds with its bend stopped, as now.
The synth's release tail after a note-off doesn't count. The channel bend
moves at a note-off only when Natural hands the lead to a note left bent
differently; otherwise it waits for the next note-on or bend, so release
tails aren't bent.

## Bend Range

One range for both outputs: the existing **MPE Bend Range** parameter
is renamed **Bend Range** (symbol `bend_range` and port index unchanged,
so saved projects and presets keep their value), 1–96 semitones, default
12 as now. The decoder already scales every bend to it (`BendEvent::bend`),
so plain output sends that value as the channel bend, as MPE sends it on
the note's channel; Auto sends the part left after the note's step,
`s − step` semitones, by the same formula:

    value = clamp(8192 + lrintf(s × 8192 / range), 0, 16383)

A bend past the range is clipped in Natural; in Auto the note steps
instead (below).

**Match the synth.** The bends are right only when the synth patch's
pitch-bend range equals Bend Range. Many plain synths and patches default
to ±2, so either set the patch's range to 12 or set Bend Range to the
patch's. The README says so next to the parameter, in the MPE and plain
sections, and in the factory preset's description.

**RPN 0** (the decision below): send Bend Range on the plain channel
(`Bn 65 00 64 00 06 range 26 00 65 7F 64 7F`) at the start, when the
range, channel or output mode changes, and when Bend Mode becomes
Natural or Auto, as MPE output already sends it on its member channels.
A synth that honours it matches by itself; for one that doesn't, the
user matches them by hand.

The UI's MIDI card shows the Bend Range stepper in both outputs (it is
greyed in plain output now).

## Natural: the lead note

- The lead is the newest note sounding. A note-on makes it the lead: if
  the channel bend isn't already the note's own bend (the note-on's
  bend, usually none), that bend is sent first, then the note-on. A
  Retrigger or Off note (after a Bend Mode change) wants no bend, so its
  note-on takes the channel to 8192, and its own bends never move it.
  Older notes still sounding jump with it: the cost of one channel.
- The lead's bend events set the channel bend (when the 14-bit value
  changes). Other notes' bend events are dropped.
- The lead's note-off: the lead passes to the newest note still sounding,
  and the channel moves to its last bend (kept per note) at once, if it
  differs (see "Decisions to make"). No notes left: no lead, and the
  bend stays where it is until the next note-on.

## Auto: one note bends freely, more go by steps

Each note has a step (whole semitones, as in Retrigger) and is sent at
its detected pitch + step; the channel carries the rest, `s − step`, only
while the note is the only one sounding.

- **One note sounding:** its bend events set the channel bend to
  `s − step`. When `|s − step|` would pass Bend Range, it steps
  (a retrigger to `pitch + lround(s)`) and the channel carries the rest
  again. Its step doesn't otherwise change, so a bend is smooth.
- **A second note starts** while the channel is bent: before its note-on,
  the channel goes to 8192 (no bend), then the bent note steps to
  `lround(s)` if that isn't its step (a retrigger: the note is struck
  again at the nearest semitone). Then the new note-on (stepped as in
  Retrigger if it is pre-bent).
- **Two or more sounding:** the channel stays at 8192, and each note's
  bends step it as in Retrigger (`bend_step`, ±0.2 semitone hysteresis;
  a step onto a pitch another note is sounding waits).
- **Back to one:** when a note-off leaves one note, the channel takes its
  `s − step` at once, as Natural's hand-over does (a note-on with no
  note-off between doesn't count: the new note follows at once).

A note's mode is still the one it started in (Auto notes and Natural
notes can sound together after a change; the channel bend then follows
the Auto rules for Auto notes and the lead rule for Natural ones: in
practice, the newest note's).

**The risk:** a guitar's earlier notes ring on (open strings, a chord
under a solo), so "the only note sounding" may be rarer than it looks and
Auto may often fall back to steps. Step 6 measures it on real recordings
before Auto is offered as more than an option.

## Where

- `core/`: `BendModeFilter` gains the output (`set_plain(bool)`), the
  bend range for Auto's remainders (`set_bend_range(int)`, the decoder's)
  and `BendMode::autobend` (appended:
  0 natural, 1 retrigger, 2 off, 3 auto). `NoteSink` gains
  `channel_bend(sample, value)`, called only in plain output. MPE output
  is as now (Auto → natural). Per note: `semitones` last seen, and the
  lead order (the newest note-on per pitch: a counter, no allocation).
- `plugin/`: `kBendRange` renamed Bend Range (description: both
  outputs, match the synth), no longer greyed in plain output;
  `bend_mode`'s range to 3 with the Auto label;
  plain output's `channel_bend` writes `En lsb msb` on the channel; a
  channel or mode change sends 8192 on the old channel with the
  notes-off; RPN 0 as above. Factory preset "Plain, smooth bends" (Auto).
- `core/tools/g2m-transcribe`: `--bend-mode natural|retrigger|off|auto`
  (with the existing `--bend-range N`), so plain files with channel bends can be
  written and listened to (without it, no Bend Mode, as before).
- Docs: README (Bend Mode, Bend Range for both outputs and matching the
  synth, the factory preset, "Plain mode sends no pitch bend" goes), `CLAUDE.md`, `docs/release-plan.md`'s
  bend modes pointing here.

## Steps

1. **Done 2026-11-27.** Core: `channel_bend`, Natural's lead in plain output;
   `test_bend_mode` cases for every rule above (break each condition and
   check a test fails, as for the decoder).
   `BendModeFilter::set_plain` (false by default, so the plugin is
   unchanged until step 4) and `NoteSink::channel_bend` (the plugin writes
   `En lsb msb` on its channel; the LV2 test's expected output the same).
   Per note, the last bend and a note-on counter; the hand-over scans the
   128 notes for the newest sounding. A note-on with no note-off between,
   or onto a retriggered note, ends the old note without a hand-over, so
   the channel moves once, for the new note. Five new tests; each of the
   eleven conditions, broken on purpose, fails one.
2. **Done 2026-11-27.** Core: Auto; tests, including the 1 → 2 → 1 transitions, the range
   step, a step onto a sounding pitch, and Sustain holding a note.
   `BendMode::autobend` (3), `BendModeFilter::set_bend_range` (the
   decoder's range, for the rest's 14-bit value; 12 until the plugin sets
   it in step 4) and a count of the notes sounding. A note held by the
   sustain pedal is simply a note sounding to the filter, so it needs no
   test of its own here; the LV2 test covers the pedal in step 4. The
   retrigger code is shared (`move`), so a step onto a sounding pitch
   waits as in Retrigger, tested there. Four new tests; each of the
   sixteen new conditions, broken on purpose, fails one (the `lead_ >= 0`
   guard only under AddressSanitizer).
3. **Done 2026-11-27.** `g2m-transcribe` flags. `--bend-mode
   natural|retrigger|off|auto` runs the events through `BendModeFilter`
   (`--bend-range` sets its range with the decoder's) and writes plain
   MIDI on channel 1, with RPN 0 at time 0 in Natural and Auto
   (`MpeOutput::send_channel_bend_range`, for the plugin too), or MPE; it
   prints the retriggers and channel bends. Without it the output is as
   before. CTest: `--bend-mode natural --mpe` writes the reference's MPE
   file and `--bend-mode off` its plain file, byte for byte. A first look
   with the bundled model on a GuitarSet jazz solo: Natural 3,745 channel
   bends; Retrigger 24 retriggers; Auto 907 channel bends and 26
   retriggers, so other notes were sounding for much of the bending
   (step 6 looks closer).
4. **Done 2026-11-27.** Plugin: Bend Range for both outputs, RPN 0, the channel-change bend reset, the UI
   stepper, the factory preset; the LV2 test compares the plugin's plain
   MIDI with the core's in every bend mode, and counts no allocations.
   Bend Range renamed (symbol and index unchanged) and no longer greyed in
   plain output; Bend Mode 0–3 with Auto; the filter gets plain output and
   the range each run. RPN 0 on the plain channel in Natural and Auto at
   the start (activation), and when the range, channel or output mode
   changes or one of those modes is chosen; ending the notes (a channel,
   mode or model change, re-activation) takes a bent channel back to 8192
   on the channel the notes were on. Factory preset "Smooth bends (plain,
   Auto)", appended so the programs before it keep their numbers. LV2
   tests: every Bend Mode in both outputs equals the core's filter, plain
   Natural's default output, Auto at Bend Range 48, the RPN 0 rules and the
   unbend; no allocations in `run()`; each new condition in the plugin,
   broken on purpose, fails one. The ASan build can't build the LV2 bundle
   (DPF's ttl generator loads the plugin without the ASan runtime first),
   so the LV2 test runs in the plain and local builds; the core tests pass
   under ASan.
5. **Done 2026-11-27.** Docs (with step 4: README, `CLAUDE.md`, the release plan, `docs/mpe-output.md`).
6. **Measured 2026-11-27; listening open.** Listen and measure on real DI recordings with the bundled model: how
   often Auto steps because another note sounds, and how Natural's lead
   sounds in chords; adjust (for example, treat a note as gone sooner)
   only on what this shows.

   The bundled model (dead band 0.25 semitone, Bend Range 12) on 12
   GuitarSet solos, 6 GuitarSet comps and 7 IDMT-SMT-GUITAR DI licks,
   through `g2m-transcribe --events` and `--bend-mode`, sustain pedal off.
   Time-weighted over each note's bend (a one-off script, not kept):

   | | solos | comps | DI licks |
   |---|---|---|---|
   | notes sounding (note-seconds) | 309 s | 157 s | 39 s |
   | time with 2+ notes sounding | 29 % | 88 % | 17 % |
   | bent time (≥ 0.25 semitone) | 56.8 s | 47.6 s | 2.2 s |
   | … of it with the note alone (Auto smooth) | 31 % | 3 % | 91 % |
   | … of it on the lead (Natural sends it) | 67 % | 34 % | 91 % |
   | other notes dragged by Natural (note-seconds) | 26 s | 45 s | 0 |
   | bent ≥ 0.5 semitone | 8.8 s | 5.5 s | 0.5 s |
   | … of it alone / on the lead | 23 % / 57 % | 5 % / 39 % | 86 % / 86 % |
   | channel bends, Natural / Auto | 36,284 / 17,000 | 16,351 / 1,252 | 1,813 / 1,804 |
   | retriggers, Retrigger / Auto | 226 / 217 | 204 / 207 | 7 / 5 |

   What it shows:

   - **Single-note DI lines** (the licks): nearly all bending is on a note
     alone, so Natural and Auto send the same bends, and both are right.
   - **The solos ring**: other notes are still sounding (by the decoder's
     note-offs) for 29 % of the time, so Auto sends a bend smoothly for
     only a third of the bent time and steps the rest (about as many
     retriggers as Retrigger). Natural sends two thirds, but bends the
     ringing notes with it for 26 note-seconds.
   - **Chords**: most "bends" in the comps are small (≥ 0.25 and < 0.5
     semitone for 42 of the 47.6 s), probably the pitch-offset head's
     noise or real detuning on chord notes. In plain Natural the newest
     note's wobble moves the whole chord (45 note-seconds dragged); Auto
     barely bends (3 %), so chords stay in tune.
   - Retriggers are rare in every mode (a few per minute): most bends
     stay within a semitone's hysteresis.

   No change made yet: the defaults stay as decided. Open, for the user:
   whether Natural is the right default for plain output given the chord
   wobble (Auto, or a larger dead band for plain output, would avoid it);
   and whether Auto should count a note as gone sooner (for example a
   note older than the newest by more than some time, since the player
   has no level per note) so solos bend smoothly more often. Files to
   listen to (plain MIDI in each mode, Bend Range 12) are in the
   git-ignored `tmp/listen/plain-bends/`.
## Decisions (the user, 2026-11-27)

- **One Bend Range** for both outputs, default 12, matched to the synth by
  the user (above).
- **RPN 0 in plain output:** sent, as MPE does (a synth set by hand to
  another range is reset to Bend Range).
- **Natural's lead hand-over** at the lead's note-off: the new lead's bend
  is sent at once, after the note-off. It differs only when the old lead
  ended still bent, and a wrong pitch that lasts is worse than a bent
  release tail.
- **Default Bend Mode:** stays Natural, which now bends in plain output
  too (a change for anyone using plain output with a bend model).
