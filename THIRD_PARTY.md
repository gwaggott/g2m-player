# Third-party code

The player plugin is built with the following code by others. Their
licence texts are in the package's `licences/` folder (copied from the
submodules in `external/` by `packaging/package.cmake`).

| Component | Version | Used for | Licence | Licence file |
|---|---|---|---|---|
| [DPF](https://github.com/DISTRHO/DPF), DISTRHO Plugin Framework, by Filipe Coelho | `4238e1c7` | the plugin formats (LV2, VST3 through its `travesty` headers, CLAP), the standalone, the window | ISC | `DPF.txt` |
| [pugl](https://github.com/lv2/pugl), by David Robillard (in DPF) | DPF's | the window and its events | ISC | `pugl.txt` |
| [CLAP](https://github.com/free-audio/clap) headers (in DPF) | DPF's | the CLAP format | MIT | (in `DPF.txt`'s terms, as distributed with DPF) |
| [NanoVG](https://github.com/memononen/nanovg) (in DPF's OpenGL library) | DPF's | linked with DPF's OpenGL UI library | zlib | `NanoVG.txt` |
| DejaVu Sans (in DPF's OpenGL library) | DPF's | DPF's default font, linked with it | Bitstream Vera / DejaVu | `DejaVuSans.txt` |
| [choc](https://github.com/Tracktion/choc) (in DPF) | DPF's | DPF's helpers | ISC | `choc.md` |
| [LVGL](https://github.com/lvgl/lvgl) | 9.4.0 | the UI's widgets and drawing | MIT | `LVGL.txt` |
| [OpenGL headers](https://github.com/KhronosGroup/OpenGL-Registry) `GL/glext.h` and `KHR/khrplatform.h`, by the Khronos Group | 20220530 | DPF's OpenGL code on Windows (in `external/khronos/`) | MIT | (in each header) |
| Montserrat font (in LVGL) | LVGL's | the UI's text | SIL Open Font Licence 1.1 | (LVGL's distribution) |

The UI's LVGL wrapper (`plugin/ui/LVGL.hpp`, `LVGL.cpp`) is adapted from
[DPF-Widgets](https://github.com/DISTRHO/DPF-Widgets) (ISC, Filipe Coelho),
with the changes marked "g2m".

The plugin's own code is licensed under the GNU General Public License,
version 3 or later (`LICENSE`). Models bundled with a package are under
CC BY 4.0, with their credits in `MODELS.md`.
