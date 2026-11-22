#include <g2m/transcriber.h>

#include <algorithm>
#include <chrono>
#include <vector>

#include "decoder.h"
#include "network.h"

namespace g2m {

struct Transcriber::Impl {
    explicit Impl(std::shared_ptr<const Model> model)
        : network(model),
          decoder(model->config.n_pitches, model->config.pitch_min, model->decoder, model->config.head_classes,
                  model->config.pitch_offset_curve >= 0),
          curves(static_cast<std::size_t>(model->config.curve_heads)),
          hop(static_cast<std::size_t>(model->config.hop)),
          block(hop) {}

    // One complete block, ending at `position`.
    void run(const float* samples, EventSink& sink) {
        const float* outputs = network.step(samples);
        const ModelConfig& c = network.model().config;
        decoder.step(
            outputs, [&](int p, float* out) { network.heads(p, out); },
            [&](int p) {
                network.curves(p, curves.data());
                return curves[c.pitch_offset_curve];
            },
            [&](int pitch, int velocity, const float* heads, const BendValue* bend) {
                NoteEvent e{position, pitch, velocity};
                if (heads && c.string_head >= 0) {
                    e.has_strings = true;
                    std::copy(heads + c.string_head, heads + c.string_head + 6, e.strings);
                }
                if (heads && c.onset_type_head >= 0) e.onset_type = onset_type(heads + c.onset_type_head, decoder.settings());
                if (bend) {
                    e.has_bend = true;
                    e.bend = bend->value;
                    e.semitones = bend->semitones;
                }
                sink.note(e);
            },
            [&](int pitch, const BendValue& bend) { sink.bend({position, pitch, bend.value, bend.semitones}); });
        if (times) times->decoder += network.since_last_stage();
    }

    Network network;
    Decoder decoder;
    std::vector<float> curves;  // scratch: one pitch's curve heads
    std::size_t hop;
    std::vector<float> block;  // a part block carried between calls
    std::size_t filled = 0;
    std::uint64_t position = 0;
    StageTimes* times = nullptr;
};

Transcriber::Transcriber(std::shared_ptr<const Model> model) : impl_(std::make_unique<Impl>(std::move(model))) {}

Transcriber::~Transcriber() = default;

void Transcriber::process(const float* audio, std::size_t n, EventSink& sink) {
    Impl& t = *impl_;
    while (n > 0) {
        if (t.filled == 0 && n >= t.hop) {  // a whole block straight from the input
            t.position += t.hop;
            t.run(audio, sink);
            audio += t.hop;
            n -= t.hop;
            continue;
        }
        std::size_t take = std::min(t.hop - t.filled, n);
        std::copy(audio, audio + take, t.block.begin() + t.filled);
        t.filled += take;
        t.position += take;
        audio += take;
        n -= take;
        if (t.filled == t.hop) {
            t.filled = 0;
            t.run(t.block.data(), sink);
        }
    }
}

void Transcriber::all_notes_off(EventSink& sink) {
    Impl& t = *impl_;
    t.decoder.flush([&](int pitch, int velocity) { sink.note({t.position, pitch, velocity}); });
}

void Transcriber::reset(EventSink& sink) {
    all_notes_off(sink);
    impl_->network.reset();
    impl_->decoder.reset();
    impl_->filled = 0;
}

std::uint64_t Transcriber::position() const {
    return impl_->position;
}

const float* Transcriber::last_outputs() const {
    return impl_->network.outputs();
}

void Transcriber::last_heads(float* out) const {
    const ModelConfig& c = impl_->network.model().config;
    std::size_t width = static_cast<std::size_t>(c.head_classes + c.curve_heads);
    for (int p = 0; p < c.n_pitches; ++p) impl_->network.all_heads(p, out + p * width);
}

const Model& Transcriber::model() const {
    return impl_->network.model();
}

const DecoderSettings& Transcriber::decoder_settings() const {
    return impl_->decoder.settings();
}

void Transcriber::set_decoder_settings(const DecoderSettings& settings) {
    impl_->decoder.set_settings(settings);
}

void Transcriber::time_stages(StageTimes* times) {
    impl_->times = times;
    impl_->network.time_stages(times);
}

}  // namespace g2m
