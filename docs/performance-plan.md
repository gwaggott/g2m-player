# CPU performance plan

Started 2026-11-08, on `player-release`. In Carla the plugin alone shows
60–70 % CPU (the user, 2026-11-08). This plan finds out why and lists the
changes, in order, that bring it down. Record measurements here as the steps
land, with the commit and model they came from.

## What Carla's figure is

Carla's per-plugin figure is the time spent in `run()` as a share of the
audio period. That is the same measure as `g2m-bench`: time per 64-sample
block (one model frame) against its real-time budget, 1,333 µs at 48 kHz.
The period size doesn't change the share, because each block always costs
one network step.

## Findings (2026-11-08, commit d9abc3f, `mdm-pitch-offset.g2m`)

The model is 128 channels, six gated layers (dilations 1–32, kernel 3), a
256-unit GRU, 51 pitches and format version 4 heads. That works out to
about a million multiply-adds per frame, 750 frames a second.

`g2m-bench --seconds 10`, noise input, denormals flushed, pinned with
`taskset` on the i9-12900F (CPUs 0–15 are P-cores, 16–23 E-cores):

| Core | Mean | Median | p99 (µs) | Share of 1,333 µs |
|---|---|---|---|---|
| P-core (CPU 2) | 374 | 372 | 413 | 28 % |
| E-core (CPU 20) | 922 | 917 | 994 | 69 % |

The 28 % in the earlier docs was measured on a P-core. **On an E-core the
plugin uses 66–69 %, which matches Carla.** The audio thread can land on
either kind of core, and the plugin can't choose which. The real target is
therefore the E-core figure, with headroom: laptops and older desktops are
no faster than an E-core.

### Why it's slow: the loops don't vectorise

Every dot product (`matvec` in `core/src/network.cpp`, the gated layers'
mixers, the front end's convolution) adds up in one `float` in a fixed
order. The compiler may not reorder float additions, so it can't use SIMD
lanes, and the whole network runs one multiply-add at a time.

The same code, built only for `g2m-bench` in the scratchpad with different
flags (5 s of noise, median µs):

| Flags | P-core | E-core |
|---|---|---|
| Release, as shipped (SSE2) | 372 | 917 |
| `-march=native` | 363 | 858 |
| SSE2 + reassociation (`-fassociative-math -fno-signed-zeros -fno-trapping-math -fno-math-errno`) | 138 | 319 |
| `-march=x86-64-v3` (AVX2, FMA) + reassociation | 109 | 221 |
| `-march=native -ffast-math` | 85 | 132 |

- **Allowing reassociation alone gives 2.7–2.9×**, with no new CPU
  requirement.
- AVX2 + FMA gives about 4×.
- `-ffast-math` reaches 7× on the E-core. Part of the gain is probably
  glibc's vector `tanh` / `exp`, which needs `-ffast-math`; that isn't
  checked yet.
- Under `-ffast-math` the core's tests, including every comparison with the
  reference within 1e-5, still pass. Two plugin tests fail:
  `-ffinite-math-only` removes the checks for NaN and infinity
  (`test_presets` `values_default_clamp_and_ignore`, `test_lv2_plugin`
  `non_finite_input_is_silence`). So global `-ffast-math` is ruled out,
  but reordering the sums is safe within the tolerance the tests set.

### Per stage (step 1, 2026-11-11)

`g2m-bench --seconds 5 --stages`, noise, flushed, mean µs per block. Every
model in `models/` has the same network (128 channels, GRU 256), so they
all take the same time to within 1 %; the heads only add to the decoder,
which asks for them at a note's start. `mdm-pitch-offset`:

| Stage | Multiply-adds | P-core (CPU 2) | E-core (CPU 20) | Share |
|---|---|---|---|---|
| Front end + rechannel | 40 k | 14.3 | 31.6 | 3–4 % |
| Gated layers (6) | 688 k | 245.2 | 615.1 | 65–66 % |
| GRU | 295 k | 93.7 | 228.6 | 25 % |
| Head (153 outputs) | 59 k | 21.3 | 51.7 | 6 % |
| Decoder (with the MPE heads) | – | 0.6 | 2.0 | 0 % |
| Total | 1.08 M | 375 | 929 | 28 % / 70 % of 1,333 µs |

- **The time follows the multiply-adds**: each stage's share of the time
  is its share of the MACs, about 0.35 ns per MAC on the P-core and
  0.85 ns on the E-core, one at a time. Steps 2–4 (the matvecs) are where
  the gain is.
- The gated layers' 768 `tanh` a frame and the GRU's 768 are hidden in
  those figures; they're small next to the matvecs now, so step 5 waits
  until steps 2–4 have shrunk the rest.
- The decoder doesn't matter.
- Timing the stages costs nothing measurable (totals as without
  `--stages`, the table above).

All models: `tmp/bench-stages.txt` (git-ignored).

### After step 2 (2026-11-11)

`dot()` in `core/src/network.cpp`: 16 running sums (element `i` into sum
`i % 16`), added pairwise at the end, used by every matvec, the mixers and
the front end. GCC vectorises it at `-O3` with SSE2, no new flags.
`g2m-bench --seconds 10 --stages`, `mdm-pitch-offset`, flushed, mean µs
per block (p99 in brackets):

| Input | P-core before | P-core after | E-core before | E-core after |
|---|---|---|---|---|
| Noise | 375 (415) | 131 (150) | 929 (1,016) | 267 (298) |
| Silence | – | 115 (139) | – | 225 (253) |
| Sultans of Swing track 3 (202 s) | – | 124 (145) | – | 245 (286) |

- **2.9× on the P-core, 3.5× on the E-core**: noise now takes 20 % of the
  budget on an E-core, p99 22 %, so **the target (under 25 % on an E-core)
  is met**, with SSE2 alone.
- Per stage on the E-core (noise): front end 28.0 µs (11 %), layers 156.7
  (59 %), GRU 69.4 (26 %), head 10.5 (4 %), decoder 1.9.
- **The front end hardly moved** (31.6 → 28.0 µs). Its second stage has a
  kernel of 8, so each of its `dot()` calls is shorter than the 16 sums
  and runs in the scalar tail. That stage's input for its one output
  column is `[32 channels][8]`, contiguous like its weight rows, so it
  could be one `dot()` of 256: step 3.
- **`tanh` now shows**: silence is 15 % faster than noise on the E-core
  (layers 126 against 157 µs, GRU 61 against 69), and only the
  activations depend on the input. glibc's `tanh` takes a short path for
  values near zero. That's step 5's case.
- Correctness, against the build before the change, on the six files
  below (GuitarSet Jazz2 p00 solo and Rock2 p05 comp, IDMT Lick1 LP_KN and
  Lick2 AR_KN, Sultans of Swing track 3, `design_for_life_di.wav`):
  MIDI byte for byte the same; every event the same (time, pitch,
  velocity, onset type, 14-bit bend); outputs within 5.5e-6, string
  probabilities 7e-7, semitones 1.4e-6. Against `reference.py` on IDMT
  Lick1: MIDI identical, outputs within 8.7e-6 (8.8e-6 before).
- `ctest` passes in `build/plain`, `build/local` (the private models
  bundled) and `build/asan`; `build/bundled`'s `test_lv2_plugin` (no
  allocation in `run()` included) too.

Bench output: `tmp/perf/bench-step2.txt`; the check files and the script
that runs them, `tmp/perf/` (git-ignored).

### Projected gated layers, format version 5 (2026-11-12)

Not one of the steps below: from the training code's weight analysis
(the gated layers' inputs use 10–88 of their 128 dimensions) and
its projection plan. `models/mdm-pitch-offset-p999.g2m`
is `mdm-pitch-offset` with each gated layer's input projected at 99.9 % of
its variance (k = 11, 29, 55, 72, 81, 87): 289,664 fewer MACs per frame
(the gated layers 688 k → 399 k), the file 5.09 → 3.93 MB, note-on F1 on
validation unchanged in every pitch range.

`g2m-bench --seconds 10 --stages`, noise, flushed, mean µs per block
(`tmp/perf/bench-p999.txt`):

| | P-core: block | gated layers | E-core: block | gated layers |
|---|---|---|---|---|
| `mdm-pitch-offset` | 141 | 82.9 | 268 | 157.2 |
| `mdm-pitch-offset-p999` | 120 | 62.4 | 249 | 137.0 |

- **15 % off the block on a P-core, 7 % on an E-core**, against 42 % of
  the gated layers' MACs. What's left in the gated layers doesn't shrink
  with `k`: 1,536 conv rows a frame still each pay `dot()`'s 16-sum
  reduction and tail, 1,536 `tanh` / sigmoid pairs, and the mixers'
  98 k MACs. Shorter rows make that fixed part dominate: **step 3 (several
  rows at once, no gather) now matters more**, for every model.
- Against `reference.py` on the six recordings of step 2: MIDI identical
  and outputs within 1.2e-5 on five; on `design_for_life_di.wav` (the one
  stereo file) every note-on is the same but one note-off is a block
  apart and outputs differ by up to 1.6e-4, exactly as with the
  unprojected model: an older difference on that file, not the
  projection's (open: the C++ and the reference read or mix it slightly
  differently, or 100 s of GRU state drifts).

### After step 3 (2026-11-12)

Every matvec but the MPE heads' (a few rows per pitch, still `dot()`)
uses weights tiled at load (`Network::Tiled`, `network.cpp`): rows in
tiles of 32, each tile's columns one after another with its 32 weights
per column, zero rows padding the last tile, the columns in the order the
step reads its inputs. A tile keeps 8 SSE2 (or NEON) vectors of running
sums in registers, one sum per row, over the columns in order: no
reduction per row, no tail. The gated convs read their taps straight from
the ring (columns ordered tap by tap), and the front end's strided
windows in place, so `taps_` and the copy are gone. The weights are
copied once per `Network` (about the model's size again).

- **The plain loop didn't work**: GCC vectorised it across columns and
  kept the sums in memory (2.6× slower than step 2). Hence the intrinsics,
  with the plain loop kept for other CPUs and for tests
  (`-DG2M_NO_SIMD`, which passes every test and matches the reference).
- The sums now run over each row's columns in order (as before step 2),
  not 16 lanes; the outputs moved by up to 7.6e-6 against step 2 on the
  six recordings, the MIDI identical for both models (and still on
  `design_for_life_di.wav` the projected model's one older note-off).

`g2m-bench --seconds 10 --stages`, flushed, mean µs per block (p99),
`tmp/perf/bench-step3.txt`:

| Model, input | P-core step 2 | P-core step 3 | E-core step 2 | E-core step 3 |
|---|---|---|---|---|
| `mdm-pitch-offset`, noise | 141 | 109 (130) | 268 | 201 (230) |
| `mdm-pitch-offset`, Sultans of Swing | – | 98 (119) | – | 180 (215) |
| `-p999`, noise | 120 | 88 (105) | 249 | 177 (207) |
| `-p999`, Sultans of Swing | – | 80 (100) | – | 155 (201) |

- 21–29 % off a block. The front end went from 27 to 5 µs on the E-core.
- With the projection (format version 5) the E-core takes 177 µs against
  step 2's 268: **13 % of the budget** on noise.
- **`tanh` is now a large share.** On the E-core silence is 20 % faster
  than noise (layers 98 against 129 µs, `-p999` 76 against 105), and only
  the activations depend on the input. **Step 5 (a fast `tanh`) is next**
  if more is wanted.

### Raspberry Pi 4 (2026-11-15, commit bd2b84b)

A Pi 4 Model B (Cortex-A72, 4 cores at 1.8 GHz, 1 MB shared L2, 2 GB;
Raspberry Pi OS Lite, Debian 13, GCC 14.2), reached as `rp4-g2m`. The core
built there with plain `g++ -std=c++17 -O3 -DNDEBUG` (no cmake on the Pi),
on the NEON path (its first run on ARM). All 11 test programs pass, and
`g2m-transcribe`'s `small.mid` and `bend.mpe.mid` are identical to the
reference's. `taskset -c 3`, `ondemand` governor at 1.8 GHz during the run,
not throttled:

| Model | Mean | p99 | Max | Budget | Layers | GRU | Head | Front end |
|---|---|---|---|---|---|---|---|---|
| `mdm-pitch-offset-p999` | 766 | 817 | 1,218 | 57 % | 392 | 289 | 54 | 31 |
| `mdm-pitch-offset` | 1,025 | 1,105 | 1,573 | 77 % | 652 | 281 | 56 | 32 |

(µs per block, noise; the stages with `--stages`, which adds about 4 µs.)

- **Memory bound.** Silence costs the same as noise, the layers and GRU run
  at about one multiply-add per nanosecond (four bytes of weight each, about
  4 GB/s), and the test model `proj.g2m`, which fits in cache, takes 13 µs.
  The weights (3.9 MB for `-p999`) don't fit in the 1 MB L2, so every block
  reads them from RAM.
- **Two instances at once** (cores 2 and 3) take 1,377 and 2,186 µs, both
  over budget: they share the memory bandwidth. A synth or anything else
  streaming memory on the other cores will slow the model too.
- The worst blocks (1.2 ms) leave almost no margin, so the current model is
  not usable live on a Pi 4. What would be: weights in float16 (half the
  bytes, converted to float32 as they load) or int8, or a smaller network
  that fits in L2. Both need a format version, in the training code too.
  A fast `tanh` (step 5) won't help here.

### Float16 weights on the Pi 4 (2026-11-15, format version 6)

The training code's float16 plan: a version 6 model's weights are
float16 values, and on AArch64 the tiles keep them as float16
(`Tiled::half`), widened with `vcvt_f32_f16` in the NEON kernel; elsewhere
they're widened to float32 at load. The same sums either way.
`mdm-pitch-offset-p999-f16` (`mpe-po`, `--project 0.999 --float16`,
1.97 MB), `taskset -c 3`, µs per block:

| Model | Mean | p99 | Max | Budget | Layers | GRU | Head |
|---|---|---|---|---|---|---|---|
| `-p999` (float32) | 764 | 825 | 1,290 | 57 % | 392 | 289 | 54 |
| `-p999-f16` | 513 | 547 | 740 | 39 % | 269 | 184 | 37 |
| `-p999-f16`, two at once | 738 / 1,087 | 790 / 1,156 | 912 / 1,371 | | | | |

- A third off a block, as the prototype said. Still bound by memory (two
  instances slow each other), so one instance per Pi, with a light synth.
- Every core test passes on the Pi with the float16 tiles on (`half`
  within 3e-7 of the reference). On the six check files (step 2's list),
  the Pi's MIDI is byte for byte x86's, every event the same but one bend
  a step of 14 bits apart (semitones 3e-8 apart, at a rounding edge),
  outputs within 5.4e-6.
- x86 is unchanged: version 6 runs there as float32 (P-core 90 µs, E-core
  174, against 91 and 174 for `-p999`).
- `models/mdm-pitch-offset-p999-f16.g2m` against `reference.py` on the
  six check files: the MIDI identical on five, outputs within 1.1e-5; on
  `design_for_life_di.wav` one note-off a block apart, the older
  difference the float32 models show too. Bundled in `build/local`.

### Carla after step 2: the clock, not the code (2026-11-11)

Carla (JACK, 48 kHz, 1,024-sample periods, one instance) showed 40–55 %
after step 2, and its audio thread used about 46 % of a core. Bypassing
the plugin dropped that to 1–3 %; closing the UI changed nothing. A
diagnostic build logged Carla's calls: 48 kHz, no resampler, 1,024
samples a call, no MIDI, the time all in the model, the same calls as an
offline lilv host that measures 20 % on an E-core. Neither dirty AVX
registers nor the input level made a difference offline.

The cause is the CPU clock. The model now runs in 4 ms bursts every
21 ms, and with the `powersave` governor and `balance_performance`
(power profile "balanced") the cores don't speed up for bursts that
short. Running the model at real-time pace (a scratch tool:
1,024-sample periods, sleeping between them):

| | Share of each period |
|---|---|
| Unpinned, normal priority | 18.5 % |
| Unpinned, SCHED_FIFO 5 (as Carla's thread) | 45.8 % (p99 58 %) |
| P-core 3 | 19.1 % (10 % flat out) |
| E-core 21 | 37.9 % (20 % flat out), the core at 1.3–2.4 GHz instead of 3.7 |

Before step 2 the plugin kept a core 70 % busy, which held its clock up,
so Carla agreed with the bench. Hosts show time, not work: the plugin
does a third of the work it did, but at about half the clock.

Nothing to fix in the plugin: the slowest periods still finish in under
60 % of the period. With the power profile on "performance"
(`powerprofilesctl set performance`) the bench's figures should hold in a
host; that's the usual setting for audio work. Steps 3–5 would shorten
the bursts further, but the clock effect would remain.

## Target

The plugin should stay **under 25 % of the budget on an E-core** (p99,
`mdm-pitch-offset`), with the reference checks still within 1e-5 and the
MIDI output identical on the recordings in `initial-implementation.md`.
That leaves room for a slower machine and for the resampler (release plan
step 7). A Raspberry Pi stays out of scope for this plan.

## Steps

1. **Measure per stage.** Add a `--stages` option to `g2m-bench` that
   times the front end, the gated layers, the GRU, the head and the decoder
   separately, so each later step shows where its gain came from. Record a
   P-core and an E-core baseline for each model in `models/`.
2. **Dot products the compiler can vectorise, without changing flags.**
   - Write one `dot(const float*, const float*, int)` with several
     independent accumulators (8 or 16, summed at the end), and use it in
     `matvec`, the mixers and the front end. GCC and Clang vectorise that
     at `-O2` and `-O3` with SSE2 or NEON, with no fast-math flags.
   - The order of the sums is then fixed in the source, so it's the same
     on every compiler and OS.
   - Check: the network and transcriber tests (1e-5 against the
     reference); `g2m-transcribe` on the recordings in
     `initial-implementation.md`, whose MIDI must stay identical; the
     bench. Expected: close to the "SSE2 + reassociation" row.
3. **Lay the weights out for the loops** (at load, in `Network`'s
   constructor; the `.g2m` format is unchanged):
   - The gated layers copy their taps out of the ring into `taps_` every
     frame, then do one matvec over `channels × kernel`. Split the weight
     by tap instead and do `kernel` matvecs straight from the ring columns,
     with no gather.
   - Fuse the GRU's input and hidden matvecs into one pass over the rows.
   - Keep weights 32- or 64-byte aligned.
4. **AVX2 + FMA where the CPU has it, chosen at run time.** A release build
   has to run on any x86-64 CPU, so it can't be built with `-march`.
   - Build the hot functions twice, with GCC/Clang's `target("avx2,fma")`
     (or `target_clones`), and pick one at load from `__builtin_cpu_supports`.
     On MSVC, a separate file compiled with `/arch:AVX2` and a `cpuid`
     check.
   - FMA rounds differently from a separate multiply and add. Run the same
     checks as step 2 on both paths, and in CI (step 7 of the release plan)
     run the tests with the AVX2 path forced off as well.
   - macOS arm64 and Linux arm64 get NEON from step 2 already; check that
     they vectorise there too.
5. **Faster `tanh` and sigmoid.** About 2,500 a frame (the gated layers,
   the GRU, the 153 note outputs). Step 1 shows whether they matter after
   steps 2–4. If they do, use a vectorisable approximation (a rational
   function, clamped), accurate to a few ulp, with the same checks against
   the reference. The heads' `exp` (the softmax) runs only for a few
   pitches a frame and can stay.
6. **Only if the target still isn't met:**
   - Skip the head rows for pitches the decoder can't use this frame (a
     decoder change; it would need `reference.py` to agree first).
   - Smaller weights: float16 done as format version 6 (2026-11-15,
     above); int8 would need another version and its accuracy measured.
   - A smaller network (channels, GRU size) is a training question; raise
     it there with these numbers.

Not planned: running the network on another thread. It would add a block
of latency or need locks between threads. A hand-written assembly or
intrinsics path for each CPU isn't needed unless step 4's compiler output
falls short.

## Checks for every step

- `ctest` in `build`, `build/bundled` and `build/asan`.
- `g2m-transcribe` on the recordings in `initial-implementation.md`, the
  MIDI identical to the previous build's, and `--events` within the
  tolerance the tests use.
- `g2m-bench` on a P-core and an E-core (`taskset -c 2` and
  `taskset -c 20` on this PC), recorded here with the commit.
- Allocation counting in `test_lv2_plugin` still passes (no allocation in
  `run()`).
- Carla's or REAPER's CPU figure for one instance, recorded here
  alongside the bench.

## Status

- 2026-11-08: findings recorded; nothing changed yet.
- 2026-11-11: step 1 done, on `perf-stages`: `g2m-bench --stages`
  (`Transcriber::time_stages`), the per-stage table above.
- 2026-11-11: step 2 done, on `perf-stages`: `dot()` with 16 sums. 3.5×
  on the E-core, 20 % of the budget: the target is met. Steps 3–5 are
  now optional headroom (slower machines, ARM, more instances).
- 2026-11-12: format version 5 (projected gated layers), on
  `g2m-v5-projection`: 15 % off a block on a P-core, 7 % on an E-core
  for `mdm-pitch-offset-p999`; step 3 is next.
- 2026-11-12: step 3 done, on `perf-step3` (from `g2m-v5-projection`):
  tiled matvecs with SSE2 / NEON, 21–29 % off a block; 15 % of the budget
  on an E-core, 13 % for the projected model. Step 5 next if needed; the
  GRU's two matvecs aren't fused (little to gain now).
- 2026-11-15: measured on a Raspberry Pi 4 (section above): 57 % of the
  budget for `mdm-pitch-offset-p999`, limited by memory bandwidth, two
  instances over budget. Smaller weights or a smaller network are what a
  Pi 4 needs.
- 2026-11-15: float16 tiles on AArch64 for format version 6
  (`g2m-v6-float16`, the float16 plan's step 3): 39 %
  of the budget on the Pi 4 for `mdm-pitch-offset-p999-f16`.
