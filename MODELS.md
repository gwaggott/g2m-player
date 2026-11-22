# Models

The model bundled with the player, and the data it was trained on.

## v1-0-alpha

`models/v1-0-alpha.g2m`, the only bundled model and the default: an alpha
release. Format version 4 (MPE heads: string, onset type and per-note
pitch bend), float32 weights, 5.1 MB. Trained 2026-11-22 (the run
`release-as72`; the training code isn't public), with the decoder's legato threshold
set to 0.9.

Trained only on:

- **GuitarSet**: Q. Xi, R. Bittner, J. Pauwels, X. Ye and J. P. Bello,
  "GuitarSet: A Dataset for Guitar Transcription", ISMIR 2018.
  [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/). Its recordings
  were converted to training pairs (resampled to 48 kHz mono, with labels
  realigned to the note attacks); the recordings aren't distributed here.
- **Geoff's Guitar DI**, the author's own sampled electric guitar
  ([CC BY 4.0](https://creativecommons.org/licenses/by/4.0/)), playing MIDI
  made by the project's own generator.

Copyright (c) 2026 Geoffrey Waggott. Licensed under
[CC BY 4.0](https://creativecommons.org/licenses/by/4.0/): you may share
and adapt it, also commercially, if you credit it (and, through it,
GuitarSet) and say what you changed. The player's code is under the
GPL (`LICENSE`); the model file isn't code and has its own licence.

Known weaknesses: some bends come out as chromatic legato notes (a run of
new notes a semitone apart); low notes, note ends, velocity and bends are
weaker than on the project's private models; and it has heard only one
electric guitar.
