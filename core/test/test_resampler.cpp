#include <g2m/resampler.h>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <random>
#include <vector>

#include "allocations.h"
#include "check.h"

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kModelRate = 48000;

struct Result {
    std::vector<float> out;
    std::vector<std::int64_t> source;  // the completing input's index in the whole stream
};

// Runs `in` through a Resampler in blocks of `block` (0: random sizes up to 300)
Result run(g2m::Resampler& r, const std::vector<float>& in, std::size_t block, unsigned seed = 1) {
    Result result;
    std::mt19937 rng(seed);
    std::vector<float> out(r.max_output(300 + block));
    std::vector<std::uint32_t> source(out.size());
    for (std::size_t i = 0; i < in.size();) {
        std::size_t n = std::min(in.size() - i, block ? block : std::uniform_int_distribution<std::size_t>(0, 300)(rng));
        std::size_t made = r.process(in.data() + i, n, out.data(), source.data());
        CHECK(made <= r.max_output(n));
        for (std::size_t j = 0; j < made; ++j) {
            result.out.push_back(out[j]);
            result.source.push_back(static_cast<std::int64_t>(i + source[j]));
        }
        i += n;
    }
    return result;
}

std::vector<float> sine(int rate, double frequency, double seconds, double amplitude = 0.5) {
    std::vector<float> x(static_cast<std::size_t>(rate * seconds));
    for (std::size_t i = 0; i < x.size(); ++i) x[i] = static_cast<float>(amplitude * std::sin(2 * kPi * frequency * i / rate));
    return x;
}

// The largest difference from the same sine sampled at 48 kHz, past the
// start (where the filter still sees the silence before the input)
double sine_error(int rate, double frequency) {
    g2m::Resampler r(rate, kModelRate);
    Result y = run(r, sine(rate, frequency, 0.5), 256);
    double worst = 0;
    std::size_t skip = static_cast<std::size_t>(4.0 * r.latency() * kModelRate / rate) + 1;
    for (std::size_t n = skip; n < y.out.size(); ++n)
        worst = std::max(worst, std::fabs(y.out[n] - 0.5 * std::sin(2 * kPi * frequency * n / kModelRate)));
    return worst;
}

const int kRates[] = {22050, 32000, 44100, 88200, 96000, 176400, 192000};

}  // namespace

TEST(supported_rates) {
    for (int rate : kRates) CHECK(g2m::Resampler::supported(rate, kModelRate));
    CHECK(g2m::Resampler::supported(48000, kModelRate));
    CHECK(!g2m::Resampler::supported(7999, kModelRate));
    CHECK(!g2m::Resampler::supported(400000, kModelRate));
    CHECK(!g2m::Resampler::supported(44101, kModelRate));  // 48000 phases
}

TEST(latency_is_a_fraction_of_a_millisecond) {
    for (int rate : kRates) {
        g2m::Resampler r(rate, kModelRate);
        double ms = 1000.0 * r.latency() / rate;
        CHECK_MSG(ms > 0.2 && ms < 0.8, "%d Hz: %g ms", rate, ms);
    }
}

TEST(sines_in_the_passband) {
    for (int rate : kRates) {
        double nyquist = std::min(rate, kModelRate) / 2.0;
        double low = sine_error(rate, 1000);
        double mid = sine_error(rate, 0.75 * nyquist);
        double high = sine_error(rate, 0.85 * nyquist);  // in the transition: 0.2 dB down
        CHECK_MSG(low < 2e-4, "%d Hz, 1 kHz: %g", rate, low);
        CHECK_MSG(mid < 2e-3, "%d Hz, %g Hz: %g", rate, 0.75 * nyquist, mid);
        CHECK_MSG(high < 0.015, "%d Hz, %g Hz: %g", rate, 0.85 * nyquist, high);
    }
}

TEST(no_aliasing_from_above_the_output_band) {
    for (int rate : {88200, 96000, 192000}) {
        g2m::Resampler r(rate, kModelRate);
        Result y = run(r, sine(rate, 30000, 0.5), 256);
        double peak = 0;
        for (std::size_t n = 1000; n < y.out.size(); ++n) peak = std::max(peak, std::fabs(static_cast<double>(y.out[n])));
        CHECK_MSG(peak < 0.5e-3, "%d Hz: %g", rate, peak);  // -60 dB below the input
    }
}

TEST(each_output_on_the_input_that_completes_it) {
    for (int rate : kRates) {
        g2m::Resampler r(rate, kModelRate);
        int g = std::gcd(rate, kModelRate);
        std::int64_t phases = kModelRate / g, step = rate / g;
        Result y = run(r, sine(rate, 440, 0.25), 0);
        CHECK(!y.out.empty());
        bool ok = true;
        for (std::size_t n = 0; n < y.source.size(); ++n)
            ok = ok && y.source[n] == static_cast<std::int64_t>(n) * step / phases + r.latency();
        CHECK_MSG(ok, "%d Hz", rate);
    }
}

TEST(any_block_sizes) {
    for (int rate : kRates) {
        g2m::Resampler whole(rate, kModelRate), pieces(rate, kModelRate);
        std::vector<float> in = sine(rate, 997, 0.2);
        Result a = run(whole, in, in.size());
        Result b = run(pieces, in, 0, static_cast<unsigned>(rate));
        CHECK_MSG(a.out == b.out && a.source == b.source, "%d Hz", rate);
    }
}

TEST(reset_starts_again) {
    g2m::Resampler r(44100, kModelRate);
    std::vector<float> in = sine(44100, 523, 0.1);
    Result first = run(r, in, 128);
    r.reset();
    Result second = run(r, in, 128);
    CHECK(first.out == second.out && first.source == second.source);
}

TEST(no_allocation_while_processing) {
    g2m::Resampler r(44100, kModelRate);
    std::vector<float> in = sine(44100, 220, 0.1);
    std::vector<float> out(r.max_output(in.size()));
    std::vector<std::uint32_t> source(out.size());
    long before = allocations;
    r.process(in.data(), in.size(), out.data(), source.data());
    r.reset();
    CHECK(allocations == before);
}
