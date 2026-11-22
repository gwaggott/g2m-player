// The streaming API: audio in, in blocks of any size; note events out.
#pragma once

#include <g2m/model.h>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace g2m {

// Time spent in each stage of the blocks run, summed, in microseconds (for
// g2m-bench --stages). The front end includes the rechannel; the decoder
// includes the MPE heads it asks for.
struct StageTimes {
    double frontend = 0, layers = 0, gru = 0, head = 0, decoder = 0;
};

struct NoteEvent {
    // The absolute sample at which the event's frame ends, counted from the
    // first sample given to the Transcriber: (k + 1) * hop for frame k. The
    // event belongs on the sample before it, the last of its block.
    std::uint64_t sample;
    int pitch;  // MIDI note
    int velocity;  // 1-127 for a note-on, 0 for a note-off
    // Note-ons of a model with MPE heads (format version 3): the onset type
    // (picked without an onset_type head), and the string probabilities,
    // string 1 first (has_strings false without a string head), from the
    // frame the velocity came from.
    OnsetType onset_type = OnsetType::picked;
    bool has_strings = false;
    float strings[6] = {};
    // Note-ons of a model with a pitch-offset head (format version 4): the
    // pitch bend this frame, 14-bit (8192 = none, scaled to the decoder's
    // bend_range), and the offset in semitones after the dead band.
    bool has_bend = false;
    int bend = 8192;
    float semitones = 0;
};

// A sounding note's pitch bend has changed (format version 4): sent after
// the frame's note-ons, for notes held from an earlier frame.
struct BendEvent {
    std::uint64_t sample;  // as NoteEvent
    int pitch;  // the note's
    int bend;  // 14-bit, 8192 = none
    float semitones;  // after the dead band
};

class EventSink {
public:
    virtual void note(const NoteEvent& event) = 0;
    // Pitch bends (format version 4); ignored unless overridden, as plain
    // MIDI carries none.
    virtual void bend(const BendEvent&) {}

protected:
    ~EventSink() = default;
};

class Transcriber {
public:
    // Allocates everything it needs; process() and the rest then allocate
    // nothing, take no locks and don't throw. The decoder starts with the
    // model's settings.
    explicit Transcriber(std::shared_ptr<const Model> model);
    ~Transcriber();
    Transcriber(const Transcriber&) = delete;
    Transcriber& operator=(const Transcriber&) = delete;

    // Takes `n` samples (at the model's sample rate, full scale ±1). Runs
    // the model on each block of `hop` samples this completes and sends
    // that block's events to `sink`, in order: per block, note-offs, then
    // note-ons, then bend events (version 4), each lowest pitch first.
    void process(const float* audio, std::size_t n, EventSink& sink);

    // Note-off for every note still on, at the current position.
    void all_notes_off(EventSink& sink);

    // all_notes_off, then silence: the model's history, the decoder's state
    // and any part block are cleared. The position keeps counting.
    void reset(EventSink& sink);

    // Samples taken so far.
    std::uint64_t position() const;

    // The model's outputs for the last complete block, [n_pitches][3]:
    // onset probability, frame probability, velocity 0-1 (zeros before the
    // first block). Valid until the next process() or reset().
    const float* last_outputs() const;

    // The MPE heads for every pitch for the last complete block,
    // [n_pitches][head_classes + curve_heads] to `out` (each head's softmax
    // or curve value, in file order, as reference.py --outputs). Computed
    // on demand, for tools and tests: the decoder computes them only where
    // it needs them. No allocation.
    void last_heads(float* out) const;

    const Model& model() const;
    const DecoderSettings& decoder_settings() const;
    // Takes effect from the next block; state is kept.
    void set_decoder_settings(const DecoderSettings& settings);

    // Adds each block's time per stage to `times` (null, the default:
    // nothing is timed). For measuring only: it reads the clock six times
    // a block.
    void time_stages(StageTimes* times);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace g2m
