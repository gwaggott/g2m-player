# Guitar audio → MIDI: player

Runs a neural note model in real time:
a DI electric guitar signal in (mono; resampled to the model's 48 kHz at
other rates), MIDI note-on (with velocity) and note-off out, with no
latency beyond the model's own (and about 0.4 ms for the resampler). The MIDI is
either plain, on one channel, or MPE, with each note on its guitar
string's channel. A note is
reported about 4 ms after the string sounds (the delay the model was
trained to), plus at most one 1.33 ms block.

The **plugin** is built with [DPF](https://github.com/DISTRHO/DPF) as LV2,
VST3, CLAP and a standalone app, from one source. It is a thin layer over
a portable **C++17 core library**, which other targets can reuse, such as
a Raspberry Pi class board. So far it is built and tested on Linux. The
plan for a release others can install is
[docs/release-plan.md](docs/release-plan.md).

The core library matches the model's NumPy reference implementation
(part of the training code, which isn't public yet) exactly. On seven real recordings (474 s, 1,393 notes) its
MIDI is byte-identical to the reference's, and its per-block outputs are
within 1e-5. See [docs/initial-implementation.md](docs/initial-implementation.md)
for the plan, the measurements and the reasons behind the design.

## Building

Needs a C++17 compiler (GCC 11 or later), CMake 3.16+, OpenGL, and two git
submodules: DPF in `external/dpf` and LVGL (the UI) in `external/lvgl`. The LV2 plugin's test needs the LV2 and lilv
development packages; the standalone uses JACK when it's installed. On
Ubuntu 24.04:

```bash
git submodule update --init --recursive
sudo apt install build-essential cmake lv2-dev liblilv-dev lilv-utils libjack-jackd2-dev
sudo apt install jalv sordi   # optional: run the plugin outside a DAW, validate its TTL
```

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

The plugins go to `build/bin/`: `g2m.lv2`, `g2m.vst3`, `g2m.clap` and the
standalone `g2m`. `-DG2M_PLUGIN_FORMATS="lv2;clap"` builds fewer formats.

To bundle models, list them, the default first; each format gets a copy
in its resources folder (`g2m-models/` next to the `.clap` file):

```bash
cmake -B build/local -DG2M_BUNDLED_MODELS="models/mdm-pitch-offset.g2m;models/mpe-on-ow.g2m"
```

Bundle only models you may share in a build you give to others
(docs/release-plan.md, "Which model can ship").

### Packages and CI

`cmake --build build --target g2m-package` zips the built formats, a
folder each, with `packaging/INSTALL.md` (where each format goes, per OS),
`THIRD_PARTY.md` and the third-party licences, as
`build/package/g2m-player-<version>-<os>-<arch>.zip`
(`packaging/package.cmake`). It refuses a build that bundles models unless
it's configured with `-DG2M_PACKAGE_SHAREABLE=ON`, which says every
bundled model may be shared.

`.github/workflows/player.yml` is a GitHub Actions workflow that builds, tests and
packages on Linux x86_64 (Ubuntu 22.04), Windows x64 (MSVC, Ninja) and
macOS universal (arm64 and x86_64, 10.15 or later) on every push, and
keeps each zip as a build artifact. Its packages bundle
one model, `models/v1-0-alpha.g2m` (the only model in the repository;
`MODELS.md` has its data and credits), built in a second build directory,
`build/release`, with `-DG2M_BUNDLED_MODELS=models/v1-0-alpha.g2m
-DG2M_PACKAGE_SHAREABLE=ON`. The macOS and Windows builds aren't signed.

## Raspberry Pi

The LV2 plugin runs headless on a Raspberry Pi 4 with a synth, in
mod-host under JACK: [`docs/raspberry-pi.md`](docs/raspberry-pi.md), with
`packaging/raspberry-pi/start-g2m.sh` to start it. It needs a format
version 6 (float16) model.

## Models

A model is a `.g2m` file exported by the training code, which isn't
public yet. The player bundles one, `models/v1-0-alpha.g2m` (`MODELS.md`),
and **Load file...** in its window loads others. It reads format versions
1 to 6: version 5 has projected gated layers, about a quarter fewer
multiply-adds; version 6 stores the weights as float16, half the size.
The format's specification is kept with the training code; until that is
published, the loader (`core/src/model_file.cpp`) and the network and
decoder in `core/src/` are its public description.

From format version 2 the decoder has a **late fallback**. When the
model misses a note's onset, which happens most often on
the first note after a silence, the note still starts once the model's
"note is sounding" output has stayed high for 2 blocks. That is usually
5–30 ms late, rather than not at all. On the model's
validation set it lifts recall for pitches not played in the previous
3 s from 0.15 to 0.83. A version 1 file plays with the fallback off.

Every model is for 48 kHz input; the player resamples other rates for
it (below). The player refuses a file whose format
version it doesn't know. Check a newly exported model against the
reference before relying on it; see [Checking against the reference](#checking-against-the-reference).

## The plugin

Install the plugins into `~/.lv2`, `~/.vst3` and `~/.clap` (or set
`G2M_LV2_INSTALL_DIR`, `G2M_VST3_INSTALL_DIR`, `G2M_CLAP_INSTALL_DIR` when
configuring). An older LV2 build installed from `lv2-plugin/` leaves
`g2m.so` and `g2m.ttl` in `~/.lv2/g2m.lv2`, so remove that folder first:

```bash
rm -rf ~/.lv2/g2m.lv2 && cmake --install build
```

Then rescan plugins in your host and insert **Guitar to MIDI** on a
track that carries the DI guitar. Any common sample rate works (8 to
384 kHz, e.g. 44.1, 48, 88.2, 96 kHz): at a rate other than the model's
48 kHz, the audio the model hears is resampled, which delays the MIDI by
about 0.4 ms. The audio output isn't resampled or delayed. At a rate it
can't convert, the plugin passes the audio through, prints an error and
sends no MIDI.

The host must be able to route a plugin's MIDI output to an instrument.
REAPER, Bitwig, Ardour and Carla can; Ableton Live and Logic can't for an
audio effect. Checked so far, all on Linux:

| Host | LV2 | VST3 | CLAP | Checked |
|---|---|---|---|---|
| REAPER | MIDI out, the window, presets | MIDI out, the window | MIDI out, the window | 2026-11-01 and 2026-11-08 |
| Carla | MIDI out, the window, presets, sustain on a CC | not found in `~/.vst3` | not found in `~/.clap` | 2026-11-08 |
| jalv | the window (offscreen, for tests) | | | 2026-11-08 |
| Bitwig, Ardour | not yet | not yet | not yet | |

Presets saved in one format load in another (LV2 to CLAP). The
standalone app, Windows and macOS are untried.

In REAPER: choose the model in the plugin's window, then put an
instrument after the plugin in the same FX chain to play its MIDI. The
audio passes through (after the noise gate, at the Output Level), so the
guitar is still heard; mute it to hear only the instrument.

The plugin's window has every control below, with the input meter (the
training range in green, the gate threshold in orange), a strip of the
notes on, the model's name and format (and why a model didn't load), and
the presets. Double-click a slider to reset it. Hosts' generic controls
and automation work too.

| Control | |
|---|---|
| Model (file) | The `.g2m` file: **Load file...** in the plugin's window, or the host's file chooser (LV2 hosts such as REAPER and jalv). Loaded in the background and saved with the project. |
| Model (menu) | A model bundled with the plugin, in every format; the first is loaded when the plugin is inserted. **Model file** keeps the file chosen above. |
| Gate Threshold | Noise gate on the input, before everything else: −96 dB ("Off", the default) to −20 dB. It opens at once when the input reaches the threshold, so attacks aren't cut, and closes 6 dB below it after a 10 ms hold. Set it just above the noise with the strings muted, to stop the noise making false notes. |
| Gate Release | 5–500 ms, default 50: how long the gate takes to fade out once it closes. |
| Input Gain | −24 to +24 dB, applied only to what the model hears. |
| MIDI Channel | 1–16, for plain output. Changing it ends the notes that are on. |
| Output Mode | **Plain** (the default): every note on the MIDI Channel. **MPE**: each note on its own guitar string's channel, for MPE synths. Changing it ends the notes that are on. See [MPE output](#mpe-output). |
| MPE Bend Range | 1–96 semitones, default 12: the pitch-bend range the plugin tells an MPE synth (RPN 0), and the scale of the model's bends. Set it to 48 for a synth that ignores RPN 0. |
| Bend Mode | What becomes of the model's string bends (models with bends only). **Natural** (the default): sent as they are, in MPE. **Retrigger**: each time a bend reaches another semitone (vibrato within 0.2 semitones past half-way is ignored) the note ends and a note at the new pitch starts, with the same velocity, on the same channel; it works in plain output too, with any synth. **Off**: no bends. Each note keeps the mode it started in. |
| Output Level | −60 to +12 dB, default 0: the level of the audio output (the gated guitar). The model doesn't hear it. |
| Mute | Silences the audio output; the MIDI carries on. |
| Sustain Mode | What the sustain pedal does to the notes it sends. **Piano** (the default): notes that end while the pedal is down sound on until it comes up; a note played again is restarted. **Sostenuto**: only the notes sounding when the pedal goes down sound on. **Freeze**: the notes sounding when the pedal goes down sound on, without bends, and no new notes start until it comes up. **Off**. |
| Sustain CC | The controller on the plugin's MIDI input that works the pedal, on any channel, down at 64 and above: 64 (the default) is a sustain pedal, 66 sostenuto. |
| Sustain | Holds the pedal down: for automation, or hosts that can't send MIDI to an audio effect. Either this or the CC puts the pedal down. |
| Onset Threshold | How sure the model must be before a note starts. 0 ("Model default") uses the model's tuned value: 0.45 for the current one. Lower for more notes, higher for fewer false ones. The late fallback, if the model has it, still starts notes the onset misses. |

| Output | |
|---|---|
| Input Level | Peak level the model hears, after Input Gain. The training audio mostly peaks between −17 and −1 dBFS, so aim for that range when playing normally. |
| Notes On | How many notes are on. |
| Pedal | The sustain pedal is down. |
| Model Loaded, Model Format, Model Error | The model playing (a bundled one, the model file, or none), its file format version, and why the last model chosen isn't playing: it couldn't be loaded (the one before plays on) or the host runs at a rate the player can't convert. |
| Note 1 … Note 6 | The notes that are on, lowest first, as MIDI note numbers (or names, where the host shows labels; C4 = 60, so the low E string is E2). |

### Sustain pedal

Send the plugin a pedal's CC on its MIDI input. In REAPER, put the pedal's
MIDI on the guitar track (record-arm with a MIDI input, or a MIDI item);
the plugin reads only that CC and doesn't pass its MIDI input on. The pedal
acts at the sample its CC arrives. In MPE mode a held note keeps its
string's channel, so new notes take other strings, or end it if they need
its string.

### Presets

The factory presets set every control except Model and Sustain, and keep
the current model:

| Preset | Changes from the defaults |
|---|---|
| Default | none |
| MPE synth | MPE output |
| Retrigger lead | Bend Mode Retrigger (plain output) |
| MPE retrigger lead | MPE output, Bend Mode Retrigger |
| Pad (freeze) | MPE output, Sustain Mode Freeze |
| Noisy pickups (gated) | Gate Threshold −60 dB, Gate Release 80 ms |

The **Preset** menu in the plugin's window lists them in every format
(LV2 hosts also list them by name), followed by your own presets. **Save
as** saves the current controls and model as a preset file, **Save**
overwrites the chosen one and **Delete** removes it; a `*` after the name
means a control has changed since it was chosen. Picking a preset again
undoes the changes. Preset files are JSON, one per preset, in
`~/.config/guitar-2-midi/presets/` (`$XDG_CONFIG_HOME` if set;
`%APPDATA%\guitar-2-midi\presets` on Windows,
`~/Library/Application Support/guitar-2-midi/presets` on macOS), so they
work in every host and format. A preset whose model is missing keeps the
current model, with a warning. Hosts' own presets work too, and save the
plugin's whole state.

### MPE output

In MPE mode the plugin sends an MPE lower zone: channel 1 is the manager
channel, and string 1 (high E) to string 6 (low E) are channels 2 to 7,
as gp2midi's `--mpe` writes. It configures the synth itself when it
starts and whenever MPE is chosen (the MPE Configuration Message, then
the bend range on each string's channel), and turns the zone off when
you switch back to plain. Set the synth to MPE mode, or to listen on
channels 1–7.

With a model that predicts the string (format version 3, with a string
head), the plugin uses its probabilities to choose each note's string,
among the free strings that can play the note while the notes ringing
stay within a 4-fret hand span. Without one it guesses: the lowest such
string. A note stays on its channel until it ends. If all six strings
are busy, the oldest note is cut to make room.

With an onset-type head, a note the model hears as a hammer-on, pull-off
or slide goes on the string of the note it continues (within 5
semitones, 12 for a slide) and overlaps it, so a synth in mono-legato
mode slurs instead of re-picking. In plain mode such notes are ordinary
note-ons.

With a pitch-offset head (format version 4), each note's bends, vibrato
and slides go out as pitch bend on its own channel, updated every 1.33 ms
block while it sounds; small offsets (under a quarter tone by default)
count as none. The MPE Bend Range control sets both the range the synth
is told (RPN 0) and the range the bends are scaled to. Plain mode sends
no pitch bend.
[docs/mpe-output.md](docs/mpe-output.md) has the exact rule and the
measurements.

Outside a DAW:

```bash
jalv.gtk3 'https://github.com/gwaggott/guitar-2-midi#player'   # then connect its MIDI out to a synth
```

The LV2 plugin's URI is `https://github.com/gwaggott/guitar-2-midi#player`.
Hosts store it in projects, so it never changes. Projects saved with the
LV2 plugin before DPF (2026-11-01) find the plugin and its controls (by
symbol), but the model was saved under a different key: choose it again.

The standalone `g2m` has no window yet (it comes with the UI). With a
JACK server running it opens JACK ports; without one it can't be set up.

## Command-line tools

```bash
build/core/g2m-transcribe --model models/M.g2m IN.wav [-o OUT.mid] [--block N] [--set KEY=VALUE] [--events OUT.json] [--mpe] [--bend-range N] [--decode REF.f32]
build/core/g2m-bench --model models/M.g2m [--wav IN.wav] [--stages]
```

- **`g2m-transcribe`** turns a WAV (any channels, integer or float
  samples; at a rate other than 48 kHz resampled as the plugin does) into
  a MIDI file. The file is laid out as the reference writes
  it: 120 BPM and 960 PPQ, starting at the start of the audio, so it lines
  up with the WAV at position 0 in a 120 BPM project. `--set` overrides a
  decoder setting, for example `--set onset_threshold=0.35`. `--outputs
  OUT.f32` also writes the model's raw outputs for every block, and
  `--set fallback_frames=0` turns the late fallback off. `--events
  OUT.json` writes every event as `reference.py --events` does, with each
  note-on's string probabilities, onset type and bend, and the bend
  events. `--mpe` writes MPE as the plugin sends it and prints how the
  strings were chosen; `--bend-range N` scales the bends to ±N semitones
  and sends that range (default: the model's, 12). `--decode REF.f32`
  decodes `reference.py --outputs`' outputs instead of running the model,
  for exact comparisons.
- **`g2m-bench`** measures single-thread time per 64-sample block against
  its 1,333 µs budget. On an i9-12900F the current model takes 110 µs per
  block on a performance core (8 % of the budget) and 200 µs on an
  efficiency core (15 %), and its projected version (`-p999`, format
  version 5) 90 µs and 180 µs, the same with float16 weights (`-p999-f16`,
  version 6). On a Raspberry Pi 4 the projected model takes 764 µs (57 %)
  and its float16 version 513 µs (39 %), as the weights don't fit the Pi's
  cache; `docs/performance-plan.md` has the measurements
  and the further steps. `--stages` splits the time between the front end, the gated
  layers, the GRU, the head and the decoder.

## Checking against the reference

After any change to the model, the export format or the core library,
the core's MIDI must be identical to the NumPy reference's on real
recordings, and its per-block outputs within about 1e-5:

```bash
build/core/g2m-transcribe --model models/M.g2m IN.wav -o cpp.mid --outputs cpp.f32
cmp ref.mid cpp.mid    # ref.mid from the reference on the same file: identical
```

The outputs (`*.f32`, float32 `[blocks][pitches][3]`) are in the
reference's layout. The library's own tests use test vectors made from
the reference and checked in (`core/test/data/`), so building and
testing need nothing else. Regenerating them (after a reference or format
change) needs the training code: `core/test/make_vectors.py`, with
`G2M_TRAINING` set to its directory.

## Layout

- `core/`: the library (`include/g2m/`: `model.h`, `transcriber.h`,
  `string_allocator.h`, `mpe.h`, `note_name.h`, `denormals.h`), its tests
  and test vectors, and the command-line tools. No dependencies beyond the C++ standard library.
- `plugin/`: the DPF plugin (`g2m_plugin.cpp`, `DistrhoPluginInfo.h`,
  `params.h`), its UI (`g2m_ui.cpp`, LVGL through `ui/`), presets, and an offline test host
  that runs the built LV2 bundle through lilv.
- `external/dpf`: DPF, a git submodule.
- `external/lvgl`: LVGL 9.4.0 (MIT), a git submodule.
- `docs/`: the implementation plan and findings.
- `models/` and `tmp/`: git-ignored.

## Limitations

- Note-on / note-off with velocity only, no pitch bend, as the model
  was first trained, plus per-note pitch bend in MPE mode with a
  version 4 model. In MPE mode the string comes from the model's string
  head when it has one, else from a rule.
- CPU: about 8 % of the real-time budget on a fast desktop core and 15 %
  on an efficiency core (SSE2; `docs/performance-plan.md`), but not yet
  measured on other machines, and too slow for a Raspberry Pi.
- From the first sessions in REAPER: notes after silence triggered
  patchily, and the input often needed a lot of gain. The late fallback
  (format version 2) addresses the first, at the cost of latency on the
  notes it recovers. How it sounds live is still to be heard.

## Licence

Copyright (c) 2026 Geoffrey Waggott.

The player's code is free software: you can redistribute it and/or
modify it under the terms of the GNU General Public License as published
by the Free Software Foundation, either version 3 of the License, or (at
your option) any later version (`LICENSE`). It is distributed in the
hope that it will be useful, but WITHOUT ANY WARRANTY; without even the
implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

The bundled model, `models/v1-0-alpha.g2m`, is under CC BY 4.0
(`MODELS.md`, with the credits for its training data). The code by
others in the plugin keeps its own licences (`THIRD_PARTY.md`).
