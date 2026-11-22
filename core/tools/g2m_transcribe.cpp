// g2m-transcribe --model M.g2m IN.wav [-o OUT.mid] [--block N] [--outputs OUT.f32] [--events OUT.json]
//                [--set KEY=VALUE ...] [--mpe [--bend-range N]]
//
// Runs a WAV through the model and decoder, fed in host-sized blocks of N
// samples (default 512), and writes the notes as a MIDI file (default: IN
// with .mid) in the same form as the training code's NumPy reference, for
// comparing the two. --outputs also writes the model's outputs per block as
// raw float32 [blocks][n_pitches][3 + the MPE heads' values], as
// reference.py --outputs does (and feeds one block at a time); --events
// writes the events as JSON, with each note-on's string probabilities,
// onset type and pitch bend, and the bend events (models with MPE heads),
// as reference.py --events does. --set overrides a decoder setting from the
// model. --mpe writes MPE instead, as reference.py --mpe does
// (docs/mpe-output.md): the configuration at time 0, then each note on its
// string's channel with its pitch bends, and prints how often each step of
// the string allocator chose the string. --bend-range N sets the decoder's
// bend_range (the bends' scale and the RPN 0 sent; default the model's).
// --decode REF.f32 skips the network: the decoder reads reference.py
// --outputs' outputs instead (the WAV gives only the length), so its events
// can be compared exactly ("Checking a port" in the format spec).
//
// A WAV at another rate is converted to the model's with g2m::Resampler
// first, as the plugin does; the output's times are then the converted
// audio's.

#include <g2m/denormals.h>
#include <g2m/mpe.h>
#include <g2m/resampler.h>
#include <g2m/transcriber.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "audio_io.h"
#include "decoder.h"

namespace {

// A note event, or a bend event (velocity 0, has_bend, is_bend)
struct Event : g2m::NoteEvent {
    bool is_bend = false;
};

struct Collect : g2m::EventSink {
    std::vector<Event> events;
    void note(const g2m::NoteEvent& e) override {
        Event x;
        static_cast<g2m::NoteEvent&>(x) = e;
        events.push_back(x);
    }
    void bend(const g2m::BendEvent& b) override {
        Event x;
        x.sample = b.sample;
        x.pitch = b.pitch;
        x.velocity = 0;
        x.has_bend = true;
        x.bend = b.bend;
        x.semitones = b.semitones;
        x.is_bend = true;
        events.push_back(x);
    }
};

// The events as MPE messages, with the model's string probabilities and
// onset types, counting the allocator's steps
struct MpeCollect : g2m::MidiWriter {
    std::vector<g2m::tools::MidiMessage> messages;
    std::uint64_t sample = 0;
    void midi(std::uint8_t s, std::uint8_t d1, std::uint8_t d2) override { messages.push_back({sample, s, d1, d2}); }
};

std::vector<g2m::tools::MidiMessage> to_mpe(const std::vector<Event>& events, int bend_range, long steps[5]) {
    MpeCollect out;
    g2m::MpeOutput::configure(bend_range, out);
    g2m::MpeOutput mpe;
    for (const Event& e : events) {
        out.sample = e.sample;
        if (e.is_bend) {
            mpe.bend(e.pitch, e.bend, out);
            continue;
        }
        if (e.velocity > 0) {
            // the step the allocator will take, from a copy of its state
            g2m::StringAllocator probe = mpe.allocator();
            const float* weights = e.has_strings ? e.strings : nullptr;
            ++steps[static_cast<int>(probe.note_on(e.pitch, weights, e.onset_type).step)];
        }
        mpe.note(e.pitch, e.velocity, out, e.has_strings ? e.strings : nullptr, e.onset_type, e.has_bend ? e.bend : -1);
    }
    return out.messages;
}

// --decode: the decoder on reference.py's outputs, [blocks][P][3 + heads in
// file order], with the events as the Transcriber would send them
bool decode_outputs(const std::string& path, const g2m::Model& model, const g2m::DecoderSettings& settings,
                    Collect& sink) {
    const g2m::ModelConfig& c = model.config;
    std::ifstream file(path, std::ios::binary);
    std::vector<char> bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    std::size_t width = 3 + c.head_classes + c.curve_heads, per_block = width * c.n_pitches;
    if (bytes.empty() || bytes.size() % (4 * per_block)) {
        std::fprintf(stderr, "%s: not whole blocks of %zu floats\n", path.c_str(), per_block);
        return false;
    }
    std::vector<float> outputs(bytes.size() / 4);
    std::memcpy(outputs.data(), bytes.data(), bytes.size());  // little-endian host assumed
    g2m::Decoder decoder(c.n_pitches, c.pitch_min, settings, c.head_classes, c.pitch_offset_curve >= 0);
    std::vector<float> notes(3 * c.n_pitches), classes(c.head_classes), curves(c.curve_heads);
    std::size_t blocks = outputs.size() / per_block;
    for (std::size_t k = 0; k < blocks; ++k) {
        const float* block = outputs.data() + k * per_block;
        std::uint64_t sample = (k + 1) * static_cast<std::uint64_t>(c.hop);
        for (int p = 0; p < c.n_pitches; ++p) std::copy(block + p * width, block + p * width + 3, notes.begin() + 3 * p);
        auto split = [&](int p) {  // pitch p's heads, file order → class and curve layouts
            const float* in = block + p * width + 3;
            float* cls = classes.data();
            float* crv = curves.data();
            for (const g2m::HeadSpec& h : c.heads) {
                if (h.curve) *crv++ = *in;
                else cls = std::copy(in, in + h.classes, cls);
                in += h.classes;
            }
        };
        decoder.step(
            notes.data(),
            [&](int p, float* out) {
                split(p);
                std::copy(classes.begin(), classes.end(), out);
            },
            [&](int p) {
                split(p);
                return curves[c.pitch_offset_curve];
            },
            [&](int pitch, int velocity, const float* heads, const g2m::BendValue* bend) {
                g2m::NoteEvent e{sample, pitch, velocity};
                if (heads && c.string_head >= 0) {
                    e.has_strings = true;
                    std::copy(heads + c.string_head, heads + c.string_head + 6, e.strings);
                }
                if (heads && c.onset_type_head >= 0) e.onset_type = g2m::onset_type(heads + c.onset_type_head, settings);
                if (bend) {
                    e.has_bend = true;
                    e.bend = bend->value;
                    e.semitones = bend->semitones;
                }
                sink.note(e);
            },
            [&](int pitch, const g2m::BendValue& b) { sink.bend({sample, pitch, b.value, b.semitones}); });
    }
    decoder.flush([&](int pitch, int) { sink.note({blocks * c.hop, pitch, 0}); });
    return true;
}

bool set_decoder(g2m::DecoderSettings& d, const std::string& assignment) {
    std::size_t eq = assignment.find('=');
    if (eq == std::string::npos) return false;
    std::string key = assignment.substr(0, eq);
    char* end;
    const char* text = assignment.c_str() + eq + 1;
    double value = std::strtod(text, &end);
    if (end == text || *end) return false;
    if (key == "onset_threshold") d.onset_threshold = static_cast<float>(value);
    else if (key == "frame_threshold") d.frame_threshold = static_cast<float>(value);
    else if (key == "strong_onset") d.strong_onset = static_cast<float>(value);
    else if (key == "retrigger_threshold") d.retrigger_threshold = static_cast<float>(value);
    else if (key == "frame_wait") d.frame_wait = static_cast<int>(value);
    else if (key == "off_frames") d.off_frames = static_cast<int>(value);
    else if (key == "gate_frames") d.gate_frames = static_cast<int>(value);
    else if (key == "fallback_frames") d.fallback_frames = static_cast<int>(value);
    else if (key == "fallback_threshold") d.fallback_threshold = static_cast<float>(value);
    else if (key == "legato_threshold") d.legato_threshold = static_cast<float>(value);
    else if (key == "slide_threshold") d.slide_threshold = static_cast<float>(value);
    else if (key == "bend_dead_band") d.bend_dead_band = static_cast<float>(value);
    else if (key == "bend_range" && value >= 1 && value <= g2m::kMaxBendRange) d.bend_range = static_cast<int>(value);
    else return false;
    return true;
}

// As reference.py --events: one object per event, note-ons with their MPE heads
std::string events_json(const std::vector<Event>& events, int sample_rate, bool onset_types) {
    static const char* const kTypes[] = {"picked", "legato", "slide"};
    std::string out = "[\n";
    char buffer[256];
    for (std::size_t i = 0; i < events.size(); ++i) {
        const Event& e = events[i];
        double time = static_cast<double>(e.sample) / sample_rate;
        if (e.is_bend) {
            std::snprintf(buffer, sizeof buffer, "{\"time\": %.17g, \"kind\": \"bend\", \"pitch\": %d, \"bend\": %d, "
                          "\"semitones\": %.9g", time, e.pitch, e.bend, static_cast<double>(e.semitones));
            out += buffer;
            out += i + 1 < events.size() ? "},\n" : "}\n";
            continue;
        }
        std::snprintf(buffer, sizeof buffer, "{\"time\": %.17g, \"kind\": \"%s\", \"pitch\": %d, \"velocity\": %d",
                      time, e.velocity ? "on" : "off", e.pitch, e.velocity);
        out += buffer;
        if (e.velocity && e.has_strings) {
            out += ", \"string\": [";
            for (int s = 0; s < 6; ++s) {
                std::snprintf(buffer, sizeof buffer, "%s%.9g", s ? ", " : "", static_cast<double>(e.strings[s]));
                out += buffer;
            }
            out += "]";
        }
        if (e.velocity && onset_types) out += std::string(", \"onset_type\": \"") + kTypes[static_cast<int>(e.onset_type)] + "\"";
        if (e.velocity && e.has_bend) {
            std::snprintf(buffer, sizeof buffer, ", \"bend\": %d, \"semitones\": %.9g", e.bend,
                          static_cast<double>(e.semitones));
            out += buffer;
        }
        out += i + 1 < events.size() ? "},\n" : "}\n";
    }
    return out + "]\n";
}

int usage() {
    std::fprintf(stderr, "usage: g2m-transcribe --model M.g2m IN.wav [-o OUT.mid] [--block N] [--outputs OUT.f32] [--events OUT.json] [--set KEY=VALUE ...] [--mpe [--bend-range N]] [--decode REF.f32]\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    std::string model_path, in_path, out_path, outputs_path, events_path, decode_path;
    std::vector<std::string> settings;
    long block = 512;
    bool mpe = false;
    long bend_range = 0;  // 0: the model's
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        bool has_value = i + 1 < argc;
        if (arg == "--model" && has_value) model_path = argv[++i];
        else if (arg == "-o" && has_value) out_path = argv[++i];
        else if (arg == "--block" && has_value) block = std::strtol(argv[++i], nullptr, 10);
        else if (arg == "--set" && has_value) settings.push_back(argv[++i]);
        else if (arg == "--outputs" && has_value) outputs_path = argv[++i];
        else if (arg == "--events" && has_value) events_path = argv[++i];
        else if (arg == "--decode" && has_value) decode_path = argv[++i];
        else if (arg == "--mpe") mpe = true;
        else if (arg == "--bend-range" && has_value) bend_range = std::strtol(argv[++i], nullptr, 10);
        else if (!arg.empty() && arg[0] != '-' && in_path.empty()) in_path = arg;
        else return usage();
    }
    if (model_path.empty() || in_path.empty() || block < 1 || bend_range < 0 || bend_range > g2m::kMaxBendRange)
        return usage();
    if (out_path.empty()) {
        std::size_t dot = in_path.find_last_of('.');
        std::size_t slash = in_path.find_last_of('/');
        out_path = (dot != std::string::npos && (slash == std::string::npos || dot > slash) ? in_path.substr(0, dot) : in_path) + ".mid";
    }

    g2m::LoadResult loaded = g2m::load_model(model_path);
    if (!loaded.model) {
        std::fprintf(stderr, "%s\n", loaded.error.c_str());
        return 1;
    }
    std::shared_ptr<const g2m::Model> model = std::move(loaded.model);
    g2m::DecoderSettings decoder = model->decoder;
    for (const std::string& s : settings) {
        if (!set_decoder(decoder, s)) {
            std::fprintf(stderr, "--set %s: expected KEY=NUMBER with KEY a decoder setting\n", s.c_str());
            return 2;
        }
    }
    if (bend_range) decoder.bend_range = static_cast<int>(bend_range);

    g2m::tools::Audio audio;
    std::string error;
    if (!g2m::tools::read_wav(in_path, audio, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    if (audio.sample_rate != model->config.sample_rate) {
        if (!g2m::Resampler::supported(audio.sample_rate, model->config.sample_rate)) {
            std::fprintf(stderr, "%s: %d Hz, the model needs %d Hz\n", in_path.c_str(), audio.sample_rate,
                         model->config.sample_rate);
            return 1;
        }
        g2m::Resampler resampler(audio.sample_rate, model->config.sample_rate);
        std::vector<float> out(resampler.max_output(audio.samples.size()));
        std::vector<std::uint32_t> source(out.size());
        out.resize(resampler.process(audio.samples.data(), audio.samples.size(), out.data(), source.data()));
        std::fprintf(stderr, "%s: resampled from %d Hz to %d Hz (latency %d samples)\n", in_path.c_str(),
                     audio.sample_rate, model->config.sample_rate, resampler.latency());
        audio.samples = std::move(out);
        audio.sample_rate = model->config.sample_rate;
    }

    g2m::Transcriber transcriber(model);
    transcriber.set_decoder_settings(decoder);
    Collect sink;
    auto start = std::chrono::steady_clock::now();
    std::vector<float> outputs;  // with --outputs: [blocks][n_pitches][3 + head classes]
    int pitches = model->config.n_pitches, classes = model->config.head_classes + model->config.curve_heads;
    std::vector<float> heads(static_cast<std::size_t>(pitches) * classes);
    if (!outputs_path.empty()) block = model->config.hop;  // one block per call, to see each block's outputs
    if (!decode_path.empty()) {
        if (!decode_outputs(decode_path, *model, decoder, sink)) return 1;
    } else {
        g2m::FlushDenormals flush;
        for (std::size_t i = 0; i < audio.samples.size(); i += block) {
            std::size_t n = std::min<std::size_t>(block, audio.samples.size() - i);
            transcriber.process(audio.samples.data() + i, n, sink);
            if (!outputs_path.empty() && n == static_cast<std::size_t>(block)) {
                transcriber.last_heads(heads.data());
                for (int p = 0; p < pitches; ++p) {
                    outputs.insert(outputs.end(), transcriber.last_outputs() + 3 * p, transcriber.last_outputs() + 3 * p + 3);
                    outputs.insert(outputs.end(), heads.begin() + p * classes, heads.begin() + (p + 1) * classes);
                }
            }
        }
        // as reference.py: the end-of-stream note-offs at the end of the last whole block
        std::size_t part = audio.samples.size() % model->config.hop;
        Collect offs;
        transcriber.all_notes_off(offs);
        for (Event e : offs.events) {
            e.sample -= part;
            sink.events.push_back(e);
        }
    }
    double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    long steps[5] = {};
    std::vector<g2m::NoteEvent> notes_only;  // the plain output carries no bends
    for (const Event& e : sink.events)
        if (!e.is_bend) notes_only.push_back(e);
    std::vector<std::uint8_t> midi =
        mpe ? g2m::tools::midi_file(to_mpe(sink.events, decoder.bend_range, steps), audio.sample_rate)
            : g2m::tools::midi_file(notes_only, audio.sample_rate);
    std::ofstream out(out_path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(midi.data()), static_cast<std::streamsize>(midi.size()));
    if (!out) {
        std::fprintf(stderr, "can't write %s\n", out_path.c_str());
        return 1;
    }
    if (!events_path.empty()) {
        std::ofstream file(events_path);
        file << events_json(sink.events, audio.sample_rate, model->config.onset_type_head >= 0);
        if (!file) {
            std::fprintf(stderr, "can't write %s\n", events_path.c_str());
            return 1;
        }
    }
    if (!outputs_path.empty()) {
        std::ofstream file(outputs_path, std::ios::binary);  // raw float32, little-endian host assumed
        file.write(reinterpret_cast<const char*>(outputs.data()), static_cast<std::streamsize>(outputs.size() * 4));
        if (!file) {
            std::fprintf(stderr, "can't write %s\n", outputs_path.c_str());
            return 1;
        }
    }

    int hop = model->config.hop;
    std::size_t blocks = audio.samples.size() / hop;
    long notes = 0;
    for (const Event& e : sink.events) notes += e.velocity > 0;
    std::printf("%s: %.1f s, %ld notes -> %s; %.0f us per %d-sample block (%.0f us of audio)\n", in_path.c_str(),
                static_cast<double>(audio.samples.size()) / audio.sample_rate, notes, out_path.c_str(),
                seconds / (blocks ? blocks : 1) * 1e6, hop, 1e6 * hop / audio.sample_rate);
    if (mpe)
        std::printf("MPE strings chosen by: continuing a note (legato) %ld, hand span %ld, range %ld, any free string %ld, "
                    "stealing %ld\n", steps[0], steps[1], steps[2], steps[3], steps[4]);
    return 0;
}
