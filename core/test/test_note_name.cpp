#include <g2m/note_name.h>

#include <cstring>

#include "check.h"

static bool named(int pitch, const char* expected) {
    char name[g2m::kNoteNameSize];
    int n = g2m::note_name(pitch, name);
    return std::strcmp(name, expected) == 0 && n == static_cast<int>(std::strlen(expected));
}

TEST(note_names) {
    CHECK(named(40, "E2"));  // low E string
    CHECK(named(38, "D2"));  // the model's range: D2-E6
    CHECK(named(88, "E6"));
    CHECK(named(60, "C4"));
    CHECK(named(61, "C#4"));
    CHECK(named(69, "A4"));
    CHECK(named(0, "C-1"));
    CHECK(named(1, "C#-1"));
    CHECK(named(127, "G9"));
    CHECK(named(-1, "?") && named(128, "?"));
}
