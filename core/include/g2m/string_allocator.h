// Chooses a guitar string for each note at its note-on: in MPE output, the
// string is the note's member channel. The rule (docs/mpe-output.md):
//
// 0. a legato or slide note continues a sounding note on a string that can
//    play it, within kLegatoDistance / kSlideDistance semitones, which it
//    ends; else it is placed as a picked note:
// 1. a free string that can play the pitch and keeps the sounding notes
//    within the hand span;
// 2. else a free string that can play the pitch;
// 3. else any free string;
// 4. else the string of the oldest sounding note, which is ended.
//
// Within a step, the string with the highest weight wins (no weights: all
// equal), ties going to the lowest string (6 before 5 ... before 1). A note
// keeps its string until its note-off.
#pragma once

#include <g2m/model.h>

#include <cstdint>

namespace g2m {

constexpr int kStrings = 6;  // 1 = high E ... 6 = low E

// Open-string pitches in standard tuning, string 1 first
constexpr int kOpenPitch[kStrings] = {64, 59, 55, 50, 45, 40};
constexpr int kFrets = 24;  // highest fret on every string
constexpr int kDropPitch = 38;  // string 6 plays down to D2, as in drop D
// Fretted notes sounding together must have frets within this distance
// (index on fret n, little finger on n + 4)
constexpr int kHandSpan = 4;
// The farthest a legato note (hammer-on / pull-off) and a slide's arrival
// may be from the note they continue, in semitones: a legato reaches
// across the hand span (open string to fret 5), a slide as far as the
// pitch-bend range
constexpr int kLegatoDistance = 5;
constexpr int kSlideDistance = 12;

// Lowest and highest pitch string `string` (1-6) can play
int string_low(int string);
int string_high(int string);

// The fret the hand span is measured with: pitch - open pitch, and 0 (open)
// for the drop-D notes below E2 on string 6
int span_fret(int string, int pitch);

class StringAllocator {
public:
    enum class Step { legato, shape, range, free, steal };

    struct Assignment {
        int string;  // 1-6
        Step step;  // the rule's step that chose it
        // The note ended to make room, or -1: the stolen note (step 4), or
        // the same pitch if it was still sounding (a repeated note-on)
        int stolen_pitch;
        // Step legato: the note continued on the same string, now ended
        // (its note-off goes after the new note-on); else -1
        int continued_pitch = -1;
    };

    // `pitch` 0-127. `weights`, if given, holds a finite weight per string
    // (string 1 first), e.g. the model's string probabilities. `type`: how
    // the note started (legato and slide try step 0).
    // No allocation, locks or exceptions.
    Assignment note_on(int pitch, const float* weights = nullptr, OnsetType type = OnsetType::picked);

    // The string the note was on, or 0 if it isn't sounding (never started,
    // or stolen); the string is then free.
    int note_off(int pitch);

    // A sounding note moves to pitch `to` on its string (a retrigger): the
    // string, or 0 if `from` isn't sounding or `to` already is.
    int move(int from, int to);

    void clear();

    int string_of(int pitch) const;  // 0 if not sounding
    int pitch_on(int string) const;  // -1 if the string is free
    int sounding() const;  // notes sounding

private:
    bool fits_hand_span(int string, int pitch) const;
    int continued_string(int pitch, const float* weights, OnsetType type) const;
    int best(const bool* allowed, const float* weights) const;

    std::int8_t string_of_[128] = {};
    int pitch_on_[kStrings] = {-1, -1, -1, -1, -1, -1};
    std::uint64_t started_[kStrings] = {};  // note-on order, for stealing the oldest
    std::uint64_t count_ = 0;
};

}  // namespace g2m
