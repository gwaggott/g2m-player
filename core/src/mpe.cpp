#include <g2m/mpe.h>

#include <algorithm>

namespace g2m {

namespace {

constexpr std::uint8_t kNoteOff = 0x80, kNoteOn = 0x90, kControl = 0xB0, kPitchBend = 0xE0;
constexpr std::uint8_t kRpnMsb = 101, kRpnLsb = 100, kDataMsb = 6, kDataLsb = 38;

std::uint8_t status(std::uint8_t kind, int channel) {
    return static_cast<std::uint8_t>(kind | channel);
}

// Sets RPN `rpn` to `value` (data entry MSB, and LSB 0 if `lsb`), then the
// null RPN so later data entries change nothing
void set_rpn(MidiWriter& out, int channel, int rpn, int value, bool lsb) {
    std::uint8_t cc = status(kControl, channel);
    out.midi(cc, kRpnMsb, 0);
    out.midi(cc, kRpnLsb, static_cast<std::uint8_t>(rpn));
    out.midi(cc, kDataMsb, static_cast<std::uint8_t>(value));
    if (lsb) out.midi(cc, kDataLsb, 0);
    out.midi(cc, kRpnMsb, 127);
    out.midi(cc, kRpnLsb, 127);
}

void send_mcm(MidiWriter& out, int members) {
    set_rpn(out, MpeOutput::kManagerChannel, 6, members, false);
}

}  // namespace

void MpeOutput::configure(int bend_range, MidiWriter& out) {
    send_mcm(out, kStrings);
    send_bend_range(bend_range, out);
}

void MpeOutput::send_bend_range(int bend_range, MidiWriter& out) {
    int range = std::clamp(bend_range, 1, kMaxBendRange);
    for (int s = 1; s <= kStrings; ++s) set_rpn(out, channel(s), 0, range, true);
}

void MpeOutput::send_channel_bend_range(int channel, int bend_range, MidiWriter& out) {
    set_rpn(out, channel, 0, std::clamp(bend_range, 1, kMaxBendRange), true);
}

void MpeOutput::release_zone(MidiWriter& out) {
    send_mcm(out, 0);
}

void MpeOutput::reset_bends() {
    std::fill(sent_, sent_ + kStrings + 1, 8192);
}

void MpeOutput::send_bend(int string, int value, MidiWriter& out) {
    if (value == sent_[string]) return;
    out.midi(status(kPitchBend, channel(string)), static_cast<std::uint8_t>(value & 0x7F),
             static_cast<std::uint8_t>(value >> 7));
    sent_[string] = value;
}

void MpeOutput::bend(int pitch, int value, MidiWriter& out) {
    if (int s = allocator_.string_of(pitch)) send_bend(s, value, out);
}

void MpeOutput::note(int pitch, int velocity, MidiWriter& out, const float* weights, OnsetType type, int bend) {
    std::uint8_t p = static_cast<std::uint8_t>(pitch);
    if (velocity == 0) {
        if (int s = allocator_.note_off(pitch)) out.midi(status(kNoteOff, channel(s)), p, 0);
        return;
    }
    int old = allocator_.string_of(pitch);  // a repeated note-on ends the note on its old string
    StringAllocator::Assignment a = allocator_.note_on(pitch, weights, type);
    if (a.stolen_pitch >= 0) {
        int from = a.stolen_pitch == pitch ? old : a.string;
        out.midi(status(kNoteOff, channel(from)), static_cast<std::uint8_t>(a.stolen_pitch), 0);
    }
    if (bend >= 0) send_bend(a.string, bend, out);
    out.midi(status(kNoteOn, channel(a.string)), p, static_cast<std::uint8_t>(velocity));
    if (a.continued_pitch >= 0)
        out.midi(status(kNoteOff, channel(a.string)), static_cast<std::uint8_t>(a.continued_pitch), 0);
}

void MpeOutput::retrigger(int from, int to, int velocity, MidiWriter& out) {
    int s = allocator_.string_of(from);
    if (!s || from == to) return;
    if (allocator_.string_of(to)) note(to, 0, out);
    out.midi(status(kNoteOff, channel(s)), static_cast<std::uint8_t>(from), 0);
    allocator_.move(from, to);
    send_bend(s, 8192, out);
    out.midi(status(kNoteOn, channel(s)), static_cast<std::uint8_t>(to), static_cast<std::uint8_t>(velocity));
}

void MpeOutput::all_notes_off(MidiWriter& out) {
    for (int pitch = 0; pitch < 128; ++pitch)
        if (int s = allocator_.note_off(pitch)) out.midi(status(kNoteOff, channel(s)), static_cast<std::uint8_t>(pitch), 0);
}

}  // namespace g2m
