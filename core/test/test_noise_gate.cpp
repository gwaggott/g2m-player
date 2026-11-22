#include <g2m/noise_gate.h>

#include <cmath>
#include <vector>

#include "check.h"

namespace {

constexpr double kRate = 48000;
constexpr std::uint32_t kHold = 480;  // 10 ms

float db(float d) {
    return std::pow(10.0f, d / 20.0f);
}

g2m::NoiseGate gate(float threshold_db, float release_ms = 10) {
    g2m::NoiseGate g;
    g.set(threshold_db, release_ms, kRate);
    g.reset();
    return g;
}

// Runs n samples of constant x; returns the last output.
float run(g2m::NoiseGate& g, float x, std::uint32_t n) {
    float y = 0;
    for (std::uint32_t i = 0; i < n; ++i) y = g.process(x);
    return y;
}

}  // namespace

TEST(off_passes_everything) {
    g2m::NoiseGate g = gate(g2m::kGateOff);
    CHECK(g.off() && g.open());
    for (float x : {0.0f, 1e-9f, -0.3f, 1.0f}) CHECK(g.process(x) == x);
    g.set(-100, 10, kRate);
    CHECK(g.off());
}

TEST(closed_below_threshold) {
    g2m::NoiseGate g = gate(-40);
    CHECK(!g.off() && !g.open());
    CHECK(run(g, db(-41), 1000) == 0);
    CHECK(run(g, -db(-41), 1000) == 0);
    CHECK(!g.open());
}

TEST(opens_on_the_first_sample) {
    g2m::NoiseGate g = gate(-40);
    run(g, db(-50), 100);
    float x = db(-39);
    CHECK(g.process(x) == x);
    CHECK(g.process(-x) == -x);  // either sign
    CHECK(g.open());
    g.reset();
    CHECK(g.process(db(-40)) == db(-40));  // at the threshold exactly
}

TEST(hold_then_release) {
    g2m::NoiseGate g = gate(-40, 10);  // 480-sample fade
    g.process(db(-30));
    float x = db(-60);
    CHECK(run(g, x, kHold) == x);  // the hold: unchanged
    float previous = x;
    for (int i = 1; i <= 480; ++i) {
        float y = g.process(x);
        CHECK_MSG(y < previous, "sample %d", i);
        CHECK(std::fabs(y - x * (1.0f - i / 480.0f)) < 1e-6f);
        previous = y;
    }
    CHECK(g.process(x) == 0);
    CHECK(!g.open());
}

TEST(release_time) {
    for (float ms : {5.0f, 50.0f, 500.0f}) {
        g2m::NoiseGate g = gate(-40, ms);
        g.process(1);
        run(g, 0.001f, kHold);
        auto fade = static_cast<std::uint32_t>(std::lround(ms * 48));
        CHECK_MSG(run(g, 0.001f, fade - 1) > 0, "%g ms", ms);
        CHECK_MSG(g.process(0.001f) == 0, "%g ms", ms);
    }
}

// Between the close level and the threshold: stays open while open, doesn't open when closed
TEST(hysteresis) {
    g2m::NoiseGate g = gate(-40);
    float between = db(-43);  // above -46, below -40
    CHECK(run(g, between, 2000) == 0);
    g.process(db(-30));
    CHECK(run(g, between, 10 * kHold) == between);
    // just below the close level: the hold runs out and it fades
    float below = db(-46.5f);
    CHECK(run(g, below, kHold) == below);
    CHECK(run(g, below, 1000) == 0);
}

// A sample above the close level during the hold restarts it
TEST(hold_restarts) {
    g2m::NoiseGate g = gate(-40);
    g.process(1);
    run(g, 0, kHold - 1);
    g.process(db(-45));  // above -46
    CHECK(run(g, 0.0001f, kHold) == 0.0001f);
    CHECK(g.process(0.0001f) < 0.0001f);
}

// During the fade only the threshold opens it again, at once
TEST(reopens_during_release) {
    g2m::NoiseGate g = gate(-40);
    g.process(1);
    run(g, 0.001f, kHold + 100);
    float faded = g.process(db(-43));  // between: keeps fading
    CHECK(faded > 0 && faded < db(-43));
    CHECK(g.process(db(-39)) == db(-39));
    CHECK(run(g, db(-60), kHold) == db(-60));  // with a new hold
}

TEST(block_process) {
    g2m::NoiseGate a = gate(-30), b = gate(-30);
    std::vector<float> in(5000), out(5000);
    for (std::size_t i = 0; i < in.size(); ++i)
        in[i] = static_cast<float>(std::sin(i * 0.05) * (i < 2000 ? 0.5 : 0.001));
    a.process(in.data(), out.data(), static_cast<std::uint32_t>(in.size()));
    for (std::size_t i = 0; i < in.size(); ++i) CHECK(out[i] == b.process(in[i]));
    CHECK(out[100] != 0 && out[4999] == 0);
}
