// A noise gate with no lookahead (the player adds no latency): it opens on
// the first sample whose magnitude reaches the threshold, so an attack is
// never cut. While open, each sample at or above the close level (6 dB
// below the threshold) restarts a 10 ms hold; after the hold the gain fades
// linearly to 0 over the release time, and a sample at the threshold opens
// it again at once. Closed is silence. A threshold at or below kGateOff
// turns it off (gain 1). No allocation.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace g2m {

constexpr float kGateOff = -96.0f;  // dBFS
constexpr float kGateHysteresisDb = 6.0f;
constexpr double kGateHoldSeconds = 0.010;

class NoiseGate {
public:
    // Threshold in dBFS, release in ms. Cheap: may be called every block.
    void set(float threshold_db, float release_ms, double sample_rate) {
        off_ = threshold_db <= kGateOff;
        open_level_ = std::pow(10.0f, threshold_db / 20.0f);
        close_level_ = std::pow(10.0f, (threshold_db - kGateHysteresisDb) / 20.0f);
        hold_samples_ = static_cast<std::uint32_t>(std::lround(kGateHoldSeconds * sample_rate));
        release_samples_ = static_cast<std::uint32_t>(std::max(1L, std::lround(release_ms * 0.001 * sample_rate)));
        fade_ = std::min(fade_, release_samples_);
    }

    // Closed, as at the start of a stream.
    void reset() {
        gain_ = 0;
        hold_ = 0;
        fade_ = 0;
    }

    bool off() const { return off_; }
    // Open, holding or fading out (not fully closed)
    bool open() const { return off_ || gain_ > 0; }

    float process(float x) {
        if (off_) return x;
        float level = std::fabs(x);
        if (level >= open_level_) {
            gain_ = 1;
            hold_ = hold_samples_;
            fade_ = release_samples_;
        } else if (gain_ == 1 && level >= close_level_) {
            hold_ = hold_samples_;
        } else if (hold_ > 0) {
            --hold_;
        } else if (fade_ > 0) {
            --fade_;
            gain_ = static_cast<float>(fade_) / static_cast<float>(release_samples_);
        }
        return x * gain_;
    }

    void process(const float* in, float* out, std::uint32_t n) {
        for (std::uint32_t i = 0; i < n; ++i) out[i] = process(in[i]);
    }

private:
    bool off_ = true;
    float open_level_ = 0, close_level_ = 0;
    std::uint32_t hold_samples_ = 0;
    std::uint32_t release_samples_ = 1;
    float gain_ = 0;
    std::uint32_t hold_ = 0;  // samples left before the fade starts
    std::uint32_t fade_ = 0;  // samples left in the fade
};

}  // namespace g2m
