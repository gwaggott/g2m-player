// What becomes of the model's pitch bends (docs/release-plan.md, "Bend
// modes"), after the sustain pedal:
//
// - natural: passed on (MPE carries them; plain MIDI drops them).
// - retrigger: the bend is rounded to whole semitones (steps), with
//   kBendStepHysteresis either side so vibrato doesn't flicker between
//   them; when the step changes, the note ends and a note at the new
//   pitch starts with the first one's velocity and no bend, on the same
//   channel (the sink's retrigger); works in plain MIDI too. The pitch is
//   clamped to 0-127; a step onto a pitch another note is sounding waits.
// - off: no bends; notes keep their detected pitch.
//
// In retrigger and off, a note-on that carries a bend carries none (8192),
// so an MPE channel starts clean. Each note keeps the mode it started in. Without a
// pitch-offset head there are no bends, so every mode passes notes as they are.
// No allocation, locks or exceptions.
#pragma once

#include <g2m/transcriber.h>

#include <cstdint>

namespace g2m {

enum class BendMode { natural, retrigger, off };

constexpr float kBendStepHysteresis = 0.2f;  // semitones past the half-way point

// Notes and bends, plus retriggers
class NoteSink : public EventSink {
public:
    // Note `from` ends and `to` starts with `velocity`, on the same channel.
    virtual void retrigger(std::uint64_t sample, int from, int to, int velocity) = 0;

protected:
    ~NoteSink() = default;
};

// The step a bend of `semitones` gives, from step `step`: unchanged until it
// is more than half a semitone plus the hysteresis away.
int bend_step(int step, float semitones);

class BendModeFilter {
public:
    // For notes started from now.
    void set_mode(BendMode mode) { mode_ = mode; }
    BendMode mode() const { return mode_; }

    void note(const NoteEvent& event, NoteSink& out);
    void bend(const BendEvent& event, NoteSink& out);

    // Forgets the notes (the caller has ended them).
    void reset();

    // The pitch a detected note sounds at, or -1 if it isn't sounding
    int sounding_pitch(int pitch) const { return notes_[pitch].on ? notes_[pitch].out : -1; }

private:
    struct Note {
        bool on = false;
        BendMode mode = BendMode::natural;
        int step = 0;
        int out = 0;  // the pitch sent
        int velocity = 0;
    };

    void end(int pitch, std::uint64_t sample, NoteSink& out);

    BendMode mode_ = BendMode::natural;
    Note notes_[128];  // by detected pitch
    int owner_[128] = {};  // by pitch sent: the detected pitch + 1, 0 for none
};

}  // namespace g2m
