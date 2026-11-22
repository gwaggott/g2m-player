#include "network.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

// G2M_NO_SIMD (a compile definition) forces the plain loops, to test them
#if defined(G2M_NO_SIMD)
#elif defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <emmintrin.h>
#define G2M_TILE_SSE2 1
#elif defined(__ARM_NEON) || defined(__aarch64__)
#include <arm_neon.h>
#define G2M_TILE_NEON 1
#if defined(__aarch64__)  // float16 conversions are part of AArch64's NEON (32-bit ARM needs an option for them)
#define G2M_TILE_F16 1
#endif
#endif

namespace g2m {

namespace {

// As reference.py computes it, to stay within float rounding of it.
inline float sigmoid(float x) {
    return 0.5f * (1.0f + std::tanh(0.5f * x));
}

// a · b over n values. 16 running sums (element i goes to sum i % 16),
// added pairwise at the end: independent lanes the compiler can put in SIMD
// registers without reordering any float addition itself, so the result is
// the same on every compiler and CPU (apart from fused multiply-adds where
// a target contracts them). For the MPE heads' few rows per pitch.
inline float dot(const float* a, const float* b, int n) {
    constexpr int kLanes = 16;
    float sums[kLanes] = {};
    int i = 0;
    for (; i + kLanes <= n; i += kLanes)
        for (int k = 0; k < kLanes; ++k) sums[k] += a[i + k] * b[i + k];
    for (int k = 0; i + k < n; ++k) sums[k] += a[i + k] * b[i + k];
    for (int width = kLanes / 2; width > 0; width /= 2)
        for (int k = 0; k < width; ++k) sums[k] += sums[k + width];
    return sums[0];
}

// y = W x + b, W row-major [rows, cols]
void matvec(const float* w, const float* x, const float* b, int rows, int cols, float* y) {
    for (int r = 0; r < rows; ++r) y[r] = dot(w + static_cast<std::size_t>(r) * cols, x, cols) + b[r];
}

// `weight(r, c)`: row r's weight for column c, in the order apply() reads them.
template <class Weight>
Network::Tiled tile(int rows, int cols, const float* bias, Weight weight) {
    constexpr int R = Network::kTile;
    Network::Tiled t;
    t.rows = rows;
    t.cols = cols;
    t.bias = bias;
    int tiles = (rows + R - 1) / R;
    t.w.assign(static_cast<std::size_t>(tiles) * cols * R, 0.0f);
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < cols; ++c)
            t.w[(static_cast<std::size_t>(r / R) * cols + c) * R + r % R] = weight(r, c);
    return t;
}

#if defined(G2M_TILE_F16)
// The tile's weights as float16 bits instead of float32: half the bytes a
// block reads. Only for a model whose weights are float16 values already
// (format version 6), so the conversion is exact.
void to_half(Network::Tiled& t) {
    t.half.resize(t.w.size());
    for (std::size_t i = 0; i < t.w.size(); ++i) {
        __fp16 h = static_cast<__fp16>(t.w[i]);
        std::memcpy(&t.half[i], &h, 2);
    }
    std::vector<float>().swap(t.w);
}
#endif

// A row-major [rows, cols] tensor
Network::Tiled tile_rows(const float* w, int rows, int cols, const float* bias) {
    return tile(rows, cols, bias, [&](int r, int c) { return w[static_cast<std::size_t>(r) * cols + c]; });
}

// y = W [parts[0] ; parts[1] ; ...] + b, each part `part_len` values (the
// columns in that order). Each row's sum runs over the columns in order.
// The SIMD paths keep R / 4 vectors of sums in registers (compilers turn
// the plain loop inside out, vectorising across columns); every path does
// the same multiplies and adds in the same order, so the results are equal.
void apply(const Network::Tiled& m, const float* const* parts, int n_parts, int part_len, float* y) {
    constexpr int R = Network::kTile;
#if defined(G2M_TILE_F16)
    if (!m.half.empty()) {  // as the NEON path below, each weight widened from float16 as it's loaded
        const float16_t* h = reinterpret_cast<const float16_t*>(m.half.data());
        for (int first = 0; first < m.rows; first += R) {
            alignas(16) float acc[R];
            constexpr int V = R / 4;
            float32x4_t sums[V];
            for (int v = 0; v < V; ++v) sums[v] = vdupq_n_f32(0.0f);
            for (int p = 0; p < n_parts; ++p) {
                const float* x = parts[p];
                for (int c = 0; c < part_len; ++c, h += R) {
                    float32x4_t xc = vdupq_n_f32(x[c]);
                    for (int v = 0; v < V; ++v)
                        sums[v] = vaddq_f32(sums[v], vmulq_f32(vcvt_f32_f16(vld1_f16(h + 4 * v)), xc));
                }
            }
            for (int v = 0; v < V; ++v) vst1q_f32(acc + 4 * v, sums[v]);
            int n = std::min(R, m.rows - first);
            for (int r = 0; r < n; ++r) y[first + r] = acc[r] + m.bias[first + r];
        }
        return;
    }
#endif
    const float* w = m.w.data();
    for (int first = 0; first < m.rows; first += R) {
        alignas(16) float acc[R];
#if defined(G2M_TILE_SSE2)
        constexpr int V = R / 4;
        __m128 sums[V];
        for (int v = 0; v < V; ++v) sums[v] = _mm_setzero_ps();
        for (int p = 0; p < n_parts; ++p) {
            const float* x = parts[p];
            for (int c = 0; c < part_len; ++c, w += R) {
                __m128 xc = _mm_set1_ps(x[c]);
                for (int v = 0; v < V; ++v) sums[v] = _mm_add_ps(sums[v], _mm_mul_ps(_mm_loadu_ps(w + 4 * v), xc));
            }
        }
        for (int v = 0; v < V; ++v) _mm_store_ps(acc + 4 * v, sums[v]);
#elif defined(G2M_TILE_NEON)
        constexpr int V = R / 4;
        float32x4_t sums[V];
        for (int v = 0; v < V; ++v) sums[v] = vdupq_n_f32(0.0f);
        for (int p = 0; p < n_parts; ++p) {
            const float* x = parts[p];
            for (int c = 0; c < part_len; ++c, w += R) {
                float32x4_t xc = vdupq_n_f32(x[c]);
                for (int v = 0; v < V; ++v) sums[v] = vaddq_f32(sums[v], vmulq_f32(vld1q_f32(w + 4 * v), xc));
            }
        }
        for (int v = 0; v < V; ++v) vst1q_f32(acc + 4 * v, sums[v]);
#else
        for (int r = 0; r < R; ++r) acc[r] = 0.0f;
        for (int p = 0; p < n_parts; ++p) {
            const float* x = parts[p];
            for (int c = 0; c < part_len; ++c, w += R)
                for (int r = 0; r < R; ++r) acc[r] += w[r] * x[c];
        }
#endif
        int n = std::min(R, m.rows - first);
        for (int r = 0; r < n; ++r) y[first + r] = acc[r] + m.bias[first + r];
    }
}

void apply(const Network::Tiled& m, const float* x, float* y) {
    apply(m, &x, 1, m.cols, y);
}

}  // namespace

Network::Network(std::shared_ptr<const Model> model) : model_(std::move(model)) {
    const ModelConfig& c = model_->config;
    channels_ = c.channels;
    kernel_ = c.kernel_size;
    gru_ = c.gru;
    n_pitches_ = c.n_pitches;

    int in_channels = 1;
    int columns = c.hop;
    std::size_t parts = static_cast<std::size_t>(kernel_);
    for (std::size_t i = 0; i < c.frontend.size(); ++i) {
        const auto& spec = c.frontend[i];
        FrontendStage s;
        std::string prefix = "frontend." + std::to_string(i);
        // [out, in, kernel] row-major: the columns are (in channel, tap) already
        s.weight = tile_rows(model_->tensor(prefix + ".weight"), spec.channels, in_channels * spec.kernel,
                             model_->tensor(prefix + ".bias"));
        s.in_channels = in_channels;
        s.out_channels = spec.channels;
        s.kernel = spec.kernel;
        s.stride = spec.stride;
        s.history = spec.kernel - spec.stride;
        s.columns = columns;
        s.input.resize(static_cast<std::size_t>(in_channels) * (s.history + columns));
        s.output.resize(static_cast<std::size_t>(spec.channels) * (columns / spec.stride));
        parts = std::max(parts, static_cast<std::size_t>(in_channels));
        column_.resize(std::max(column_.size(), static_cast<std::size_t>(spec.channels)));
        frontend_.push_back(std::move(s));
        in_channels = spec.channels;
        columns /= spec.stride;
    }
    rechannel_ = tile_rows(model_->tensor("rechannel.weight"), channels_, in_channels, model_->tensor("rechannel.bias"));

    for (std::size_t l = 0; l < c.dilations.size(); ++l) {
        GatedLayer layer;
        std::string i = std::to_string(l);
        int k = c.projections[l];
        layer.width = k > 0 ? k : channels_;
        layer.proj_bias = k > 0 ? model_->tensor("proj." + i + ".bias") : nullptr;
        if (k > 0) layer.proj = tile_rows(model_->tensor("proj." + i + ".weight"), k, channels_, layer.proj_bias);
        // [2C, width, K] → columns (tap, value): the ring's columns are read straight
        const float* conv = model_->tensor("convs." + i + ".weight");
        int w = layer.width, kernel = kernel_;
        layer.conv = tile(2 * channels_, kernel_ * w, model_->tensor("convs." + i + ".bias"), [&](int r, int col) {
            return conv[(static_cast<std::size_t>(r) * w + col % w) * kernel + col / w];
        });
        layer.mix = tile_rows(model_->tensor("mixers." + i + ".weight"), channels_, channels_,
                              model_->tensor("mixers." + i + ".bias"));
        layer.dilation = c.dilations[l];
        layer.span = (kernel_ - 1) * layer.dilation + 1;
        layer.ring.resize(static_cast<std::size_t>(layer.span) * layer.width);
        layers_.push_back(std::move(layer));
    }
    gru_ih_ = tile_rows(model_->tensor("gru.weight_ih_l0"), 3 * gru_, channels_, model_->tensor("gru.bias_ih_l0"));
    gru_hh_ = tile_rows(model_->tensor("gru.weight_hh_l0"), 3 * gru_, gru_, model_->tensor("gru.bias_hh_l0"));
    head_ = tile_rows(model_->tensor("head.weight"), 3 * n_pitches_, channels_ + gru_, model_->tensor("head.bias"));
    for (const HeadSpec& h : c.heads)
        extra_heads_.push_back({model_->tensor("extra_heads." + h.name + ".weight"),
                                model_->tensor("extra_heads." + h.name + ".bias"), h.classes, h.curve, h.min, h.max});

    features_.resize(channels_ + gru_);
    parts_.resize(parts);
    gates_.resize(2 * channels_);
    gated_.resize(channels_);
    gru_input_.resize(3 * gru_);
    gru_hidden_.resize(3 * gru_);
    outputs_.resize(3 * n_pitches_);
#if defined(G2M_TILE_F16)
    if (model_->float16_weights) {
        for (FrontendStage& s : frontend_) to_half(s.weight);
        to_half(rechannel_);
        for (GatedLayer& layer : layers_) {
            if (layer.proj.rows > 0) to_half(layer.proj);
            to_half(layer.conv);
            to_half(layer.mix);
        }
        to_half(gru_ih_);
        to_half(gru_hh_);
        to_half(head_);
    }
#endif
    reset();
}

bool Network::float16_supported() {
#if defined(G2M_TILE_F16)
    return true;
#else
    return false;
#endif
}

double Network::since_last_stage() {
    auto now = std::chrono::steady_clock::now();
    double us = std::chrono::duration<double, std::micro>(now - lap_).count();
    lap_ = now;
    return us;
}

void Network::reset() {
    for (FrontendStage& s : frontend_) std::fill(s.input.begin(), s.input.end(), 0.0f);
    for (GatedLayer& layer : layers_) {
        // as if silent before: a projected layer holds the projection of x = 0, its bias
        for (int f = 0; f < layer.span; ++f) {
            float* column = layer.ring.data() + static_cast<std::size_t>(f) * layer.width;
            if (layer.proj_bias) std::copy(layer.proj_bias, layer.proj_bias + layer.width, column);
            else std::fill(column, column + layer.width, 0.0f);
        }
        layer.position = 0;
    }
    std::fill(features_.begin(), features_.end(), 0.0f);  // includes the GRU's hidden state
    std::fill(outputs_.begin(), outputs_.end(), 0.0f);
}

const float* Network::step(const float* block) {
    if (times_) lap_ = std::chrono::steady_clock::now();

    // Front end: X = [history | x] per input channel, strided conv + ReLU,
    // then the last `history` columns of X become the next block's history.
    // Output column j reads each input channel's `kernel` values from j * stride.
    const float* x = block;
    for (FrontendStage& s : frontend_) {
        int width = s.history + s.columns;
        for (int c = 0; c < s.in_channels; ++c)
            std::copy(x + c * s.columns, x + (c + 1) * s.columns, s.input.data() + c * width + s.history);
        int out_columns = s.columns / s.stride;
        float* column = column_.data();
        for (int j = 0; j < out_columns; ++j) {
            for (int c = 0; c < s.in_channels; ++c) parts_[c] = s.input.data() + c * width + j * s.stride;
            apply(s.weight, parts_.data(), s.in_channels, s.kernel, column);
            for (int o = 0; o < s.out_channels; ++o) s.output[o * out_columns + j] = std::max(column[o], 0.0f);
        }
        for (int c = 0; c < s.in_channels; ++c) {
            float* row = s.input.data() + c * width;
            std::copy(row + s.columns, row + width, row);
        }
        x = s.output.data();
    }

    // Rechannel: the last stage leaves one column, the frame's features
    float* features = features_.data();
    apply(rechannel_, x, features);
    lap(&StageTimes::frontend);

    // Gated dilated layers. Each ring holds the layer's last `span` inputs,
    // projected first if the layer is (format version 5); tap t (oldest
    // first) is (K - 1 - t) * dilation frames back, read in place.
    for (GatedLayer& layer : layers_) {
        int w = layer.width;
        float* slot = layer.ring.data() + static_cast<std::size_t>(layer.position) * w;
        if (layer.proj_bias) apply(layer.proj, features, slot);
        else std::copy(features, features + channels_, slot);
        for (int t = 0; t < kernel_; ++t) {
            int frame = layer.position - (kernel_ - 1 - t) * layer.dilation;
            if (frame < 0) frame += layer.span;
            parts_[t] = layer.ring.data() + static_cast<std::size_t>(frame) * w;
        }
        layer.position = layer.position + 1 == layer.span ? 0 : layer.position + 1;

        apply(layer.conv, parts_.data(), kernel_, w, gates_.data());
        for (int c = 0; c < channels_; ++c)
            gated_[c] = std::tanh(gates_[c]) * sigmoid(gates_[channels_ + c]);
        apply(layer.mix, gated_.data(), gates_.data());
        for (int o = 0; o < channels_; ++o) features[o] += gates_[o];
    }
    lap(&StageTimes::layers);

    // GRU (gates r, u, n in that order), the hidden state after the features
    float* h = features + channels_;
    apply(gru_ih_, features, gru_input_.data());
    apply(gru_hh_, h, gru_hidden_.data());
    const float* gi = gru_input_.data();
    const float* gh = gru_hidden_.data();
    for (int k = 0; k < gru_; ++k) {
        float r = sigmoid(gi[k] + gh[k]);
        float u = sigmoid(gi[gru_ + k] + gh[gru_ + k]);
        float n = std::tanh(gi[2 * gru_ + k] + r * gh[2 * gru_ + k]);
        h[k] = (1.0f - u) * n + u * h[k];
    }
    lap(&StageTimes::gru);

    // Head on [features ; h]
    apply(head_, features, outputs_.data());
    for (float& y : outputs_) y = sigmoid(y);
    lap(&StageTimes::head);
    return outputs_.data();
}

// One head for pitch index `p`: its rows on the features, then a softmax
// (class head) or a clamp (curve)
void Network::head(const ExtraHead& h, int p, float* out) const {
    int inputs = channels_ + gru_;
    std::size_t first = static_cast<std::size_t>(p) * h.classes;  // this pitch's rows
    matvec(h.weight + first * inputs, features_.data(), h.bias + first, h.classes, inputs, out);
    if (h.curve) {
        out[0] = std::min(std::max(out[0], h.min), h.max);
        return;
    }
    float top = *std::max_element(out, out + h.classes);
    float sum = 0;
    for (int k = 0; k < h.classes; ++k) sum += out[k] = std::exp(out[k] - top);
    for (int k = 0; k < h.classes; ++k) out[k] /= sum;
}

void Network::heads(int p, float* out) const {
    for (const ExtraHead& h : extra_heads_) {
        if (h.curve) continue;
        head(h, p, out);
        out += h.classes;
    }
}

void Network::curves(int p, float* out) const {
    for (const ExtraHead& h : extra_heads_)
        if (h.curve) head(h, p, out++);
}

void Network::all_heads(int p, float* out) const {
    for (const ExtraHead& h : extra_heads_) {
        head(h, p, out);
        out += h.classes;
    }
}

}  // namespace g2m
