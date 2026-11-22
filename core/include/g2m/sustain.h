// A sustain pedal between the Transcriber's events and the MIDI writers
// (docs/release-plan.md, "Sustain"). Modes:
//
// - piano: while the pedal is down, note-offs are deferred; a deferred note
//   played again gets its note-off and then the new note-on; pedal up sends
//   the deferred note-offs.
// - sostenuto: only the notes sounding at pedal down are held (their
//   note-offs deferred) until pedal up; a held note played again gets its
//   note-off and then the new note-on, and is no longer held; later notes
//   play normally.
// - freeze: the notes sounding at pedal down are held, their bends stop,
//   and every new note-on is blocked; pedal up ends the held notes, and
//   notes the decoder started meanwhile are never sent (they would be late).
//
// Every event sent has the sample it was called with; pedal-up note-offs go
// lowest pitch first. In MPE output a held note keeps its string's channel,
// so the allocator gives new notes other strings, or steals the held note's
// string when it must, ending it first. No allocation, locks or exceptions.
#pragma once

#include <g2m/transcriber.h>

#include <cstdint>

namespace g2m {

enum class SustainMode { off, piano, sostenuto, freeze };

class Sustain {
public:
    // Changing mode releases what the old mode held, as pedal up would.
    void set_mode(SustainMode mode, std::uint64_t sample, EventSink& out);
    // Pedal down or up at `sample`; nothing if unchanged.
    void set_pedal(bool down, std::uint64_t sample, EventSink& out);

    // The Transcriber's events, passed on, held back or dropped.
    void note(const NoteEvent& event, EventSink& out);
    void bend(const BendEvent& event, EventSink& out);

    // Forgets the notes (the caller has ended them); keeps the mode and pedal.
    void reset();

    SustainMode mode() const { return mode_; }
    bool pedal() const { return pedal_; }
    // A note sent and not yet ended
    bool sounding(int pitch) const { return sent_[pitch]; }
    // Sounding only because of the pedal
    bool held(int pitch) const { return held_[pitch]; }

private:
    void release(std::uint64_t sample, EventSink& out);
    void end(int pitch, std::uint64_t sample, EventSink& out);

    SustainMode mode_ = SustainMode::off;
    bool pedal_ = false;
    bool sent_[128] = {};  // note-on sent, note-off not yet
    bool latched_[128] = {};  // sostenuto, freeze: sounding at pedal down
    bool held_[128] = {};  // the note-off deferred
};

}  // namespace g2m
