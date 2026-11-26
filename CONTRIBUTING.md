# Contributing

g2m-player is an alpha, maintained by one person in spare time, so
replies may be slow.

## Reports

Bug reports are the most helpful contribution, especially from hosts and
systems other than Linux with REAPER: the Windows and macOS builds pass
CI but have had little use yet. Please use the
[bug report form](https://github.com/gwaggott/g2m-player/issues/new?template=bug_report.yml).
If the notes are wrong, a short DI recording of what you played helps
most.

For security problems, see [SECURITY.md](SECURITY.md).

## Code

For anything more than a small fix, please open an issue first so we can
agree on the approach before you spend time on it.

- Contributions are accepted under the project's licence,
  [GPL-3.0-or-later](LICENSE).
- The README describes the build. Run the tests (`ctest` in the build
  directory) before sending a pull request; CI must pass on Linux
  (x86_64 and ARM64), Windows and macOS.
- Keep the code portable to GCC, Clang, MSVC and Apple Clang, and the
  audio thread free of allocation and locks.
- Plugin parameters are only ever added at the end, and the plugin's IDs
  never change, so saved sessions keep working.

## Models

The bundled model (`models/v1-0-alpha.g2m`, CC BY 4.0, see `MODELS.md`)
is made by training code that isn't public, so model files and changes
to how models are trained can't be accepted here. Reports of what a
model gets wrong are very welcome.
