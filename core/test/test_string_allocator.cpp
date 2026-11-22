// The string allocator on hand-made note sequences: every step of the rule.

#include <g2m/string_allocator.h>

#include "check.h"

using g2m::StringAllocator;
using Step = StringAllocator::Step;

namespace {

bool is(const StringAllocator::Assignment& a, int string, Step step, int stolen = -1) {
    return a.string == string && a.step == step && a.stolen_pitch == stolen;
}

}  // namespace

TEST(string_ranges) {
    int low[] = {64, 59, 55, 50, 45, 38};
    int high[] = {88, 83, 79, 74, 69, 64};
    for (int s = 1; s <= 6; ++s) CHECK(g2m::string_low(s) == low[s - 1] && g2m::string_high(s) == high[s - 1]);
    CHECK(g2m::span_fret(6, 38) == 0 && g2m::span_fret(6, 39) == 0 && g2m::span_fret(6, 40) == 0);
    CHECK(g2m::span_fret(6, 41) == 1 && g2m::span_fret(1, 88) == 24 && g2m::span_fret(3, 60) == 5);
}

// Alone, a note goes to the lowest string whose range has it
TEST(lowest_string_that_can_play) {
    struct {
        int pitch, string;
    } cases[] = {{38, 6}, {39, 6}, {40, 6}, {64, 6}, {65, 5}, {69, 5}, {70, 4}, {74, 4},
                 {75, 3}, {79, 3}, {80, 2}, {83, 2}, {84, 1}, {88, 1}};
    for (auto c : cases) {
        StringAllocator a;
        CHECK_MSG(is(a.note_on(c.pitch), c.string, Step::shape), "pitch %d", c.pitch);
        CHECK(a.string_of(c.pitch) == c.string && a.pitch_on(c.string) == c.pitch && a.sounding() == 1);
    }
}

// An open E major chord, arpeggiated with every note held
TEST(chord_fills_strings_upwards) {
    StringAllocator a;
    int pitches[] = {40, 47, 52, 56, 59, 64};
    for (int i = 0; i < 6; ++i) CHECK(is(a.note_on(pitches[i]), 6 - i, Step::shape));
    CHECK(a.sounding() == 6);
}

TEST(hand_span) {
    {  // frets 5 and 9: a span of 4 fits
        StringAllocator a;
        a.note_on(45);  // string 6, fret 5
        CHECK(is(a.note_on(54), 5, Step::shape));  // string 5, fret 9
    }
    {  // frets 5 and 10 don't: the next string, fret 5
        StringAllocator a;
        a.note_on(45);
        CHECK(is(a.note_on(55), 4, Step::shape));
    }
    {  // below the lowest fret held: frets 10 and 6 fit
        StringAllocator a;
        a.note_on(50);  // string 6, fret 10
        CHECK(is(a.note_on(51), 5, Step::shape));  // string 5, fret 6
    }
    {  // open strings sounding don't count
        StringAllocator a;
        a.note_on(40);  // string 6 open
        CHECK(is(a.note_on(57), 5, Step::shape));  // string 5, fret 12
    }
    {  // nor does a new open note
        StringAllocator a;
        a.note_on(60);  // string 6, fret 20
        CHECK(is(a.note_on(45), 5, Step::shape));  // string 5 open
    }
    {  // the drop-D notes count as open
        StringAllocator a;
        a.note_on(38);
        CHECK(is(a.note_on(57), 5, Step::shape));
    }
    {  // all fretted notes are checked, not only the nearest string
        StringAllocator a;
        a.note_on(45);  // string 6, fret 5
        a.note_on(54);  // string 5, fret 9
        CHECK(is(a.note_on(54 + 6), 3, Step::shape));  // string 4 fret 10 is too far from 5; string 3 fret 5
    }
}

// No free string keeps the span: the lowest free string that can play it
TEST(range_step) {
    StringAllocator a;
    a.note_on(60);  // string 6, fret 20
    CHECK(is(a.note_on(47), 5, Step::range));  // only string 5 has it, at fret 2
    StringAllocator b;
    b.note_on(50);  // string 6, fret 10
    CHECK(is(b.note_on(49), 5, Step::range));  // string 5, fret 4: a span of 6
}

// No free string can play it: the lowest free string
TEST(free_step) {
    StringAllocator a;
    a.note_on(60);  // string 6
    CHECK(is(a.note_on(40), 5, Step::free));  // only string 6 has E2
    StringAllocator b;
    CHECK(is(b.note_on(20), 6, Step::free));  // out of every range
    CHECK(is(b.note_on(100), 5, Step::free));
}

// Every string busy: the oldest note is ended and its string reused
TEST(steal_oldest) {
    StringAllocator a;
    int pitches[] = {40, 47, 52, 56, 59, 64};
    for (int p : pitches) a.note_on(p);
    CHECK(is(a.note_on(70), 6, Step::steal, 40));
    CHECK(a.string_of(40) == 0 && a.string_of(70) == 6 && a.sounding() == 6);
    CHECK(a.note_off(40) == 0);  // the stolen note's own note-off changes nothing
    CHECK(a.sounding() == 6);
    // the oldest is now B2 on string 5; a string freed and reused is new again
    CHECK(a.note_off(52) == 4);
    CHECK(is(a.note_on(52), 4, Step::range));  // E4 sits at "fret 30" on string 6
    CHECK(is(a.note_on(72), 5, Step::steal, 47));
    CHECK(is(a.note_on(73), 3, Step::steal, 56));
}

TEST(note_off) {
    StringAllocator a;
    CHECK(a.note_off(50) == 0);  // never started
    a.note_on(50);
    a.note_on(55);
    CHECK(a.note_off(50) == 6);
    CHECK(a.note_off(50) == 0);
    CHECK(a.pitch_on(6) == -1 && a.string_of(50) == 0 && a.sounding() == 1);
    CHECK(is(a.note_on(50), 6, Step::shape));  // free again
}

// A note-on for a pitch still sounding ends it first
TEST(repeated_note_on) {
    StringAllocator a;
    a.note_on(50);
    CHECK(is(a.note_on(50), 6, Step::shape, 50));
    CHECK(a.sounding() == 1);
}

TEST(weights) {
    float equal[6] = {0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f};
    {
        StringAllocator a;
        CHECK(is(a.note_on(60, equal), 6, Step::shape));  // ties: the lowest string
    }
    {
        StringAllocator a;
        float w[6] = {0, 0.1f, 0.6f, 0.2f, 0.1f, 0};
        CHECK(is(a.note_on(60, w), 3, Step::shape));
    }
    {  // a heavier string that can't play the pitch is never chosen while one can
        StringAllocator a;
        float w[6] = {0.9f, 0, 0, 0, 0, 0.1f};
        CHECK(is(a.note_on(50, w), 6, Step::shape));
    }
    {  // a heavier string that breaks the span loses to one that keeps it
        StringAllocator a;
        a.note_on(45);  // string 6, fret 5
        float w[6] = {0, 0, 0, 0.2f, 0.8f, 0};
        CHECK(is(a.note_on(55, w), 4, Step::shape));
    }
    {  // weights choose within the range step too
        StringAllocator a;
        a.note_on(64);  // string 6, fret 24
        float w[6] = {0, 0, 0.2f, 0.5f, 0.3f, 0};
        CHECK(is(a.note_on(57, w), 4, Step::range));
    }
    {  // and within the free step
        StringAllocator a;
        a.note_on(60);
        float w[6] = {0, 0.7f, 0, 0, 0.3f, 0};
        CHECK(is(a.note_on(40, w), 2, Step::free));
    }
}

TEST(clear) {
    StringAllocator a;
    a.note_on(40);
    a.note_on(60);
    a.clear();
    CHECK(a.sounding() == 0 && a.string_of(40) == 0 && a.pitch_on(6) == -1);
    CHECK(is(a.note_on(60), 6, Step::shape));
}

using g2m::OnsetType;

static bool continues(const StringAllocator::Assignment& a, int string, int continued) {
    return a.string == string && a.step == Step::legato && a.continued_pitch == continued && a.stolen_pitch == -1;
}

// Step 0: a legato or slide note continues the note on a string that can
// play it, within reach, and ends it
TEST(legato_continues_the_note_on_its_string) {
    StringAllocator a;
    a.note_on(45);  // string 6, fret 5
    CHECK(continues(a.note_on(47, nullptr, OnsetType::legato), 6, 45));  // hammer-on: fret 7, same string
    CHECK(a.string_of(45) == 0 && a.string_of(47) == 6 && a.pitch_on(6) == 47 && a.sounding() == 1);
    CHECK(a.note_off(45) == 0);  // the continued note's own note-off changes nothing
    CHECK(continues(a.note_on(42, nullptr, OnsetType::legato), 6, 47));  // pull-off: 5 down, at the reach
    CHECK(continues(a.note_on(54, nullptr, OnsetType::slide), 6, 42));  // a slide reaches 12
    CHECK(a.note_off(54) == 6 && a.sounding() == 0);
}

TEST(legato_out_of_reach_is_placed_as_picked) {
    {  // legato 6 away: picked rules, here the next string
        StringAllocator a;
        a.note_on(45);
        StringAllocator::Assignment r = a.note_on(51, nullptr, OnsetType::legato);
        CHECK(r.step != Step::legato && r.string == 5 && r.continued_pitch == -1 && a.string_of(45) == 6);
    }
    {  // a slide 13 away
        StringAllocator a;
        a.note_on(45);
        CHECK(a.note_on(58, nullptr, OnsetType::slide).step != Step::legato);
    }
    {  // nothing sounding
        StringAllocator a;
        CHECK(is(a.note_on(50, nullptr, OnsetType::legato), 6, Step::shape));
    }
    {  // the string can't play the new pitch: E4 (string 1 open) pulled off to D#4 is out of its range
        StringAllocator a;
        float w[6] = {1, 0, 0, 0, 0, 0};
        a.note_on(64, w);  // string 1
        StringAllocator::Assignment r = a.note_on(63, nullptr, OnsetType::legato);
        CHECK(r.step != Step::legato && r.string != 1 && a.string_of(64) == 1);
    }
    {  // a picked note never continues one
        StringAllocator a;
        a.note_on(45);
        CHECK(a.note_on(47).step != Step::legato && a.sounding() == 2);
    }
}

TEST(legato_chooses_by_weight_then_distance_then_newest) {
    {  // without weights: the nearer pitch
        StringAllocator a;
        a.note_on(45);  // string 6
        a.note_on(50);  // string 5, fret 5
        CHECK(continues(a.note_on(48, nullptr, OnsetType::legato), 5, 50));  // 2 from D3, 3 from A2
    }
    {  // equally near: the newer note
        StringAllocator a;
        a.note_on(45);  // string 6
        a.note_on(49);  // string 5, fret 4
        CHECK(continues(a.note_on(47, nullptr, OnsetType::legato), 5, 49));
        StringAllocator b;
        b.note_on(49);  // string 6, fret 9
        b.note_on(45);  // string 5 open
        CHECK(continues(b.note_on(47, nullptr, OnsetType::legato), 5, 45));
    }
    {  // the heads' weight before distance
        StringAllocator a;
        a.note_on(45);  // string 6
        a.note_on(50);  // string 5
        float w[6] = {0, 0, 0, 0, 0.3f, 0.7f};
        CHECK(continues(a.note_on(48, w, OnsetType::legato), 6, 45));
        StringAllocator b;
        b.note_on(45);
        b.note_on(50);
        float equal[6] = {0, 0, 0, 0, 0.5f, 0.5f};  // equal weights: distance decides
        CHECK(continues(b.note_on(48, equal, OnsetType::legato), 5, 50));
    }
}

// A repeated note-on that is also legato: the pitch is ended first, then
// it may continue another note
TEST(repeated_legato_note_on) {
    StringAllocator a;
    a.note_on(45);  // string 6
    a.note_on(50);  // string 5
    StringAllocator::Assignment r = a.note_on(50, nullptr, OnsetType::legato);
    CHECK(r.stolen_pitch == 50 && r.step == Step::legato && r.string == 6 && r.continued_pitch == 45);
    CHECK(a.sounding() == 1 && a.string_of(50) == 6);
}

// A legato note is a new note: it isn't the oldest when a string is stolen
TEST(legato_note_is_newest) {
    StringAllocator a;
    for (int p : {40, 47, 52, 56, 59, 64}) a.note_on(p);  // strings 6 ... 1, E2 the oldest
    CHECK(continues(a.note_on(42, nullptr, OnsetType::legato), 6, 40));  // E2 → F#2 on string 6
    CHECK(is(a.note_on(70), 5, Step::steal, 47));  // B2 is now the oldest
}
