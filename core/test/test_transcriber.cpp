#include <g2m/transcriber.h>

#include <random>
#include <set>

#include "allocations.h"
#include "check.h"
#include "decoder.h"
#include "vectors.h"

struct Collect : g2m::EventSink {
    std::vector<g2m::NoteEvent> events;
    Collect() { events.reserve(100000); }  // so collecting doesn't allocate
    void note(const g2m::NoteEvent& e) override { events.push_back(e); }
};

static bool same(const std::vector<g2m::NoteEvent>& a, const std::vector<g2m::NoteEvent>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i].sample != b[i].sample || a[i].pitch != b[i].pitch || a[i].velocity != b[i].velocity) return false;
    return true;
}

// The reference's events as the Transcriber times them: frame k ends at
// sample (k + 1) * hop; the end-of-stream note-offs at the end of the input.
static std::vector<g2m::NoteEvent> expected_events(const std::string& name, int hop) {
    vectors::Events ref = vectors::events(name);
    std::vector<g2m::NoteEvent> out;
    for (const vectors::Event& e : ref.events) {
        long frames = e.frame < ref.blocks ? e.frame + 1 : ref.blocks;
        out.push_back({static_cast<std::uint64_t>(frames) * hop, e.pitch, e.velocity});
    }
    return out;
}

// Whole input in one call, then all notes off.
static std::vector<g2m::NoteEvent> transcribe(std::shared_ptr<const g2m::Model> model, const std::vector<float>& audio) {
    g2m::Transcriber t(model);
    Collect sink;
    t.process(audio.data(), audio.size(), sink);
    t.all_notes_off(sink);
    return sink.events;
}

TEST(decoder_matches_reference) {
    for (const char* name : vectors::kNames) {
        auto model = vectors::load(name);
        if (!model) continue;
        const g2m::ModelConfig& c = model->config;
        std::vector<float> outputs = vectors::outputs(name);
        vectors::Events ref = vectors::events(name);
        std::vector<vectors::Event> got;
        g2m::Decoder decoder(c.n_pitches, c.pitch_min, model->decoder);
        std::size_t per_block = 3 * c.n_pitches;
        for (long k = 0; k < ref.blocks; ++k)
            decoder.step(outputs.data() + k * per_block, [&](int p, int v) { got.push_back({k, p, v, {}, {}}); });
        decoder.flush([&](int p, int v) { got.push_back({ref.blocks, p, v, {}, {}}); });
        CHECK_MSG(got == ref.events, "%s: %zu events, the reference %zu", name, got.size(), ref.events.size());
        std::fprintf(stderr, "  %s: %zu events\n", name, got.size());
    }
}

TEST(transcriber_matches_reference) {
    for (const char* name : vectors::kNames) {
        auto model = vectors::load(name);
        if (!model) continue;
        std::vector<g2m::NoteEvent> got = transcribe(model, vectors::input(name));
        CHECK_MSG(same(got, expected_events(name, model->config.hop)), "%s", name);
    }
}

// Timing the stages changes nothing else, and every stage gets some time.
TEST(time_stages) {
    for (const char* name : vectors::kNames) {
        auto model = vectors::load(name);
        if (!model) continue;
        std::vector<float> audio = vectors::input(name);
        g2m::Transcriber t(model);
        g2m::StageTimes times;
        t.time_stages(&times);
        Collect sink;
        t.process(audio.data(), audio.size(), sink);
        t.all_notes_off(sink);
        CHECK_MSG(same(sink.events, expected_events(name, model->config.hop)), "%s", name);
        CHECK(times.frontend > 0 && times.layers > 0 && times.gru > 0 && times.head > 0 && times.decoder > 0);
        g2m::StageTimes before = times;
        t.time_stages(nullptr);
        t.process(audio.data(), audio.size(), sink);
        CHECK(times.frontend == before.frontend && times.decoder == before.decoder);
    }
}

TEST(any_block_size) {
    for (const char* name : vectors::kNames) {
        auto model = vectors::load(name);
        if (!model) continue;
        std::vector<float> audio = vectors::input(name);
        std::vector<g2m::NoteEvent> whole = transcribe(model, audio);
        std::mt19937 rng(7);
        for (int size : {1, 63, 64, 100, 511, 4096, -1}) {  // -1: random sizes, 0 included
            std::uniform_int_distribution<int> random_size(0, 300);
            g2m::Transcriber t(model);
            Collect sink;
            for (std::size_t i = 0; i < audio.size();) {
                std::size_t n = std::min<std::size_t>(size < 0 ? random_size(rng) : size, audio.size() - i);
                t.process(audio.data() + i, n, sink);
                i += n;
            }
            t.all_notes_off(sink);
            CHECK_MSG(same(sink.events, whole), "%s, block size %d", name, size);
            CHECK(t.position() == audio.size());
        }
    }
}

TEST(all_notes_off_and_reset) {
    auto model = vectors::load("odd");
    if (!model) return;
    std::vector<float> audio = vectors::input("odd");
    std::size_t part = 300 * model->config.hop + 10;  // mid-block
    g2m::Transcriber t(model);
    Collect sink;
    t.process(audio.data(), part, sink);
    std::set<int> held;
    for (const g2m::NoteEvent& e : sink.events) {
        if (e.velocity > 0) held.insert(e.pitch);
        else held.erase(e.pitch);
    }
    CHECK(!held.empty());  // else this test checks nothing

    Collect offs;
    t.all_notes_off(offs);
    CHECK(offs.events.size() == held.size());
    for (const g2m::NoteEvent& e : offs.events) CHECK(e.velocity == 0 && held.count(e.pitch) && e.sample == part);
    Collect again;
    t.all_notes_off(again);
    CHECK(again.events.empty());

    // after a reset, the input from the start gives the same events as a
    // fresh Transcriber, shifted by where the reset happened
    t.process(audio.data() + part, 1000, sink);
    std::uint64_t at = t.position();
    Collect after;
    t.reset(after);
    std::size_t reset_offs = after.events.size();
    for (const g2m::NoteEvent& e : after.events) CHECK(e.velocity == 0 && e.sample == at);
    t.process(audio.data(), audio.size(), after);
    t.all_notes_off(after);
    std::vector<g2m::NoteEvent> shifted(after.events.begin() + reset_offs, after.events.end());
    for (g2m::NoteEvent& e : shifted) e.sample -= at;
    CHECK(same(shifted, transcribe(model, audio)));
}

TEST(decoder_settings) {
    auto model = vectors::load("small");
    if (!model) return;
    g2m::Transcriber t(model);
    CHECK(t.decoder_settings().onset_threshold == model->decoder.onset_threshold);
    CHECK(t.decoder_settings().gate_frames == model->decoder.gate_frames);
    g2m::DecoderSettings never = model->decoder;
    never.onset_threshold = 1.5f;
    never.fallback_frames = 0;  // else the late fallback still starts notes
    t.set_decoder_settings(never);
    CHECK(t.decoder_settings().onset_threshold == 1.5f);
    std::vector<float> audio = vectors::input("small");
    Collect sink;
    t.process(audio.data(), audio.size(), sink);
    CHECK(sink.events.empty());
}

// The test vectors' late fallback starts notes that onsets alone don't.
TEST(late_fallback_adds_notes) {
    auto count_ons = [](const std::vector<g2m::NoteEvent>& events) {
        long n = 0;
        for (const g2m::NoteEvent& e : events) n += e.velocity > 0;
        return n;
    };
    for (const char* name : vectors::kNames) {
        auto model = vectors::load(name);
        if (!model) continue;
        CHECK(model->decoder.fallback_frames > 0);
        std::vector<float> audio = vectors::input(name);
        g2m::Transcriber t(model);
        g2m::DecoderSettings off = model->decoder;
        off.fallback_frames = 0;
        t.set_decoder_settings(off);
        Collect sink;
        t.process(audio.data(), audio.size(), sink);
        CHECK_MSG(count_ons(sink.events) < count_ons(transcribe(model, audio)), "%s", name);
    }
}

TEST(no_allocations) {
    auto model = vectors::load("odd");
    if (!model) return;
    std::vector<float> audio = vectors::input("odd");
    g2m::Transcriber t(model);
    Collect sink;
    long before = allocations;
    for (std::size_t i = 0; i + 100 <= audio.size(); i += 100) t.process(audio.data() + i, 100, sink);
    t.all_notes_off(sink);
    t.reset(sink);
    t.process(audio.data(), 5000, sink);
    CHECK(allocations == before);
    CHECK(!sink.events.empty());
}

namespace {

const char* type_name(g2m::OnsetType t) {
    return t == g2m::OnsetType::legato ? "legato" : t == g2m::OnsetType::slide ? "slide" : "picked";
}

// A decoder event with its note-on's MPE heads, as in NAME.events.json
vectors::Event with_heads(long frame, int pitch, int velocity, const float* heads, const g2m::ModelConfig& c,
                          const g2m::DecoderSettings& s) {
    vectors::Event e{frame, pitch, velocity, {}, {}};
    if (heads) {
        e.strings.assign(heads + c.string_head, heads + c.string_head + 6);
        e.onset_type = type_name(g2m::onset_type(heads + c.onset_type_head, s));
    }
    return e;
}

}  // namespace

// The decoder fed the reference's outputs, MPE heads included, gives its
// events exactly, with each note-on's heads from the right frame.
TEST(decoder_with_heads_matches_reference) {
    auto model = vectors::load(vectors::kMpe);
    if (!model) return;
    const g2m::ModelConfig& c = model->config;
    std::vector<float> outputs = vectors::outputs(vectors::kMpe);
    vectors::Events ref = vectors::events(vectors::kMpe);
    std::size_t width = 3 + c.head_classes;
    g2m::Decoder decoder(c.n_pitches, c.pitch_min, model->decoder, c.head_classes);
    std::vector<float> notes(3 * c.n_pitches);
    std::vector<vectors::Event> got;
    long computed = 0;
    for (long k = 0; k < ref.blocks; ++k) {
        const float* block = outputs.data() + k * c.n_pitches * width;
        for (int p = 0; p < c.n_pitches; ++p) std::copy(block + p * width, block + p * width + 3, notes.begin() + 3 * p);
        decoder.step(
            notes.data(),
            [&](int p, float* out) {
                ++computed;
                std::copy(block + p * width + 3, block + (p + 1) * width, out);
            },
            [&](int pitch, int velocity, const float* heads) {
                got.push_back(with_heads(k, pitch, velocity, heads, c, model->decoder));
            });
    }
    decoder.flush([&](int p, int v) { got.push_back({ref.blocks, p, v, {}, {}}); });
    bool same_events = got == ref.events;
    bool same_heads = got.size() == ref.events.size();
    for (std::size_t i = 0; same_heads && i < got.size(); ++i)
        same_heads = got[i].strings == ref.events[i].strings && got[i].onset_type == ref.events[i].onset_type;
    CHECK_MSG(same_events && same_heads, "%zu events, the reference %zu", got.size(), ref.events.size());
    int types[3] = {};
    for (const vectors::Event& e : got)
        if (e.velocity) types[e.onset_type == "picked" ? 0 : e.onset_type == "legato" ? 1 : 2]++;
    CHECK(types[0] > 0 && types[1] > 0 && types[2] > 0);
    // the heads are computed only in a few frames per note, not every frame
    std::fprintf(stderr, "  mpe: heads computed for %ld of %ld pitch-frames\n", computed, ref.blocks * c.n_pitches);
    CHECK(computed < ref.blocks * c.n_pitches / 10);
}

// The whole Transcriber on the input: the reference's events, onset types
// and string probabilities (within float rounding of the network).
TEST(transcriber_with_heads_matches_reference) {
    auto model = vectors::load(vectors::kMpe);
    if (!model) return;
    std::vector<float> audio = vectors::input(vectors::kMpe);
    g2m::Transcriber t(model);
    Collect sink;
    long before = allocations;
    for (std::size_t i = 0; i < audio.size(); i += 100) t.process(audio.data() + i, std::min<std::size_t>(100, audio.size() - i), sink);
    CHECK(allocations == before);
    t.all_notes_off(sink);
    vectors::Events ref = vectors::events(vectors::kMpe);
    CHECK(same(sink.events, expected_events(vectors::kMpe, model->config.hop)));
    float worst = 0;
    bool types_ok = sink.events.size() == ref.events.size();
    for (std::size_t i = 0; types_ok && i < sink.events.size(); ++i) {
        const g2m::NoteEvent& e = sink.events[i];
        const vectors::Event& r = ref.events[i];
        types_ok = e.has_strings == (e.velocity > 0) && (e.velocity == 0 || type_name(e.onset_type) == r.onset_type);
        for (int s = 0; types_ok && e.velocity && s < 6; ++s) worst = std::max(worst, std::fabs(e.strings[s] - r.strings[s]));
    }
    CHECK(types_ok);
    CHECK_MSG(worst <= 1e-5f, "string probabilities differ by %g", worst);

    // last_heads: every pitch's heads for the last block, as in the outputs
    std::vector<float> heads(model->config.n_pitches * model->config.head_classes);
    t.last_heads(heads.data());
    std::vector<float> outputs = vectors::outputs(vectors::kMpe);
    std::size_t width = 3 + model->config.head_classes;
    const float* last = outputs.data() + outputs.size() - model->config.n_pitches * width;
    float diff = 0;
    for (int p = 0; p < model->config.n_pitches; ++p)
        for (int i = 0; i < model->config.head_classes; ++i)
            diff = std::max(diff, std::fabs(heads[p * model->config.head_classes + i] - last[p * width + 3 + i]));
    CHECK(diff <= 1e-5f);
}

// Version 2 models have no heads: every note-on is picked, with no strings.
TEST(no_heads_means_picked) {
    for (const char* name : vectors::kNames) {
        auto model = vectors::load(name);
        if (!model) continue;
        CHECK(model->config.heads.empty() && model->config.head_classes == 0);
        for (const g2m::NoteEvent& e : transcribe(model, vectors::input(name)))
            CHECK(!e.has_strings && e.onset_type == g2m::OnsetType::picked);
    }
}

TEST(onset_type_decision) {
    g2m::DecoderSettings s;
    s.legato_threshold = 0.3f;
    s.slide_threshold = 0.6f;
    float picked[] = {0.8f, 0.2f, 0.0f}, legato[] = {0.5f, 0.3f, 0.2f}, slide[] = {0.0f, 0.4f, 0.6f},
          both[] = {0.1f, 0.35f, 0.55f};
    CHECK(g2m::onset_type(picked, s) == g2m::OnsetType::picked);
    CHECK(g2m::onset_type(legato, s) == g2m::OnsetType::legato);  // at the threshold
    CHECK(g2m::onset_type(slide, s) == g2m::OnsetType::slide);  // slide first, at its threshold
    CHECK(g2m::onset_type(both, s) == g2m::OnsetType::legato);
}

// Format version 4: the 14-bit conversion of a pitch offset
TEST(bend_value_conversion) {
    g2m::DecoderSettings s;  // dead band 0.25, range 12: 8192 / 12 = 682.67 per semitone
    auto value = [&](float offset) { return g2m::bend_value(offset, s).value; };
    CHECK(value(0) == 8192 && value(0.2499f) == 8192 && value(-0.2499f) == 8192);  // the dead band
    CHECK(value(0.25f) == 8192 + 171 && g2m::bend_value(0.25f, s).semitones == 0.25f);  // 170.67 → 171
    CHECK(g2m::bend_value(0.1f, s).semitones == 0.0f);
    CHECK(value(1) == 8192 + 683 && value(-1) == 8192 - 683);
    CHECK(value(12) == 16383 && value(-12) == 0 && value(20) == 16383 && value(-20) == 0);  // clamped
    s.bend_range = 2;  // 4096 per semitone: half-way values round to even
    s.bend_dead_band = 0;
    CHECK(value(0.5f / 4096) == 8192 && value(1.5f / 4096) == 8194 && value(-0.5f / 4096) == 8192);
    CHECK(value(1) == 8192 + 4096 && value(2) == 16383 && value(-2) == 0);
    s.bend_range = 48;
    CHECK(value(12) == 8192 + 2048);
}

// The decoder fed the reference's outputs, curve head included, gives its
// events exactly: note-ons with their bends, and the bend events
TEST(decoder_with_bends_matches_reference) {
    auto model = vectors::load(vectors::kBend);
    if (!model) return;
    const g2m::ModelConfig& c = model->config;
    CHECK(c.curve_heads == 1 && c.pitch_offset_curve == 0 && c.head_classes == 9);
    std::vector<float> outputs = vectors::outputs(vectors::kBend);
    vectors::Events ref = vectors::events(vectors::kBend);
    std::size_t width = 3 + c.head_classes + 1;  // string, onset type, then the pitch offset (file order)
    g2m::Decoder decoder(c.n_pitches, c.pitch_min, model->decoder, c.head_classes, true);
    std::vector<float> notes(3 * c.n_pitches);
    std::vector<vectors::Event> got;
    long offsets = 0;
    for (long k = 0; k < ref.blocks; ++k) {
        const float* block = outputs.data() + k * c.n_pitches * width;
        for (int p = 0; p < c.n_pitches; ++p) std::copy(block + p * width, block + p * width + 3, notes.begin() + 3 * p);
        decoder.step(
            notes.data(), [&](int p, float* out) { std::copy(block + p * width + 3, block + p * width + 12, out); },
            [&](int p) {
                ++offsets;
                return block[p * width + 12];
            },
            [&](int pitch, int velocity, const float* heads, const g2m::BendValue* bend) {
                vectors::Event e = with_heads(k, pitch, velocity, heads, c, model->decoder);
                CHECK((bend != nullptr) == (velocity > 0));
                if (bend) {
                    e.has_bend = true;
                    e.bend = bend->value;
                    e.semitones = bend->semitones;
                }
                got.push_back(e);
            },
            [&](int pitch, const g2m::BendValue& bend) {
                vectors::Event e{k, pitch, bend.value, {}, {}};
                e.is_bend = e.has_bend = true;
                e.bend = bend.value;
                e.semitones = bend.semitones;
                got.push_back(e);
            });
    }
    decoder.flush([&](int p, int v) { got.push_back({ref.blocks, p, v, {}, {}}); });
    bool same_all = got == ref.events;
    for (std::size_t i = 0; same_all && i < got.size(); ++i) {
        const vectors::Event &a = got[i], &b = ref.events[i];
        same_all = a.strings == b.strings && a.onset_type == b.onset_type && a.has_bend == b.has_bend &&
                   a.bend == b.bend && a.semitones == b.semitones;
    }
    long bends = 0, nonzero = 0;
    for (const vectors::Event& e : got) {
        bends += e.is_bend;
        nonzero += e.has_bend && e.bend != 8192;
    }
    CHECK_MSG(same_all, "%zu events, the reference %zu", got.size(), ref.events.size());
    CHECK(bends > 1000 && nonzero > 1000);
    std::fprintf(stderr, "  bend: %zu events, %ld bend events; offsets read for %ld of %ld pitch-frames\n", got.size(),
                 bends, offsets, ref.blocks * c.n_pitches);
}

// The whole Transcriber: the reference's events and bends (version 4's
// `bend`, version 6's `half`)
static void transcriber_with_bends_matches_reference(const char* name) {
    auto model = vectors::load(name);
    if (!model) return;
    std::vector<float> audio = vectors::input(name);
    struct Bends : g2m::EventSink {
        std::vector<g2m::NoteEvent> notes;
        std::vector<g2m::BendEvent> bends;
        std::vector<int> order;  // 0 note, 1 bend
        Bends() {
            notes.reserve(10000);
            bends.reserve(100000);
            order.reserve(110000);
        }
        void note(const g2m::NoteEvent& e) override {
            notes.push_back(e);
            order.push_back(0);
        }
        void bend(const g2m::BendEvent& e) override {
            bends.push_back(e);
            order.push_back(1);
        }
    } sink;
    g2m::Transcriber t(model);
    long before = allocations;
    for (std::size_t i = 0; i < audio.size(); i += 100) t.process(audio.data() + i, std::min<std::size_t>(100, audio.size() - i), sink);
    CHECK(allocations == before);
    t.all_notes_off(sink);
    vectors::Events ref = vectors::events(name);
    std::size_t n = 0, b = 0, mismatches = 0;
    bool aligned = sink.order.size() == ref.events.size();
    for (std::size_t i = 0; aligned && i < ref.events.size(); ++i) {
        const vectors::Event& r = ref.events[i];
        std::uint64_t sample = static_cast<std::uint64_t>(std::min(r.frame + 1, ref.blocks)) * model->config.hop;
        if (r.is_bend) {
            aligned = sink.order[i] == 1;
            const g2m::BendEvent& e = sink.bends[b++];
            aligned = aligned && e.sample == sample && e.pitch == r.pitch;
            mismatches += e.bend != r.bend;
        } else {
            aligned = sink.order[i] == 0;
            const g2m::NoteEvent& e = sink.notes[n++];
            aligned = aligned && e.sample == sample && e.pitch == r.pitch && e.velocity == r.velocity &&
                      e.has_bend == r.has_bend;
            mismatches += e.has_bend && e.bend != r.bend;
        }
    }
    CHECK_MSG(aligned && mismatches == 0, "%s: %zu events, the reference %zu; %zu bend values differ", name,
              sink.order.size(), ref.events.size(), mismatches);
    CHECK(b > 0 && n > 0);
}

// ... and a v3 model sends no bends
TEST(transcriber_with_bends_matches_reference) {
    transcriber_with_bends_matches_reference(vectors::kBend);
    struct NoBends : g2m::EventSink {
        long bends = 0, with_bend = 0;
        void note(const g2m::NoteEvent& e) override { with_bend += e.has_bend; }
        void bend(const g2m::BendEvent&) override { ++bends; }
    } v3;
    auto mpe = vectors::load(vectors::kMpe);
    if (!mpe) return;
    g2m::Transcriber t3(mpe);
    std::vector<float> audio3 = vectors::input(vectors::kMpe);
    t3.process(audio3.data(), audio3.size(), v3);
    CHECK(v3.bends == 0 && v3.with_bend == 0);
}

// Format version 5: the projected model's notes, as the reference
TEST(transcriber_with_projection_matches_reference) {
    auto model = vectors::load(vectors::kProj);
    if (!model) return;
    std::vector<g2m::NoteEvent> got = transcribe(model, vectors::input(vectors::kProj));
    std::vector<g2m::NoteEvent> want = expected_events(vectors::kProj, model->config.hop);
    CHECK_MSG(same(got, want), "%zu events, the reference %zu", got.size(), want.size());
    CHECK(want.size() > 300);
}

// Format version 6: float16 weights, the reference's notes and bends
TEST(transcriber_with_float16_weights_matches_reference) {
    transcriber_with_bends_matches_reference(vectors::kHalf);
}
