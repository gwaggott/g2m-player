// MIDI note number → name with octave, C4 = 60 (scientific pitch: the low E
// string is E2), sharps only: "E2", "C#4". No allocation, for audio threads.
#pragma once

namespace g2m {

constexpr int kNoteNameSize = 5;  // longest: "C#-1" and its terminator

// Writes the name of `pitch` (0-127) into `out`; returns its length.
inline int note_name(int pitch, char out[kNoteNameSize]) {
    static const char* const names[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    if (pitch < 0 || pitch > 127) {
        out[0] = '?';
        out[1] = '\0';
        return 1;
    }
    int n = 0;
    for (const char* c = names[pitch % 12]; *c; ++c) out[n++] = *c;
    int octave = pitch / 12 - 1;
    if (octave < 0) out[n++] = '-';
    out[n++] = static_cast<char>('0' + (octave < 0 ? -octave : octave));
    out[n] = '\0';
    return n;
}

}  // namespace g2m
