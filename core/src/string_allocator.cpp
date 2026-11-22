#include <g2m/string_allocator.h>

#include <algorithm>

namespace g2m {

int string_low(int string) {
    return string == kStrings ? kDropPitch : kOpenPitch[string - 1];
}

int string_high(int string) {
    return kOpenPitch[string - 1] + kFrets;
}

int span_fret(int string, int pitch) {
    return std::max(pitch - kOpenPitch[string - 1], 0);
}

StringAllocator::Assignment StringAllocator::note_on(int pitch, const float* weights, OnsetType type) {
    Assignment a{0, Step::shape, -1};
    if (string_of_[pitch]) {
        a.stolen_pitch = pitch;
        note_off(pitch);
    }
    if (type != OnsetType::picked && (a.string = continued_string(pitch, weights, type))) {
        a.step = Step::legato;
        a.continued_pitch = pitch_on_[a.string - 1];
        note_off(a.continued_pitch);
        pitch_on_[a.string - 1] = pitch;
        started_[a.string - 1] = count_++;
        string_of_[pitch] = static_cast<std::int8_t>(a.string);
        return a;
    }
    bool free[kStrings], playable[kStrings], fits[kStrings];
    for (int s = 1; s <= kStrings; ++s) {
        free[s - 1] = pitch_on_[s - 1] < 0;
        playable[s - 1] = free[s - 1] && pitch >= string_low(s) && pitch <= string_high(s);
        fits[s - 1] = playable[s - 1] && fits_hand_span(s, pitch);
    }
    if ((a.string = best(fits, weights))) {
        a.step = Step::shape;
    } else if ((a.string = best(playable, weights))) {
        a.step = Step::range;
    } else if ((a.string = best(free, weights))) {
        a.step = Step::free;
    } else {
        a.step = Step::steal;
        a.string = 1;
        for (int s = 2; s <= kStrings; ++s)
            if (started_[s - 1] < started_[a.string - 1]) a.string = s;
        a.stolen_pitch = pitch_on_[a.string - 1];
        note_off(a.stolen_pitch);
    }
    pitch_on_[a.string - 1] = pitch;
    started_[a.string - 1] = count_++;
    string_of_[pitch] = static_cast<std::int8_t>(a.string);
    return a;
}

int StringAllocator::note_off(int pitch) {
    int s = string_of_[pitch];
    if (!s) return 0;
    string_of_[pitch] = 0;
    pitch_on_[s - 1] = -1;
    return s;
}

int StringAllocator::move(int from, int to) {
    int s = string_of_[from];
    if (!s || string_of_[to]) return 0;
    string_of_[from] = 0;
    string_of_[to] = static_cast<std::int8_t>(s);
    pitch_on_[s - 1] = to;
    return s;
}

void StringAllocator::clear() {
    *this = StringAllocator();
}

int StringAllocator::string_of(int pitch) const {
    return string_of_[pitch];
}

int StringAllocator::pitch_on(int string) const {
    return pitch_on_[string - 1];
}

int StringAllocator::sounding() const {
    int n = 0;
    for (int p : pitch_on_) n += p >= 0;
    return n;
}

// Step 0: the string of the sounding note a legato or slide note continues:
// a string that can play `pitch`, sounding a note 1 to kLegatoDistance
// (legato) or kSlideDistance (slide) semitones away. Of several, the highest
// weight, then the nearest pitch, then the newest note. 0 if none.
int StringAllocator::continued_string(int pitch, const float* weights, OnsetType type) const {
    int reach = type == OnsetType::slide ? kSlideDistance : kLegatoDistance;
    int chosen = 0, chosen_distance = 0;
    for (int s = 1; s <= kStrings; ++s) {
        int q = pitch_on_[s - 1];
        int distance = q < 0 ? 0 : q > pitch ? q - pitch : pitch - q;
        if (distance < 1 || distance > reach || pitch < string_low(s) || pitch > string_high(s)) continue;
        float w = weights ? weights[s - 1] : 0, best = weights && chosen ? weights[chosen - 1] : 0;
        bool better = !chosen || w > best ||
                      (w == best && (distance < chosen_distance ||
                                     (distance == chosen_distance && started_[s - 1] > started_[chosen - 1])));
        if (better) {
            chosen = s;
            chosen_distance = distance;
        }
    }
    return chosen;
}

// Whether the fretted notes sounding, with `pitch` on `string`, are within
// kHandSpan frets of each other. Open strings don't count.
bool StringAllocator::fits_hand_span(int string, int pitch) const {
    int lo = span_fret(string, pitch), hi = lo;
    for (int s = 1; s <= kStrings; ++s) {
        int p = pitch_on_[s - 1];
        if (p < 0) continue;
        int f = span_fret(s, p);
        if (f == 0) continue;
        if (lo == 0) lo = hi = f;  // the new note is open
        lo = std::min(lo, f);
        hi = std::max(hi, f);
    }
    return hi - lo <= kHandSpan;
}

// The allowed string with the highest weight, ties to the lowest string;
// 0 if none is allowed
int StringAllocator::best(const bool* allowed, const float* weights) const {
    int chosen = 0;
    for (int s = kStrings; s >= 1; --s) {
        if (!allowed[s - 1]) continue;
        if (!chosen || (weights && weights[s - 1] > weights[chosen - 1])) chosen = s;
    }
    return chosen;
}

}  // namespace g2m
