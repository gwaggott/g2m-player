// A note model loaded from a .g2m file (the format spec, versions 1-6).
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace g2m {

// Format versions this library reads: version 1 is read as version 2 with
// the late fallback off, versions 1-2 as version 3 with no MPE heads, and
// versions 1-3 as version 4 with no curve heads (no pitch bend),
// versions 1-4 as version 5 with no projected gated layers, and versions
// 1-5 as version 6 with float32 weights (version 6 stores them as float16).
constexpr int kFormatVersion = 6;
constexpr int kMinFormatVersion = 1;

// How a note started, from the onset-type head (format version 3): picked,
// legato (a hammer-on or pull-off on a string still sounding) or slide (a
// legato slide's arrival at a new pitch). Without the head, picked.
enum class OnsetType { picked, legato, slide };

// An MPE head: per pitch per frame, either a softmax over its classes (a
// class head, format version 3: "string", 6 classes, strings 1-6, and
// "onset_type", 3: picked, legato, slide) or one value clamped to
// [min, max] (a curve head, version 4: "pitch_offset", in semitones).
struct HeadSpec {
    std::string name;
    int classes = 0;  // 1 for a curve
    bool curve = false;
    float min = 0, max = 0;  // a curve's range
};

struct FrontendStage {
    int channels;
    int kernel;
    int stride;
};

struct ModelConfig {
    int sample_rate = 0;
    int hop = 0;  // samples per block; one frame of outputs per block
    int pitch_min = 0;
    int pitch_max = 0;
    int n_pitches = 0;
    std::vector<FrontendStage> frontend;
    int channels = 0;
    int kernel_size = 0;
    std::vector<int> dilations;
    // Per gated layer, the values its input is projected on before its conv
    // (format version 5), or 0: not projected (every layer before version 5)
    std::vector<int> projections;
    int gru = 0;
    std::vector<HeadSpec> heads;  // MPE heads, in file order (none before version 3)
    int head_classes = 0;  // the class heads' classes added up: per pitch, their outputs one after another
    int string_head = -1;  // where each class head's classes start within those, or -1 if the model lacks it
    int onset_type_head = -1;
    int curve_heads = 0;  // curve heads (version 4): one value each per pitch, in file order
    int pitch_offset_curve = -1;  // the pitch-offset head among them, or -1
};

// Thresholds are float: the reference compares float32 probabilities with
// them in float32.
struct DecoderSettings {
    float onset_threshold = 0.5f;
    float frame_threshold = 0.5f;
    int frame_wait = 30;
    int off_frames = 15;
    int gate_frames = 0;
    float strong_onset = 1.0f;
    float retrigger_threshold = 0.0f;
    // Late fallback (format version 2): a pitch with no note on and no gated
    // onset waiting starts one when its frame probability has been at or
    // above fallback_threshold (>= frame_threshold) for fallback_frames
    // frames in a row. 0 frames: off.
    int fallback_frames = 0;
    float fallback_threshold = 0.5f;
    // Onset type (format version 3, with an onset_type head): slide if its
    // probability >= slide_threshold, else legato if >= legato_threshold,
    // else picked.
    float legato_threshold = 0.5f;
    float slide_threshold = 0.5f;
    // Pitch bend (format version 4, with a pitch_offset head): offsets
    // below bend_dead_band semitones count as none; bend_range (1-96
    // semitones) is the range the 14-bit bends are scaled to, as sent in
    // RPN 0 (a player may override it, e.g. for a synth fixed at 48).
    float bend_dead_band = 0.25f;
    int bend_range = 12;
};

struct Tensor {
    std::string name;
    std::vector<int> shape;  // row-major
    std::size_t offset = 0;  // in values from the start of the weights
    std::size_t size = 0;
};

struct Model {
    int format_version = 0;
    ModelConfig config;
    DecoderSettings decoder;  // the defaults the model was tuned with
    double label_delay_ms = 0;
    std::vector<Tensor> tensors;  // in file order
    std::vector<float> weights;  // float32, widened from float16 in version 6
    bool float16_weights = false;  // version 6: every weight is exactly a float16 value

    // A tensor's data, or nullptr if the model has none of that name.
    const float* tensor(const std::string& name) const;
};

// An IEEE 754 binary16 value (its bits) as float32: exact, subnormals,
// infinities and NaN included.
float half_to_float(std::uint16_t bits);

struct LoadResult {
    std::unique_ptr<Model> model;  // null on failure
    std::string error;
};

// Reads and checks a model file. Never throws; on failure, `error` says why.
// Allocates: call it off the audio thread.
LoadResult load_model(const std::string& path);
LoadResult load_model(const std::uint8_t* data, std::size_t size);

}  // namespace g2m
