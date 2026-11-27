#include <g2m/bend_mode.h>

#include <vector>

#include "check.h"

namespace {

struct Event {
    std::uint64_t sample;
    char kind;  // 'n' note, 'b' bend, 'r' retrigger, 'c' channel bend (plain)
    int pitch, value;  // note: velocity; bend, channel bend: 14-bit; retrigger: the new pitch
    int bend = -2;  // a note-on's bend (-1: none)
    bool operator==(const Event& o) const {
        return sample == o.sample && kind == o.kind && pitch == o.pitch && value == o.value && bend == o.bend;
    }
};

struct Out : g2m::NoteSink {
    std::vector<Event> events;
    void note(const g2m::NoteEvent& e) override {
        events.push_back({e.sample, 'n', e.pitch, e.velocity, e.velocity ? (e.has_bend ? e.bend : -1) : -2});
    }
    void bend(const g2m::BendEvent& e) override { events.push_back({e.sample, 'b', e.pitch, e.bend}); }
    void retrigger(std::uint64_t sample, int from, int to, int velocity) override {
        events.push_back({sample, 'r', from, to, velocity});
    }
    void channel_bend(std::uint64_t sample, int value) override { events.push_back({sample, 'c', -1, value}); }
    std::vector<Event> take() {
        std::vector<Event> all;
        all.swap(events);
        return all;
    }
};

// A natural bend's 14-bit value at 12 semitones, as the decoder makes it
int natural(float semitones) {
    return static_cast<int>(8192 + semitones * 8192 / 12);
}

struct Rig {
    g2m::BendModeFilter filter;
    Out out;
    explicit Rig(g2m::BendMode mode, bool plain = false) {
        filter.set_mode(mode);
        filter.set_plain(plain);
    }
    void on(int pitch, std::uint64_t t, float semitones = 0, bool has_bend = true, int velocity = 100) {
        g2m::NoteEvent e;
        e.sample = t;
        e.pitch = pitch;
        e.velocity = velocity;
        e.has_bend = has_bend;
        e.semitones = semitones;
        e.bend = natural(semitones);
        filter.note(e, out);
    }
    void off(int pitch, std::uint64_t t) {
        g2m::NoteEvent e;
        e.sample = t;
        e.pitch = pitch;
        e.velocity = 0;
        filter.note(e, out);
    }
    void bend(int pitch, std::uint64_t t, float semitones) {
        g2m::BendEvent e;
        e.sample = t;
        e.pitch = pitch;
        e.semitones = semitones;
        e.bend = natural(semitones);
        filter.bend(e, out);
    }
};

using V = std::vector<Event>;

}  // namespace

TEST(steps_with_hysteresis) {
    CHECK(g2m::bend_step(0, 0.69f) == 0);
    CHECK(g2m::bend_step(0, 0.7f) == 1);
    CHECK(g2m::bend_step(0, -0.7f) == -1);
    CHECK(g2m::bend_step(1, 0.31f) == 1);  // back down: past 0.3
    CHECK(g2m::bend_step(1, 0.3f) == 0);
    CHECK(g2m::bend_step(0, 2.4f) == 2);  // a jump
    CHECK(g2m::bend_step(2, 1.4f) == 2);
}

TEST(natural_passes_everything) {
    Rig r(g2m::BendMode::natural);
    r.on(60, 10, 0.3f);
    r.bend(60, 20, 0.9f);
    r.bend(61, 21, 0.9f);  // not sounding
    r.off(60, 30);
    r.off(60, 31);
    CHECK((r.out.take() == V{{10, 'n', 60, 100, natural(0.3f)}, {20, 'b', 60, natural(0.9f)}, {30, 'n', 60, 0}}));
}

TEST(retrigger) {
    Rig r(g2m::BendMode::retrigger);
    r.on(60, 10, 0.1f, true, 90);
    r.bend(60, 20, 0.6f);
    r.bend(60, 30, 0.8f);  // 60 -> 61
    r.bend(60, 40, 1.75f);  // 61 -> 62
    r.bend(60, 50, 1.4f);  // stays
    r.bend(60, 60, 0.2f);  // 62 -> 60
    r.off(60, 70);  // ends the note at the pitch it sounds at
    CHECK((r.out.take() == V{{10, 'n', 60, 90, 8192}, {30, 'r', 60, 61, 90}, {40, 'r', 61, 62, 90},
                             {60, 'r', 62, 60, 90}, {70, 'n', 60, 0}}));
    r.on(50, 80, 1.0f);  // a pre-bend starts at the bent pitch
    CHECK(r.filter.sounding_pitch(50) == 51);
    r.off(50, 90);
    CHECK((r.out.take() == V{{80, 'n', 51, 100, 8192}, {90, 'n', 51, 0}}));
}

TEST(retrigger_clamps_and_waits) {
    Rig r(g2m::BendMode::retrigger);
    r.on(127, 10);
    r.bend(127, 20, 1.0f);  // clamped: no new pitch
    r.on(64, 30);
    r.on(65, 31);
    r.bend(64, 40, 1.0f);  // 65 is sounding: waits
    CHECK((r.out.take() == V{{10, 'n', 127, 100, 8192}, {30, 'n', 64, 100, 8192}, {31, 'n', 65, 100, 8192}}));
    r.off(65, 50);
    r.bend(64, 60, 1.0f);  // now free
    CHECK((r.out.take() == V{{50, 'n', 65, 0}, {60, 'r', 64, 65, 100}}));
    r.on(65, 70);  // the detected note at the retriggered pitch ends the retriggered one
    r.off(64, 80);  // already ended: nothing
    CHECK((r.out.take() == V{{70, 'n', 65, 0}, {70, 'n', 65, 100, 8192}}));
    r.bend(127, 90, 0.0f);  // back from the clamp: 127 again, nothing
    r.on(0, 100);
    r.bend(0, 110, -1.0f);  // clamped at 0
    CHECK((r.out.take() == V{{100, 'n', 0, 100, 8192}}));
}

// A note at the pitch a retriggered note left plays alongside it
TEST(retrigger_frees_the_old_pitch) {
    Rig r(g2m::BendMode::retrigger);
    r.on(60, 10);
    r.bend(60, 20, 1.0f);
    r.on(60 + 2, 25);
    r.on(60, 30);  // the detected note 60 again: the retriggered one (at 61) ends first, as a re-strike
    r.off(60, 40);
    r.on(59, 50);
    r.bend(59, 60, 1.0f);  // to 60, free now
    r.on(61, 70);
    r.off(62, 80);
    CHECK((r.out.take() == V{{10, 'n', 60, 100, 8192}, {20, 'r', 60, 61, 100}, {25, 'n', 62, 100, 8192},
                             {30, 'n', 61, 0}, {30, 'n', 60, 100, 8192}, {40, 'n', 60, 0},
                             {50, 'n', 59, 100, 8192}, {60, 'r', 59, 60, 100}, {70, 'n', 61, 100, 8192},
                             {80, 'n', 62, 0}}));
    CHECK(r.filter.sounding_pitch(59) == 60 && r.filter.sounding_pitch(61) == 61);
    // two steps on: a note at the pitch passed through doesn't end it
    Rig q(g2m::BendMode::retrigger);
    q.on(60, 10);
    q.bend(60, 20, 1.0f);
    q.bend(60, 30, 2.0f);
    q.on(61, 40);
    CHECK(q.filter.sounding_pitch(60) == 62 && q.filter.sounding_pitch(61) == 61);
}

TEST(off_drops_bends) {
    Rig r(g2m::BendMode::off);
    r.on(60, 10, 1.3f);
    r.bend(60, 20, 2.0f);
    r.off(60, 30);
    CHECK((r.out.take() == V{{10, 'n', 60, 100, 8192}, {30, 'n', 60, 0}}));
}

// Each note keeps the mode it started with
TEST(mode_per_note) {
    Rig r(g2m::BendMode::retrigger);
    r.on(60, 10);
    r.bend(60, 20, 1.0f);
    r.filter.set_mode(g2m::BendMode::natural);
    r.on(50, 25);
    r.bend(60, 30, 2.0f);  // still retriggers
    r.bend(50, 31, 0.5f);  // natural
    r.off(60, 40);
    CHECK((r.out.take() == V{{10, 'n', 60, 100, 8192}, {20, 'r', 60, 61, 100}, {25, 'n', 50, 100, natural(0)},
                             {30, 'r', 61, 62, 100}, {31, 'b', 50, natural(0.5f)}, {40, 'n', 62, 0}}));
}

// Without a pitch-offset head, notes pass unchanged in every mode
TEST(no_bend_head) {
    for (auto mode : {g2m::BendMode::retrigger, g2m::BendMode::off}) {
        Rig r(mode);
        r.on(60, 10, 0, false);
        r.off(60, 20);
        CHECK((r.out.take() == V{{10, 'n', 60, 100, -1}, {20, 'n', 60, 0}}));
    }
}

TEST(repeated_note_on_and_reset) {
    Rig r(g2m::BendMode::retrigger);
    r.on(60, 10);
    r.bend(60, 20, 1.0f);
    r.on(60, 30);  // no note-off between: the old one ends first
    CHECK((r.out.take() == V{{10, 'n', 60, 100, 8192}, {20, 'r', 60, 61, 100}, {30, 'n', 61, 0},
                             {30, 'n', 60, 100, 8192}}));
    r.filter.reset();
    r.off(60, 40);
    r.bend(60, 41, 1.0f);
    CHECK(r.out.take().empty());
    CHECK(r.filter.sounding_pitch(60) == -1);
}

// Plain output, natural: the channel bend follows the newest note sounding
TEST(plain_lead_bends_the_channel) {
    Rig r(g2m::BendMode::natural, true);
    int b = natural(0.5f), c = natural(1.0f), d = natural(2.0f);
    r.on(60, 10);  // its bend is the channel's already: no channel bend
    r.bend(60, 20, 0.5f);
    r.bend(60, 21, 0.5f);  // the same value: nothing
    r.bend(61, 22, 0.5f);  // not sounding
    CHECK((r.out.take() == V{{10, 'n', 60, 100, natural(0)}, {20, 'c', -1, b}}));
    r.on(64, 30, 1.0f);  // the new lead: its bend first, then the note-on
    r.bend(60, 40, 2.0f);  // not the lead: dropped, but kept
    r.bend(64, 41, 0.5f);
    CHECK((r.out.take() == V{{30, 'c', -1, c}, {30, 'n', 64, 100, c}, {41, 'c', -1, b}}));
    r.off(64, 50);  // the lead ends: 60 leads, its bend at once, after the note-off
    CHECK((r.out.take() == V{{50, 'n', 64, 0}, {50, 'c', -1, d}}));
    r.bend(60, 60, 2.0f);  // already there
    r.off(60, 70);  // no note left: the bend stays
    r.on(62, 80);  // the next note-on takes it back to its own
    CHECK((r.out.take() == V{{70, 'n', 60, 0}, {80, 'c', -1, 8192}, {80, 'n', 62, 100, natural(0)}}));
}

// The hand-over goes to the newest note left, and only when the lead ends
TEST(plain_lead_hand_over) {
    Rig r(g2m::BendMode::natural, true);
    r.on(50, 10);
    r.on(55, 20);
    r.on(52, 30);  // the lead: newest, though not highest
    r.bend(50, 31, 1.0f);
    r.bend(55, 32, 0.5f);
    r.off(55, 40);  // not the lead: nothing more
    CHECK((r.out.take() == V{{10, 'n', 50, 100, natural(0)}, {20, 'n', 55, 100, natural(0)},
                             {30, 'n', 52, 100, natural(0)}, {40, 'n', 55, 0}}));
    r.on(57, 50);
    r.off(57, 60);  // 52 (newer than 50) leads; its bend is the channel's: nothing
    r.off(52, 70);  // 50 leads, bent
    CHECK((r.out.take() == V{{50, 'n', 57, 100, natural(0)}, {60, 'n', 57, 0}, {70, 'n', 52, 0},
                             {70, 'c', -1, natural(1.0f)}}));
}

// A note-on with no note-off between, or onto a retriggered note, sets the
// channel once, for the new note
TEST(plain_note_on_ends_without_hand_over) {
    Rig r(g2m::BendMode::natural, true);
    r.on(50, 10, 1.0f);
    r.on(60, 20);
    r.on(60, 30, 0.5f);  // 60 again: ends and restarts; 50 doesn't take over in between
    CHECK((r.out.take() == V{{10, 'c', -1, natural(1.0f)}, {10, 'n', 50, 100, natural(1.0f)},
                             {20, 'c', -1, 8192}, {20, 'n', 60, 100, natural(0)}, {30, 'n', 60, 0},
                             {30, 'c', -1, natural(0.5f)}, {30, 'n', 60, 100, natural(0.5f)}}));
    Rig q(g2m::BendMode::retrigger, true);
    q.on(60, 10);
    q.bend(60, 20, 1.0f);  // to 61
    q.filter.set_mode(g2m::BendMode::natural);
    q.on(50, 25, 2.0f);
    q.on(61, 30);  // the detected note at the retriggered pitch ends it; 50 doesn't take over
    CHECK((q.out.take() == V{{10, 'n', 60, 100, 8192}, {20, 'r', 60, 61, 100}, {25, 'c', -1, natural(2.0f)},
                             {25, 'n', 50, 100, natural(2.0f)}, {30, 'n', 61, 0}, {30, 'c', -1, 8192},
                             {30, 'n', 61, 100, natural(0)}}));
}

// Retrigger and off notes want no channel bend, and their bends don't move it
TEST(plain_other_modes_unbend_the_channel) {
    for (auto mode : {g2m::BendMode::retrigger, g2m::BendMode::off}) {
        Rig r(g2m::BendMode::natural, true);
        r.on(50, 10, 1.0f);
        r.filter.set_mode(mode);
        r.on(60, 20, 0.2f);  // the lead, at no bend
        r.bend(60, 30, 0.3f);
        r.bend(50, 31, 0.5f);  // natural, not the lead: kept
        r.off(60, 40);  // 50 leads again: its last bend
        CHECK((r.out.take() == V{{10, 'c', -1, natural(1.0f)}, {10, 'n', 50, 100, natural(1.0f)},
                                 {20, 'c', -1, 8192}, {20, 'n', 60, 100, 8192}, {40, 'n', 60, 0},
                                 {40, 'c', -1, natural(0.5f)}}));
        // a hand-over to a retrigger or off note: no bend
        r.on(60, 50);
        r.off(50, 51);
        r.bend(60, 52, 0.4f);
        r.off(60, 60);
        r.filter.set_mode(g2m::BendMode::natural);
        r.on(62, 61);
        CHECK((r.out.take() == V{{50, 'c', -1, 8192}, {50, 'n', 60, 100, 8192}, {51, 'n', 50, 0},
                                 {60, 'n', 60, 0}, {61, 'n', 62, 100, natural(0)}}));
    }
}

// Without a bend head the channel is never bent; reset forgets the channel
TEST(plain_no_bend_head_and_reset) {
    Rig r(g2m::BendMode::natural, true);
    r.on(60, 10, 1.0f, false);  // a bend value with has_bend false is ignored
    r.on(62, 20, 0, false);
    r.off(62, 30);
    CHECK((r.out.take() == V{{10, 'n', 60, 100, -1}, {20, 'n', 62, 100, -1}, {30, 'n', 62, 0}}));
    Rig q(g2m::BendMode::natural, true);
    q.on(60, 10, 1.0f);
    q.filter.reset();  // the caller ended the notes and reset the channel
    q.off(60, 20);
    q.on(60, 30);
    q.on(64, 40, 1.0f);
    q.filter.reset();
    q.on(64, 50, 1.0f);  // the channel is 8192 again: sent
    CHECK((q.out.take() == V{{10, 'c', -1, natural(1.0f)}, {10, 'n', 60, 100, natural(1.0f)},
                             {30, 'n', 60, 100, natural(0)}, {40, 'c', -1, natural(1.0f)},
                             {40, 'n', 64, 100, natural(1.0f)}, {50, 'c', -1, natural(1.0f)},
                             {50, 'n', 64, 100, natural(1.0f)}}));
}
