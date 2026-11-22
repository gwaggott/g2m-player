// The note network, one block of `hop` samples at a time
// (the format spec's "State" and "One block").
#pragma once

#include <g2m/model.h>
#include <g2m/transcriber.h>

#include <chrono>
#include <cstdint>

#include <memory>
#include <vector>

namespace g2m {

class Network {
public:
    // Allocates all state and scratch buffers; step() then allocates nothing.
    explicit Network(std::shared_ptr<const Model> model);

    // Zero state: as if the input had been silent before.
    void reset();

    // `hop` samples → n_pitches × 3 outputs: onset probability, frame
    // probability and velocity (0-1) per pitch. The pointer stays valid
    // until the next step().
    const float* step(const float* block);

    // The last step's outputs (zeros before the first, and after reset()).
    const float* outputs() const { return outputs_.data(); }

    // Whether the tiled weights are held as float16 (a version 6 model on
    // AArch64 NEON), widened in the kernel; elsewhere they're float32. The
    // results are the same either way: the values and the sums are.
    bool float16_tiles() const { return rechannel_.w.empty(); }
    static bool float16_supported();

    // The class heads (format version 3) for pitch index `p` (0-based) from
    // the last step's features: each head's softmax, in file order,
    // config.head_classes values to `out`. Computed on demand, as the
    // decoder needs them only at a note's start; no allocation.
    void heads(int p, float* out) const;

    // The curve heads (version 4) for pitch index `p`: each head's value,
    // clamped to its range, config.curve_heads values to `out`.
    void curves(int p, float* out) const;

    // Every head for pitch index `p` in file order, class probabilities or
    // curve value (head_classes + curve_heads values): the reference's
    // --outputs layout, for tools and tests.
    void all_heads(int p, float* out) const;

    const Model& model() const { return *model_; }

    // Adds each step's time per stage to `times` (null: off).
    void time_stages(StageTimes* times) { times_ = times; }
    // Microseconds since the last stage ended (or this was last called),
    // restarting the count: for timing the decoder after step().
    double since_last_stage();

    // A weight matrix laid out for the step: rows in tiles of kTile, each
    // tile's columns one after another with the tile's kTile weights per
    // column, padded with zero rows; columns in the order the step reads
    // its inputs. Each row sums its columns in that order, one running sum
    // per row, kTile rows at once: SIMD across rows, no reduction at the end.
    static constexpr int kTile = 32;
    struct Tiled {
        int rows = 0, cols = 0;
        std::vector<float> w;  // [tiles][cols][kTile]
        std::vector<std::uint16_t> half;  // or the same as float16 bits (then w is empty)
        const float* bias = nullptr;  // [rows], in the model
    };

private:
    struct FrontendStage {
        Tiled weight;  // rows out, columns (in channel, tap)
        int in_channels, out_channels, kernel, stride;
        int history;  // kernel - stride columns kept from the last block
        int columns;  // new input columns per block
        std::vector<float> input;  // [in_channels][history + columns]
        std::vector<float> output;  // [out_channels][columns / stride]
    };

    struct GatedLayer {
        Tiled proj;  // [k, C], or no rows: not projected (format version 5)
        const float* proj_bias;  // [k], or null
        int width;  // values per frame the conv reads: k, or C
        Tiled conv;  // 2C rows, columns (tap, oldest first; value)
        Tiled mix;  // [C, C]
        int dilation;
        int span;  // frames the ring holds: (K - 1) * dilation + 1
        int position = 0;  // where the current frame goes
        std::vector<float> ring;  // [span][width], this layer's inputs (projected)
    };

    std::shared_ptr<const Model> model_;
    StageTimes* times_ = nullptr;
    std::chrono::steady_clock::time_point lap_;
    void lap(double StageTimes::*stage) {
        if (times_) times_->*stage += since_last_stage();
    }
    int channels_, kernel_, gru_, n_pitches_;
    std::vector<FrontendStage> frontend_;
    Tiled rechannel_;
    std::vector<GatedLayer> layers_;
    Tiled gru_ih_, gru_hh_, head_;
    struct ExtraHead {
        const float* weight;  // [classes * P, C + H]
        const float* bias;
        int classes;  // 1 for a curve
        bool curve;
        float min, max;  // a curve's range
    };
    void head(const ExtraHead& h, int p, float* out) const;
    std::vector<ExtraHead> extra_heads_;

    std::vector<float> features_;  // [C + H]: the layers' output, then the GRU's hidden state
    std::vector<const float*> parts_;  // the inputs of one tiled matvec, in column order
    std::vector<float> column_;  // a front-end stage's output column
    std::vector<float> gates_;  // [2C]
    std::vector<float> gated_;  // [C]
    std::vector<float> gru_input_;  // [3H]
    std::vector<float> gru_hidden_;  // [3H]
    std::vector<float> outputs_;  // [3P]
};

}  // namespace g2m
