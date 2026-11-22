// MPE output: the configuration messages byte for byte, notes on their
// strings' channels, and a valid stream from the Transcriber's events.

#include <g2m/mpe.h>
#include <g2m/transcriber.h>

#include <random>
#include <vector>

#include "allocations.h"
#include "check.h"
#include "vectors.h"

namespace {

struct Message {
    std::uint64_t sample;
    int status, data1, data2;
    bool operator==(const Message& o) const {
        return sample == o.sample && status == o.status && data1 == o.data1 && data2 == o.data2;
    }
};

struct Collect : g2m::MidiWriter {
    std::vector<Message> messages;
    std::uint64_t sample = 0;
    Collect() { messages.reserve(100000); }  // so collecting doesn't allocate
    void midi(std::uint8_t s, std::uint8_t d1, std::uint8_t d2) override { messages.push_back({sample, s, d1, d2}); }
};

std::vector<Message> rpn(int channel, int number, int value, bool lsb) {
    int cc = 0xB0 | channel;
    std::vector<Message> m = {{0, cc, 101, 0}, {0, cc, 100, number}, {0, cc, 6, value}};
    if (lsb) m.push_back({0, cc, 38, 0});
    m.push_back({0, cc, 101, 127});
    m.push_back({0, cc, 100, 127});
    return m;
}

// Checks an MPE stream as a receiver would see it: notes only on member
// channels 2-7, at most one at a time on each, note-offs for the note on
// the channel; returns the number of notes left sounding.
// A legato overlap is allowed: a note-on on a channel already sounding a
// note must be followed at once by that note's note-off there.
struct Validator {
    int on[16];
    int overlapped = -1;  // the channel of an overlap awaiting its note-off
    int overlap_pitch = -1;
    int overlaps = 0;
    bool ok = true;
    Validator() { std::fill(on, on + 16, -1); }
    void see(const Message& m) {
        int kind = m.status & 0xF0, channel = m.status & 0x0F;
        if (kind == 0xB0) return;
        bool member = channel >= 1 && channel <= 6;
        if (overlapped >= 0) {
            bool closes = kind == 0x80 && channel == overlapped && m.data1 == overlap_pitch;
            CHECK_MSG(closes, "overlap on channel %d not ended at once", overlapped + 1);
            ok = ok && closes;
            overlapped = -1;
            ++overlaps;
            return;
        }
        if (kind == 0x90) {
            CHECK_MSG(member && m.data2 > 0, "note-on %d on channel %d at %llu", m.data1, channel + 1,
                      static_cast<unsigned long long>(m.sample));
            ok = ok && member;
            if (member && on[channel] >= 0) {
                overlapped = channel;
                overlap_pitch = on[channel];
            }
            on[channel] = m.data1;
        } else {
            CHECK_MSG(kind == 0x80 && member && on[channel] == m.data1 && m.data2 == 0, "note-off %d on channel %d",
                      m.data1, channel + 1);
            ok = ok && kind == 0x80 && member && on[channel] == m.data1;
            on[channel] = -1;
        }
    }
    int sounding() const {
        int n = overlapped >= 0;
        for (int p : on) n += p >= 0;
        return n;
    }
};

struct Feed : g2m::EventSink {
    g2m::MpeOutput mpe;
    Collect out;
    std::vector<g2m::NoteEvent> events;
    Feed() { events.reserve(100000); }
    void note(const g2m::NoteEvent& e) override {
        events.push_back(e);
        out.sample = e.sample;
        mpe.note(e.pitch, e.velocity, out);
    }
};

}  // namespace

TEST(configuration_messages) {
    Collect out;
    g2m::MpeOutput::configure(12, out);
    std::vector<Message> want = rpn(0, 6, 6, false);  // the MCM: 6 member channels
    for (int channel = 1; channel <= 6; ++channel) {
        std::vector<Message> range = rpn(channel, 0, 12, true);
        want.insert(want.end(), range.begin(), range.end());
    }
    CHECK(out.messages == want);

    Collect wide;
    g2m::MpeOutput::send_bend_range(48, wide);
    CHECK(wide.messages.size() == 6 * 6 && wide.messages[2].data2 == 48 && wide.messages[2].status == 0xB1);
    Collect clamped;
    g2m::MpeOutput::send_bend_range(0, clamped);
    g2m::MpeOutput::send_bend_range(200, clamped);
    CHECK(clamped.messages[2].data2 == 1 && clamped.messages[36 + 2].data2 == 96);

    Collect off;
    g2m::MpeOutput::release_zone(off);
    CHECK(off.messages == rpn(0, 6, 0, false));
}

TEST(notes_on_string_channels) {
    g2m::MpeOutput mpe;
    Collect out;
    mpe.note(40, 100, out);  // string 6: channel 7
    mpe.note(47, 90, out);   // string 5: channel 6
    mpe.note(64, 80, out);   // string 2, channel 3: fret 5 (frets 14 and 9 on strings 4 and 3 are too far from 2)
    mpe.note(47, 0, out);
    mpe.note(47, 0, out);  // not sounding: nothing
    std::vector<Message> want = {{0, 0x96, 40, 100}, {0, 0x95, 47, 90}, {0, 0x92, 64, 80}, {0, 0x85, 47, 0}};
    CHECK(out.messages == want);
    CHECK(mpe.allocator().string_of(64) == 2);
    out.messages.clear();
    mpe.all_notes_off(out);
    want = {{0, 0x86, 40, 0}, {0, 0x82, 64, 0}};
    CHECK(out.messages == want);
    CHECK(mpe.allocator().sounding() == 0);
}

TEST(steal_sends_note_off_first) {
    g2m::MpeOutput mpe;
    Collect out;
    for (int p : {40, 47, 52, 56, 59, 64}) mpe.note(p, 100, out);
    out.messages.clear();
    mpe.note(70, 60, out);
    std::vector<Message> want = {{0, 0x86, 40, 0}, {0, 0x96, 70, 60}};
    CHECK(out.messages == want);
    out.messages.clear();
    mpe.note(40, 0, out);  // the stolen note's own note-off: nothing
    CHECK(out.messages.empty());
}

TEST(repeated_note_on_ends_the_old_note) {
    g2m::MpeOutput mpe;
    Collect out;
    mpe.note(60, 100, out);  // string 6
    mpe.note(65, 100, out);  // string 5
    mpe.note(60, 0, out);
    mpe.note(64, 100, out);  // string 6, fret 24
    mpe.note(65, 90, out);   // still on string 5: off there, then on again
    std::vector<Message> tail(out.messages.end() - 2, out.messages.end());
    std::vector<Message> want = {{0, 0x85, 65, 0}, {0, 0x95, 65, 90}};
    CHECK(tail == want);
    float weights[6] = {0, 0, 0, 0, 0, 0};
    weights[3] = 1;  // string 4
    mpe.note(64, 0, out);  // alone, so every string keeps the span
    mpe.note(65, 80, out, weights);  // moves to string 4: the off goes to the old channel
    tail.assign(out.messages.end() - 2, out.messages.end());
    want = {{0, 0x85, 65, 0}, {0, 0x94, 65, 80}};
    CHECK(tail == want);
}

// Random note sequences, as the decoder sends them (per pitch: on, off,
// on ...): the stream stays valid and every note-on gets through.
TEST(random_sequences_stay_valid) {
    std::mt19937 rng(11);
    std::uniform_int_distribution<int> pitch(38, 88), velocity(1, 127);
    int steals = 0;
    for (int run = 0; run < 50; ++run) {
        g2m::MpeOutput mpe;
        Collect out;
        Validator v;
        bool held[128] = {};
        int ons = 0;
        for (int i = 0; i < 2000; ++i) {
            int p = pitch(rng);
            int vel = held[p] ? 0 : velocity(rng);
            held[p] = vel > 0;
            ons += vel > 0;
            std::size_t before = out.messages.size();
            mpe.note(p, vel, out);
            for (std::size_t k = before; k < out.messages.size(); ++k) v.see(out.messages[k]);
            steals += out.messages.size() - before == 2;
        }
        mpe.all_notes_off(out);
        int sent = 0;
        for (const Message& m : out.messages) sent += (m.status & 0xF0) == 0x90;
        CHECK(sent == ons);
        CHECK(v.ok && mpe.allocator().sounding() == 0);
    }
    CHECK(steals > 0);  // the sequences went past six notes
}

// The Transcriber's events through MpeOutput: a valid stream with the same
// note-ons at the same samples, and note-offs at their events' samples
// (except for notes stolen earlier).
TEST(transcriber_events_as_mpe) {
    for (const char* name : vectors::kNames) {
        auto model = vectors::load(name);
        if (!model) continue;
        std::vector<float> audio = vectors::input(name);
        g2m::Transcriber t(model);
        Feed feed;
        t.process(audio.data(), audio.size(), feed);
        t.all_notes_off(feed);
        Validator v;
        for (const Message& m : feed.out.messages) v.see(m);
        CHECK(v.ok && v.sounding() == 0);
        std::size_t i = 0;
        int steals = 0;
        for (const g2m::NoteEvent& e : feed.events) {
            if (i < feed.out.messages.size() && feed.out.messages[i].sample == e.sample &&
                (feed.out.messages[i].status & 0xF0) == 0x80 && e.velocity > 0 && feed.out.messages[i].data1 != e.pitch) {
                ++i;  // a stolen note's off, before this note-on
                ++steals;
            }
            bool stolen_off = e.velocity == 0 && (i >= feed.out.messages.size() || feed.out.messages[i].data1 != e.pitch ||
                                                  feed.out.messages[i].sample != e.sample);
            if (stolen_off) continue;  // nothing sent for it
            const Message& m = feed.out.messages[i++];
            CHECK_MSG(m.sample == e.sample && m.data1 == e.pitch && m.data2 == e.velocity &&
                          ((m.status & 0xF0) == 0x90) == (e.velocity > 0),
                      "%s: event at %llu", name, static_cast<unsigned long long>(e.sample));
        }
        CHECK(i == feed.out.messages.size());
        std::fprintf(stderr, "  %s: %zu events, %d stolen\n", name, feed.events.size(), steals);
    }
}

TEST(no_allocations) {
    g2m::MpeOutput mpe;
    Collect out;
    std::mt19937 rng(3);
    std::uniform_int_distribution<int> pitch(0, 127), velocity(0, 127);
    float weights[6] = {0.1f, 0.2f, 0.3f, 0.1f, 0.2f, 0.1f};
    long before = allocations;
    g2m::MpeOutput::configure(12, out);
    for (int i = 0; i < 5000; ++i) mpe.note(pitch(rng), velocity(rng), out, i % 2 ? weights : nullptr);
    mpe.all_notes_off(out);
    g2m::MpeOutput::release_zone(out);
    CHECK(allocations == before);
}

// Legato and slide note-ons: the new note on the continued note's channel,
// then that note's note-off there
TEST(legato_overlap) {
    g2m::MpeOutput mpe;
    Collect out;
    Validator v;
    mpe.note(45, 100, out);  // string 6: channel 7
    mpe.note(47, 70, out, nullptr, g2m::OnsetType::legato);
    mpe.note(45, 0, out);  // the continued note's own note-off: nothing
    mpe.note(52, 60, out, nullptr, g2m::OnsetType::slide);
    mpe.note(64, 90, out, nullptr, g2m::OnsetType::legato);  // 12 away: picked rules, string 4 fret 14
    std::vector<Message> want = {{0, 0x96, 45, 100}, {0, 0x96, 47, 70}, {0, 0x86, 45, 0},
                                 {0, 0x96, 52, 60},  {0, 0x86, 47, 0},  {0, 0x94, 64, 90}};
    CHECK(out.messages == want);
    for (const Message& m : out.messages) v.see(m);
    CHECK(v.ok && v.overlaps == 2 && v.sounding() == 2);
}

// The Transcriber's events from the version 3 test model, with their
// string probabilities and onset types: a valid stream with overlaps, and
// every legato note that has a note to continue in reach continues it.
TEST(transcriber_events_with_heads_as_mpe) {
    auto model = vectors::load(vectors::kMpe);
    if (!model) return;
    std::vector<float> audio = vectors::input(vectors::kMpe);
    g2m::Transcriber t(model);
    struct HeadsFeed : g2m::EventSink {
        g2m::MpeOutput mpe;
        Collect out;
        Validator v;
        int legato_ons = 0, continued = 0, followed_weights = 0, picked_ons = 0;
        void note(const g2m::NoteEvent& e) override {
            out.sample = e.sample;
            std::size_t before = out.messages.size();
            g2m::StringAllocator probe = mpe.allocator();
            g2m::StringAllocator::Assignment a = probe.note_on(e.pitch, e.strings, e.onset_type);
            mpe.note(e.pitch, e.velocity, out, e.has_strings ? e.strings : nullptr, e.onset_type);
            for (std::size_t k = before; k < out.messages.size(); ++k) v.see(out.messages[k]);
            if (!e.velocity) return;
            if (e.onset_type == g2m::OnsetType::picked) ++picked_ons;
            else ++legato_ons;
            continued += a.step == g2m::StringAllocator::Step::legato;
            int best = 1;  // the heads' top string
            for (int s = 2; s <= 6; ++s)
                if (e.strings[s - 1] > e.strings[best - 1]) best = s;
            followed_weights += a.string == best;
        }
    } feed;
    t.process(audio.data(), audio.size(), feed);
    t.all_notes_off(feed);
    CHECK(feed.v.ok && feed.v.sounding() == 0);
    std::fprintf(stderr, "  mpe: %d picked, %d legato / slide note-ons, %d continuing a note (%d overlaps); "
                 "%d on the heads' top string\n", feed.picked_ons, feed.legato_ons, feed.continued, feed.v.overlaps,
                 feed.followed_weights);
    CHECK(feed.continued > 0 && feed.continued == feed.v.overlaps);
    CHECK(feed.followed_weights > 0);
}

// Pitch bends (format version 4): before the note-on when the channel's
// last bend differs, bend events on the note's channel, none for a note
// not on a string, and the channels' memory reset with the configuration
TEST(pitch_bends) {
    g2m::MpeOutput mpe;
    Collect out;
    mpe.note(45, 100, out, nullptr, g2m::OnsetType::picked, 8192);  // channel 7 already at 8192: no bend
    mpe.bend(45, 9000, out);
    mpe.bend(45, 9000, out);  // unchanged: nothing
    mpe.note(45, 0, out);
    mpe.note(52, 90, out, nullptr, g2m::OnsetType::picked, 9000);  // channel 7 still at 9000: no bend
    mpe.note(52, 0, out);
    mpe.note(50, 80, out, nullptr, g2m::OnsetType::picked, 8192);  // back to 8192 first
    mpe.bend(60, 9100, out);  // not sounding: nothing
    std::vector<Message> want = {{0, 0x96, 45, 100}, {0, 0xE6, 9000 & 0x7F, 9000 >> 7}, {0, 0x86, 45, 0},
                                 {0, 0x96, 52, 90},  {0, 0x86, 52, 0},  {0, 0xE6, 0, 64}, {0, 0x96, 50, 80}};
    CHECK(out.messages == want);

    out.messages.clear();  // a legato continuation: the bend before the new note-on, the old note-off after
    mpe.note(52, 70, out, nullptr, g2m::OnsetType::legato, 16383);
    mpe.bend(50, 100, out);  // the continued note's bends are ignored
    want = {{0, 0xE6, 0x7F, 0x7F}, {0, 0x96, 52, 70}, {0, 0x86, 50, 0}};
    CHECK(out.messages == want);

    out.messages.clear();
    mpe.note(52, 0, out);
    mpe.reset_bends();  // as after a new configuration: every channel counts as at 8192
    mpe.note(45, 60, out, nullptr, g2m::OnsetType::picked, 8192);  // channel 7, last sent 16383, now forgotten
    mpe.note(50, 60, out, nullptr, g2m::OnsetType::picked, 0);  // string 5, fret 5
    want = {{0, 0x86, 52, 0}, {0, 0x96, 45, 60}, {0, 0xE5, 0, 0}, {0, 0x95, 50, 60}};
    CHECK(out.messages == want);
}

// A retrigger moves the note to the new pitch on its own channel, the
// channel's bend back at the centre in between
TEST(retrigger_keeps_the_channel) {
    g2m::MpeOutput mpe;
    Collect out;
    float on_string_3[6] = {0, 0, 1, 0, 0, 0};
    mpe.note(57, 90, out, on_string_3, g2m::OnsetType::picked, 9000);
    mpe.note(62, 80, out);
    int channel = g2m::MpeOutput::channel(mpe.allocator().string_of(57));
    CHECK(channel == 3);
    out.messages.clear();
    mpe.retrigger(57, 58, 90, out);
    CHECK((out.messages == std::vector<Message>{{0, 0x80 | channel, 57, 0}, {0, 0xE0 | channel, 0, 64},
                                                {0, 0x90 | channel, 58, 90}}));
    CHECK(mpe.allocator().string_of(58) == 3 && mpe.allocator().string_of(57) == 0);
    out.messages.clear();
    mpe.retrigger(58, 59, 90, out);  // the bend is at the centre already
    CHECK((out.messages == std::vector<Message>{{0, 0x80 | channel, 58, 0}, {0, 0x90 | channel, 59, 90}}));
    // onto a pitch sounding on another string: that note ends first
    int other = g2m::MpeOutput::channel(mpe.allocator().string_of(62));
    out.messages.clear();
    mpe.retrigger(59, 62, 90, out);
    CHECK((out.messages == std::vector<Message>{{0, 0x80 | other, 62, 0}, {0, 0x80 | channel, 59, 0},
                                                {0, 0x90 | channel, 62, 90}}));
    CHECK(mpe.allocator().string_of(62) == 3 && mpe.allocator().pitch_on(other - 0) != 62);
    out.messages.clear();
    mpe.retrigger(40, 41, 90, out);  // not sounding: nothing
    mpe.retrigger(62, 62, 90, out);
    CHECK(out.messages.empty());
    mpe.note(62, 0, out);
    CHECK((out.messages == std::vector<Message>{{0, 0x80 | channel, 62, 0}}));
}
