#include "presets.h"

#include "json.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace g2m::presets {

namespace fs = std::filesystem;

namespace {

void append_string(std::string& out, const std::string& s) {
    out += '"';
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\u%04x", c);
            out += buf;
        } else {
            out += static_cast<char>(c);
        }
    }
    out += '"';
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace

std::vector<float> values(const Preset& preset, const std::vector<ParameterInfo>& info) {
    std::vector<float> out;
    for (const ParameterInfo& p : info) out.push_back(p.def);
    for (const auto& [symbol, value] : preset.parameters) {
        for (std::size_t i = 0; i < info.size(); ++i) {
            if (info[i].symbol == symbol && std::isfinite(value)) out[i] = std::clamp(value, info[i].min, info[i].max);
        }
    }
    return out;
}

std::string to_json(const Preset& preset) {
    std::string out = "{\n  \"format\": ";
    append_string(out, kFormat);
    out += ",\n  \"version\": " + std::to_string(kVersion) + ",\n  \"name\": ";
    append_string(out, preset.name);
    if (!preset.model.empty()) {
        out += ",\n  \"model\": ";
        append_string(out, preset.model);
    }
    out += ",\n  \"parameters\": {";
    for (std::size_t i = 0; i < preset.parameters.size(); ++i) {
        out += i ? ",\n    " : "\n    ";
        append_string(out, preset.parameters[i].first);
        char buf[32];
        std::snprintf(buf, sizeof buf, ": %.9g", preset.parameters[i].second);
        out += buf;
    }
    out += preset.parameters.empty() ? "}\n}\n" : "\n  }\n}\n";
    return out;
}

bool from_json(std::string_view text, Preset& preset, std::string& error) {
    json::Value doc;
    if (!json::parse(text, doc, error)) return false;
    const json::Value* format = doc.is_object() ? doc.get("format") : nullptr;
    if (!format || !format->is_string() || format->string != kFormat) {
        error = "not a guitar-2-midi preset";
        return false;
    }
    int version = 0;
    const json::Value* v = doc.get("version");
    if (!v || !v->as_int(version) || version < 1) {
        error = "no preset version";
        return false;
    }
    if (version > kVersion) {
        error = "made by a newer version of the plugin";
        return false;
    }
    Preset p;
    const json::Value* name = doc.get("name");
    if (!name || !name->is_string() || name->string.empty()) {
        error = "no preset name";
        return false;
    }
    p.name = name->string;
    if (const json::Value* model = doc.get("model")) {
        if (!model->is_string()) {
            error = "model is not a string";
            return false;
        }
        p.model = model->string;
    }
    const json::Value* params = doc.get("parameters");
    if (!params || !params->is_object()) {
        error = "no parameters";
        return false;
    }
    for (std::size_t i = 0; i < params->keys.size(); ++i) {
        if (!params->items[i].is_number()) {
            error = "parameter " + params->keys[i] + " is not a number";
            return false;
        }
        p.parameters.emplace_back(params->keys[i], static_cast<float>(params->items[i].number));
    }
    preset = std::move(p);
    return true;
}

std::string user_dir() {
#if defined(_WIN32)
    const char* appdata = std::getenv("APPDATA");
    if (!appdata || !*appdata) return {};
    return (fs::path(appdata) / "guitar-2-midi" / "presets").string();
#else
    const char* home = std::getenv("HOME");
#if defined(__APPLE__)
    if (!home || !*home) return {};
    return (fs::path(home) / "Library" / "Application Support" / "guitar-2-midi" / "presets").string();
#else
    const char* config = std::getenv("XDG_CONFIG_HOME");
    if (config && *config == '/') return (fs::path(config) / "guitar-2-midi" / "presets").string();
    if (!home || !*home) return {};
    return (fs::path(home) / ".config" / "guitar-2-midi" / "presets").string();
#endif
#endif
}

std::string file_name(const std::string& name) {
    std::string out;
    bool usable = false;
    for (unsigned char c : name) {
        bool bad = c < 0x20 || c == 0x7f || std::string_view("/\\:*?\"<>|").find(static_cast<char>(c)) != std::string_view::npos;
        out += bad ? '_' : static_cast<char>(c);
        usable = usable || (!bad && c != ' ' && c != '.');
    }
    if (!usable) return {};
    return out + ".json";
}

std::vector<Entry> list(const std::string& dir) {
    std::vector<Entry> out;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().extension() != ".json") continue;
        Preset p;
        std::string error;
        if (load(it->path().string(), p, error)) out.push_back({p.name, it->path().string()});
    }
    std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) {
        std::string la = lower(a.name), lb = lower(b.name);
        return la != lb ? la < lb : a.name < b.name;
    });
    return out;
}

bool load(const std::string& path, Preset& preset, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "can't open " + path;
        return false;
    }
    std::ostringstream text;
    text << in.rdbuf();
    return from_json(text.str(), preset, error);
}

bool save(const std::string& dir, const Preset& preset, std::string& error) {
    std::string file = file_name(preset.name);
    if (file.empty()) {
        error = "the preset needs a name";
        return false;
    }
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        error = "can't create " + dir + ": " + ec.message();
        return false;
    }
    fs::path path = fs::path(dir) / file;
    fs::path temp = path;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out << to_json(preset);
        if (!out.flush()) {
            error = "can't write " + temp.string();
            return false;
        }
    }
    fs::rename(temp, path, ec);  // a reader never sees half a file
    if (ec) {
        fs::remove(temp, ec);
        error = "can't write " + path.string();
        return false;
    }
    return true;
}

bool remove(const std::string& path) {
    std::error_code ec;
    return fs::remove(path, ec);
}

}  // namespace g2m::presets
