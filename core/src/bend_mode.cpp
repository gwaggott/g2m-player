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
        end(p, e.sample, out, true);
        return;
    }
    end(p, e.sample, out, false);  // a note-on with no note-off between
    float s = e.has_bend ? e.semitones : 0.0f;
    Note n;
    n.on = true;
    n.mode = mode_ == BendMode::autobend && !plain_ ? BendMode::natural : mode_;
    n.velocity = e.velocity;
    if (e.has_bend) n.bend = e.bend;
    n.semitones = s;
    n.pitch = p;
    n.order = ++order_;
    // Auto: a note alone on the channel no longer is; it steps to its nearest semitone
    if (plain_ && sounding_ == 1 && lead_ >= 0 && notes_[lead_].mode == BendMode::autobend) {
        Note& alone = notes_[lead_];
        set_channel(e.sample, 8192, out);
        move(alone, static_cast<int>(std::lround(alone.semitones)), e.sample, out);
    }
    if (n.mode == BendMode::retrigger) n.step = bend_step(0, s);
    if (n.mode == BendMode::autobend) {
        if (sounding_ > 0) n.step = bend_step(0, s);
        else if (std::fabs(s) > range_) n.step = static_cast<int>(std::lround(s));
    }
    n.out = std::clamp(p + n.step, 0, 127);
    if (int other = owner_[n.out]) end(other - 1, e.sample, out, false);  // a retriggered note was there
    NoteEvent sent = e;
    sent.pitch = n.out;
    if (n.mode != BendMode::natural) {
        sent.bend = 8192;
        sent.semitones = 0;
    }
    notes_[p] = n;
    owner_[n.out] = p + 1;
    ++sounding_;
    if (plain_) {
        lead_ = p;
        set_channel(e.sample, wanted(notes_[p]), out);
    }
    out.note(sent);
}

void BendModeFilter::bend(const BendEvent& e, NoteSink& out) {
    Note& n = notes_[e.pitch];
    if (!n.on || n.mode == BendMode::off) return;
    n.bend = e.bend;
    n.semitones = e.semitones;
    if (n.mode == BendMode::natural) {
        if (!plain_) out.bend(e);
        else if (e.pitch == lead_) set_channel(e.sample, e.bend, out);
        return;
    }
    if (n.mode == BendMode::autobend && sounding_ == 1) {
        if (std::fabs(e.semitones - (n.out - n.pitch)) > range_)
            move(n, static_cast<int>(std::lround(e.semitones)), e.sample, out);
        set_channel(e.sample, rest(n), out);
        return;
    }
    move(n, bend_step(n.step, e.semitones), e.sample, out);
}

void BendModeFilter::move(Note& n, int step, std::uint64_t sample, NoteSink& out) {
    if (step == n.step) return;
    int to = std::clamp(n.pitch + step, 0, 127);
    if (owner_[to] && owner_[to] != n.pitch + 1) return;  // another note is there: wait
    n.step = step;
    if (to == n.out) return;  // clamped
    out.retrigger(sample, n.out, to, n.velocity);
    owner_[n.out] = 0;
    owner_[to] = n.pitch + 1;
    n.out = to;
}

int BendModeFilter::rest(const Note& n) const {
    float r = n.semitones - static_cast<float>(n.out - n.pitch);
    return std::clamp(8192 + static_cast<int>(std::lrintf(r * (8192.0f / static_cast<float>(range_)))), 0, 16383);
}

int BendModeFilter::wanted(const Note& n) const {
    if (n.mode == BendMode::natural) return n.bend;
    if (n.mode == BendMode::autobend && sounding_ == 1) return rest(n);
    return 8192;
}

void BendModeFilter::reset() {
    for (Note& n : notes_) n = Note();
    std::fill(owner_, owner_ + 128, 0);
    channel_ = 8192;
    lead_ = -1;
    sounding_ = 0;
}

void BendModeFilter::set_channel(std::uint64_t sample, int value, NoteSink& out) {
    if (value == channel_) return;
    channel_ = value;
    out.channel_bend(sample, value);
}

void BendModeFilter::end(int pitch, std::uint64_t sample, NoteSink& out, bool hand_over) {
    Note& n = notes_[pitch];
    if (!n.on) return;
    NoteEvent off;
    off.sample = sample;
    off.pitch = n.out;
    off.velocity = 0;
    n.on = false;
    owner_[n.out] = 0;
    --sounding_;
    out.note(off);
    if (!plain_) return;
    if (pitch == lead_) {
        lead_ = -1;
        if (!hand_over) return;  // a note-on follows and sets the channel
        for (int q = 0; q < 128; ++q)
            if (notes_[q].on && (lead_ < 0 || notes_[q].order > notes_[lead_].order)) lead_ = q;
    }
    // the lead's bend (Natural), or the rest of an Auto note now alone
    if (hand_over && lead_ >= 0) set_channel(sample, wanted(notes_[lead_]), out);
}

}  // namespace g2m
