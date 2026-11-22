#include <g2m/sustain.h>

#include <vector>

#include "check.h"

namespace {

struct Event {
    std::uint64_t sample;
    int pitch, velocity;  // velocity -1: a bend
    bool operator==(const Event& o) const { return sample == o.sample && pitch == o.pitch && velocity == o.velocity; }
};

struct Out : g2m::EventSink {
    std::vector<Event> events;
    void note(const g2m::NoteEvent& e) override { events.push_back({e.sample, e.pitch, e.velocity}); }
    void bend(const g2m::BendEvent& e) override { events.push_back({e.sample, e.pitch, -1}); }
    // the events since the last call
    std::vector<Event> take() {
        std::vector<Event> all;
        all.swap(events);
        return all;
    }
};

struct Rig {
    g2m::Sustain sustain;
    Out out;
    explicit Rig(g2m::SustainMode mode) { sustain.set_mode(mode, 0, out); }
    void on(int pitch, std::uint64_t t, int velocity = 100) {
        g2m::NoteEvent e;
        e.sample = t;
        e.pitch = pitch;
        e.velocity = velocity;
        sustain.note(e, out);
    }
    void off(int pitch, std::uint64_t t) { on(pitch, t, 0); }
    void bend(int pitch, std::uint64_t t) {
        g2m::BendEvent e;
        e.sample = t;
        e.pitch = pitch;
        e.bend = 9000;
        sustain.bend(e, out);
    }
    void pedal(bool down, std::uint64_t t) { sustain.set_pedal(down, t, out); }
};

using V = std::vector<Event>;
constexpr int kBend = -1;

}  // namespace

TEST(off_passes_everything) {
    Rig r(g2m::SustainMode::off);
    r.pedal(true, 1);
    r.on(60, 10);
    r.bend(60, 15);
    r.off(60, 20);
    r.pedal(false, 30);
    CHECK((r.out.take() == V{{10, 60, 100}, {15, 60, kBend}, {20, 60, 0}}));
}

TEST(pedal_up_passes_everything) {
    for (auto mode : {g2m::SustainMode::piano, g2m::SustainMode::sostenuto, g2m::SustainMode::freeze}) {
        Rig r(mode);
        r.on(60, 10);
        r.bend(60, 15);
        r.off(60, 20);
        r.off(61, 21);  // never sent: dropped
        CHECK((r.out.take() == V{{10, 60, 100}, {15, 60, kBend}, {20, 60, 0}}));
    }
}

TEST(piano) {
    Rig r(g2m::SustainMode::piano);
    r.on(60, 10);
    r.pedal(true, 12);
    r.off(60, 20);  // deferred
    r.on(64, 30);
    r.off(64, 40);  // deferred too: notes after pedal down are held
    r.on(67, 45);
    CHECK((r.out.take() == V{{10, 60, 100}, {30, 64, 100}, {45, 67, 100}}));
    CHECK(r.sustain.held(60) && r.sustain.held(64) && !r.sustain.held(67));
    r.on(60, 50);  // played again: its note-off first
    CHECK((r.out.take() == V{{50, 60, 0}, {50, 60, 100}}));
    r.pedal(false, 60);  // the deferred note-offs, lowest first; 60 and 67 still on
    CHECK((r.out.take() == V{{60, 64, 0}}));
    r.off(60, 70);
    r.off(67, 71);
    CHECK((r.out.take() == V{{70, 60, 0}, {71, 67, 0}}));
}

TEST(sostenuto) {
    Rig r(g2m::SustainMode::sostenuto);
    r.on(60, 10);
    r.on(64, 11);
    r.pedal(true, 12);
    r.off(60, 20);  // latched: deferred
    r.on(67, 30);
    r.off(67, 40);  // after pedal down: plays normally
    CHECK((r.out.take() == V{{10, 60, 100}, {11, 64, 100}, {30, 67, 100}, {40, 67, 0}}));
    r.off(64, 41);  // latched: deferred
    r.on(64, 50);  // played again: off, on, no longer latched
    CHECK((r.out.take() == V{{50, 64, 0}, {50, 64, 100}}));
    r.off(64, 55);
    CHECK((r.out.take() == V{{55, 64, 0}}));
    r.pedal(false, 60);
    CHECK((r.out.take() == V{{60, 60, 0}}));
}

// Sostenuto: a latched note still on at pedal up just plays on
TEST(sostenuto_note_still_on) {
    Rig r(g2m::SustainMode::sostenuto);
    r.on(60, 10);
    r.pedal(true, 12);
    r.pedal(false, 20);
    r.off(60, 30);
    CHECK((r.out.take() == V{{10, 60, 100}, {30, 60, 0}}));
}

TEST(freeze) {
    Rig r(g2m::SustainMode::freeze);
    r.on(60, 10);
    r.on(64, 11);
    r.bend(64, 11);
    r.pedal(true, 12);
    r.bend(64, 15);  // frozen
    r.off(60, 20);  // held
    r.on(67, 30);  // blocked
    r.bend(67, 31);
    r.on(60, 32);  // blocked, though 60 is held
    CHECK((r.out.take() == V{{10, 60, 100}, {11, 64, 100}, {11, 64, kBend}}));
    r.pedal(false, 40);  // ends every frozen note, 64 though still on
    CHECK((r.out.take() == V{{40, 60, 0}, {40, 64, 0}}));
    r.bend(64, 41);
    r.off(64, 42);
    r.off(67, 43);  // started during the freeze: never sent
    r.off(60, 44);
    r.on(69, 50);  // detection resumes
    CHECK((r.out.take() == V{{50, 69, 100}}));
}

TEST(pedal_repeats_are_ignored) {
    Rig r(g2m::SustainMode::piano);
    r.on(60, 10);
    r.pedal(true, 11);
    r.off(60, 12);
    r.pedal(true, 13);
    r.out.take();
    r.sustain.set_mode(g2m::SustainMode::piano, 14, r.out);  // unchanged: still held
    CHECK(r.out.take().empty());
    r.pedal(false, 20);
    r.pedal(false, 21);
    CHECK((r.out.take() == V{{20, 60, 0}}));
}

// A pedal sends many values above 64: sostenuto latches only at the first
TEST(sostenuto_latches_once) {
    Rig r(g2m::SustainMode::sostenuto);
    r.pedal(true, 10);
    r.on(60, 11);
    r.pedal(true, 12);
    r.off(60, 13);
    CHECK((r.out.take() == V{{11, 60, 100}, {13, 60, 0}}));
}

TEST(mode_change_releases) {
    Rig r(g2m::SustainMode::piano);
    r.on(60, 10);
    r.pedal(true, 11);
    r.off(60, 12);
    r.on(62, 13);
    r.out.take();
    r.sustain.set_mode(g2m::SustainMode::freeze, 20, r.out);  // 60 released; 62 latched
    CHECK((r.out.take() == V{{20, 60, 0}}));
    r.on(64, 21);  // blocked
    r.sustain.set_mode(g2m::SustainMode::off, 30, r.out);  // the frozen note ends
    CHECK((r.out.take() == V{{30, 62, 0}}));
    r.on(64, 31);
    CHECK((r.out.take() == V{{31, 64, 100}}));
    r.sustain.set_mode(g2m::SustainMode::off, 32, r.out);  // unchanged: nothing
    CHECK(r.out.take().empty());
}

TEST(reset_forgets_notes) {
    Rig r(g2m::SustainMode::piano);
    r.on(60, 10);
    r.pedal(true, 11);
    r.off(60, 12);
    r.sustain.reset();
    CHECK(!r.sustain.sounding(60) && r.sustain.pedal());
    r.pedal(false, 20);
    r.off(60, 21);
    CHECK((r.out.take() == V{{10, 60, 100}}));
}

// The note-on's heads pass through unchanged
TEST(note_on_fields_pass_through) {
    g2m::Sustain s;
    struct Last : g2m::EventSink {
        g2m::NoteEvent e;
        void note(const g2m::NoteEvent& n) override { e = n; }
    } last;
    g2m::NoteEvent e;
    e.sample = 5;
    e.pitch = 50;
    e.velocity = 90;
    e.onset_type = g2m::OnsetType::slide;
    e.has_strings = true;
    e.strings[3] = 0.7f;
    e.has_bend = true;
    e.bend = 8500;
    s.note(e, last);
    CHECK(last.e.onset_type == g2m::OnsetType::slide && last.e.has_strings && last.e.strings[3] == 0.7f &&
          last.e.has_bend && last.e.bend == 8500 && last.e.sample == 5);
}
