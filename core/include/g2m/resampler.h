// A streaming sample-rate converter for the model's input: a fixed ratio
// L / M (the two rates over their greatest common divisor), as a polyphase
// windowed-sinc filter (Kaiser window), with the cutoff just below the lower
// rate's Nyquist frequency.
//
// Each output sample is made as soon as the input it needs has arrived, and
// process() says which input sample completed it, so a caller can place
// what the output leads to (the model's events) on the input's timeline.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace g2m {

class Resampler {
public:
    // Rates this converts between: both 8 kHz to 384 kHz, with at most
    // kMaxPhases phases (L) and steps (M). Every common audio rate is.
    static bool supported(int in_rate, int out_rate);
    static constexpr int kMaxPhases = 1024;

    // Allocates everything; process() and reset() then allocate nothing.
    // The rates must be supported.
    Resampler(int in_rate, int out_rate);

    // Takes `n` input samples; writes the output samples they complete to
    // `out` and returns how many. `source[j]` is the index in `in` of the
    // input sample that completed out[j]. `out` and `source` need room for
    // max_output(n).
    std::size_t process(const float* in, std::size_t n, float* out, std::uint32_t* source);

    // The most outputs process() can return for `n` inputs.
    std::size_t max_output(std::size_t n) const;

    // The delay, in input samples (rounded up): output n stands for input
    // time n * M / L, but needs the input up to this many samples later.
    int latency() const { return reach_; }

    // Silence: the history and position as at construction.
    void reset();

private:
    int phases_;  // L
    int step_;  // M
    int taps_;  // per phase: inputs base - reach_ + 1 .. base + reach_
    int reach_;
    std::vector<float> table_;  // [phases_][taps_], tap 0 the oldest input
    std::vector<float> history_;  // the last taps_ inputs, twice over, so each window is contiguous
    int write_ = 0;  // where the next input goes in history_'s first half
    std::int64_t received_ = 0;  // inputs taken
    std::int64_t base_ = 0;  // the next output's input index: floor(n * M / L)
    int phase_ = 0;  // and its phase: n * M mod L
};

}  // namespace g2m
