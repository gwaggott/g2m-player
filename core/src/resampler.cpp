#include <g2m/resampler.h>

#include <algorithm>
#include <cmath>
#include <numeric>

namespace g2m {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kCutoff = 0.95;  // of the lower rate's Nyquist frequency
constexpr int kZeroCrossings = 16;  // of the sinc on each side, at the cutoff
constexpr double kBeta = 8.0;  // the Kaiser window's: about 80 dB of stopband
constexpr int kMinRate = 8000, kMaxRate = 384000;

// The modified Bessel function of the first kind, order 0
double bessel_i0(double x) {
    double sum = 1, term = 1;
    for (int k = 1; k < 50; ++k) {
        term *= (x / (2 * k)) * (x / (2 * k));
        sum += term;
        if (term < sum * 1e-17) break;
    }
    return sum;
}

}  // namespace

bool Resampler::supported(int in_rate, int out_rate) {
    if (in_rate < kMinRate || in_rate > kMaxRate || out_rate < kMinRate || out_rate > kMaxRate) return false;
    int g = std::gcd(in_rate, out_rate);
    return out_rate / g <= kMaxPhases && in_rate / g <= kMaxPhases;
}

Resampler::Resampler(int in_rate, int out_rate) {
    int g = std::gcd(in_rate, out_rate);
    phases_ = out_rate / g;
    step_ = in_rate / g;
    // The cutoff in cycles per input sample times two: 1 is the input's Nyquist
    double r = std::min(1.0, static_cast<double>(phases_) / step_) * kCutoff;
    double half = kZeroCrossings / r;  // the window's half width, in input samples
    reach_ = static_cast<int>(std::ceil(half));
    taps_ = 2 * reach_;
    table_.resize(static_cast<std::size_t>(phases_) * taps_);
    double window_scale = 1 / bessel_i0(kBeta);
    for (int p = 0; p < phases_; ++p) {
        float* row = table_.data() + static_cast<std::size_t>(p) * taps_;
        double sum = 0;
        for (int k = 0; k < taps_; ++k) {
            // tap k is input base - reach + 1 + k; the output is at base + p / L
            double t = (k - reach_ + 1) - static_cast<double>(p) / phases_;
            double h = 0;
            if (std::fabs(t) < half) {
                double x = r * t;
                double sinc = x == 0 ? 1 : std::sin(kPi * x) / (kPi * x);
                double w = t / half;
                h = r * sinc * bessel_i0(kBeta * std::sqrt(1 - w * w)) * window_scale;
            }
            row[k] = static_cast<float>(h);
            sum += h;
        }
        for (int k = 0; k < taps_; ++k) row[k] = static_cast<float>(row[k] / sum);  // unity gain at DC
    }
    history_.resize(2 * static_cast<std::size_t>(taps_));
    reset();
}

void Resampler::reset() {
    std::fill(history_.begin(), history_.end(), 0.0f);
    write_ = 0;
    received_ = 0;
    base_ = 0;
    phase_ = 0;
}

std::size_t Resampler::max_output(std::size_t n) const {
    return (n * phases_ + step_ - 1) / step_ + 1;
}

std::size_t Resampler::process(const float* in, std::size_t n, float* out, std::uint32_t* source) {
    std::size_t made = 0;
    for (std::size_t i = 0; i < n; ++i) {
        history_[write_] = history_[write_ + taps_] = in[i];
        write_ = write_ + 1 == taps_ ? 0 : write_ + 1;
        ++received_;
        // Each output is made on the input that completes its window, so the
        // window is always the last taps_ inputs.
        while (base_ + reach_ < received_) {
            const float* x = history_.data() + write_;
            const float* h = table_.data() + static_cast<std::size_t>(phase_) * taps_;
            float sum = 0;
            for (int k = 0; k < taps_; ++k) sum += h[k] * x[k];
            out[made] = sum;
            source[made] = static_cast<std::uint32_t>(i);
            ++made;
            phase_ += step_;
            base_ += phase_ / phases_;
            phase_ %= phases_;
        }
    }
    return made;
}

}  // namespace g2m
