// Frame outputs → note events, per pitch, one frame at a time
// (the format spec's "Decoder", format version 4: the late
// fallback, the MPE heads taken at each note-on, and each sounding note's
// pitch bend; reference.py's Decoder).
#pragma once

#include <g2m/model.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace g2m {

// Velocity output in 0-1 → MIDI velocity 1-127, rounding half to even as
// np.rint does (lrintf in the default rounding mode).
inline int midi_velocity(float v) {
    long r = std::lrintf(v * 127.0f);
    return r < 1 ? 1 : r > 127 ? 127 : static_cast<int>(r);
}

// Onset-type probabilities (picked, legato, slide) → the onset type: slide
// first, then legato
inline OnsetType onset_type(const float* p, const DecoderSettings& s) {
    if (p[2] >= s.slide_threshold) return OnsetType::slide;
    return p[1] >= s.legato_threshold ? OnsetType::legato : OnsetType::picked;
}

constexpr int kBendCentre = 8192, kBendMax = 16383;

// A pitch bend: the 14-bit value (8192 = none) and the offset it came from
// after the dead band, in semitones
struct BendValue {
    int value = kBendCentre;
    float semitones = 0;
};

// A pitch offset in semitones → its bend: 0 within the dead band, scaled to
// bend_range, rounded half to even, clamped to 14 bits. The division and
// the product in float, as the reference does them in float32.
inline BendValue bend_value(float offset, const DecoderSettings& s) {
    BendValue b;
    b.semitones = std::fabs(offset) < s.bend_dead_band ? 0.0f : offset;
    float scale = 8192.0f / static_cast<float>(s.bend_range);
    float product = b.semitones * scale;
    long r = std::lrintf(product);
    long v = kBendCentre + r;
    b.value = static_cast<int>(v < 0 ? 0 : v > kBendMax ? kBendMax : v);
    return b;
}

class Decoder {
public:
    // `head_classes`: the class heads' outputs per pitch (0: none);
    // `pitch_offset`: the model has a pitch-offset head (pitch bends).
    Decoder(int n_pitches, int pitch_min, const DecoderSettings& settings, int head_classes = 0,
            bool pitch_offset = false)
        : pitch_min_(pitch_min), settings_(settings), pitches_(n_pitches), bends_(pitch_offset), classes_(head_classes),
          wait_heads_(static_cast<std::size_t>(n_pitches) * head_classes),
          high_heads_(wait_heads_.size()), start_heads_(wait_heads_.size()), current_(head_classes) {}

    void reset() {
        for (Pitch& p : pitches_) p = Pitch();
        std::fill(wait_heads_.begin(), wait_heads_.end(), 0.0f);
        std::fill(high_heads_.begin(), high_heads_.end(), 0.0f);
    }

    const DecoderSettings& settings() const { return settings_; }
    void set_settings(const DecoderSettings& settings) { settings_ = settings; }

    // One frame of outputs ([n_pitches][3]: onset, frame, velocity). Calls
    // emit(pitch, velocity) for each event: note-offs (velocity 0) first,
    // then note-ons, each lowest pitch first.
    template <class Emit>
    void step(const float* outputs, Emit&& emit) {
        step(outputs, [](int, float*) {}, [&](int pitch, int velocity, const float*) { emit(pitch, velocity); });
    }

    // The same, with the MPE heads: heads(i, out) writes pitch index i's
    // head outputs for this frame (head_classes values), called only in the
    // frames that need them (an onset edge, the first frame at or above
    // the fallback threshold, a note's start). emit(pitch, velocity, heads)
    // gets each note-on's heads, from the frame its velocity comes from
    // (null with no heads, and for note-offs); valid during the call.
    template <class Heads, class Emit>
    void step(const float* outputs, Heads&& heads, Emit&& emit) {
        step(
            outputs, heads, [](int) { return 0.0f; },
            [&](int pitch, int velocity, const float* h, const BendValue*) { emit(pitch, velocity, h); },
            [](int, const BendValue&) {});
    }

    // The same, with pitch bends (a pitch-offset head): offset(i) returns
    // pitch index i's pitch offset this frame, asked for every note that
    // starts or sounds on. emit(pitch, velocity, heads, bend) also gets a
    // note-on's bend (null without the head, and for note-offs), and
    // bend(pitch, bend) is called, after the note-ons, lowest pitch first,
    // for each note held from an earlier frame whose value has changed.
    template <class Heads, class Offset, class Emit, class Bend>
    void step(const float* outputs, Heads&& heads, Offset&& offset, Emit&& emit, Bend&& bend) {
        const DecoderSettings& s = settings_;
        std::size_t k = static_cast<std::size_t>(classes_);
        for (std::size_t i = 0; i < pitches_.size(); ++i) {
            Pitch& p = pitches_[i];
            float o = outputs[3 * i], f = outputs[3 * i + 1], v = outputs[3 * i + 2];
            bool computed = false;
            auto current = [&]() -> const float* {  // this frame's heads, computed once
                if (!computed) heads(static_cast<int>(i), current_.data());
                computed = true;
                return current_.data();
            };
            auto keep = [&](std::vector<float>& to, const float* from) {
                std::copy(from, from + k, to.begin() + i * k);
            };

            bool above = o >= s.onset_threshold;
            bool edge = above && !p.onset_above;
            p.onset_above = above;
            bool active = f >= s.frame_threshold;
            p.age += 1;
            p.wait_age += 1;
            p.low = active ? 0 : p.low + 1;
            p.high = f >= s.fallback_threshold ? p.high + 1 : 0;
            if (p.high == 1) p.high_velocity = v;
            if (k && p.high == 1 && s.fallback_frames > 0) keep(high_heads_, current());
            bool fell = p.risen && p.low >= s.off_frames;
            bool never_rose = !p.risen && !active && p.age >= s.frame_wait;

            bool accepted = edge && !(p.note_on && active && o < s.retrigger_threshold);
            bool at_once = accepted && (s.gate_frames == 0 || active || o >= s.strong_onset);
            bool waited = p.waiting && !edge;
            bool confirmed = waited && active;
            bool expired = waited && !active && p.wait_age >= s.gate_frames;
            bool start = at_once || confirmed;
            p.start_velocity = confirmed ? p.wait_velocity : v;
            if (k && start) keep(start_heads_, confirmed ? wait_heads_.data() + i * k : current());
            p.waiting = (waited && !confirmed && !expired) || (accepted && !at_once);
            if (edge) {
                p.wait_age = 0;
                p.wait_velocity = v;
                if (k) keep(wait_heads_, current());
            }

            p.ends = p.note_on && (start || fell || never_rose);
            // late fallback: a pitch with no note (and no gated onset
            // waiting) whose frame probability has stayed high
            bool late = s.fallback_frames > 0 && p.high == s.fallback_frames &&
                        !((p.note_on && !p.ends) || start || p.waiting);
            if (late) {
                start = true;
                p.start_velocity = p.high_velocity;
                if (k) keep(start_heads_, high_heads_.data() + i * k);
            }
            p.starts = start;
            // pitch bend: this frame's value for a note starting or held
            // (held: on before, not ending, so not starting either)
            bool held = p.note_on && !p.ends;
            p.bend_changed = false;
            if (bends_ && (start || held)) {
                p.bend = bend_value(offset(static_cast<int>(i)), s);
                p.bend_changed = held && p.bend.value != p.bend_value;
                if (start || p.bend_changed) p.bend_value = p.bend.value;
            }
            p.risen = start ? active : (p.risen || active);
            if (start) p.age = 0;
            p.note_on = (p.note_on && !p.ends) || start;
        }
        for (std::size_t i = 0; i < pitches_.size(); ++i)
            if (pitches_[i].ends) emit(pitch_min_ + static_cast<int>(i), 0, nullptr, nullptr);
        for (std::size_t i = 0; i < pitches_.size(); ++i)
            if (pitches_[i].starts)
                emit(pitch_min_ + static_cast<int>(i), midi_velocity(pitches_[i].start_velocity),
                     k ? start_heads_.data() + i * k : nullptr, bends_ ? &pitches_[i].bend : nullptr);
        for (std::size_t i = 0; i < pitches_.size(); ++i)
            if (pitches_[i].bend_changed) bend(pitch_min_ + static_cast<int>(i), pitches_[i].bend);
    }

    // Note-offs for every note still on, lowest pitch first.
    template <class Emit>
    void flush(Emit&& emit) {
        for (std::size_t i = 0; i < pitches_.size(); ++i) {
            if (pitches_[i].note_on) {
                pitches_[i].note_on = false;
                emit(pitch_min_ + static_cast<int>(i), 0);
            }
        }
    }

private:
    struct Pitch {
        bool onset_above = false;
        bool note_on = false;
        bool risen = false;  // frame probability reached the threshold since the note started
        bool waiting = false;  // a gated onset waiting for the frame probability
        std::int64_t low = 0;  // frames in a row below the frame threshold
        std::int64_t age = 0;  // frames since the note started
        std::int64_t wait_age = 0;
        float wait_velocity = 0;
        std::int64_t high = 0;  // frames in a row at or above the fallback threshold
        float high_velocity = 0;  // velocity output in the first of them
        int bend_value = kBendCentre;  // the note's last pitch-bend value, carried or sent
        // this frame's results, emitted after all pitches are decided
        bool ends = false;
        bool starts = false;
        float start_velocity = 0;
        BendValue bend;  // this frame's, for a note starting or held
        bool bend_changed = false;  // a bend event
    };

    int pitch_min_;
    DecoderSettings settings_;
    std::vector<Pitch> pitches_;
    bool bends_;
    // per pitch, head_classes each: the heads at a waiting onset's edge, at
    // the first high frame, and this frame's note-on's
    int classes_;
    std::vector<float> wait_heads_, high_heads_, start_heads_;
    std::vector<float> current_;  // scratch: this frame's heads for one pitch
};

}  // namespace g2m
