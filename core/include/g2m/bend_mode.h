// What becomes of the model's pitch bends (docs/release-plan.md, "Bend
// modes"), after the sustain pedal:
//
// - natural: passed on (MPE carries them on each note's channel). In plain
//   output (set_plain) the channel's one pitch bend follows the lead, the
//   newest note sounding (docs/plain-bends.md): its bends are sent as
//   channel bends, other notes' are dropped, and when the lead ends the
//   newest note left takes over, its last bend sent at once if it differs.
// - retrigger: the bend is rounded to whole semitones (steps), with
//   kBendStepHysteresis either side so vibrato doesn't flicker between
//   them; when the step changes, the note ends and a note at the new
//   pitch starts with the first one's velocity and no bend, on the same
//   channel (the sink's retrigger); works in plain MIDI too. The pitch is
//   clamped to 0-127; a step onto a pitch another note is sounding waits.
// - off: no bends; notes keep their detected pitch.
//
// In retrigger and off, a note-on that carries a bend carries none (8192),
// so an MPE channel starts clean. In plain output every note-on makes its
// note the lead and first sets the channel bend to the note's own (8192 in
// retrigger and off) if it isn't already. Each note keeps the mode it started in. Without a
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
    // Plain output only: the channel's pitch bend, 14-bit.
    virtual void channel_bend(std::uint64_t sample, int value) = 0;

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
    // Plain output (one channel bend) or not (MPE: per-note bends). Call
    // reset() when it changes.
    void set_plain(bool plain) { plain_ = plain; }

    void note(const NoteEvent& event, NoteSink& out);
    void bend(const BendEvent& event, NoteSink& out);

    // Forgets the notes (the caller has ended them) and takes the channel
    // bend to be 8192 again (the caller has reset it, or uses a new channel).
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
        int bend = 8192;  // the last bend, 14-bit (plain natural: for the hand-over)
        std::uint32_t order = 0;  // when it started, for the lead
    };

    // `hand_over`: plain output passes the lead on if this note had it.
    void end(int pitch, std::uint64_t sample, NoteSink& out, bool hand_over);
    // Plain output: the channel bend to `value`, if it isn't already
    void set_channel(std::uint64_t sample, int value, NoteSink& out);
    // The bend a note wants on the channel: its own if natural, else none
    static int wanted(const Note& n) { return n.mode == BendMode::natural ? n.bend : 8192; }

    BendMode mode_ = BendMode::natural;
    Note notes_[128];  // by detected pitch
    int owner_[128] = {};  // by pitch sent: the detected pitch + 1, 0 for none
    bool plain_ = false;
    int channel_ = 8192;  // plain: the channel bend sent
    int lead_ = -1;  // plain: the lead's detected pitch, -1 for none
    std::uint32_t order_ = 0;  // note-ons so far
};

}  // namespace g2m
