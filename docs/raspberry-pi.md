# Raspberry Pi 4: the player with a synth, headless

Set up 2026-11-15. A Raspberry Pi 4 Model B (2 GB) running Raspberry Pi OS
Lite (Debian 13), a Line 6 HX Stomp as its USB audio interface, JACK, and
mod-host with the player and synthv1:

```
HX Stomp dry guitar (capture 5) → player (LV2) → synthv1 (LV2) → HX Stomp outputs 1/2
```

It works: the synth follows the guitar, and JACK shows no xruns while
playing.

## Build

The Pi builds the LV2 plugin itself (about 15 minutes with `-j4`, LVGL
included). Packages, beyond the LV2, lilv and JACK development ones:

```bash
sudo apt install cmake libgl-dev libx11-dev libxext-dev libxrandr-dev libxcursor-dev libdbus-1-dev
```

The X11 and OpenGL ones are only for the plugin's UI, which mod-host never
shows but the build still compiles.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DG2M_PLUGIN_FORMATS=lv2 \
  -DG2M_BUNDLED_MODELS="models/mdm-pitch-offset-p999-f16.g2m"
cmake --build build -j4 && ctest --test-dir build && cmake --install build   # into ~/.lv2
```

Bundle a format version 6 model: its float16 weights are what make it fit
on the Pi (`performance-plan.md`, "Float16 weights on the Pi 4"). The
current one is private (made with IDMT and the MDM Strat), so this build is
for the Pi only, not to be shared.

## Run

`packaging/raspberry-pi/start-g2m.sh` (copied to the Pi's home folder)
starts JACK on the interface, then mod-host, loads the player and the
synth, and connects them; `start-g2m.sh stop` stops both. Its settings are
variables at the top: the card (`hw:Stomp`), the period (256 frames, 3
periods), the dry input, the outputs and the synth. Logs go to
`~/g2m-logs/`.

Things the script takes care of:

- **synthv1 starts Qt**, which aborts without a display (and takes
  mod-host with it): mod-host runs with `QT_QPA_PLATFORM=offscreen`.
- **No feedback port:** with `-f` mod-host waits for a client on it before
  it answers commands. Without one, `param_get` can't read the player's
  output ports (input level, notes, model status).
- **The HX Stomp's dry signal is USB capture 5** (the user, 2026-11-15);
  outputs 1/2 are its main outputs. It's quiet: the script sets the
  player's Input Gain to +12 dB (`INPUT_GAIN`; below).
- Adding plugins and connecting clients gives a few xruns (14 at start-up);
  none follow. **Any JACK client starting or stopping costs about 15 xruns**
  (a MIDI monitor started and stopped five times: 73), so don't attach tools
  to JACK while playing.

## Measured (2026-11-15)

JACK at 48 kHz, 256-frame periods ×3 (5.3 ms each), the player with
`mdm-pitch-offset-p999-f16` and synthv1 at its defaults:

- JACK's DSP load 43–46 %; mod-host's real-time thread about 45–50 % of a
  core; the Pi at 44 °C, not throttled.
- No xruns over minutes of playing.
- 136 note-ons in the first two minutes; the chords came out as chords.

## Input gain (2026-11-15)

The player's velocities by Input Gain, the guitar playing throughout, 20 s
a step, up from 0 dB and back down (note-ons and velocities, up / down):

| Gain | Note-ons | Velocity median | p90 | Max |
|---|---|---|---|---|
| 0 dB | 67 / 88 | 33 / 23 | 51 / 35 | 68 / 45 |
| +6 dB | 95 / 90 | 53 / 56 | 75 / 89 | 92 / 102 |
| **+12 dB** | 110 / 111 | 66 / 65 | 99 / 97 | 111 / 115 |
| +18 dB | 140 / 132 | 90 / 108 | 120 / 120 | 125 / 124 |
| +24 dB | 185 / 358 | 115 / 118 | 126 / 126 | 127 / 127 |

From +18 dB the velocities pile up at 127 and the note count climbs (extra
notes). +12 dB spreads the velocities over most of the range with a few
more notes than +6 dB (110 against 90–95; softer notes found, or a few
extra): the script's default, and it sounds right played (the user, 2026-11-15). It depends on the level the Stomp
sends, so set it again if that changes.

## Open

- **Latency:** not measured. JACK's buffering is 3 periods (16 ms), plus the
  model's label delay (5.2 ms) and the HX Stomp's own; a shorter period (128)
  doubles the per-period overhead with this load.
- **Starting at boot**, and saving the synth's sound (a mod-host preset or
  state file).
- **ssh by name** (`rp4-g2m.local`) sometimes fails to resolve; the address
  works (`ssh -o HostName=ADDRESS -o HostKeyAlias=rp4-g2m.local rp4-g2m`).
