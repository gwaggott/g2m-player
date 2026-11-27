// MPE output: a lower zone with manager channel 1 and one member channel
// per string from channel 2, highest string first (string s on channel
// s + 1). Notes go to their string's channel, chosen by a StringAllocator
// at the note-on. See docs/mpe-output.md.
#pragma once

#include <g2m/string_allocator.h>

#include <cstdint>

namespace g2m {

constexpr int kDefaultBendRange = 12;  // semitones, sent as RPN 0
constexpr int kMaxBendRange = 96;  // MPE's limit

// Receives MIDI messages of three bytes; the caller decides their timing.
class MidiWriter {
public:
    virtual void midi(std::uint8_t status, std::uint8_t data1, std::uint8_t data2) = 0;

protected:
    ~MidiWriter() = default;
};

class MpeOutput {
public:
    // MIDI channel, 0-15, of a string (1-6) and of the zone's manager
    static int channel(int string) { return string; }
    static constexpr int kManagerChannel = 0;

    // The MPE Configuration Message (6 member channels), then the pitch-bend
    // range (RPN 0, `bend_range` semitones, 1-96) on every member channel:
    // the MCM resets the range to 48. Each RPN is closed with the null RPN.
    static void configure(int bend_range, MidiWriter& out);
    static void send_bend_range(int bend_range, MidiWriter& out);
    // The pitch-bend range (RPN 0, 1-96 semitones) on one channel, 0-15:
    // plain output's (docs/plain-bends.md).
    static void send_channel_bend_range(int channel, int bend_range, MidiWriter& out);
    // The MCM with no member channels: the zone is turned off.
    static void release_zone(MidiWriter& out);

    // Forget the pitch bends sent: every member channel counts as at 8192,
    // as after the configuration (call it whenever that is sent).
    void reset_bends();

    // A note-on (velocity 1-127) on the string the allocator chooses, after
    // a note-off for any note it ended to make room; a legato or slide
    // note-on that continues a note on its string is followed by that
    // note's note-off on the same channel (an overlap: mono legato). Or a
    // note-off (velocity 0) on the note's string, nothing if it isn't
    // sounding. `weights`, `type`: as for StringAllocator::note_on.
    // `bend` (format version 4, -1 for none): the note-on's 14-bit pitch
    // bend, sent on its channel just before it if the channel's last bend
    // differs. No allocation, locks or exceptions.
    void note(int pitch, int velocity, MidiWriter& out, const float* weights = nullptr,
              OnsetType type = OnsetType::picked, int bend = -1);

    // A sounding note's new pitch bend, on its channel if it differs from
    // the channel's last; nothing if the note isn't on a string (stolen, or
    // continued by a legato note).
    void bend(int pitch, int value, MidiWriter& out);

    // A retrigger: note `from` ends and `to` starts with `velocity` on the
    // same channel, with the channel's bend back at the centre (8192) in
    // between. A note already sounding at `to` elsewhere is ended first.
    // Nothing if `from` isn't sounding.
    void retrigger(int from, int to, int velocity, MidiWriter& out);

    // Note-off for every sounding note, lowest pitch first.
    void all_notes_off(MidiWriter& out);

    const StringAllocator& allocator() const { return allocator_; }

private:
    void send_bend(int string, int value, MidiWriter& out);

    StringAllocator allocator_;
    int sent_[kStrings + 1] = {8192, 8192, 8192, 8192, 8192, 8192, 8192};  // last bend per string's channel
};

}  // namespace g2m
