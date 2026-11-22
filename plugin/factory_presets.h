// The factory presets (docs/release-plan.md, "Presets"), the plugin's
// programs: the LV2 preset list, the VST3 program number. The first is the
// defaults (DPF's standalone loads it at start). They keep the current
// model. Values by LV2 symbol, in the parameters' own units.
#pragma once

#include "presets.h"

#include <vector>

namespace g2m::presets {

inline const std::vector<Preset>& factory_presets() {
    static const std::vector<Preset> presets = {
        {"Default", "", {}},
        {"MPE synth", "", {{"output_mode", 1}}},
        {"Retrigger lead", "", {{"bend_mode", 1}}},
        {"MPE retrigger lead", "", {{"output_mode", 1}, {"bend_mode", 1}}},
        {"Pad (freeze)", "", {{"output_mode", 1}, {"sustain_mode", 3}}},
        {"Noisy pickups (gated)", "", {{"gate_threshold", -60}, {"gate_release", 80}}},
    };
    return presets;
}

}  // namespace g2m::presets
