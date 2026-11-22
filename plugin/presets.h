// Presets (docs/release-plan.md, "Presets"): parameter values by LV2 symbol,
// plus an optional model reference ("bundled:<name>" or a file path; empty
// keeps the current model). User presets are JSON files in user_dir(), one
// per preset:
//
//   {"format": "guitar-2-midi preset", "version": 1, "name": "Lead",
//    "model": "bundled:strat", "parameters": {"bend_mode": 1, ...}}
//
// Parameters a preset doesn't name get their defaults, so a preset saved by
// an older version gives the same settings in a newer one; symbols the
// plugin doesn't know are ignored. Not for the audio thread.
#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace g2m::presets {

constexpr char kFormat[] = "guitar-2-midi preset";
constexpr int kVersion = 1;

struct Preset {
    std::string name;
    std::string model;
    std::vector<std::pair<std::string, float>> parameters;  // by symbol
};

// A parameter a preset can set
struct ParameterInfo {
    std::string symbol;
    float def, min, max;
};

// Every parameter's value from `preset`, in the order of `info`: its own
// (clamped to the range), the others at their defaults.
std::vector<float> values(const Preset& preset, const std::vector<ParameterInfo>& info);

std::string to_json(const Preset& preset);
// False with `error` if the text isn't a preset this version can read.
bool from_json(std::string_view text, Preset& preset, std::string& error);

// The user preset folder: ~/.config/guitar-2-midi/presets ($XDG_CONFIG_HOME
// if set), %APPDATA%\guitar-2-midi\presets, ~/Library/Application
// Support/guitar-2-midi/presets. Empty if the environment gives no home.
std::string user_dir();

// A preset's file name: the name with characters that aren't allowed in
// file names replaced by '_', plus ".json". Empty for a name with nothing
// usable in it.
std::string file_name(const std::string& name);

struct Entry {
    std::string name;
    std::string path;
};
// The presets in `dir` that can be read, sorted by name (none if `dir` is missing).
std::vector<Entry> list(const std::string& dir);
bool load(const std::string& path, Preset& preset, std::string& error);
// Writes dir/file_name(preset.name), creating `dir`, replacing a preset of that name.
bool save(const std::string& dir, const Preset& preset, std::string& error);
bool remove(const std::string& path);

}  // namespace g2m::presets
