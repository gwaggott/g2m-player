# Initial implementation: core library and LV2 plugin

Goal: an LV2 plugin that takes a 48 kHz DI guitar signal and emits MIDI
note-on / note-off events in real time from a model trained in
the training code. It is built on a portable C++ core library that matches the
training code's NumPy reference: identical note events on the test
vectors.

**Status: closed 2026-10-26.** All steps are done. The plugin runs in
REAPER with `data4-big-wd03-e120`, and its MIDI matches the reference.
What's left is listed under "Later"; how to build and use it is in the
README.

Each step below is one commit, for review before the next starts. A step's
tests pass before it's committed, and results that steer later steps
(CPU time, differences from the reference) are written in this file under
the step.

## Decisions (2026-10-24)

- **Weights file**: the training code's step 11 comes first (export, NumPy
  reference, the format spec); this project consumes that format.
- **Core dependencies**: plain C++17 and its standard library only.
  Hand-written loops; SIMD later, after measuring.
- **Plugin model loading**: a file chosen at runtime (an `atom:Path`
  parameter set by `patch:Set`), loaded on the LV2 worker thread, swapped
  in on the audio thread, saved in plugin state. This follows
  `neural-amp-modeler-lv2`.
- **Sample rate**: 48 kHz only. At any other rate the plugin logs an error
  and passes audio through with no MIDI. Resampling is left for later.

## Design summary

- **Core API** (namespace `g2m`):
  - `Model`: immutable weights and config, loaded from a `.g2m` file
    (`load_model(path)` → model or error message). One `Model` can back
    several instances.
  - `Transcriber`: built from a `Model`, owns all state and scratch
    buffers. `process(const float* audio, size_t n, EventSink&)` takes any
    number of samples, runs one network step for each completed 64-sample
    block, and decodes it. Events carry pitch, velocity (0 = note-off) and
    the absolute sample at which their frame ends. Also has `reset()`
    (silence history, with note-offs for held notes) and `all_notes_off()`.
  - Decoder settings default to those in the file. A `DecoderSettings`
    struct can override them, for example the onset threshold as a plugin
    control later.
- **Timing**: no added latency. An event goes on the host-buffer sample
  where its 64-sample block completes. Audio passes through unchanged, so
  the plugin reports zero latency.
- **Cost of the current model** (`data4-big-wd03`): 1.08 M MACs per
  frame × 750 frames/s ≈ 0.8 G MAC/s. That is easy for one desktop core.
  On a Raspberry Pi core it is tight, so per-frame time is measured from
  step 4 on.

## Prerequisites

- **Toolchain** (installed 2026-10-24): `g++` 13, `cmake` 3.28, `lv2-dev`
  1.18, `liblilv-dev` 0.24 (offline plugin test host), `lilv-utils`
  (`lv2info`, `lv2ls`), `jalv` (quick manual runs), `sordi`
  (`sord_validate` for the TTL), `gdb`, `clang-format`. REAPER is
  installed for real playing. `clang-format` came from noble rather than
  the `focal-security` source in this machine's apt configuration, whose
  LLVM 18 doesn't fit noble's.
- **The training code's step 11**: done 2026-10-24. The format spec is at
  version 1. In brief:
  - Magic `G2MW`, a uint32 header length, a JSON header padded with spaces
    so the weights start at a multiple of 16 bytes, then little-endian
    float32 weights.
  - The header carries `data_floats` and a zlib CRC-32 of the weight
    bytes, `data_crc32`.
  - Tensors are found by name, with their shape and offset given in the
    header. Unknown header keys are to be ignored.
  - The model gives probabilities (sigmoid already applied), not logits:
    onset, frame and velocity per pitch.
  - `reference.py` computes the sigmoid as `0.5 · (1 + tanh(0.5 · x))`.
    The port does the same, to stay close to it.
  - A port is checked by its outputs, to within about 1e-5 of the
    reference, and by its decoder: given the reference's outputs, the
    port's decoder must give identical events.
  - `reference.py` imports only NumPy, and `export.export_model` builds a
    file from any `NoteModel`, so `core/test/make_vectors.py` uses both.
  - The first real export is
    `data4-big-wd03-e120`: 4.3 MB, with
    1.08 M MACs per frame.

## Steps

### 0. Project skeleton

- `git init`, `.gitignore`, `CLAUDE.md`, `core/`, `lv2-plugin/`, this
  plan.

### 1. Build system, model file loader, test vectors

- Top-level `CMakeLists.txt`. `core/` builds a static library `g2m_core`
  (position-independent, so it links into the plugin's `.so`) with
  `-Wall -Wextra`. Tests are plain executables using a small check
  header, run by CTest.
- `core/src/json.*`: a minimal JSON parser for the header only (load
  time, may allocate).
- `core/src/model_file.*` (`g2m::load_model`) reads and checks the file
  as the spec says:
  - the magic, the header length, `format`, `format_version` and
    `outputs`;
  - the weights' size and CRC-32;
  - that the config is consistent: strides multiply to `hop`, each
    kernel is at least its stride, and
    `n_pitches = pitch_max − pitch_min + 1`;
  - that every tensor the config implies is present with the shape the
    config gives, inside the data, and that there are no unknown tensors.

  Errors are returned as messages; the loader never throws. It doesn't
  restrict the sample rate; the host compares its own rate with the
  model's.
- `core/test/make_vectors.py`, run with the training code's Python environment:
  - Builds two random-weight `NoteModel`s and exports them with
    `export.export_model`. `small` has the default model's structure at a
    tiny size. `odd` has three front-end stages, kernel 2 and dilations
    1 and 3, to catch indexing that only works for the default shapes.
  - Scales each head and centres its biases on the input (as
    the training code's export test does), so the outputs cross the
    decoder's thresholds often.
  - Runs `reference.transcribe` on bursts of noise and writes the input
    audio, the per-block outputs (raw float32) and the events (JSON) to
    `core/test/data/`. That is a few hundred kB, committed.
- Tests: the test models load, and their config and tensor shapes match.
  Bad magic, truncation, a wrong CRC, an unknown version, an unknown
  tensor and a shape that disagrees with the config are each rejected
  with a message.

Result (2026-10-24):
- The test vectors come from the training code as of 2026-10-24. `small` has
  10,411 parameters, 1,000 blocks and 76 note-ons; `odd` has 3,289
  parameters, 600 blocks and 310 note-ons. Together they are 600 kB.
- The JSON and model-file tests pass, including under the address and
  undefined-behaviour sanitizers.
- The loader reads `data4-big-wd03-e120/model.g2m` (1,074,681 weights,
  36 tensors) in 17 ms on this PC.
- Numbers are parsed with `std::from_chars`, so a host's locale (a
  decimal comma) can't change them as it could with `strtod`. This needs
  GCC 11 or later; check the compilers of other targets when they come.

### 2. The network, one block at a time

- `core/src/layers.*`:
  - strided causal conv with history;
  - 1x1 conv;
  - dilated causal conv on a ring buffer of `(kernel − 1) · dilation`
    frames;
  - GRU cell;
  - linear layer;
  - activations: `std::tanh`, and the sigmoid computed as the reference
    does.
- `core/src/network.*`: `step(block[64]) → outputs[pitches × 3]`
  (probabilities), with all state preallocated.
- Tests:
  - Outputs per block match the reference over the whole test input of
    both test models, to within 1e-5.
  - `reset()` gives the same outputs again.
  - In a debug build, a check that `step` doesn't allocate (a counting
    `operator new` in the test).

Result (2026-10-24):
- The layers are written inline in `core/src/network.cpp` rather than in
  a separate `layers.*`; at this size there was nothing to share.
- Each front end stage keeps its history and the new block in one
  channel-major buffer. Each gated layer keeps a ring of its last
  `(K − 1) · d + 1` inputs, from which the `K` taps are gathered.
- The weights are used where they lie in the `Model`, with no relayout.
- Test vectors: the largest difference from the reference is 2.1e-7 on
  `small` and 1.2e-7 on `odd`. Tests pass under the sanitizers, and
  `step` makes no allocations.
- Real model, `data4-big-wd03-e120`, on 20 s of
  a DI recording, `design_for_life_di.wav` (from 30 s in, 15,000 blocks):
  - largest output difference from `reference.py --outputs` 5.5e-6,
    mean 3.1e-8: inside the spec's 1e-5;
  - single-thread time per block on this PC with `-O3` and plain loops:
    median 381 µs, p99 416 µs, worst 571 µs, against 1,333 µs. That is
    29 % of the budget on the desktop.
  - The dot products use one accumulator, which stops the compiler from
    vectorising them (it may not reorder float sums). Several
    accumulators and a weight layout for SIMD would cut the time several
    times over. That matters for a Raspberry Pi, not for the desktop
    plugin; it belongs to step 4's optimisation, measured there.

### 3. Decoder and the streaming `Transcriber`

- `core/src/decoder.*`: a port of `Decoder.step`, kept in the same
  structure so it can be checked line by line. It uses fixed per-pitch
  arrays and writes events into a fixed-capacity buffer.
- `core/include/g2m/transcriber.h`: the public API above, with the input
  buffered into 64-sample blocks.
- Tests:
  - Fed the reference's outputs, the decoder gives exactly the
    reference's events: pitch, velocity and frame.
  - The full `Transcriber` on the test audio gives the same events. If a
    value at a threshold ever flips, the decoder test above stays the
    exact check.
  - Feeding the same audio in chunks of 1, 63, 64, 100, 511 and 4096
    samples, or random sizes, gives identical events and sample
    positions.
  - `all_notes_off` and `reset` behave as described.

Result (2026-10-24):
- The decoder is `core/src/decoder.h`, a line-by-line port of the
  spec's pseudocode. The public API is
  `core/include/g2m/transcriber.h`:
  - `Transcriber::process(audio, n, EventSink&)`, plus
    `all_notes_off`, `reset`, `position` and
    `decoder_settings` / `set_decoder_settings`.
  - Events carry the absolute sample at which their frame ends,
    `(k + 1) · hop`. A host puts each one on the sample before that,
    within its buffer.
  - A whole block in the input is run in place; only part blocks are
    copied.
- Test vectors: given the reference's outputs, the decoder gives exactly
  its events (152 for `small`, 620 for `odd`). The full `Transcriber`
  gives the same events from the audio, at every block size tried.
  `process`, `all_notes_off` and `reset` make no allocations. Tests pass
  under the sanitizers.
- Do the vectors reach every decoder branch? Each of six mutations was
  tried in turn: `>=` → `>` on the gate expiry, off frames and frame
  wait, no re-trigger guard, no strong-onset bypass, and the onset's
  velocity not kept for a gated start. The tests failed for every one.
  The strong-onset bypass was only reached after `small`'s
  `strong_onset` went from 0.9 to 0.52, because its gated onsets sit
  around 0.5.
- Real model, `data4-big-wd03-e120`, on the same 20 s of
  `design_for_life_di.wav` as step 2, fed in 480-sample buffers: the
  134 events (67 notes) are identical to `reference.py`'s, sample for
  sample.

### 4. Command-line tools and first measurements

- `core/tools/g2m-transcribe in.wav -o out.mid [--model m.g2m]`:
  - Reads 16/24-bit PCM and float WAV, mono or mixed down. Rates other
    than 48 kHz are rejected.
  - Writes a MIDI file on the training code's timeline (120 BPM,
    960 PPQ).
- `core/tools/g2m-bench --model m.g2m`: single-thread time per frame
  (median, p99, worst) on noise and on a real file.
- With a real exported model (`data4-big-wd03-e120` or later, in
  `models/`):
  - Compare `g2m-transcribe` with `reference.py --wav --outputs` on a few
    test files: the largest output difference, and the notes that match.
  - Any differing events are listed and explained. Float summation order
    can flip a value at a threshold; there should be very few, if any.
  - Record the CPU time per frame on this machine against the 1.33 ms
    budget.
- If the time is not comfortably within budget, add an optimisation step
  here (memory layout, SIMD-friendly loops, fast activations) before the
  plugin.

Result (2026-10-24):
- The tools are `core/tools/`: `g2m_transcribe.cpp`, `g2m_bench.cpp`,
  and `audio_io.*` (WAV reader and MIDI file writer). The WAV reader
  takes PCM 8/16/24/32-bit, float 32/64 and `WAVE_FORMAT_EXTENSIBLE`,
  averaging channels to mono as the reference does.
- `g2m-transcribe --outputs` writes the model's outputs as the reference
  does. To support it, `Transcriber::last_outputs()` was added to the
  public API.
- `core/include/g2m/denormals.h` (`FlushDenormals`): a scoped guard that
  flushes denormals to zero (x86 FTZ and DAZ, ARM FZ), for the plugin's
  `run()`.
- `make_vectors.py` also writes `small.wav` (24-bit, ending in half a
  block) and the reference's `small.mid` for it. The `transcribe_cli`
  test requires `g2m-transcribe` to write the same file byte for byte.
- Compared with `reference.py` using model `data4-big-wd03-e120`, on
  seven files: two each from the test split's GuitarSet, IDMT and
  rendered tabs, plus `design_for_life_di.wav` (stereo,
  24-bit):

  | file | s | notes | largest output diff | MIDI |
  |---|---|---|---|---|
  | guitarset Jazz2 p00 solo | 20.5 | 77 | 4.0e-6 | identical |
  | guitarset Rock2 p05 comp | 45.2 | 255 | 3.3e-6 | identical |
  | idmt Lick1 LP_KN | 11.7 | 6 | 4.4e-6 | identical |
  | idmt Lick2 AR_KN | 14.4 | 16 | 3.1e-6 | identical |
  | Sultans of Swing track 3 | 203.6 | 362 | 7.2e-6 | identical |
  | Here Comes the Sun v10 | 78.8 | 407 | 8.3e-6 | identical |
  | Design for Life DI | 100.4 | 270 | 1.0e-5 | identical |

  The mean output difference is 3–4e-8 on every file. The MIDI is also
  identical when fed in 512-sample host blocks.
- `g2m-bench` on this PC (i9-12900F, one thread, `-O3`, no
  `-march=native`), 20 s each:

  | input | mean | median | p99 | max |
  |---|---|---|---|---|
  | noise, denormals flushed | 370 | 368 | 405 | 955 µs |
  | silence, flushed | 355 | 353 | 391 | 575 µs |
  | noise, not flushed | 373 | 371 | 407 | 953 µs |
  | silence, not flushed | 355 | 353 | 391 | 551 µs |

  A block takes 28 % of its 1,333 µs budget. Denormals cost nothing
  measurable with this model, even in silence. The plugin still flushes
  them, because another model might produce them.
- Decision: the desktop plugin is comfortably within budget, so no
  optimisation step before step 5. A Raspberry Pi is not.
  - Rough scaling to a Pi 4 (Cortex-A72 at 1.8 GHz) puts plain loops
    at several times this time, which is over budget.
  - SIMD-friendly loops (several accumulators, weights laid out for
    NEON / AVX) are the first thing to do when a Pi or bare-metal
    target starts. They are listed under "Later".

### 5. LV2 plugin

- `lv2-plugin/`:
  - `manifest.ttl.in` and `g2m.ttl.in`, configured by CMake into
    `build/g2m.lv2/`.
  - Plugin source and a `cmake --install` target to `~/.lv2`.
- Ports:
  - control atom input (`patch:Set` / `patch:Get`);
  - notify atom output;
  - audio in, and audio out as a pass-through, so the guitar still sounds
    in the chain;
  - MIDI out (an `atom:Sequence` of `midi:MidiEvent`);
  - control ports: MIDI channel (1–16) and input gain in dB, since the
    model is sensitive to level.
- Features:
  - `urid:map` and `work:schedule` are required; `log`, `options` and
    `state` are optional.
  - Model path parameter, loaded on the worker thread and swapped on the
    audio thread. The old instance is freed on the worker.
  - Note-offs for held notes on a model swap, `deactivate`, and a MIDI
    channel change.
  - State save / restore of the model path, with `state:mapPath`.
- Denormals are flushed to zero in `run`.
- `lv2-plugin/test/run_plugin.cpp`: an offline host using lilv.
  - It loads the bundle, sets the model, and feeds a WAV in random block
    sizes.
  - Its MIDI is checked against `g2m-transcribe` on the test vectors in
    CTest.
- Manual check: `lv2info` and `jalv` load it.

Result (2026-10-24):
- The URI is `https://github.com/gwaggott/guitar-2-midi#player`, and the
  model parameter is the same with `-model` appended. The URI lives in
  `lv2-plugin/CMakeLists.txt`. It reaches the code through a configured
  header, `g2m_uri.h`, because CMake drops command-line macros that
  contain `#`.
- The bundle is built into `build/g2m.lv2/` (`g2m.so`, `manifest.ttl`,
  `g2m.ttl`). `cmake --install build` copies it to `~/.lv2` (cache
  variable `G2M_LV2_INSTALL_DIR`).
- `lv2-plugin/src/g2m_lv2.cpp`:
  - Ports: as planned, 7 in all. The plugin class is
    `lv2:AnalyserPlugin`.
  - The worker builds both the model and its `Transcriber`, so nothing
    is allocated on the audio thread.
  - `restore` loads and swaps synchronously. The plugin doesn't declare
    `state:threadSafeRestore`, so the host never calls it during `run`.
  - A model for another sample rate is accepted with a logged error.
    Its path is kept and reported, so it survives in the session, but
    the plugin emits no MIDI.
  - A file that fails to load is logged, and the current model is kept.
  - The plugin tracks the notes it has sent. On a model swap, an
    `activate` or a MIDI channel change, it sends their note-offs at
    the start of the next `run` (on the old channel, for a channel
    change).
  - Input gain ramps linearly over each buffer and only affects what
    the model hears.
- `lv2-plugin/test/test_lv2_plugin.cpp` is an offline lilv host with a
  synchronous worker. Its 8 tests:
  - check the ports;
  - require MIDI identical to the core `Transcriber`'s on `small.wav`,
    fed in random blocks of 1–700 samples, on each event's frame's last
    sample;
  - check input gain (−6 dB);
  - check note-offs on a channel change and on a model swap;
  - restore a saved state (with `lilv_state`) and require the right
    MIDI;
  - check that 44.1 kHz gives no MIDI while audio still passes through;
  - check that a bad file keeps the current model.

  All pass, including under the sanitizers. Three deliberate breaks were
  each caught: event offset off by one, held notes not tracked, gain not
  ramped.
- `sord_validate`, with the installed LV2 schemas: 0 errors.
- `lv2info` shows the bundle as expected.
- `jalv` loads the plugin on the PipeWire / JACK server (48 kHz, 1,024
  frames). `jalv`'s console can't set a path parameter, so loading a
  model needs `jalv.gtk3` or a host: step 6.

### 6. Playing it in a host

- In REAPER, and in jalv with JACK or PipeWire: live guitar or a looped
  DI file → plugin → a synth, at buffer sizes 64–1024.
- Record:
  - DSP load;
  - any xruns;
  - whether the latency feels right;
  - how the output sounds compared with the offline transcription.
- Add the controls that playing shows are needed, such as an onset
  threshold override or a "use model default" switch. Then write a
  `README.md` with build, install and use instructions.

First session in REAPER (2026-10-25, `data4-big-wd03-e120`, live DI
guitar), as reported:
- The plugin loads, and REAPER offers a file browser for the Model
  parameter.
- DSP load is fine, with no real dropouts. Once REAPER stopped producing
  audio; it isn't known whether the plugin caused it.
- The latency feels responsive, and repeated notes trigger well. New
  notes after silence trigger patchily.
- Input Gain has to be high before notes trigger, and set too high it
  gives many false onsets. An onset threshold control was asked for.

Checked offline: are notes after silence harder for the model? Not on
the test split. Recall of `data4-big-wd03-e120`'s test-set MIDI from
the training code's evaluation, by the silence before each label note
(no label note sounding):

| silence before | notes | recall |
|---|---|---|
| none (overlapping) | 23,048 | 0.426 |
| 0–50 ms | 1,722 | 0.401 |
| 50–200 ms | 1,639 | 0.561 |
| 0.2–0.5 s | 734 | 0.571 |
| 0.5–1 s | 142 | 0.415 |
| > 1 s | 170 | 0.665 |

Notes after silence are rare in the data (312 after more than 0.5 s),
but the model recalls them at least as well as the rest. The live
patchiness is more likely to come from something the dataset lacks.
Two candidates:
- level: the training audio peaks between −17 and −1 dBFS;
- the noise floor (hum, pickup noise) raised with the gain between notes.

A DI recording of a passage that misbehaves will tell; see below.

Changes:
- **Onset Threshold** control (port 7, 0–0.95). 0, labelled "Model
  default", uses the model's tuned value. Any other value overrides it
  live.
- **Input Level** meter (output port 8, dB): the peak of what the model
  hears, after Input Gain, falling with a 0.3 s time constant. The
  comment points to the training range of −17 to −1 dBFS.
- Non-finite input samples go to the model as silence. A NaN or
  infinity from upstream would otherwise stay in the GRU's state for
  good.
- Tests:
  - the threshold matches the core library at 0, 0.3, 0.45, 0.6 and
    0.95;
  - the meter reads a −12 dB sine, follows the gain and falls at the
    stated rate;
  - NaN and infinity in the input give the same notes as zeros there.

  Writing the meter test found a bug before it shipped: `-n` on an
  unsigned sample count made the meter's fall factor infinite after
  the second buffer.

A display of the notes that are on (asked for 2026-10-25: "just a list
of note names with their octave numbers"), done without a custom UI:
- Output ports `note_count` (index 9) and `note_1` … `note_6` (10–15):
  the held notes, lowest first, −1 for an empty slot.
  - Each Note port is `lv2:enumeration`, with a scale point labelling
    every MIDI note with its name. CMake generates these into the TTL
    (`G2M_NOTE_PORTS`, 48 kB).
  - A host that shows scale-point labels on output ports displays
    "E2", "C#4" and so on. One that doesn't shows MIDI numbers.
  - Six slots, one per string. Re-strikes can briefly overlap, so the
    count may exceed 6.
- A readable `patch` property, `…#player-notes` (`atom:String`), such as
  "E2 B2 E3". It is sent on notify whenever the set of held notes
  changes, at the start, and on `patch:Get`. Hosts that show readable
  properties, `jalv.gtk3` among them, display it as text.
- Names use C4 = MIDI 60, so the low E string is E2, with sharps only:
  `core/include/g2m/note_name.h` (no allocation; tested).
- The `note_display` test checks the ports and the string after every
  random-sized run over `small.wav` against the MIDI sent. Three
  deliberate breaks were each caught: change detection removed, no
  string on `patch:Get`, empty slots not cleared.
- In REAPER (2026-10-25): the Note ports show correctly while playing.

Closed (2026-10-26):
- `README.md` written: build, models, the plugin's controls and outputs,
  the tools, checking against the reference, and limitations.
- Left open, and moved to "Later": the patchy notes after silence, the
  input level, and the one time REAPER stopped. The model and the
  reference are being improved in the training code, so the live problems
  are investigated there first. This project picks up new models and
  any format change.

## Later (not in this plan)

- Patchy notes after silence and the need for high input gain (step 6).
  The test data shows no weakness after silence, so look at the live
  signal:
  - record a dry DI passage that misbehaves (48 kHz, gain 0 dB);
  - run `g2m-transcribe --outputs` and look at the onset and frame
    probabilities at the missed notes: just under the threshold,
    dropped by the gate, or hidden in noise?
  - compare the Input Level reading with the training range, −17 to
    −1 dBFS.

  Likely fixes are in the training code: augmentation with level, noise and
  hum.
- The one time REAPER stopped producing audio (step 6), cause unknown.
  If it recurs, note whether a model load was in progress. If REAPER
  runs the LV2 worker in the audio thread, loading would block it for
  about 20 ms; a lighter swap would then help.
- New models from the training code: export, copy to `models/`, and check
  against the reference (README). A format version change means
  updating `core/src/model_file.cpp` and `network.*` together with the
  test vectors.
- Resampling for 44.1 / 88.2 / 96 kHz hosts.
- Pitch bend, once the training code has it. That needs a format version bump
  here.
- SIMD / NEON kernels and fast activations. A Raspberry Pi build and
  timing.
- Other targets on the same core: CLAP / VST3, a standalone JACK app,
  bare metal (weights as a `const` array, no file I/O, no heap).
- A plugin UI (a model file picker is enough at first; hosts provide one
  for `atom:Path` parameters).
