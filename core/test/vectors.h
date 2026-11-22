// Reading the test vectors in test/data/ (written by make_vectors.py).
#pragma once

#include <g2m/model.h>

#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "check.h"
#include "json.h"

namespace vectors {

inline const std::string kData = G2M_TEST_DATA;
inline const char* const kNames[] = {"small", "odd"};  // format version 2, no MPE heads
inline const char* const kMpe = "mpe";  // version 3 with the string and onset-type heads
inline const char* const kBend = "bend";  // version 4: those and the pitch-offset head
inline const char* const kProj = "proj";  // version 5: kMpe's model with its gated layers projected
inline const char* const kHalf = "half";  // version 6: kBend's model projected, with float16 weights

// Raw little-endian float32 (a little-endian host is assumed in the tests).
inline std::vector<float> read_f32(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    std::vector<char> bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    CHECK_MSG(!bytes.empty() && bytes.size() % 4 == 0, "can't read %s", path.c_str());
    std::vector<float> out(bytes.size() / 4);
    std::memcpy(out.data(), bytes.data(), out.size() * 4);
    return out;
}

inline std::shared_ptr<const g2m::Model> load(const std::string& name) {
    g2m::LoadResult r = g2m::load_model(kData + "/" + name + ".g2m");
    CHECK_MSG(r.model, "%s", r.error.c_str());
    return std::move(r.model);
}

inline std::vector<float> input(const std::string& name) {
    return read_f32(kData + "/" + name + ".input.f32");
}

// [blocks][n_pitches][3 + the MPE heads' classes]
inline std::vector<float> outputs(const std::string& name) {
    return read_f32(kData + "/" + name + ".outputs.f32");
}

struct Event {
    long frame;  // the block it was emitted after; blocks for the end-of-stream note-offs
    int pitch;
    int velocity;  // 0 for a note-off
    // a note-on's MPE heads (models with heads): string probabilities and onset type
    std::vector<float> strings;
    std::string onset_type;
    // version 4: a bend event (velocity holds its 14-bit value), or a
    // note-on's bend; the offset in semitones after the dead band
    bool is_bend = false;
    bool has_bend = false;
    int bend = 8192;
    float semitones = 0;
    bool operator==(const Event& o) const {
        return frame == o.frame && pitch == o.pitch && velocity == o.velocity && is_bend == o.is_bend;
    }
};

struct Events {
    long blocks = 0;
    std::vector<Event> events;
};

// NAME.events.json
inline Events events(const std::string& name) {
    std::ifstream file(kData + "/" + name + ".events.json");
    std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    g2m::json::Value root;
    std::string error;
    Events out;
    if (!g2m::json::parse(text, root, error)) {
        CHECK_MSG(false, "%s.events.json: %s", name.c_str(), error.c_str());
        return out;
    }
    int blocks = 0;
    root.get("blocks")->as_int(blocks);
    out.blocks = blocks;
    for (const g2m::json::Value& e : root.get("events")->items) {
        int frame = 0, pitch = 0, velocity = 0;
        e.items[0].as_int(frame);
        e.items[2].as_int(pitch);
        e.items[3].as_int(velocity);
        Event event{frame, pitch, velocity, {}, {}};
        if (e.items[1].string == "bend") {  // [frame, "bend", pitch, value, semitones]
            event.is_bend = event.has_bend = true;
            event.bend = velocity;
            event.semitones = static_cast<float>(e.items[4].number);
            out.events.push_back(event);
            continue;
        }
        bool on = e.items[1].string == "on";
        CHECK(on == (velocity > 0));
        if (e.items.size() >= 6) {
            for (const g2m::json::Value& p : e.items[4].items) event.strings.push_back(static_cast<float>(p.number));
            event.onset_type = e.items[5].string;
        }
        if (e.items.size() == 8) {
            e.items[6].as_int(event.bend);
            event.has_bend = true;
            event.semitones = static_cast<float>(e.items[7].number);
        }
        out.events.push_back(event);
    }
    return out;
}

}  // namespace vectors
