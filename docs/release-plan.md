# Plan: a player others can try

Started 2026-11-01 on branch `player-release`. **Status:** DPF chosen
(the user, 2026-11-01). Step 1 is built and tested offline; checking it in
hosts is still to do. The other decisions marked **(open)** are the user's
to make.

The goal is a player package that other people can install and use. The
user's feature list:

1. a better UI: lightweight, clean, cross-platform;
2. an audio output level and mute;
3. a noise gate before the input gain;
4. sustain controlled by a MIDI CC, with three modes: piano, sostenuto, and
   freeze (the current notes are held and new notes are blocked until the
   pedal is released);
5. bend modes: natural, quantised to semitones, quantised with retrigger,
   and off;
6. a package with a model included, and the model still selectable;
7. saveable presets for the parameters.

Below are the design for each feature, then three additions that other
people will need, then the steps.

## Blocker: which model can ship

**Settled 2026-11-22:** `v1-0-alpha.g2m` (step 8), trained on GuitarSet
and the user's own sampled guitar only.

Every current model was trained on data that must not be published. The
manifest that `data5-e120` and every later run read has these pairs
(2026-11-01):

| Source | Pairs | Can a model trained on it be shared? |
|---|---|---|
| GuitarSet | 358 | yes: CC BY 4.0, with credit |
| IDMT-SMT-GUITAR (datasets 1 and 2) | 287 | **no**: academic use only, nothing published |
| gp2midi renders, StratNeckSC (built from IDMT) | 204 | **no** |
| gp2midi renders, MDM Strat | 99 | **no**: private use only |
| gp2midi renders, Nick's LP Studio | 145 | unconfirmed: the library's terms |
| gp2midi renders, ASAT | 32 | unconfirmed: the library's terms |

So a shippable model needs a separate training run on a "public" subset.
Options:

- **A. GuitarSet only.** Clearly allowed, but small (about 3 h, acoustic
  guitar through a hexaphonic pickup). Expect noticeably worse results on
  electric DI than the current models.
- **B. GuitarSet plus the LP Studio and ASAT renders**, once their licences
  are checked and allow training and sharing a model. The tabs those renders
  come from are mostly commercial songs. A model trained on them doesn't
  contain the tabs, but that is the user's call too.
- **C. Ship no model.** The package contains the player only, and users load
  a `.g2m` file themselves. This doesn't meet feature 6.

Whatever is chosen, the training code gets a `--sources` filter (or a
manifest subset) and a run named for its data (for example
`public-gs-e120`). The model is exported like any other run, and its
provenance (sources, licences, credits) goes into the package's
`MODELS.md`. The player work below doesn't depend on this decision: it can
be built and tested with the current private models.

## Framework: DPF (recommended, open)

The plugin is LV2 only today, with no UI of its own (hosts draw generic
controls). Other people will use Windows and macOS hosts, so the plugin has
to support the formats those hosts load.

**Recommendation: [DPF](https://github.com/DISTRHO/DPF)** (DISTRHO Plugin
Framework, ISC licence):

- one plugin class builds LV2, VST3, CLAP, AU (macOS) and a standalone JACK
  app, on Linux, Windows and macOS;
- the UI uses DGL (OpenGL + NanoVG): small, no toolkit;
- it supports CMake and is a git submodule, with nothing to install.

The alternatives are worse fits:

- JUCE is heavy and AGPL or paid;
- iPlug2's Linux support is incomplete;
- CLAP with clap-wrapper and our own UI means more plumbing to write and
  maintain.

What the port changes:

- `lv2-plugin/` is replaced by `plugin/`, a DPF plugin over the same
  `core/`. The real-time rules don't change. DPF has no LV2 worker, so model
  loading runs on a plugin-owned background thread with the same
  build-then-swap.
- **The LV2 URI and the port symbols are kept**, so existing REAPER
  projects load: the URI, `input_gain`, `midi_channel`, `onset_threshold`,
  `output_mode`, `bend_range`, and the model path as state. The offline
  lilv test carries over to check this.

**MIDI out from an audio plugin depends on the host.** This decides where
the player is actually usable, so the README must say it plainly:

- REAPER (every format), Bitwig (CLAP, VST3), Ardour and Carla can route a
  plugin's MIDI output to a synth;
- Ableton Live and Logic can't route MIDI out of an audio-effect plugin;
- VST3 carries note on/off but only limited CC and pitch bend, so MPE output
  is best in CLAP and LV2.

That makes the **standalone app** important: audio interface in, a virtual
MIDI port out, so it works with any DAW or synth. DPF's standalone has JACK
everywhere, and native audio and MIDI on Windows and macOS through
RtAudio/RtMidi. Check its MIDI output on each OS before relying on it (step
1).

## Features

Signal path:

```
in → noise gate → input gain → model → decoder → sustain → bend mode → MIDI out
        └──────────────→ output level / mute → audio out
```

The audio output is the gated input, before the input gain. The user hears
what the gate lets through, at a level set separately from the gain the
model needs.

### 1. UI

One fixed-size, scalable window, drawn with [LVGL](https://lvgl.io) (MIT)
through DPF-Widgets' LVGL wrapper (ISC; the user's choice, 2026-11-08, over
DGL with NanoVG and Dear ImGui, for LVGL's ready-made widgets and themes).
One embedded font, light and dark.

The wrapper (`generic/LVGL.cpp`, DPF-Widgets 730da63) gives each UI
instance its own LVGL state (`LV_ENABLE_GLOBAL_CUSTOM`) and switches to it
before LVGL's timer handler and on resize. A patched copy lives in
`plugin/ui/`, for two gaps found reading it:

- the switch is private, so the UI's own LVGL calls (from
  `parameterChanged`, `stateChanged`) could act on another open instance:
  the copy adds a public `makeCurrent()`, called before them;
- typed text comes from key events, which carry the unshifted key, so
  capitals and accented letters would be lost in a preset name: the copy
  takes text from DPF's character input events.

Two more, found driving the full panel with `xte` on Xvfb (2026-11-08):

- the mouse button is read as a state when LVGL polls, so a click shorter
  than the time between polls (`xte`'s, or a touchpad tap) was lost: the
  copy keeps a press until LVGL has read it;
- queued keys were all reported as pressed, so LVGL took the second for
  the first held down and dropped it (fast typing, "My Lead 2" saved as
  "M"): the copy reports each key pressed, then released.

The first step is a small test UI that checks both in Carla (two instances
open, parameters changed from the host, a name typed with capitals and
accents), before the full panel:

- Model: the bundled models in a list, plus "Load file…" (DPF's file
  browser); the loaded model's name and format; load errors.
- Input: a level meter with the gate threshold drawn on it, gate threshold,
  input gain, and the training range (−17 to −1 dBFS) marked on the meter.
- Detection: onset threshold, and a strip that lights for each note on (the
  current Notes On display, drawn).
- MIDI: output mode (plain / MPE), channel, MPE bend range, bend mode,
  sustain mode, sustain CC, and a sustain indicator.
- Output: level, mute.
- Presets: a menu, Save, Save as, Delete.

Every control is also a host parameter, so a host's generic UI and
automation still work. The UI only reads and sets parameters.

### 2. Output level and mute

Two parameters: `output_level` (−60 to +12 dB, default 0) and
`output_mute` (default off). A short ramp (about 5 ms) on changes avoids
clicks. They affect the audio output only, never the model.

### 3. Noise gate

Parameters: `gate_threshold` (−96 to −20 dBFS; −96 means off, the default)
and `gate_release` (5–500 ms, default 50).

- No lookahead, because the player adds no latency. The gate opens within a
  sample when the peak crosses the threshold, so an attack isn't cut.
- Hysteresis: it closes 6 dB below the threshold, after a fixed 10 ms hold,
  and then fades out over the release time.
- Closed means silence. The model has seen digital silence before notes in
  the renders.

The gate goes in `core/` (`NoiseGate`, header-only, no allocation), tested
on synthetic signals. It is worth checking on a live recording that the
gate removes the false notes noise causes, without delaying onsets: the
"patchy notes after silence" item in `initial-implementation.md`.

### 4. Sustain

A MIDI input port (an atom input in LV2; the formats' normal note input
elsewhere) reads one CC: `sustain_cc`, default 64. The pedal is down at
values of 64 and above. A `sustain` parameter does the same as the pedal,
for hosts that can't send MIDI to an audio plugin, and for automation. Both
are combined with OR. `sustain_mode`:

- **Off.**
- **Piano** (damper): while the pedal is down, note-offs are deferred.
  - A deferred note that is played again gets a note-off and then the new
    note-on.
  - Pedal up sends every deferred note-off.
- **Sostenuto:** only the notes sounding at pedal-down are held, until pedal
  up. Later notes behave normally.
- **Freeze:** at pedal-down, the sounding notes are held and new note-ons
  are blocked.
  - Bends on held notes freeze at their last value.
  - Pedal up sends the held note-offs, and detection resumes with the next
    new onset. Notes the decoder started during the freeze are never sent:
    they would arrive late.

**MPE (open, recommended default):** a channel is a string, so a new note
on a string whose channel holds a sustained note ends that note first, as
on a real guitar. The other choice, letting both notes share the channel,
would make the new note's bends move the sustained one too.

Where: `core/` (`Sustain`, between the `Transcriber`'s events and the MIDI
writers). Tests cover every mode, re-strikes and pedal changes inside a
block. Events at the same sample keep the order: offs, then ons.

### 5. Bend modes

`bend_mode`:

- **Natural:** as now. MPE carries per-note bends; plain carries none.
- **Retrigger:** the bend is rounded to whole semitones, with ±0.2
  semitone hysteresis so vibrato doesn't flicker between steps. When it
  reaches a new semitone, the note ends and a note at the new pitch starts,
  with the original velocity and no pitch bend, on the same channel.
  - It works in plain mode too, and with any synth, with no MPE needed.
  - The new pitch is clamped to 0–127.
- **Off:** no bends are sent. Notes keep their detected pitch.

A Semitone mode (bends in whole-semitone steps without retriggering, MPE
only) was built and then dropped (the user, 2026-11-08): of little use in
practice.

"Retriever" in the request is read as "retrigger".

Where: `core/` (`BendMode`, after Sustain, so a frozen or deferred note
doesn't retrigger), with tests.

### 6. Bundled models

- The package has a `models/` folder inside the plugin bundle (LV2 bundle,
  VST3 `Resources/`, CLAP and AU alongside) with the shippable `.g2m`
  files and `MODELS.md`. At about 5 MB each, a few models are fine.
- The `model` state stores either `bundled:<name>` or a file path, so
  presets and projects stay portable across machines.
- A new instance loads the default bundled model at once, so the plugin
  works when first inserted.

### 7. Presets

- **Factory presets** in the package, for example "Plain, clean DI",
  "MPE synth", "Retrigger lead" and "Pad (freeze)". LV2 hosts list them;
  DPF gives VST3 and CLAP no named list, so the UI's menu shows them too.
- **User presets** are JSON files in the user's config folder
  (`~/.config/guitar-2-midi/presets/`, `%APPDATA%\…`,
  `~/Library/Application Support/…`). The UI saves, loads and deletes them,
  so the same preset works in every host and format.
- A preset holds every parameter, plus the model reference: `bundled:<name>`
  or a path. Loading a preset whose model is missing keeps the current model
  and shows a warning.
- Hosts' own preset systems keep working, because they save the plugin's
  state.

## Additions others will need

These weren't requested, but each one blocks other users:

- **Sample rates.** The plugin is 48 kHz only and emits nothing at other
  rates, while many users run 44.1 kHz. Add a resampler in `core/`: a fixed
  polyphase filter for 44.1 / 88.2 / 96 kHz to 48 kHz, allocated at
  activate.
  - It adds a little latency at 44.1, a fraction of a millisecond. Report it
    to the host.
  - Checks: the model's events at 48 kHz against a 44.1 kHz copy of the
    same audio, through the resampler.
  - **Done 2026-11-08** (step 7): `core/include/g2m/resampler.h`.
    - A polyphase windowed sinc (Kaiser, beta 8, 16 zero crossings each
      side, cutoff 0.95 of the lower rate's Nyquist), for any rate pair
      from 8 to 384 kHz with at most 1,024 phases: every common rate. It's
      built with the model's Transcriber, off the audio thread, and swapped
      in with it.
    - Latency: 17 input samples at 44.1 kHz (0.39 ms), 34 at 96 kHz
      (0.35 ms). Each event goes on the host sample that completed its
      frame, so the MIDI is that much later than at 48 kHz.
    - **Not reported to the host**, a change from the plan above: the
      plugin doesn't report the model's own latency either, and reporting
      it would make hosts shift the dry audio output, which isn't delayed.
    - The audio output stays at the host rate, unresampled.
    - Tests: `test_resampler` (sines within 2e-4 at 1 kHz and 2e-3 at 75 %
      of Nyquist, 0.2 dB down at 85 %; 30 kHz from 96 kHz at -60 dB;
      outputs and their sources independent of block sizes; no
      allocation) and `test_lv2_plugin` (at 44.1 and 96 kHz the plugin's
      MIDI equals the core library's through the resampler, also after
      re-activation; 6 kHz gives no MIDI and the "wrong sample rate"
      status).
    - Real recordings, `mdm-pitch-offset`, `g2m-transcribe --events` on a
      48 kHz WAV and on 44.1 and 96 kHz copies made with SciPy's
      `resample_poly` (note-ons matched by pitch within two frames):

      | file | s | notes at 48 kHz | 44.1 kHz | 96 kHz |
      |---|---|---|---|---|
      | guitarset Jazz2-110 p00 solo | 34.9 | 129 | 129, all in the same frame | 129, same |
      | guitarset Rock2-142 p05 comp | 27.0 | 333 | 333, same | 333, same |
      | idmt Lick1 LP_KN | 11.7 | 19 | 19, same | 19, same |
      | Sultans of Swing track 3 | 202.6 | 501 | 502: the 501, same, and one more | 502, likewise |
- **Builds and CI.** A GitHub Actions workflow builds and tests on Linux
  x86_64, Windows x64 and macOS universal, and packages zips per OS.
  - Unsigned macOS builds need users to clear quarantine (`xattr`). Signing
    and notarisation need an Apple Developer account **(open)**.
  - Windows builds are unsigned (SmartScreen warns).
  - CPU on the desktop was about 30 % of one core in real time on a P-core
    and 66–69 % on an E-core, which matched what Carla showed (2026-11-08).
    Since `docs/performance-plan.md` step 3 (2026-11-12) it is 8 % on a
    P-core and 15 % on an E-core (13 % for the projected model). Measure it on the CI machines with
    `g2m-bench` before calling it fit for laptops.
- **A licence for the code.** Decided 2026-11-22 (the user): the code
  under GPL-3.0-or-later (`LICENSE`, and the plugin's `getLicense()`,
  which DPF writes into the LV2 metadata as the SPDX URL), the model
  under CC BY 4.0 (`MODELS.md`). DPF (ISC) and the other components
  are permissive, so any licence was possible. The package also needs `THIRD_PARTY.md` for DPF
  and the font, and `MODELS.md` for the model's data credits.

## Steps

Each step is a commit on `player-release` with tests. CI runs from step 7.

1. Add DPF as a submodule. Port the plugin with no custom UI, keeping the
   URI and symbols, and the LV2 test passing. Check MIDI output from VST3,
   CLAP and the standalone in REAPER, and the standalone's MIDI port on
   Linux.

   **Done 2026-11-01**; checked in REAPER (the standalone untried, no
   JACK server here):
   - DPF `4238e1c7` (its `main`, 2025-10-23) in `external/dpf`, with pugl
     as its submodule;
   - `plugin/` builds LV2, VST3, CLAP and a standalone;
   - the 18 LV2 tests pass unchanged in substance: MIDI identical to the
     core's in both modes, no allocation in `run()`, the model restored
     from state; also under the sanitizers.

   What changed for the LV2 plugin:
   - The URI and the parameter symbols are kept. The ports are renumbered
     (DPF puts the audio and event ports first), and the audio ports are
     `lv2_in` / `lv2_out`. Hosts that restore controls by symbol keep
     them.
   - The model's state key is now `…#player#model` (DPF appends `#key` to
     the URI), not `…#player-model`. Projects saved before need the model
     chosen again.
   - The **Notes** text property is gone; the Note parameters remain. The
     UI will show the notes.
   - Load errors go to stderr rather than the host's log.
   - DPF needs MIDI input on for its standalone to build, so the plugin
     has a MIDI input already (unused until step 3).

   The standalone has no window, and DPF's native-audio fallback needs one
   to set up devices. So until the UI exists it only works with a JACK
   server.

   In REAPER (the user, 2026-11-01) all three formats load and the LV2
   plays, but VST3 and CLAP have no file chooser for the model. So the
   bundled-model part of step 5 was brought forward: `G2M_BUNDLED_MODELS`
   and a **Model** parameter (appended as `model_choice`) that chooses
   one, with a loader thread, `bundled:<name>` in state, and the first
   bundled model loaded when the plugin is created. Tested in a second
   build that bundles two test vectors. With it, VST3 and CLAP work in
   REAPER too (the user, 2026-11-01).
2. Core: `NoiseGate`, output level and mute; parameters and tests.

   **Done 2026-11-01**, as designed above. `core/include/g2m/noise_gate.h`
   counts the fade in samples (a float step left a residue), and
   `test_noise_gate` covers opening, hold, hysteresis, release times and
   reopening during the fade. The four parameters are appended
   (`gate_threshold`, `gate_release`, `output_level`, `output_mute`;
   22 LV2 ports). `run()` works a chunk at a time: gate, then the model and
   the output, so the output may share the input's buffer. With the gate
   off at 0 dB the output is still the input bit for bit. The LV2 test
   checks the output and the MIDI against the core's gate (with +24 dB of
   input gain, so the gated stretch changes the random test model's notes)
   and mute's ramp. In REAPER (the user, 2026-11-01) the gate and mute
   work as expected.
3. Core: `Sustain` and the MIDI input; tests.

   **Done 2026-11-01.** `core/include/g2m/sustain.h` and
   `src/sustain.cpp`, with `test_sustain` (every mode, re-strikes, repeated
   pedal values, mode changes; each condition broken once to check a test
   fails). Details settled while building it:
   - Sostenuto: a held note played again restarts and is no longer held.
   - Freeze: pedal up ends every frozen note, including those the model
     still hears; their later note-offs are dropped.
   - Changing the mode releases what the old mode held.
   - MPE needed no change: a held note keeps its string's channel, so the
     allocator gives new notes free strings, or steals the held note's
     string, ending it first.

   Parameters appended: `sustain_mode` (Off, Piano (default), Sostenuto,
   Freeze), `sustain_cc` (default 64), `sustain` and the output
   `sustain_state` (26 LV2 ports). `run()` splits the buffer at the MIDI
   input's events, so the pedal acts at its CC's sample. Other MIDI input
   is ignored, not passed on. The LV2 test sends the CC at exact samples,
   with repeated values and other controllers, and compares every mode
   with the core. In Carla (the user, 2026-11-08) the pedal on its MIDI CC
   works as expected.
4. Core: `BendMode`; tests.

   **Done 2026-11-02.** `core/include/g2m/bend_mode.h` (`BendMode`,
   `BendModeFilter`, `NoteSink` with a `retrigger` call) and
   `src/bend_mode.cpp`, with `test_bend_mode`. Each condition was broken
   once to check a test fails; the 0–127 clamp's failure reads out of
   bounds, which no event shows, so it stays as the guard. Details settled
   while building it:
   - Retrigger in MPE keeps the exact channel: `MpeOutput::retrigger` with
     `StringAllocator::move` (the allocator alone could pick another
     string). A note already at the new pitch on another string ends
     first.
   - In the filter, a step onto a pitch another note is sounding waits. A
     detected note at a retriggered note's pitch ends that note.
   - A note-on carries 8192 (Retrigger, Off), so an MPE channel starts
     clean. A pre-bent note starts at the bent pitch (Retrigger).
   - Each note keeps the mode it started in, so a change of mode never
     leaves a note at the wrong pitch.

   Parameter appended: `bend_mode` (Natural (default), Retrigger, Off; 27
   LV2 ports). The LV2 test compares every mode in plain and MPE output
   with the core, and checks that retriggered notes the plugin ends itself aren't ended again.
   In REAPER (the user, 2026-11-08) all the modes work. Semitone was then
   dropped (above), leaving Natural, Retrigger and Off.
5. Bundled models and the `bundled:` reference (done early, with step
   1); presets (factory and user).

   **Done 2026-11-08** except the user presets' menu, which needs the UI
   (step 6). Found while building it: DPF gives factory presets a named
   list only in LV2 (`presets.ttl`); VST3 gets an unnamed, hidden program
   number (no unit info or program list), and CLAP nothing. So the UI's
   preset menu (step 6) is what makes presets work in every format, for
   factory and user presets alike.
   - `plugin/presets.*` (`g2m_presets`, `test_presets`): presets as JSON by
     parameter symbol (`format`, `version` 1, `name`, an optional `model`,
     `parameters`); missing parameters take their defaults, unknown ones
     are ignored, values are clamped; a newer version is refused. The user
     folder, file names (unsafe characters replaced), list (sorted by name,
     unreadable files skipped), load, save (to a temporary file, then
     renamed) and remove.
   - `plugin/factory_presets.h`: Default, MPE synth, Retrigger lead, MPE
     retrigger lead, Pad (freeze), Noisy pickups (gated). They set every
     parameter but the Model and the Sustain switch, and keep the model.
     They are the plugin's DPF programs (`DISTRHO_PLUGIN_WANT_PROGRAMS`).
   - DPF's `presets.ttl` is broken where an output parameter follows an
     input (it ends the port list early); `plugin/cmake/fix_lv2_presets.cmake`
     joins it up after the build and drops the Model and Sustain ports.
   - The LV2 test restores each LV2 preset with lilv and compares it with
     the factory table (and that the model is kept), and selects each
     program through the LV2 programs extension (DPF's `loadProgram`, as
     VST3 uses it) to check the Model and Sustain are left alone.
   In Carla (the user, 2026-11-08) the LV2 build lists the six presets.
6. The UI.

   **Started 2026-11-08: the test UI.** LVGL 9.4.0 is a submodule
   (`external/lvgl`, MIT; the template plugin used a 9.4 development commit,
   the release came later). The wrapper copy in `plugin/ui/` has
   `makeCurrent()` and typed text from DPF's character input events, as
   above. `plugin/g2m_ui.cpp` shows the input level, Input Gain (a slider,
   one host gesture per drag), Output Mode and Bend Mode, and a name box
   that echoes what's typed. `plugin/params.h` holds the parameter indices
   for both sides. Builds and tests pass; the LV2 UI opens and draws in
   jalv on Xvfb with Mesa's softpipe (the default llvmpipe crashes creating
   the GL context there, before any plugin code runs). DPF's `dgl-opengl`
   needs GNU extensions (`typeof` in `WebViewImpl.cpp`).
   Checked in Carla (the user, 2026-11-08): the UI's changes reach Carla's
   parameters and Carla's reach the UI; two instances' UIs each follow only
   their own; typed text echoes correctly.

   **The full panel (2026-11-08)**, 760×480, LVGL's dark theme:
   - `params.h` now holds the parameter definitions (DPF `Parameter`s), so
     the UI has the host's ranges, labels and symbols (for preset files).
   - Three output parameters, appended: Model Loaded (none, the model file
     or a bundled model), Model Format, Model Error (couldn't load: the
     previous model plays on; another sample rate). They give the UI the
     model's status in every format: DPF's `updateStateValue` does nothing
     in CLAP and isn't wired in VST3, and LV2 sends the UI the state
     value asked for, not the one loaded. The LV2 test checks them.
   - Presets: one menu, the factory presets then the user's files; Save
     (user presets), Save as (a name dialog; replacing a file asks first),
     Delete (asks first). `*` marks a change since the preset was chosen;
     choosing it again undoes the changes. The menu re-reads the folder
     when opened. A preset's missing model keeps the current one, with a
     warning.
   - Model: the bundled models' menu (showing the model loaded), "Load
     file..." (DPF's file browser; it sets the Model choice to "Model file"
     first so the loader leaves it alone), the format and the error.
   - Cards: Input (the meter, -60 to +6 dB, with the training range and
     the gate threshold at the model's level, i.e. plus the Input Gain;
     gate threshold and release (a log slider), input gain, onset
     threshold), MIDI (output mode, channel and bend range, greyed when
     unused, bend mode), Sustain (mode, CC, the pedal light), Audio output
     (level, mute). A strip of keys from E2 to E6 lights the notes on, with
     their names. Double-click a slider for its default.
   - Driven on Xvfb with `xte`: Save as with typed text, the replace and
     delete dialogs, a factory and a user preset applied (model included),
     the bundled model menu, a slider marking the preset changed and
     choosing it again. Not checked: DPF's file browser (it uses the
     desktop portal over D-Bus, which isn't on the test display), scaling.
   - Left: a light theme (dark only for now); the UI's own settings would
     need a place to live.
   - LV2 checked in Carla (2026-11-08): every control drives its parameter;
     "Load file..." works, though the portal's dialog is slow to open and
     list the folder the first time. The slider knobs were cut off by the
     box around each slider; the box now lets them draw past it.
   - All three formats checked in REAPER (2026-11-08): the panel looks right
     in LV2, VST3 and CLAP. (Carla didn't find the VST3 and CLAP plugins in
     their default folders.)
   - Presets shared between formats (the user, 2026-11-08): one saved from
     LV2 loaded in CLAP.
7. Resampler; CI for the three OSes; packaging; the README's host table.
   - The resampler: done 2026-11-08 ("Sample rates" above).
   - CI and packaging, written 2026-11-08, not yet run (nothing pushed).
     The same day the player moved to its own repository (private), and
     the order of the rest changed (the user, 2026-11-08): first a model
     that may be shared (GuitarSet and renders, possibly from our own
     virtual instrument and generated MIDI), then
     that model bundled, the repository made public and CI switched on.
     Until then the workflow waited in `ci/`, where GitHub doesn't run it;
     since 2026-11-22 it is `.github/workflows/player.yml`, on in the
     public repository `gwaggott/g2m-player`.
     - `player.yml`: Linux x86_64 (Ubuntu 22.04),
       Windows x64 (MSVC through `vcvars64.bat`, Ninja) and macOS
       universal (arm64 + x86_64, 10.15+): configure, build, `ctest`, the
       `g2m-package` target, the zip kept as an artifact. No models were
       bundled then: the repository had none that may be shared. Since
       2026-11-22 a second build, `build/release`, bundles
       `models/v1-0-alpha.g2m` and makes the package (step 8).
     - For MSVC: warning flags per compiler (`G2M_WARNINGS`), and the two
       `g2m-transcribe` tests compare files with
       `core/test/run_and_compare.cmake` instead of `sh` and `cmp`.
     - `packaging/package.cmake` (the `g2m-package` target): a folder per
       format, the standalone with its `resources/`, `packaging/INSTALL.md`
       (for users), `THIRD_PARTY.md` and the licence texts from the
       submodules. Checked on Linux: the zip is laid out as intended, and
       the local build with private models is refused.
     - First runs, 2026-11-22/23 (branch `ci-fixes`): all three pass,
       packages included, after these fixes. macOS: `macos-14` is no
       longer offered (now `macos-15`); `g2m.clap` is a bundle, so it is
       installed as a folder; Apple's libc++ has no floating-point
       `from_chars` for a 10.15 target, so `json.cpp` reads numbers with a
       classic-locale stream there (`G2M_JSON_STREAM_NUMBERS`). Windows
       (MSVC): Khronos's `glext.h` (`external/khronos/`), as the SDK's
       OpenGL headers stop at 1.1; `/Zc:__cplusplus` (LVGL.hpp checks it)
       and `/utf-8`; no GNU `?:` in `LVGL.cpp`; `winmm` for DPF's
       `d_gettime_ms`. The workflow: Node.js 24 actions, the MSVC
       environment from `vcvars64.bat`, and arguments with a dot quoted
       (PowerShell splits `-DX=a.b` at the dot). Configure, build and test
       output goes to logs whose errors are repeated as annotations,
       which can be read without signing in.
     - Not yet: the plugins tried in hosts on Windows and macOS;
       `g2m-bench` on the CI machines needs a full-size model the
       repository can hold (a random-weights one would do: time doesn't
       depend on the weights); signing (open); a release step (packages
       stay artifacts until the code has a licence; it has had one since
       2026-11-22).
   - The README's host table: done 2026-11-08, from the checks in steps
     1–6 (REAPER and Carla on Linux).
8. The shippable model (after the blocker decision): train, export, check
   against `reference.py`, bundle, and write `MODELS.md`.

   **Done 2026-11-22:** `v1-0-alpha.g2m`, the training run `release-as72`
   (GuitarSet and the user's own sampled guitar playing generated MIDI),
   checked against the reference and tried by the user, who accepted it as an alpha:
   its worst fault is some bends coming out as chromatic legato notes. It
   is the only model in the repository (`models/` stays git-ignored for
   the rest) and the only one in CI's packages; `MODELS.md` has its
   credits. The code is under GPL-3.0-or-later and the model under CC BY
   4.0. Since 2026-11-23 it is the same model exported as format version 6,
   projected at 0.999 with float16 weights (2.1 MB, 19 % fewer MACs; both
   now the exporter's defaults). On validation, against float32: note-on
   F1 0.3075 (0.3078), string accuracy 0.6752 (0.6754), onset type 0.6282
   (0.6284), bent-frame pitch error 0.4730 (0.4726). On five test files
   about 3 % of note-ons move or change, borderline cases either way;
   plain MIDI identical to the reference. Published 2026-11-22 as `gwaggott/g2m-player` (public, from a
   single commit, CI on).

Steps 2–4 are independent of each other and of the UI. Step 8 is in
the training code and can run whenever the GPU is free.
