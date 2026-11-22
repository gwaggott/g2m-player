#include "factory_presets.h"
#include "presets.h"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>

#include "check.h"

namespace fs = std::filesystem;
using g2m::presets::Preset;

namespace {

bool same(const Preset& a, const Preset& b) {
    return a.name == b.name && a.model == b.model && a.parameters == b.parameters;
}

// An empty folder for one test, removed afterwards
struct TempDir {
    fs::path path;
    explicit TempDir(const char* name) : path(fs::temp_directory_path() / ("g2m-test-presets-" + std::string(name))) {
        fs::remove_all(path);
    }
    ~TempDir() { fs::remove_all(path); }
    std::string str() const { return path.string(); }
};

void write(const fs::path& path, const std::string& text) {
    std::ofstream(path, std::ios::binary) << text;
}

}  // namespace

TEST(round_trip) {
    Preset p{"Lead \"1\" \\ tab\there", "bundled:strat", {{"bend_mode", 1}, {"gate_threshold", -60.5f}, {"input_gain", 0.1f}}};
    Preset back;
    std::string error;
    CHECK(g2m::presets::from_json(g2m::presets::to_json(p), back, error));
    CHECK(same(p, back));
    Preset bare{"Bare", "", {}};
    CHECK(g2m::presets::from_json(g2m::presets::to_json(bare), back, error));
    CHECK(same(bare, back));
    CHECK(!check::contains(g2m::presets::to_json(bare), "model"));  // no model: keep the current one
}

TEST(rejects_what_it_cannot_read) {
    Preset p{"Keep", "", {{"bend_mode", 2}}};
    std::string error;
    auto rejects = [&](const std::string& text, const std::string& why) {
        Preset kept = p;
        bool ok = g2m::presets::from_json(text, kept, error);
        CHECK_MSG(!ok && check::contains(error, why), "%s: %s", text.c_str(), error.c_str());
        CHECK(same(kept, p));  // unchanged
    };
    rejects("{", "");
    rejects(R"({"format": "something else", "version": 1, "name": "x", "parameters": {}})", "not a guitar-2-midi preset");
    rejects(R"({"version": 1, "name": "x", "parameters": {}})", "not a guitar-2-midi preset");
    rejects(R"({"format": "guitar-2-midi preset", "name": "x", "parameters": {}})", "no preset version");
    rejects(R"({"format": "guitar-2-midi preset", "version": 0, "name": "x", "parameters": {}})", "no preset version");
    rejects(R"({"format": "guitar-2-midi preset", "version": 2, "name": "x", "parameters": {}})", "newer version");
    rejects(R"({"format": "guitar-2-midi preset", "version": 1, "name": "", "parameters": {}})", "no preset name");
    rejects(R"({"format": "guitar-2-midi preset", "version": 1, "parameters": {}})", "no preset name");
    rejects(R"({"format": "guitar-2-midi preset", "version": 1, "name": "x", "model": 3, "parameters": {}})", "model");
    rejects(R"({"format": "guitar-2-midi preset", "version": 1, "name": "x"})", "no parameters");
    rejects(R"({"format": "guitar-2-midi preset", "version": 1, "name": "x", "parameters": {"a": "1"}})", "a is not a number");
    CHECK(g2m::presets::from_json(R"({"format": "guitar-2-midi preset", "version": 1, "name": "x", "parameters": {},
                                      "added later": true})", p, error));  // other keys ignored
}

TEST(values_default_clamp_and_ignore) {
    std::vector<g2m::presets::ParameterInfo> info = {{"a", 1, 0, 10}, {"b", -5, -20, 0}, {"c", 0, 0, 3}};
    Preset p{"x", "", {{"b", -50}, {"c", 2}, {"unknown", 7}, {"a", NAN}}};
    CHECK((g2m::presets::values(p, info) == std::vector<float>{1, -20, 2}));
    p.parameters.push_back({"c", 9});  // the last one wins, clamped
    CHECK((g2m::presets::values(p, info) == std::vector<float>{1, -20, 3}));
    CHECK((g2m::presets::values(Preset{}, info) == std::vector<float>{1, -5, 0}));
}

TEST(file_names) {
    CHECK(g2m::presets::file_name("Lead") == "Lead.json");
    CHECK(g2m::presets::file_name("a/b\\c:d*e?f\"g<h>i|j") == "a_b_c_d_e_f_g_h_i_j.json");
    CHECK(g2m::presets::file_name("tab\there") == "tab_here.json");
    CHECK(g2m::presets::file_name("Pad (freeze) é") == "Pad (freeze) é.json");
    for (const char* name : {"", " ", "..", ". .", "/"}) CHECK_MSG(g2m::presets::file_name(name).empty(), "'%s'", name);
}

TEST(save_list_load_remove) {
    TempDir dir("files");
    std::string folder = (dir.path / "nested" / "presets").string();  // created by save
    CHECK(g2m::presets::list(folder).empty());
    std::string error;
    Preset lead{"lead", "/models/a.g2m", {{"bend_mode", 1}}};
    Preset pad{"Pad", "", {{"sustain_mode", 3}}};
    CHECK(g2m::presets::save(folder, lead, error));
    CHECK(g2m::presets::save(folder, pad, error));
    write(fs::path(folder) / "broken.json", "{");
    write(fs::path(folder) / "notes.txt", "not a preset");
    std::vector<g2m::presets::Entry> entries = g2m::presets::list(folder);
    CHECK(entries.size() == 2 && entries[0].name == "lead" && entries[1].name == "Pad");  // by name, any case
    Preset back;
    CHECK(g2m::presets::load(entries[1].path, back, error) && same(back, pad));
    lead.parameters = {{"bend_mode", 2}};  // replaced
    CHECK(g2m::presets::save(folder, lead, error));
    entries = g2m::presets::list(folder);
    CHECK(entries.size() == 2 && g2m::presets::load(entries[0].path, back, error) && same(back, lead));
    CHECK(!fs::exists(entries[0].path + ".tmp"));
    CHECK(g2m::presets::remove(entries[0].path));
    CHECK(!g2m::presets::remove(entries[0].path));
    CHECK(g2m::presets::list(folder).size() == 1);
    CHECK(!g2m::presets::load((fs::path(folder) / "missing.json").string(), back, error) && check::contains(error, "can't open"));
    CHECK(!g2m::presets::save(folder, Preset{"..", "", {}}, error) && check::contains(error, "name"));
    write(dir.path / "a file", "");
    CHECK(!g2m::presets::save((dir.path / "a file").string(), pad, error) && check::contains(error, "can't create"));
}

TEST(user_dir) {
#if !defined(_WIN32) && !defined(__APPLE__)
    const char* old_config = std::getenv("XDG_CONFIG_HOME");
    std::string saved = old_config ? old_config : "";
    setenv("XDG_CONFIG_HOME", "/cfg", 1);
    CHECK(g2m::presets::user_dir() == "/cfg/guitar-2-midi/presets");
    setenv("XDG_CONFIG_HOME", "relative", 1);  // not absolute: ignored, as the XDG spec says
    std::string home = std::getenv("HOME") ? std::getenv("HOME") : "";
    CHECK(g2m::presets::user_dir() == home + "/.config/guitar-2-midi/presets");
    unsetenv("XDG_CONFIG_HOME");
    CHECK(g2m::presets::user_dir() == home + "/.config/guitar-2-midi/presets");
    if (old_config) setenv("XDG_CONFIG_HOME", saved.c_str(), 1);
#endif
}

TEST(factory_presets) {
    const auto& presets = g2m::presets::factory_presets();
    CHECK(presets.size() >= 2 && presets[0].name == "Default" && presets[0].parameters.empty());
    std::set<std::string> names;
    for (const Preset& p : presets) {
        CHECK_MSG(names.insert(p.name).second, "%s twice", p.name.c_str());
        CHECK(p.model.empty());  // they keep the current model
    }
}
