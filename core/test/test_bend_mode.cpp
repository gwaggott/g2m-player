#include <g2m/bend_mode.h>

#include <vector>

#include "check.h"

namespace {

struct Event {
    std::uint64_t sample;
    char kind;  // 'n' note, 'b' bend, 'r' retrigger
    int pitch, value;  // note: velocity; bend: 14-bit; retrigger: the new pitch
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
    explicit Rig(g2m::BendMode mode) { filter.set_mode(mode); }
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
