#include "network.h"

#include <cmath>
#include <fstream>
#include <iterator>

#include "allocations.h"
#include "check.h"
#include "vectors.h"

// The largest difference from the reference's outputs over the whole input.
static float max_difference(g2m::Network& net, const std::vector<float>& audio, const std::vector<float>& expected) {
    int hop = net.model().config.hop;
    std::size_t per_block = 3 * net.model().config.n_pitches;
    std::size_t blocks = audio.size() / hop;
    CHECK(expected.size() == blocks * per_block);
    float worst = 0;
    for (std::size_t k = 0; k < blocks && (k + 1) * per_block <= expected.size(); ++k) {
        const float* y = net.step(audio.data() + k * hop);
        for (std::size_t i = 0; i < per_block; ++i) worst = std::max(worst, std::fabs(y[i] - expected[k * per_block + i]));
    }
    return worst;
}

TEST(matches_reference) {
    for (const char* name : vectors::kNames) {
        auto model = vectors::load(name);
        if (!model) continue;
        g2m::Network net(model);
        float worst = max_difference(net, vectors::input(name), vectors::outputs(name));
        std::fprintf(stderr, "  %s: largest difference from the reference %.2g\n", name, worst);
        CHECK_MSG(worst <= 1e-5f, "%s: %g", name, worst);
    }
}

TEST(reset_restarts) {
    for (const char* name : vectors::kNames) {
        auto model = vectors::load(name);
        if (!model) continue;
        g2m::Network net(model);
        std::vector<float> audio = vectors::input(name);
        int hop = model->config.hop;
        std::size_t per_block = 3 * model->config.n_pitches;
        std::vector<float> first;
        for (int k = 0; k < 100; ++k) {
            const float* y = net.step(audio.data() + k * hop);
            first.insert(first.end(), y, y + per_block);
        }
        net.reset();
        bool same = true;
        for (int k = 0; k < 100; ++k) {
            const float* y = net.step(audio.data() + k * hop);
            for (std::size_t i = 0; i < per_block; ++i) same = same && y[i] == first[k * per_block + i];
        }
        CHECK_MSG(same, "%s", name);
    }
}

TEST(step_does_not_allocate) {
    auto model = vectors::load("small");
    if (!model) return;
    g2m::Network net(model);
    std::vector<float> audio = vectors::input("small");
    long before = allocations;
    for (int k = 0; k < 200; ++k) net.step(audio.data() + k * model->config.hop);
    net.reset();
    net.step(audio.data());
    CHECK(allocations == before);
}

// Format version 3: the note outputs, and the MPE heads computed for each
// pitch from the same step's features, match the reference's outputs.
TEST(heads_match_reference) {
    auto model = vectors::load(vectors::kMpe);
    if (!model) return;
    const g2m::ModelConfig& c = model->config;
    CHECK(c.head_classes == 9);
    g2m::Network net(model);
    std::vector<float> audio = vectors::input(vectors::kMpe);
    std::vector<float> expected = vectors::outputs(vectors::kMpe);
    std::size_t width = 3 + c.head_classes, blocks = audio.size() / c.hop;
    CHECK(expected.size() == blocks * c.n_pitches * width);
    float worst_notes = 0, worst_heads = 0;
    std::vector<float> heads(c.head_classes);
    long before = allocations;
    for (std::size_t k = 0; k < blocks && (k + 1) * c.n_pitches * width <= expected.size(); ++k) {
        const float* y = net.step(audio.data() + k * c.hop);
        for (int p = 0; p < c.n_pitches; ++p) {
            const float* want = expected.data() + (k * c.n_pitches + p) * width;
            for (int i = 0; i < 3; ++i) worst_notes = std::max(worst_notes, std::fabs(y[3 * p + i] - want[i]));
            net.heads(p, heads.data());
            for (int i = 0; i < c.head_classes; ++i) worst_heads = std::max(worst_heads, std::fabs(heads[i] - want[3 + i]));
        }
    }
    CHECK(allocations == before);
    std::fprintf(stderr, "  mpe: largest difference from the reference %.2g (notes), %.2g (heads)\n", worst_notes,
                 worst_heads);
    CHECK(worst_notes <= 1e-5f && worst_heads <= 1e-5f);
}

// Format version 4: the curve head, clamped, and every head in file order
TEST(curves_match_reference) {
    auto model = vectors::load(vectors::kBend);
    if (!model) return;
    const g2m::ModelConfig& c = model->config;
    g2m::Network net(model);
    std::vector<float> audio = vectors::input(vectors::kBend);
    std::vector<float> expected = vectors::outputs(vectors::kBend);
    std::size_t width = 3 + c.head_classes + c.curve_heads, blocks = audio.size() / c.hop;
    CHECK(width == 13 && expected.size() == blocks * c.n_pitches * width);
    float worst = 0, curve_worst = 0;
    long clamped = 0;
    std::vector<float> all(width - 3), curve(1), classes(c.head_classes);
    for (std::size_t k = 0; k < blocks && (k + 1) * c.n_pitches * width <= expected.size(); ++k) {
        net.step(audio.data() + k * c.hop);
        for (int p = 0; p < c.n_pitches; ++p) {
            const float* want = expected.data() + (k * c.n_pitches + p) * width + 3;
            net.all_heads(p, all.data());
            net.curves(p, curve.data());
            net.heads(p, classes.data());
            for (std::size_t i = 0; i < all.size(); ++i) worst = std::max(worst, std::fabs(all[i] - want[i]));
            curve_worst = std::max(curve_worst, std::fabs(curve[0] - want[9]));
            for (int i = 0; i < c.head_classes; ++i) worst = std::max(worst, std::fabs(classes[i] - want[i]));
            clamped += std::fabs(curve[0]) == 12.0f;
        }
    }
    std::fprintf(stderr, "  bend: heads within %.2g, pitch offset within %.2g; %ld values clamped at 12\n", worst,
                 curve_worst, clamped);
    CHECK(worst <= 1e-5f && curve_worst <= 1e-5f);
}

// The curve is clamped to the header's range: here narrowed to ±0.5
TEST(curve_is_clamped) {
    std::ifstream file(vectors::kData + "/bend.g2m", std::ios::binary);
    std::string blob{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    for (auto [from, to] : {std::pair<std::string, std::string>{"\"min\": -12.0", "\"min\": -0.50"},
                            {"\"max\": 12.0", "\"max\": 0.50"}}) {
        std::size_t at = blob.find(from);
        CHECK(at != std::string::npos);
        if (at != std::string::npos) blob.replace(at, from.size(), to);  // same length: the layout is unchanged
    }
    g2m::LoadResult r = g2m::load_model(reinterpret_cast<const std::uint8_t*>(blob.data()), blob.size());
    CHECK_MSG(r.model, "%s", r.error.c_str());
    if (!r.model) return;
    std::shared_ptr<const g2m::Model> model(std::move(r.model));
    g2m::Network net(model);
    std::vector<float> audio = vectors::input(vectors::kBend);
    long at_limit = 0, inside = 0;
    float curve = 0;
    for (int k = 0; k < 500; ++k) {
        net.step(audio.data() + k * model->config.hop);
        for (int p = 0; p < model->config.n_pitches; ++p) {
            net.curves(p, &curve);
            CHECK(curve >= -0.5f && curve <= 0.5f);
            at_limit += std::fabs(curve) == 0.5f;
            inside += std::fabs(curve) < 0.5f;
        }
    }
    CHECK(at_limit > 0 && inside > 0);
}

// Format version 5: projected gated layers (one not), with its heads; reset
// restarts from the projection of silence; no allocation. Also version 6's
// `half`: float16 weights, every head.
static void projected_matches_reference(const char* name) {
    auto model = vectors::load(name);
    if (!model) return;
    const g2m::ModelConfig& c = model->config;
    g2m::Network net(model);
    std::vector<float> audio = vectors::input(name);
    std::vector<float> expected = vectors::outputs(name);
    std::size_t width = 3 + c.head_classes + c.curve_heads, blocks = audio.size() / c.hop;
    CHECK(expected.size() == blocks * c.n_pitches * width);
    std::vector<float> heads(width - 3);
    for (int pass = 0; pass < 2; ++pass) {
        float worst = 0;
        long before = allocations;
        for (std::size_t k = 0; k < blocks && (k + 1) * c.n_pitches * width <= expected.size(); ++k) {
            const float* y = net.step(audio.data() + k * c.hop);
            for (int p = 0; p < c.n_pitches; ++p) {
                const float* want = expected.data() + (k * c.n_pitches + p) * width;
                for (int i = 0; i < 3; ++i) worst = std::max(worst, std::fabs(y[3 * p + i] - want[i]));
                net.all_heads(p, heads.data());
                for (std::size_t i = 0; i < heads.size(); ++i) worst = std::max(worst, std::fabs(heads[i] - want[3 + i]));
            }
        }
        CHECK(allocations == before);
        std::fprintf(stderr, "  %s%s: largest difference from the reference %.2g\n", name, pass ? " after reset" : "",
                     worst);
        CHECK(worst <= 1e-5f);
        net.reset();
    }
}

TEST(projected_matches_reference) {
    projected_matches_reference(vectors::kProj);
}

// On AArch64 the weights stay float16 (widened in the kernel), elsewhere
// float32; the same tolerance either way
TEST(float16_weights_match_reference) {
    projected_matches_reference(vectors::kHalf);
    auto half = vectors::load(vectors::kHalf), proj = vectors::load(vectors::kProj);
    if (!half || !proj) return;
    CHECK(g2m::Network(half).float16_tiles() == g2m::Network::float16_supported());
    CHECK(!g2m::Network(proj).float16_tiles());
    std::fprintf(stderr, "  float16 tiles %s\n", g2m::Network::float16_supported() ? "on" : "off (float32 here)");
}
