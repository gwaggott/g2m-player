# Guitar to MIDI: installing

Guitar to MIDI turns a DI electric guitar signal into MIDI notes, in real
time, with a small neural network. Put it on the track that carries your
guitar's DI signal and send its MIDI output to a synth.

This package has the plugin in three formats (LV2, VST3, CLAP) and a
standalone app. You only need the format your host uses.

## Where the files go

Copy the plugin into the folder for its format, then rescan plugins in
your host.

| Format | Linux | Windows | macOS |
|---|---|---|---|
| VST3: `VST3/g2m.vst3` | `~/.vst3/` | `C:\Program Files\Common Files\VST3\` | `~/Library/Audio/Plug-Ins/VST3/` |
| CLAP: `CLAP/g2m.clap` | `~/.clap/` | `C:\Program Files\Common Files\CLAP\` | `~/Library/Audio/Plug-Ins/CLAP/` |
| LV2: `LV2/g2m.lv2` | `~/.lv2/` | `%APPDATA%\LV2\` | `~/Library/Audio/Plug-Ins/LV2/` |

If `CLAP/` has a `g2m-models` folder, copy it next to `g2m.clap`. The
standalone app runs from its own folder, with `resources/` beside it.

**macOS:** the builds aren't signed, so macOS blocks them at first. After
copying, clear the quarantine flag in Terminal, for example:

```
xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/VST3/g2m.vst3
```

**Windows:** the builds aren't signed, so SmartScreen may warn before the
standalone app runs.

## Your host has to route MIDI out of an audio plugin

- These can: REAPER (every format), Bitwig (CLAP, VST3), Ardour and Carla.
- Ableton Live and Logic can't route MIDI out of an audio-effect plugin.
- MPE output (one channel per string, with per-note pitch bend) works best
  in CLAP and LV2; VST3 carries notes but little else.

## Using it

- **Model:** choose a bundled model in the Model menu, or "Load file..." to
  load a `.g2m` model file. Without a model there's no MIDI.
- **Sample rate:** any common rate works. The model hears 48 kHz; at other
  rates its input is resampled, which delays the MIDI by about 0.4 ms.
- **Input level:** the meter's green band is the level the model was
  trained on. Use Input Gain to bring your guitar into it.
- **Presets** you save are files, shared by every format:
  `~/.config/guitar-2-midi/presets/` on Linux,
  `%APPDATA%\guitar-2-midi\presets\` on Windows,
  `~/Library/Application Support/guitar-2-midi/presets/` on macOS.
- **CPU:** one instance uses about a third of a fast desktop core, and
  more on a laptop or an efficiency core. Check your host's CPU meter.

The player is free software under the GNU General Public License,
version 3 or later (`LICENSE.txt`). Its model is under CC BY 4.0
(`MODELS.md`, with its credits). `THIRD_PARTY.md` lists the code by
others that the plugin is built with, and `licences/` has their licences.
