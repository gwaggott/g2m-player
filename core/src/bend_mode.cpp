#include <g2m/bend_mode.h>

#include <algorithm>
#include <cmath>

namespace g2m {

int bend_step(int step, float semitones) {
    float reach = 0.5f + kBendStepHysteresis;
    if (semitones >= step + reach || semitones <= step - reach) return static_cast<int>(std::lround(semitones));
    return step;
}

void BendModeFilter::note(const NoteEvent& e, NoteSink& out) {
    int p = e.pitch;
    if (e.velocity == 0) {
        end(p, e.sample, out);
        return;
    }
    end(p, e.sample, out);  // a note-on with no note-off between
    Note n;
    n.on = true;
    n.mode = mode_;
    n.velocity = e.velocity;
    if (n.mode == BendMode::retrigger) n.step = bend_step(0, e.semitones);
    n.out = std::clamp(p + n.step, 0, 127);
    if (int other = owner_[n.out]) end(other - 1, e.sample, out);  // a retriggered note was there
    NoteEvent sent = e;
    sent.pitch = n.out;
    if (n.mode != BendMode::natural) {
        sent.bend = 8192;
        sent.semitones = 0;
    }
    notes_[p] = n;
    owner_[n.out] = p + 1;
    out.note(sent);
}

void BendModeFilter::bend(const BendEvent& e, NoteSink& out) {
    Note& n = notes_[e.pitch];
    if (!n.on || n.mode == BendMode::off) return;
    if (n.mode == BendMode::natural) {
        out.bend(e);
        return;
    }
    int step = bend_step(n.step, e.semitones);
    if (step == n.step) return;
    int to = std::clamp(e.pitch + step, 0, 127);
    if (owner_[to] && owner_[to] != e.pitch + 1) return;  // another note is there: wait
    n.step = step;
    if (to == n.out) return;  // clamped
    out.retrigger(e.sample, n.out, to, n.velocity);
    owner_[n.out] = 0;
    owner_[to] = e.pitch + 1;
    n.out = to;
}

void BendModeFilter::reset() {
    for (Note& n : notes_) n = Note();
    std::fill(owner_, owner_ + 128, 0);
}

void BendModeFilter::end(int pitch, std::uint64_t sample, NoteSink& out) {
    Note& n = notes_[pitch];
    if (!n.on) return;
    NoteEvent off;
    off.sample = sample;
    off.pitch = n.out;
    off.velocity = 0;
    n.on = false;
    owner_[n.out] = 0;
    out.note(off);
}

}  // namespace g2m
