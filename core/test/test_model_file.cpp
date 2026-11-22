#include <g2m/model.h>

#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>

#include "check.h"

static const std::string kData = G2M_TEST_DATA;

static std::vector<std::uint8_t> read_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

using Edits = std::vector<std::pair<std::string, std::string>>;

// The small test model with each `from` replaced by its `to` in its header
// (which must contain it), re-padded so the weights stay aligned to 16 bytes.
static std::vector<std::uint8_t> edited(const Edits& edits, const std::string& name = "small") {
    std::vector<std::uint8_t> blob = read_file(kData + "/" + name + ".g2m");
    std::uint32_t length;
    std::memcpy(&length, blob.data() + 4, 4);  // little-endian host assumed in the tests
    std::string header(blob.begin() + 8, blob.begin() + 8 + length);
    for (const auto& [from, to] : edits) {
        std::size_t at = header.find(from);
        CHECK_MSG(at != std::string::npos, "'%s' not in the header", from.c_str());
        if (at != std::string::npos) header.replace(at, from.size(), to);
    }
    while (header.back() == ' ') header.pop_back();
    while ((8 + header.size()) % 16) header += ' ';
    std::uint32_t new_length = static_cast<std::uint32_t>(header.size());
    std::vector<std::uint8_t> out(blob.begin(), blob.begin() + 4);
    auto* length_bytes = reinterpret_cast<std::uint8_t*>(&new_length);
    out.insert(out.end(), length_bytes, length_bytes + 4);
    out.insert(out.end(), header.begin(), header.end());
    out.insert(out.end(), blob.begin() + 8 + length, blob.end());
    return out;
}

static std::vector<std::uint8_t> edited(const std::string& from, const std::string& to) {
    return edited(Edits{{from, to}});
}

static std::string load_error(const std::vector<std::uint8_t>& blob) {
    g2m::LoadResult r = g2m::load_model(blob.data(), blob.size());
    CHECK(!r.model);
    CHECK(!r.error.empty());
    return r.error;
}

static void check_rejected(const std::vector<std::uint8_t>& blob, const std::string& message) {
    std::string error = load_error(blob);
    CHECK_MSG(check::contains(error, message), "error '%s' lacks '%s'", error.c_str(), message.c_str());
}

TEST(loads_small) {
    g2m::LoadResult r = g2m::load_model(kData + "/small.g2m");
    CHECK_MSG(r.model, "%s", r.error.c_str());
    if (!r.model) return;
    const g2m::Model& m = *r.model;
    const g2m::ModelConfig& c = m.config;
    CHECK(c.sample_rate == 48000 && c.hop == 64);
    CHECK(c.pitch_min == 38 && c.pitch_max == 42 && c.n_pitches == 5);
    CHECK(c.frontend.size() == 2);
    CHECK(c.frontend[0].channels == 8 && c.frontend[0].kernel == 32 && c.frontend[0].stride == 16);
    CHECK(c.frontend[1].channels == 12 && c.frontend[1].kernel == 8 && c.frontend[1].stride == 4);
    CHECK(c.channels == 16 && c.kernel_size == 3 && c.gru == 24);
    CHECK((c.dilations == std::vector<int>{1, 2, 4}));
    CHECK(m.decoder.onset_threshold == 0.45f && m.decoder.frame_threshold == 0.5f);
    CHECK(m.decoder.frame_wait == 30 && m.decoder.off_frames == 15 && m.decoder.gate_frames == 3);
    CHECK(m.decoder.strong_onset == 0.52f && m.decoder.retrigger_threshold == 0.0f);
    CHECK(m.decoder.fallback_frames == 2 && m.decoder.fallback_threshold == 0.53f);
    CHECK(m.format_version == 2);
    CHECK(c.heads.empty() && c.head_classes == 0 && c.string_head == -1 && c.onset_type_head == -1);
    CHECK(m.decoder.legato_threshold == 0.5f && m.decoder.slide_threshold == 0.5f);  // as version 3 without heads
    CHECK(m.label_delay_ms == 5.208);
    CHECK(m.weights.size() == 10411);  // = parameters
    CHECK(m.tensors.size() == 4 + 2 + 4 * 3 + 4 + 2);
    CHECK(m.tensors.front().name == "frontend.0.weight" && m.tensors.back().name == "head.bias");
    CHECK((m.tensors.back().shape == std::vector<int>{15}));
    CHECK(m.tensor("head.bias") == m.weights.data() + m.weights.size() - 15);
    CHECK(m.tensor("frontend.0.weight") == m.weights.data());
    CHECK(m.tensor("nonsense") == nullptr);
}

TEST(loads_odd) {
    g2m::LoadResult r = g2m::load_model(kData + "/odd.g2m");
    CHECK_MSG(r.model, "%s", r.error.c_str());
    if (!r.model) return;
    const g2m::ModelConfig& c = r.model->config;
    CHECK(c.frontend.size() == 3 && c.kernel_size == 2 && (c.dilations == std::vector<int>{1, 3}));
    CHECK(c.pitch_min == 40 && c.n_pitches == 7);
    CHECK(r.model->weights.size() == 3289);
    CHECK(r.model->decoder.gate_frames == 0 && r.model->decoder.retrigger_threshold == 0.8f);
    CHECK(r.model->decoder.fallback_frames == 3 && r.model->decoder.fallback_threshold == 0.51f);
}

TEST(edited_header_still_loads) {  // the test helper itself
    std::vector<std::uint8_t> blob = edited("\"run\": \"test vectors: small\"", "\"run\": \"x\"");
    g2m::LoadResult r = g2m::load_model(blob.data(), blob.size());
    CHECK_MSG(r.model, "%s", r.error.c_str());
}

TEST(rejects_bad_files) {
    std::vector<std::uint8_t> good = read_file(kData + "/small.g2m");
    CHECK(check::contains(g2m::load_model(kData + "/no-such-file.g2m").error, "can't open"));

    std::vector<std::uint8_t> blob = good;
    blob[0] = 'X';
    check_rejected(blob, "magic");
    check_rejected({good.begin(), good.begin() + 6}, "magic");

    blob = good;
    blob[7] = 0x7F;  // header length far past the end
    check_rejected(blob, "too short");

    check_rejected({good.begin(), good.end() - 4}, "wrong size");
    blob = good;
    blob.back() ^= 1;
    check_rejected(blob, "CRC");

    check_rejected(edited("{", "["), "JSON");
    check_rejected(edited("\"format_version\": 2", "\"format_version\": 7"), "version");
    check_rejected(edited("\"format_version\": 2", "\"format_version\": 3"), "legato_threshold");  // required in version 3
    check_rejected(edited("\"format_version\": 2", "\"format_version\": 0"), "version");
    check_rejected(edited("\"fallback_frames\": 2,", ""), "fallback_frames");  // required in version 2
    check_rejected(edited("\"fallback_threshold\": 0.53", "\"fallback_threshold\": 0.4"), "below frame_threshold");
    check_rejected(edited("guitar2midi-note-model", "something-else"), "format");
    check_rejected(edited("\"head.bias\"", "\"head.blas\""), "unknown tensor 'head.blas'");
    check_rejected(edited("\"gru\": 24", "\"gru\": 25"), "shape");
    check_rejected(edited("\"pitch_max\": 42", "\"pitch_max\": 43"), "n_pitches");
    check_rejected(edited("    12,\n    8,\n    4\n", "    12,\n    8,\n    2\n"), "strides");
    check_rejected(edited("    8,\n    32,\n    16\n", "    8,\n    8,\n    16\n"), "kernel >= stride");
    check_rejected(edited("\"off_frames\": 15,", ""), "off_frames");
    check_rejected(edited("\"outputs\": [", "\"outputs\": [\"x\", "), "outputs");
    check_rejected(edited("\"offset\": 256", "\"offset\": 10411"), "outside the weights");
    check_rejected(edited("\"name\": \"frontend.0.bias\"", "\"name\": \"frontend.0.weight\""), "twice");
}

// Version 1 = version 2 with the late fallback off; its decoder has no
// fallback settings.
TEST(reads_version_1) {
    std::vector<std::uint8_t> blob = edited(Edits{{"\"format_version\": 2", "\"format_version\": 1"},
                                                  {",\n  \"fallback_frames\": 2,\n  \"fallback_threshold\": 0.53", ""}});
    g2m::LoadResult r = g2m::load_model(blob.data(), blob.size());
    CHECK_MSG(r.model, "%s", r.error.c_str());
    if (!r.model) return;
    CHECK(r.model->format_version == 1);
    CHECK(r.model->decoder.fallback_frames == 0);
    CHECK(r.model->decoder.onset_threshold == 0.45f && r.model->decoder.gate_frames == 3);
}

TEST(ignores_unknown_keys) {
    std::vector<std::uint8_t> blob = edited("\"hop\": 64,", "\"hop\": 64, \"future\": {\"x\": [1, 2]},");
    g2m::LoadResult r = g2m::load_model(blob.data(), blob.size());
    CHECK_MSG(r.model, "%s", r.error.c_str());
}

TEST(loads_mpe) {
    g2m::LoadResult r = g2m::load_model(kData + "/mpe.g2m");
    CHECK_MSG(r.model, "%s", r.error.c_str());
    if (!r.model) return;
    const g2m::Model& m = *r.model;
    const g2m::ModelConfig& c = m.config;
    CHECK(m.format_version == 3);
    CHECK(c.heads.size() == 2 && c.heads[0].name == "string" && c.heads[0].classes == 6);
    CHECK(c.heads[1].name == "onset_type" && c.heads[1].classes == 3);
    CHECK(c.head_classes == 9 && c.string_head == 0 && c.onset_type_head == 6);
    CHECK(m.decoder.legato_threshold == 0.55f && m.decoder.slide_threshold == 0.4f);
    CHECK(m.tensors.back().name == "extra_heads.onset_type.bias" && (m.tensors.back().shape == std::vector<int>{24}));
    const float* string_weight = m.tensor("extra_heads.string.weight");
    CHECK(string_weight && m.weights.size() == 13732);
}

TEST(rejects_bad_mpe_heads) {
    auto mpe = [](const Edits& edits) { return edited(edits, "mpe"); };
    check_rejected(mpe({{"\"slide_threshold\": 0.4", "\"x\": 0.4"}}), "slide_threshold");
    check_rejected(mpe({{"\"format_version\": 3", "\"format_version\": 2"}}), "heads before format version 3");
    check_rejected(mpe({{"\"name\": \"string\"", "\"name\": \"pitch_offset\""}}), "unknown head 'pitch_offset'");
    check_rejected(mpe({{"\"slide\"\n", "\"glide\"\n"}}), "unknown head 'onset_type'");  // classes not the spec's
    check_rejected(mpe({{"    \"6\"\n", "    \"6\", \"7\"\n"}}), "unknown head 'string'");
    check_rejected(mpe({{"\"heads\": [", "\"heads\": {\"x\": ["}, {"\n ],\n \"label_delay_ms\"", "]},\n \"label_delay_ms\""}}),
                   "'heads' isn't a list");
    check_rejected(mpe({{"\"name\": \"onset_type\",\n   \"classes\": [\n    \"picked\",\n    \"legato\",\n    \"slide\"\n   ]",
                         "\"name\": \"string\", \"classes\": [\"1\", \"2\", \"3\", \"4\", \"5\", \"6\"]"}}),
                   "listed twice");
    check_rejected(mpe({{"\"name\": \"string\",", ""}}), "lacks a name");
    check_rejected(mpe({{"\"extra_heads.string.bias\"", "\"extra_heads.string.blas\""}}), "unknown tensor");
}

TEST(loads_bend) {
    g2m::LoadResult r = g2m::load_model(kData + "/bend.g2m");
    CHECK_MSG(r.model, "%s", r.error.c_str());
    if (!r.model) return;
    const g2m::Model& m = *r.model;
    const g2m::ModelConfig& c = m.config;
    CHECK(m.format_version == 4 && c.heads.size() == 3);
    CHECK(c.heads[2].name == "pitch_offset" && c.heads[2].curve && c.heads[2].classes == 1);
    CHECK(c.heads[2].min == -12.0f && c.heads[2].max == 12.0f);
    CHECK(c.head_classes == 9 && c.curve_heads == 1 && c.pitch_offset_curve == 0);
    CHECK(m.decoder.bend_dead_band == 0.3f && m.decoder.bend_range == 24);
    CHECK(m.tensors.back().name == "extra_heads.pitch_offset.bias" && (m.tensors.back().shape == std::vector<int>{8}));
    // earlier versions get the defaults and no curve
    g2m::LoadResult v3 = g2m::load_model(kData + "/mpe.g2m");
    CHECK(v3.model && v3.model->decoder.bend_dead_band == 0.25f && v3.model->decoder.bend_range == 12);
    CHECK(v3.model && v3.model->config.curve_heads == 0 && v3.model->config.pitch_offset_curve == -1);
}

TEST(rejects_bad_curve_heads) {
    auto bend = [](const Edits& edits) { return edited(edits, "bend"); };
    check_rejected(bend({{"\"bend_range\": 24", "\"x\": 24"}}), "bend_range");
    check_rejected(bend({{"\"bend_range\": 24", "\"bend_range\": 97"}}), "bend_range");
    check_rejected(bend({{"\"bend_dead_band\": 0.3", "\"x\": 0.3"}}), "bend_dead_band");
    check_rejected(bend({{"\"format_version\": 4", "\"format_version\": 3"}, {",\n  \"bend_dead_band\": 0.3,\n  \"bend_range\": 24", ""}}),
                   "curve heads before format version 4");
    check_rejected(bend({{"\"name\": \"pitch_offset\"", "\"name\": \"damping\""}}), "unknown curve head 'damping'");
    check_rejected(bend({{"\"unit\": \"semitones\"", "\"unit\": \"cents\""}}), "unknown curve head 'pitch_offset'");
    check_rejected(bend({{"\"unit\": \"semitones\",", ""}}), "lacks a unit or a range");
    check_rejected(bend({{"\"min\": -12.0", "\"min\": 13.0"}}), "lacks a unit or a range");
    check_rejected(bend({{"\"name\": \"string\",", "\"name\": \"pitch_offset\", \"curve\": {\"unit\": \"semitones\", \"min\": -1, \"max\": 1},"}}),
                   "listed twice");
    check_rejected(bend({{"\"extra_heads.pitch_offset.bias\"", "\"extra_heads.pitch_offset.blas\""}}), "unknown tensor");
}

// Format version 5: projected gated layers
TEST(loads_proj) {
    g2m::LoadResult r = g2m::load_model(kData + "/proj.g2m");
    CHECK_MSG(r.model, "%s", r.error.c_str());
    if (!r.model) return;
    const g2m::Model& m = *r.model;
    CHECK(m.format_version == 5);
    CHECK((m.config.projections == std::vector<int>{5, 0, 11}));
    CHECK(m.tensor("proj.0.weight") && m.tensor("proj.2.bias") && !m.tensor("proj.1.weight"));
    for (const auto& t : m.tensors) {
        if (t.name == "proj.0.weight") CHECK((t.shape == std::vector<int>{5, 16}));
        if (t.name == "convs.0.weight") CHECK((t.shape == std::vector<int>{32, 5, 3}));
        if (t.name == "convs.1.weight") CHECK((t.shape == std::vector<int>{32, 16, 3}));
        if (t.name == "convs.2.weight") CHECK((t.shape == std::vector<int>{32, 11, 3}));
    }
    g2m::LoadResult v4 = g2m::load_model(kData + "/bend.g2m");
    CHECK(v4.model && (v4.model->config.projections == std::vector<int>{0, 0, 0}));
}

// Version 6: float16 weights, widened exactly; the same model as float32
// would give, once rounded
TEST(loads_half) {
    g2m::LoadResult r = g2m::load_model(kData + "/half.g2m");
    CHECK_MSG(r.model, "%s", r.error.c_str());
    if (!r.model) return;
    const g2m::Model& m = *r.model;
    CHECK(m.format_version == 6 && m.float16_weights);
    CHECK((m.config.projections == std::vector<int>{5, 0, 11}));
    CHECK(m.config.curve_heads == 1 && m.config.heads.size() == 3);
    std::vector<std::uint8_t> blob = read_file(kData + "/half.g2m");
    CHECK(m.weights.size() * 2 + 8 < blob.size() && (blob.size() - m.weights.size() * 2) % 16 == 0);
    long nonzero = 0;
    for (float w : m.weights) {
        nonzero += w != 0.0f;
        int e;  // a float16 value: 11 significant bits at most
        double frac = std::frexp(std::fabs(static_cast<double>(w)), &e);
        CHECK(std::ldexp(frac, 11) == std::floor(std::ldexp(frac, 11)));
    }
    CHECK(nonzero > static_cast<long>(m.weights.size()) / 2);
    g2m::LoadResult v5 = g2m::load_model(kData + "/proj.g2m");
    CHECK(v5.model && !v5.model->float16_weights);
}

TEST(rejects_bad_float16_files) {
    auto half = [](const Edits& edits) { return edited(edits, "half"); };
    check_rejected(half({{"\"weight_type\": \"float16\",", ""}}), "weight_type must be");
    check_rejected(half({{"\"weight_type\": \"float16\"", "\"weight_type\": \"float32\""}}), "weight_type must be");
    check_rejected(half({{"\"format_version\": 6", "\"format_version\": 5"}}), "weight_type before format version 6");
    check_rejected(edited({{"\"format_version\": 5,", "\"format_version\": 6,\n \"weight_type\": \"float16\","}}, "proj"),
                   "wrong size");  // float32 bytes
    // projections may be all 0 in version 6
    std::vector<std::uint8_t> unprojected = half({{"\"projections\": [\n   5,\n   0,\n   11\n  ]", "\"projections\": [0, 0, 0]"}});
    std::string error = load_error(unprojected);  // only because its tensors are the projected ones
    CHECK_MSG(!check::contains(error, "without projections") && check::contains(error, "tensor"), "%s", error.c_str());
}

// Every float16 bit pattern: sign · 2^(e − 15) · (1 + m / 1024), subnormals
// m · 2^−24, then infinities and NaN; and a few values NumPy gives
TEST(half_to_float) {
    for (std::uint32_t bits = 0; bits < 0x10000; ++bits) {
        float f = g2m::half_to_float(static_cast<std::uint16_t>(bits));
        int e = (bits >> 10) & 0x1f, m = bits & 0x3ff;
        double sign = (bits & 0x8000) ? -1.0 : 1.0;
        if (e == 0x1f) {
            CHECK(m ? std::isnan(f) : std::isinf(f) && std::signbit(f) == (sign < 0));
            continue;
        }
        double want = sign * (e ? std::ldexp(1.0 + m / 1024.0, e - 15) : std::ldexp(m, -24));
        CHECK_MSG(static_cast<double>(f) == want && std::signbit(f) == (sign < 0), "0x%04x: %.9g, want %.9g",
                  bits, static_cast<double>(f), want);
    }
    // np.float16 bit patterns → np.float32
    CHECK(g2m::half_to_float(0x3c00) == 1.0f);
    CHECK(g2m::half_to_float(0xc000) == -2.0f);
    CHECK(g2m::half_to_float(0x7bff) == 65504.0f);
    CHECK(g2m::half_to_float(0x0400) == 6.103515625e-05f);
    CHECK(g2m::half_to_float(0x0001) == 5.9604644775390625e-08f);
    CHECK(g2m::half_to_float(0x3555) == 0.333251953125f);
}

TEST(rejects_bad_projections) {
    auto proj = [](const Edits& edits) { return edited(edits, "proj"); };
    const std::string list = "\"projections\": [\n   5,\n   0,\n   11\n  ]";
    check_rejected(proj({{list, "\"projections\": [5, 0]"}}), "one entry per gated layer");
    check_rejected(proj({{list, "\"projections\": [5, 0, 17]"}}), "0 to channels");
    check_rejected(proj({{list, "\"projections\": [5, -1, 11]"}}), "0 to channels");
    check_rejected(proj({{list, "\"projections\": [0, 0, 0]"}}), "version 5 model without projections");
    check_rejected(proj({{"\"format_version\": 5", "\"format_version\": 4"}}), "projections before format version 5");
    check_rejected(proj({{list, "\"projections\": [5, 0, 10]"}}), "shape that doesn't match");
    check_rejected(proj({{"\"proj.0.bias\"", "\"proj.1.bias\""}}), "unknown tensor");
}
