# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

Runs the note models trained by the training code (private, not in this repository) in real time: a DI electric guitar signal (mono, any common rate; the model hears 48 kHz) in, MIDI note-on (with velocity) and note-off out, with no added latency beyond the model's own (and the resampler's, about 0.4 ms, at other rates); plain on one channel, or MPE with one channel per string. The plugin is built with DPF as LV2, VST3, CLAP and a standalone app; later targets (a Raspberry Pi class board, possibly bare metal) reuse the same core library.

- `core/`: the portable C++ library. Loads an exported model file, runs it one 64-sample block at a time, decodes the outputs into note events. No dependencies beyond the C++17 standard library; no platform, plugin or host code.
- `plugin/`: the plugin, a thin DPF adapter over `core/` (until 2026-11-01 a hand-written LV2 plugin in `lv2-plugin/`).
- `external/dpf`: DPF (DISTRHO Plugin Framework, ISC), a git submodule.
- `external/lvgl`: LVGL 9.4.0 (MIT), a git submodule, for the plugin's UI.
- `external/khronos`: Khronos's `GL/glext.h` and `KHR/khrplatform.h` (MIT, copied from Ubuntu's libgl-dev), for DPF's OpenGL code on Windows, whose SDK has no `glext.h`.

Started 2026-10-24. The first plan, `docs/initial-implementation.md` (core library and LV2 plugin), closed 2026-10-26; its measurements and decisions stay there, and its "Later" list is the backlog. Start a new doc in `docs/` for the next piece of work. The current one is `docs/release-plan.md`: a player others can install (DPF, UI, gate, sustain, bend modes, bundled models, presets). MPE output (steps 1 and 3 of the player's part of the MPE plan: the rule-based allocator, then format version 3's string probabilities and legato overlaps, and version 4's per-note pitch bend) is recorded in `docs/mpe-output.md`, and CPU time (since step 3, weights tiled 32 rows at a time with SSE2 / NEON kernels in `network.cpp`, `G2M_NO_SIMD` for the plain loops: 8 % of the budget on a P-core, 15 % on an E-core; 7 % / 13 % for a projected version 5 model; on a Raspberry Pi 4, 57 % for that model and 39 % with version 6's float16 weights, kept as float16 in the tiles on AArch64; the stages with `g2m-bench --stages`) in `docs/performance-plan.md`, which has the string allocator's rule, legato step included, in the detail the training code's allocator needs to port it. The headless Raspberry Pi 4 setup (the LV2 plugin with synthv1 in mod-host, JACK on an HX Stomp, `packaging/raspberry-pi/start-g2m.sh`) is `docs/raspberry-pi.md`. User-facing build and use instructions are in `README.md`.

## The model and its file

The training code owns the model, the weights file format and its spec: its model, decoder, exporter (checkpoint → `.g2m` file), pure-NumPy streaming reference (the spec for this port) and format spec. Read those, not this file, for the exact computation; `CLAUDE.local.md` (git-ignored) says where they are. If the C++ and the reference disagree, the reference is right unless it is shown to be wrong, and then it is fixed there first.

Current architecture (all sizes come from the file's header, never hard-coded):

1. Front end: strided causal `Conv1d` + ReLU stages on the raw audio whose strides multiply to `hop` (64): one frame per block.
2. `rechannel` 1x1 conv, then gated dilated causal convs: `x += mix(tanh(a) * sigmoid(b))`, `[a, b] = conv(x)`; from format version 5 a layer may be projected: its conv reads `z = proj(x)` (k values, kept in its ring, reset to `proj`'s bias) instead of `x` (the training code's projection plan).
3. A single-layer GRU (PyTorch gate order and equations: r, z, n).
4. A linear head on `[conv features, GRU output]` → `(pitches, 3)` logits: onset, frame, velocity; sigmoid of each.
5. From format version 3, optional MPE heads on the same input: class heads `string` (6 classes) and `onset_type` (picked, legato, slide), a softmax per pitch, and from version 4 curve heads, one clamped value per pitch (`pitch_offset`, semitones); each computed only for the pitches and frames the decoder needs.
6. Decoder: per pitch, one frame at a time, a port of `Decoder.step` (rising-edge onsets, gate, frame wait, off frames, re-strikes, from format version 2 the late fallback: a note starts when the frame probability stays high without an onset, from version 3 each note-on's heads, taken from the frame its velocity comes from, and its onset type, and from version 4 each sounding note's pitch bend every frame, with a dead band, as 14-bit values and bend events). Its settings come from the file (tuned per model) and may be overridden.

A model change in the training code that adds an op or a decoder setting means a new format version there and here; the loader reads versions 1–6 (`kMinFormatVersion`–`kFormatVersion` in `model.h`; version 1 = 2 with the fallback off, versions 1–2 = 3 with no heads, versions 1–3 = 4 with no curve heads, versions 1–4 = 5 with no projected layers, versions 1–5 = 6 with float32 weights; version 6's float16 weights are widened exactly at load, `half_to_float`, so `Model::weights` is always float32; on AArch64 the network's tiles keep them as float16, `Network::float16_tiles()`, the training code's float16 plan) and rejects versions, layer types and heads it doesn't know. `docs/decoder-fallback.md` is the record of the version 2 change.

## Real-time and portability rules

- Everything is allocated when a model is loaded; `process` never allocates, locks, does I/O or throws. Loading (file I/O, parsing, allocation) happens off the audio thread; the plugin swaps a fully built instance in.
- Plain loops over contiguous `float` arrays, weights laid out for row-major matrix-vector products. Optimise (SIMD, NEON, fast `tanh` / `sigmoid`) only after measuring, behind the same API, and only if decoded events on the test vectors stay identical.
- Flush denormals to zero on the audio thread (x86 FTZ/DAZ, ARM FZ).
- Any host block size: the core buffers input into 64-sample blocks itself. An event decided in frame `k` belongs at the end of the frame, absolute sample `(k + 1) * hop`, as in the training code; the plugin places it on that frame's last sample within the host buffer.
- Other host rates: the model's input goes through `g2m::Resampler` (about 0.4 ms of delay on the MIDI, not reported to the host); the audio output isn't resampled. At a rate it can't convert (outside 8–384 kHz, or more than 1,024 phases) the plugin logs an error and emits no MIDI.
- Velocity: `clip(round_half_even(v * 127), 1, 127)` to match `np.rint` (`std::nearbyint` in the default rounding mode).

## Layout

Planned; update as it's built (see `docs/initial-implementation.md`):

- `CMakeLists.txt`: top level, adds `core/`, `external/dpf` and `plugin/`.
- `core/include/g2m/`: public headers (namespace `g2m`): `model.h` (`load_model`, `Model`, `DecoderSettings`), `note_name.h` (MIDI number → "E2", C4 = 60), `denormals.h` (`FlushDenormals` for audio callbacks), `resampler.h` (`Resampler`: the host rate to the model's, polyphase windowed sinc, each output tagged with the input sample that completed it), `noise_gate.h` (`NoiseGate`, header-only: no lookahead, hold and hysteresis, a linear fade), `sustain.h` (`Sustain`: piano, sostenuto and freeze pedal modes between the `Transcriber`'s events and the MIDI writers), `bend_mode.h` (`BendModeFilter`, after `Sustain`: natural, retrigger and off bend modes, retriggers to a `NoteSink`; `MpeOutput::retrigger` keeps the note's string), `transcriber.h` (`Transcriber`, the streaming API: audio in any block size, `NoteEvent`s, and `BendEvent`s for version 4, to an `EventSink`, each timed at the absolute sample where its frame ends), `string_allocator.h` (`StringAllocator`: the string, i.e. MPE member channel, of each note at its note-on, weighted by the model's string probabilities when it has them, and the legato step: a legato or slide note continues a note on its string) and `mpe.h` (`MpeOutput`: MPE configuration messages and notes on their strings' channels, to a `MidiWriter`). `core/src/`: `json.*` (header parser), `model_file.cpp` (loader and checks), `network.*` (the model, one block at a time), `decoder.h` (line-by-line port of the spec's decoder), `transcriber.cpp`, `string_allocator.cpp`, `mpe.cpp`, `sustain.cpp`, `bend_mode.cpp`, `resampler.cpp`.
- `core/tools/`: `g2m-transcribe` (WAV → MIDI file, for comparing with the reference's `--wav`; `--events` for the events with their MPE heads and bends, `--mpe` for MPE files and the allocator's step counts, `--decode` to decode the reference's outputs instead of running the model) and `g2m-bench` (CPU time per frame; `--stages` per stage). Minimal WAV reader and SMF writer, no libraries.
- `core/test/`: test executables run by CTest, with `check.h` (a minimal harness), `vectors.h` (reads the test vectors) and `allocations.h` (counts `operator new`, for no-allocation checks); `core/test/data/`: test vectors (small random-weight models: `small` and `odd` in format version 2, `mpe` in version 3 with both heads, `bend` in version 4 with the pitch-offset head too (and `bend.mpe.mid`, reference.py's MPE output), `proj` in version 5 (`mpe`'s model with its gated layers projected, k = 5, 0, 11), `half` in version 6 (`bend`'s model projected the same way, float16 weights, and `half.mpe.mid`); input audio, the reference's outputs per block and events, with each note-on's heads for `mpe`), made by `core/test/make_vectors.py` with the training code's Python (`G2M_TRAINING`; `CLAUDE.local.md`). Decoder (heads included) and string-allocator (legato step included) tests should reach every branch: when changing them, break a condition on purpose and check a test fails.
- `plugin/`: `g2m_plugin.cpp` (the plugin: the noise gate on the input, whose output the model hears after the Input Gain and the audio output carries at the Output Level (or muted), a chunk at a time; the sustain pedal (a CC on the MIDI input, the buffer split at its events so it acts at their sample, or the Sustain parameter) between the model's events and the MIDI output, then the bend mode; parameters (the old LV2 port symbols; new ones are appended, never renumbered), the "model" state loaded in `setState` (off the audio thread) and swapped in under a lock `run()` only tries, held-note tracking, the notes-on parameters, the model status outputs (Model Loaded, Model Format, Model Error) for the UI, plain / MPE output modes), the bundled models (`G2M_BUNDLED_MODELS`: names compiled into the generated `g2m_bundled_models.h`, files copied into each format's resources folder, chosen with the Model parameter, loaded by a loader thread, saved in state as `bundled:<name>`), `presets.*` (preset JSON files by parameter symbol, the user preset folder; library `g2m_presets`, tested by `test/test_presets.cpp`), `factory_presets.h` (the factory presets, the plugin's programs), `cmake/fix_lv2_presets.cmake` (repairs DPF's generated `presets.ttl`, which breaks where an output parameter follows an input, and drops the Model and Sustain ports from it), `params.h` (the parameter indices and definitions, DPF `Parameter`s, shared with the UI so it has the same ranges, labels and symbols), `g2m_ui.cpp` (the UI, with LVGL: the presets menu with user preset files, the model menu and Load file (DPF's file browser), cards for the input (meter, gate, gain, onset threshold), MIDI, sustain and audio output, the notes strip; it only sets parameters and the "model" state; `docs/release-plan.md`, "UI"), `ui/` (`LVGL.hpp`/`.cpp`: DPF-Widgets' LVGL wrapper, copied at 730da63 with changes marked "g2m": `makeCurrent()`, typed text from character input, quick clicks and fast typing kept; `lv_conf.h`: LVGL's settings), `DistrhoPluginInfo.h` (DPF's settings: the URI, IDs, ports, the UI), `CMakeLists.txt` (`dpf_add_plugin`, `G2M_PLUGIN_FORMATS`, `G2M_BUNDLED_MODELS`, install), `test/test_lv2_plugin.cpp` (an offline lilv host for the built LV2 bundle, with a synchronous worker; its MIDI must equal the core `Transcriber`'s, through `MpeOutput` in MPE mode; it counts allocations inside the plugin's `run()`).
- `docs/`: plans and findings (measurements, CPU time per frame, comparisons with the reference), with the commit and model they came from.
- `packaging/`: `package.cmake` (the `g2m-package` target: a zip with a folder per format, `INSTALL.md`, `THIRD_PARTY.md`, the player's `LICENSE` and the third-party licences; refuses bundled models unless `G2M_PACKAGE_SHAREABLE`) and `INSTALL.md` (for users: where each format goes per OS, MIDI routing, presets). `THIRD_PARTY.md`: the code by others in the plugin. CI: `.github/workflows/player.yml` (Linux, Windows with MSVC, macOS universal: build, test, and a package bundling `models/v1-0-alpha.g2m`), on every push. Keep the build portable to MSVC and Apple Clang: warning flags through `G2M_WARNINGS`, no shell in tests (`core/test/run_and_compare.cmake`).
- Git-ignored: `build/`, `tmp/` (scratch), `models/` (exported `.g2m` files of real models), except `models/v1-0-alpha.g2m`, the shareable model CI bundles in the packages (`MODELS.md`).

## Commands

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j   # build
ctest --test-dir build --output-on-failure                            # tests
cmake -B build/bundled -DG2M_BUNDLED_MODELS="core/test/data/small.g2m;core/test/data/mpe.g2m" && cmake --build build/bundled -j \
  && build/bundled/plugin/test_lv2_plugin   # the bundled-model tests (the plain build skips them; this one skips the rest)
ASAN_OPTIONS=verify_asan_link_order=0 cmake -B build/asan -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  && cmake --build build/asan -j && ctest --test-dir build/asan --output-on-failure   # tests under sanitizers (the ASAN_OPTIONS let the LV2 TTL generator load the plugin while it links)
build/core/g2m-transcribe --model models/M.g2m IN.wav [-o OUT.mid] [--block N] [--outputs OUT.f32] [--events OUT.json] [--set KEY=VALUE] [--mpe] [--bend-range N] [--decode REF.f32]   # WAV → MIDI, as reference.py (or MPE)
build/core/g2m-bench --model models/M.g2m [--wav IN.wav] [--seconds S] [--stages]   # time per block vs the real-time budget (--stages: per stage)
```

Plugins (built into `build/bin/`: `g2m.lv2`, `g2m.vst3`, `g2m.clap`, standalone `g2m`; LV2 URI `https://github.com/gwaggott/guitar-2-midi#player`, never change it; `git submodule update --init --recursive` first):

```bash
cmake --install build                                                  # copy them to ~/.lv2, ~/.vst3, ~/.clap (G2M_*_INSTALL_DIR)
LV2_PATH=DIR lv2info 'https://github.com/gwaggott/guitar-2-midi#player'  # DIR holds bundles (a link to build/bin/g2m.lv2 will do), not the bundle itself
LV2_PATH=DIR jalv.gtk3 'https://github.com/gwaggott/guitar-2-midi#player' # run it live; the GUI can set the Model file
sord_validate $(dpkg -L lv2-dev | grep '\.ttl$' | grep -v -e manifest.ttl -e '\.meta\.ttl$' -e /eg-) build/bin/g2m.lv2/*.ttl   # check the TTL
```

To check a new model against the reference (the commands with paths are in `CLAUDE.local.md`): export it in the training code, copy it to `models/`, run `reference.py MODEL --wav IN --outputs ref.f32 -o ref.mid` and `g2m-transcribe --outputs cpp.f32 -o cpp.mid` on a few files, and compare (MIDI identical, outputs within about 1e-5). With MPE heads, add `--events` on both sides and compare the note-ons' onset types and string probabilities too; with a pitch-offset head also `--mpe` (the reference: `--mpe OUT.mid`), and expect rare one-step bend differences from float rounding at a 14-bit boundary, so check the decoder exactly with `g2m-transcribe --decode ref.f32`.

Add the tool, plugin install and host commands here as they appear.

## The repository

Public on GitHub as `gwaggott/g2m-player` since 2026-11-22, starting from a single commit: the earlier history (in a private repository, and before that with the training code) isn't published, as it refers to the training code. This repository is the only home of the player code; CI runs on every push.

- The model, the `.g2m` format and its spec belong to the training code (its format spec and reference). A new op, output or decoder setting means a new format version there and here, changed together: the spec, the exporter, the reference, this loader and both sides' tests, each repository's commit naming the other's.
- Licences (decided 2026-11-22): the code is GPL-3.0-or-later (`LICENSE`; the plugin's `getLicense()`), the bundled model CC BY 4.0 (`MODELS.md`). Code by others must stay GPL-3-compatible and be listed in `THIRD_PARTY.md`.
- Work on a branch; merge into `main` only when the user asks, and push only when asked. Commit messages end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Stage files by name: never commit symlinks (a worktree's links to `models/` or builds).
- No ARM CI: after changing `network.cpp`'s NEON or float16 paths, build and test on the Raspberry Pi 4 (`ssh rp4-g2m`): the core with plain `g++ -std=gnu++17 -O3` into `~/g2m-bench` (`docs/performance-plan.md`, "Raspberry Pi 4"), or the whole build with CMake in `~/g2m-player` (`docs/raspberry-pi.md`). Its JACK and mod-host chain may be in use: ask before stopping or restarting it.
- One heavy job at a time on this machine: don't start a long build or benchmark while a GPU training run or REAPER render from another session is going.
- Bundled models: local builds bundle private models (IDMT, the MDM Strat: private use only). Never share or publish a build, package or model made from them.
- The LV2 URI (`https://github.com/gwaggott/guitar-2-midi#player`) and the parameter symbols never change; parameters are only appended.
- The training code isn't public: tracked files (docs, comments, code) call it "the training code" and never name its repository, link to it, or give its paths or commits; its parts are "the reference" (`reference.py`), "the exporter" (`export.py`) and "the format spec". Its location and the cross-repository workflow are in `CLAUDE.local.md`, which is git-ignored.
- Keep `README.md`, this file and the plan docs current when something lands. Scratch output goes in `tmp/` (git-ignored).

## Conventions

- C++17, CMake. Match the style of the training code: small plain functions, comments only where something isn't obvious, prose docs that record measurements and the reasons behind decisions.
- Inspiration: Neural Amp Modeler (a local checkout's path is in `CLAUDE.local.md`): `NeuralAmpModelerCore` (MIT) for streaming conv layers and ring buffers, `neural-amp-modeler-lv2` (GPL-3) for the plugin structure (worker-thread model loading, RT-safe swap, state save / restore). Borrow ideas; don't copy GPL code into this project, and credit MIT code that is adapted.
- Tests must not need a real trained model, a GPU or an audio host; real-model checks are tools whose results go in `docs/`.
