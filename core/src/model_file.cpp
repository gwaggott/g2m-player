#include <g2m/model.h>

#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <utility>

#include "json.h"

namespace g2m {

namespace {

constexpr char kMagic[4] = {'G', '2', 'M', 'W'};
constexpr const char* kFormat = "guitar2midi-note-model";
constexpr int kMaxDimension = 1 << 20;  // guards the size arithmetic against absurd headers

// zlib / PNG CRC-32
constexpr std::array<std::uint32_t, 256> crc_table() {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t n = 0; n < 256; ++n) {
        std::uint32_t c = n;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        table[n] = c;
    }
    return table;
}

std::uint32_t crc32(const std::uint8_t* data, std::size_t size) {
    static constexpr auto table = crc_table();
    std::uint32_t c = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < size; ++i) c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

std::uint32_t read_u32(const std::uint8_t* p) {
    return std::uint32_t(p[0]) | std::uint32_t(p[1]) << 8 | std::uint32_t(p[2]) << 16 | std::uint32_t(p[3]) << 24;
}

LoadResult failure(std::string message) {
    return {nullptr, std::move(message)};
}

// Reads `key` of `object` as an int in [min, max]; on failure sets `error`.
bool get_int(const json::Value& object, const char* key, int min, int max, int& out, std::string& error) {
    const json::Value* v = object.get(key);
    if (!v || !v->as_int(out) || out < min || out > max) {
        error = std::string("header: '") + key + "' missing or out of range";
        return false;
    }
    return true;
}

bool get_float(const json::Value& object, const char* key, float& out, std::string& error) {
    const json::Value* v = object.get(key);
    if (!v || !v->is_number()) {
        error = std::string("header: '") + key + "' missing or not a number";
        return false;
    }
    out = static_cast<float>(v->number);
    return true;
}

// The MPE heads (version 3; absent before): only the known ones, with
// their classes in the spec's order; curve heads (version 4) by name and
// unit, with their range. A new head is a new format version.
// A curve head: {"unit": ..., "min": ..., "max": ...}; the known one is
// pitch_offset in semitones.
bool read_curve(const json::Value& name, const json::Value& curve, ModelConfig& c, std::string& error) {
    const json::Value* unit = curve.get("unit");
    HeadSpec h{name.string, 1, true, 0, 0};
    if (!curve.is_object() || !unit || !unit->is_string() || !get_float(curve, "min", h.min, error) ||
        !get_float(curve, "max", h.max, error) || h.min > h.max) {
        error = "header: curve head '" + name.string + "' lacks a unit or a range";
        return false;
    }
    if (name.string != "pitch_offset" || unit->string != "semitones") {
        error = "header: unknown curve head '" + name.string + "' (or its unit)";
        return false;
    }
    if (c.pitch_offset_curve >= 0) {
        error = "header: head '" + name.string + "' listed twice";
        return false;
    }
    c.pitch_offset_curve = c.curve_heads++;
    c.heads.push_back(h);
    return true;
}

bool read_heads(const json::Value& header, ModelConfig& c, std::string& error) {
    const json::Value* heads = header.get("heads");
    if (!heads) return true;
    if (!heads->is_array()) {
        error = "header: 'heads' isn't a list";
        return false;
    }
    const std::vector<std::string> strings = {"1", "2", "3", "4", "5", "6"};
    const std::vector<std::string> onset_types = {"picked", "legato", "slide"};
    for (const json::Value& h : heads->items) {
        const json::Value* name = h.get("name");
        const json::Value* classes = h.get("classes");
        const json::Value* curve = h.get("curve");
        if (name && name->is_string() && curve) {
            if (!read_curve(*name, *curve, c, error)) return false;
            continue;
        }
        if (!name || !name->is_string() || !classes || !classes->is_array()) {
            error = "header: a head lacks a name or classes";
            return false;
        }
        std::vector<std::string> names;
        for (const json::Value& k : classes->items) names.push_back(k.is_string() ? k.string : "");
        int* start = nullptr;
        if (name->string == "string" && names == strings) start = &c.string_head;
        else if (name->string == "onset_type" && names == onset_types) start = &c.onset_type_head;
        if (!start) {
            error = "header: unknown head '" + name->string + "' (or its classes)";
            return false;
        }
        if (*start >= 0) {
            error = "header: head '" + name->string + "' listed twice";
            return false;
        }
        *start = c.head_classes;
        c.heads.push_back({name->string, static_cast<int>(names.size())});
        c.head_classes += static_cast<int>(names.size());
    }
    return true;
}

bool read_config(const json::Value& header, ModelConfig& c, std::string& error) {
    const json::Value* m = header.get("model");
    if (!m || !m->is_object()) {
        error = "header: 'model' missing";
        return false;
    }
    if (!get_int(header, "sample_rate", 1, 1000000, c.sample_rate, error) ||
        !get_int(header, "hop", 1, kMaxDimension, c.hop, error) ||
        !get_int(header, "pitch_min", 0, 127, c.pitch_min, error) ||
        !get_int(header, "pitch_max", c.pitch_min, 127, c.pitch_max, error) ||
        !get_int(*m, "n_pitches", 1, 128, c.n_pitches, error) ||
        !get_int(*m, "channels", 1, kMaxDimension, c.channels, error) ||
        !get_int(*m, "kernel_size", 1, kMaxDimension, c.kernel_size, error) ||
        !get_int(*m, "gru", 1, kMaxDimension, c.gru, error))
        return false;
    if (c.n_pitches != c.pitch_max - c.pitch_min + 1) {
        error = "header: n_pitches doesn't match the pitch range";
        return false;
    }

    const json::Value* outputs = header.get("outputs");
    const char* expected_outputs[] = {"onset", "frame", "velocity"};
    bool outputs_ok = outputs && outputs->is_array() && outputs->items.size() == 3;
    for (int i = 0; outputs_ok && i < 3; ++i)
        outputs_ok = outputs->items[i].is_string() && outputs->items[i].string == expected_outputs[i];
    if (!outputs_ok) {
        error = "header: 'outputs' isn't [\"onset\", \"frame\", \"velocity\"]";
        return false;
    }

    const json::Value* frontend = m->get("frontend");
    if (!frontend || !frontend->is_array() || frontend->items.empty()) {
        error = "header: 'frontend' missing or empty";
        return false;
    }
    long long strides = 1;
    for (const json::Value& stage : frontend->items) {
        FrontendStage s{};
        if (!stage.is_array() || stage.items.size() != 3 || !stage.items[0].as_int(s.channels) ||
            !stage.items[1].as_int(s.kernel) || !stage.items[2].as_int(s.stride) || s.channels < 1 ||
            s.channels > kMaxDimension || s.stride < 1 || s.kernel < s.stride || s.kernel > kMaxDimension) {
            error = "header: a front-end stage isn't [channels, kernel >= stride, stride >= 1]";
            return false;
        }
        strides *= s.stride;
        if (strides > c.hop) break;
        c.frontend.push_back(s);
    }
    if (strides != c.hop) {
        error = "header: front-end strides don't multiply to hop";
        return false;
    }

    const json::Value* dilations = m->get("dilations");
    if (!dilations || !dilations->is_array()) {
        error = "header: 'dilations' missing";
        return false;
    }
    for (const json::Value& d : dilations->items) {
        int value;
        if (!d.as_int(value) || value < 1 || value > kMaxDimension) {
            error = "header: a dilation isn't a positive integer";
            return false;
        }
        c.dilations.push_back(value);
    }

    c.projections.assign(c.dilations.size(), 0);
    if (const json::Value* projections = m->get("projections")) {
        if (!projections->is_array() || projections->items.size() != c.dilations.size()) {
            error = "header: 'projections' isn't one entry per gated layer";
            return false;
        }
        for (std::size_t l = 0; l < c.dilations.size(); ++l) {
            if (!projections->items[l].as_int(c.projections[l]) || c.projections[l] < 0 ||
                c.projections[l] > c.channels) {
                error = "header: a projection isn't 0 to channels";
                return false;
            }
        }
    }
    return read_heads(header, c, error);
}

// Version 2 has every setting; version 1 has no late fallback (off);
// version 3 adds the onset-type thresholds (0.5 before), version 4 the
// bend settings (their defaults before).
bool read_decoder(const json::Value& header, int version, DecoderSettings& d, std::string& error) {
    const json::Value* v = header.get("decoder");
    if (!v || !v->is_object()) {
        error = "header: 'decoder' missing";
        return false;
    }
    constexpr int kMaxFrames = 1 << 30;
    bool ok = get_float(*v, "onset_threshold", d.onset_threshold, error) &&
              get_float(*v, "frame_threshold", d.frame_threshold, error) &&
              get_int(*v, "frame_wait", 0, kMaxFrames, d.frame_wait, error) &&
              get_int(*v, "off_frames", 0, kMaxFrames, d.off_frames, error) &&
              get_int(*v, "gate_frames", 0, kMaxFrames, d.gate_frames, error) &&
              get_float(*v, "strong_onset", d.strong_onset, error) &&
              get_float(*v, "retrigger_threshold", d.retrigger_threshold, error);
    if (!ok) return false;
    if (version >= 2 && (!get_int(*v, "fallback_frames", 0, kMaxFrames, d.fallback_frames, error) ||
                         !get_float(*v, "fallback_threshold", d.fallback_threshold, error)))
        return false;
    if (version >= 3 && (!get_float(*v, "legato_threshold", d.legato_threshold, error) ||
                         !get_float(*v, "slide_threshold", d.slide_threshold, error)))
        return false;
    if (version >= 4 && (!get_float(*v, "bend_dead_band", d.bend_dead_band, error) ||
                         !get_int(*v, "bend_range", 1, 96, d.bend_range, error)))
        return false;
    if (d.fallback_frames > 0 && d.fallback_threshold < d.frame_threshold) {
        error = "header: decoder fallback_threshold is below frame_threshold";
        return false;
    }
    return true;
}

// The tensors a config implies, in file order, with their shapes.
std::vector<std::pair<std::string, std::vector<int>>> expected_tensors(const ModelConfig& c) {
    std::vector<std::pair<std::string, std::vector<int>>> t;
    int in_channels = 1;
    for (std::size_t i = 0; i < c.frontend.size(); ++i) {
        const FrontendStage& s = c.frontend[i];
        std::string prefix = "frontend." + std::to_string(i);
        t.push_back({prefix + ".weight", {s.channels, in_channels, s.kernel}});
        t.push_back({prefix + ".bias", {s.channels}});
        in_channels = s.channels;
    }
    t.push_back({"rechannel.weight", {c.channels, in_channels, 1}});
    t.push_back({"rechannel.bias", {c.channels}});
    for (std::size_t l = 0; l < c.dilations.size(); ++l) {
        std::string i = std::to_string(l);
        int k = c.projections[l];
        if (k > 0) {
            t.push_back({"proj." + i + ".weight", {k, c.channels}});
            t.push_back({"proj." + i + ".bias", {k}});
        }
        t.push_back({"convs." + i + ".weight", {2 * c.channels, k > 0 ? k : c.channels, c.kernel_size}});
        t.push_back({"convs." + i + ".bias", {2 * c.channels}});
        t.push_back({"mixers." + i + ".weight", {c.channels, c.channels, 1}});
        t.push_back({"mixers." + i + ".bias", {c.channels}});
    }
    t.push_back({"gru.weight_ih_l0", {3 * c.gru, c.channels}});
    t.push_back({"gru.weight_hh_l0", {3 * c.gru, c.gru}});
    t.push_back({"gru.bias_ih_l0", {3 * c.gru}});
    t.push_back({"gru.bias_hh_l0", {3 * c.gru}});
    t.push_back({"head.weight", {3 * c.n_pitches, c.channels + c.gru}});
    t.push_back({"head.bias", {3 * c.n_pitches}});
    for (const HeadSpec& h : c.heads) {
        t.push_back({"extra_heads." + h.name + ".weight", {h.classes * c.n_pitches, c.channels + c.gru}});
        t.push_back({"extra_heads." + h.name + ".bias", {h.classes * c.n_pitches}});
    }
    return t;
}

bool read_tensors(const json::Value& header, const ModelConfig& config, std::size_t data_floats,
                  std::vector<Tensor>& tensors, std::string& error) {
    const json::Value* list = header.get("tensors");
    if (!list || !list->is_array()) {
        error = "header: 'tensors' missing";
        return false;
    }
    auto expected = expected_tensors(config);
    std::vector<bool> seen(expected.size(), false);
    for (const json::Value& entry : list->items) {
        const json::Value* name = entry.get("name");
        const json::Value* shape = entry.get("shape");
        const json::Value* offset = entry.get("offset");
        if (!name || !name->is_string() || !shape || !shape->is_array() || !offset || !offset->is_number() ||
            offset->number < 0 || offset->number != static_cast<double>(static_cast<std::size_t>(offset->number))) {
            error = "header: a tensor entry lacks a name, shape or offset";
            return false;
        }
        std::size_t index = 0;
        while (index < expected.size() && expected[index].first != name->string) ++index;
        if (index == expected.size()) {
            error = "header: unknown tensor '" + name->string + "'";
            return false;
        }
        if (seen[index]) {
            error = "header: tensor '" + name->string + "' listed twice";
            return false;
        }
        seen[index] = true;

        Tensor t;
        t.name = name->string;
        t.offset = static_cast<std::size_t>(offset->number);
        t.size = 1;
        for (const json::Value& d : shape->items) {
            int n;
            if (!d.as_int(n)) n = -1;
            t.shape.push_back(n);
            t.size *= static_cast<std::size_t>(n < 0 ? 0 : n);
        }
        if (t.shape != expected[index].second) {
            error = "header: tensor '" + t.name + "' has a shape that doesn't match the model config";
            return false;
        }
        if (t.offset > data_floats || t.size > data_floats - t.offset) {
            error = "header: tensor '" + t.name + "' lies outside the weights";
            return false;
        }
        tensors.push_back(std::move(t));
    }
    for (std::size_t i = 0; i < expected.size(); ++i) {
        if (!seen[i]) {
            error = "header: tensor '" + expected[i].first + "' missing";
            return false;
        }
    }
    return true;
}

}  // namespace

const float* Model::tensor(const std::string& name) const {
    for (const Tensor& t : tensors)
        if (t.name == name) return weights.data() + t.offset;
    return nullptr;
}

float half_to_float(std::uint16_t bits) {
    std::uint32_t sign = static_cast<std::uint32_t>(bits >> 15) << 31;
    std::uint32_t exponent = (bits >> 10) & 0x1f, mantissa = bits & 0x3ff;
    std::uint32_t out;
    if (exponent == 0x1f) {  // infinity or NaN
        out = sign | 0x7f800000u | mantissa << 13;
    } else if (exponent != 0) {  // normal: rebias the exponent
        out = sign | (exponent - 15 + 127) << 23 | mantissa << 13;
    } else if (mantissa == 0) {
        out = sign;
    } else {  // subnormal, mantissa · 2⁻²⁴: normalise (it's a normal float32)
        int shift = 0;
        while (!(mantissa & 0x400)) {
            mantissa <<= 1;
            ++shift;
        }
        out = sign | static_cast<std::uint32_t>(127 - 14 - shift) << 23 | (mantissa & 0x3ff) << 13;
    }
    float f;
    std::memcpy(&f, &out, 4);
    return f;
}

LoadResult load_model(const std::uint8_t* data, std::size_t size) {
    if (size < 8 || std::memcmp(data, kMagic, 4) != 0) return failure("not a .g2m model file (bad magic)");
    std::size_t header_size = read_u32(data + 4);
    if (header_size > size - 8) return failure("file too short for its header");

    json::Value header;
    std::string error;
    std::string_view text(reinterpret_cast<const char*>(data + 8), header_size);
    if (!json::parse(text, header, error)) return failure("header isn't valid JSON: " + error);
    if (!header.is_object()) return failure("header isn't a JSON object");

    const json::Value* format = header.get("format");
    const json::Value* version = header.get("format_version");
    int version_number = 0;
    if (!format || !format->is_string() || format->string != kFormat)
        return failure("not a guitar2midi note model (format)");
    if (!version || !version->as_int(version_number) || version_number < kMinFormatVersion ||
        version_number > kFormatVersion)
        return failure("unsupported format version (this player reads versions " + std::to_string(kMinFormatVersion) +
                       " to " + std::to_string(kFormatVersion) + ")");

    const std::uint8_t* weights = data + 8 + header_size;
    std::size_t weight_bytes = size - 8 - header_size;
    const json::Value* floats = header.get("data_floats");
    const json::Value* crc = header.get("data_crc32");
    // version 6: float16 weights, `weight_type` "float16"; absent before
    bool float16 = version_number >= 6;
    const json::Value* weight_type = header.get("weight_type");
    if (float16 ? !weight_type || !weight_type->is_string() || weight_type->string != "float16" : weight_type != nullptr)
        return failure(float16 ? "header: a version 6 model's weight_type must be \"float16\""
                               : "header: weight_type before format version 6");
    std::size_t value_bytes = float16 ? 2 : 4;
    if (!floats || !floats->is_number() || floats->number * value_bytes != static_cast<double>(weight_bytes))
        return failure("weights have the wrong size (truncated file?)");
    if (!crc || !crc->is_number() || crc->number != static_cast<double>(crc32(weights, weight_bytes)))
        return failure("weights fail their CRC-32 check (corrupt file?)");

    auto model = std::make_unique<Model>();
    std::size_t data_floats = weight_bytes / value_bytes;
    model->format_version = version_number;
    if (!read_config(header, model->config, error) || !read_decoder(header, version_number, model->decoder, error))
        return failure(error);
    bool projected = false;
    for (int k : model->config.projections) projected = projected || k > 0;
    if (version_number < 5 && projected) return failure("header: projections before format version 5");
    if (version_number == 5 && !projected) return failure("header: a version 5 model without projections");
    if (!read_tensors(header, model->config, data_floats, model->tensors, error)) return failure(error);
    if (version_number < 3 && !model->config.heads.empty()) return failure("header: MPE heads before format version 3");
    if (version_number < 4 && model->config.curve_heads > 0) return failure("header: curve heads before format version 4");
    if (const json::Value* delay = header.get("label_delay_ms"); delay && delay->is_number())
        model->label_delay_ms = delay->number;

    model->weights.resize(data_floats);
    model->float16_weights = float16;
    for (std::size_t i = 0; i < data_floats; ++i) {
        if (float16) {  // export.py writes only finite values
            model->weights[i] = half_to_float(static_cast<std::uint16_t>(weights[2 * i] | weights[2 * i + 1] << 8));
            if (!std::isfinite(model->weights[i])) return failure("weights: a float16 value isn't finite");
        } else {
            std::uint32_t bits = read_u32(weights + 4 * i);
            std::memcpy(&model->weights[i], &bits, 4);
        }
    }
    return {std::move(model), {}};
}

LoadResult load_model(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return failure("can't open " + path);
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (file.bad()) return failure("can't read " + path);
    LoadResult result = load_model(bytes.data(), bytes.size());
    if (!result.model) result.error = path + ": " + result.error;
    return result;
}

}  // namespace g2m
