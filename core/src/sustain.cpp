#include <g2m/sustain.h>

namespace g2m {

void Sustain::set_mode(SustainMode mode, std::uint64_t sample, EventSink& out) {
    if (mode == mode_) return;
    release(sample, out);
    mode_ = mode;
    if (pedal_ && mode_ != SustainMode::off) {
        for (int p = 0; p < 128; ++p) latched_[p] = sent_[p];  // as if the pedal went down now
    }
}

void Sustain::set_pedal(bool down, std::uint64_t sample, EventSink& out) {
    if (down == pedal_) return;
    pedal_ = down;
    if (down) {
        for (int p = 0; p < 128; ++p) latched_[p] = sent_[p];
    } else {
        release(sample, out);
    }
}

void Sustain::note(const NoteEvent& e, EventSink& out) {
    int p = e.pitch;
    bool engaged = pedal_ && mode_ != SustainMode::off;
    if (e.velocity == 0) {
        if (!sent_[p]) return;  // already ended, or never sent
        bool defer = engaged && (mode_ == SustainMode::piano || latched_[p]);
        if (defer) {
            held_[p] = true;
            return;
        }
        sent_[p] = false;
        out.note(e);
        return;
    }
    if (engaged && mode_ == SustainMode::freeze) return;
    if (sent_[p]) end(p, e.sample, out);  // played again while held
    latched_[p] = false;
    sent_[p] = true;
    out.note(e);
}

void Sustain::bend(const BendEvent& e, EventSink& out) {
    if (!sent_[e.pitch]) return;
    if (pedal_ && mode_ == SustainMode::freeze && latched_[e.pitch]) return;
    out.bend(e);
}

void Sustain::reset() {
    for (int p = 0; p < 128; ++p) sent_[p] = latched_[p] = held_[p] = false;
}

// Ends what the pedal holds: the deferred notes, and in freeze every latched one.
void Sustain::release(std::uint64_t sample, EventSink& out) {
    for (int p = 0; p < 128; ++p) {
        bool frozen = mode_ == SustainMode::freeze && latched_[p];
        if (sent_[p] && (held_[p] || frozen)) end(p, sample, out);
        latched_[p] = false;
    }
}

void Sustain::end(int pitch, std::uint64_t sample, EventSink& out) {
    NoteEvent off;
    off.sample = sample;
    off.pitch = pitch;
    off.velocity = 0;
    sent_[pitch] = held_[pitch] = false;
    out.note(off);
}

}  // namespace g2m
