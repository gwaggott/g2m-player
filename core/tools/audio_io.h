// WAV reading and MIDI file writing for the command-line tools (not the
// library: a plugin gets its audio from the host).
#pragma once

#include <g2m/transcriber.h>

#include <cstdint>
#include <string>
#include <vector>

namespace g2m::tools {

struct Audio {
    std::vector<float> samples;  // mono, full scale ±1
    int sample_rate = 0;
};

// PCM 8/16/24/32-bit integer or 32/64-bit float WAV (plain or
// WAVE_FORMAT_EXTENSIBLE); channels are averaged to mono, as reference.py
// does. Returns false with a message on failure.
bool read_wav(const std::string& path, Audio& out, std::string& error);
bool parse_wav(const std::vector<std::uint8_t>& bytes, Audio& out, std::string& error);

// Note events → a standard MIDI file as reference.py writes it: format 0,
// 960 PPQ, 120 BPM, channel 1, note-off as 0x80 pitch 0. Event times are
// their samples at `sample_rate`, rounded to the nearest tick.
std::vector<std::uint8_t> midi_file(const std::vector<NoteEvent>& events, int sample_rate);

// Any three-byte channel messages, timed in samples like NoteEvent
struct MidiMessage {
    std::uint64_t sample;
    std::uint8_t status, data1, data2;
};

// The same file for any messages (e.g. MPE, on several channels)
std::vector<std::uint8_t> midi_file(const std::vector<MidiMessage>& messages, int sample_rate);

}  // namespace g2m::tools
