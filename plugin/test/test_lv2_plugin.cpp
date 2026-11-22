// An offline LV2 host (lilv) that runs the built g2m.lv2 bundle and checks
// its MIDI against the core library's Transcriber on the same audio.

#include <g2m/bend_mode.h>
#include <g2m/mpe.h>
#include <g2m/noise_gate.h>
#include <g2m/note_name.h>
#include <g2m/sustain.h>
#include <g2m/resampler.h>
#include <g2m/transcriber.h>

#include <lilv/lilv.h>
#include <lv2/presets/presets.h>
#include <lv2/atom/forge.h>
#include <lv2/atom/util.h>
#include <lv2/log/log.h>
#include <lv2/buf-size/buf-size.h>
#include <lv2/midi/midi.h>
#include <lv2/options/options.h>
#include <lv2/patch/patch.h>
#include <lv2/state/state.h>
#include <lv2/urid/urid.h>
#include <lv2/worker/worker.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstring>
#include <fstream>
#include <iterator>
#include <chrono>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// The plugin's operator new resolves to this one: the executable exports it
// because libstdc++ refers to it
#include "allocations.h"
#include "audio_io.h"
#include "check.h"
#include "factory_presets.h"
#include "DistrhoPluginInfo.h"
#include "g2m_bundled_models.h"  // G2M_BUNDLED_MODELS: the bundled-model tests run when it is
                                  // core/test/data/small.g2m;core/test/data/mpe.g2m

namespace {

#define G2M_LV2_URI DISTRHO_PLUGIN_URI
#define G2M_MODEL_URI G2M_LV2_URI "#model"  // DPF's URI for the "model" state

const std::string kData = G2M_TEST_DATA;
const std::string kModel = kData + "/small.g2m";
constexpr std::size_t kAtomCapacity = 1 << 16;
constexpr std::uint32_t kMaxBlock = 700;

struct Midi {
    std::uint64_t time;  // sample, counted from the start of the audio
    int status, pitch, velocity;
    bool operator==(const Midi& o) const {
        return time == o.time && status == o.status && pitch == o.pitch && velocity == o.velocity;
    }
};

int log_vprintf(LV2_Log_Handle h, LV2_URID, const char* fmt, va_list args);

int log_printf(LV2_Log_Handle h, LV2_URID type, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int n = log_vprintf(h, type, fmt, args);
    va_end(args);
    return n;
}

// URID map and log features shared by all instances
struct Features {
    std::unordered_map<std::string, LV2_URID> ids;
    std::vector<std::string> uris;
    std::string log;

    LV2_URID_Map map{this, [](LV2_URID_Map_Handle h, const char* uri) {
                         Features* f = static_cast<Features*>(h);
                         auto [it, added] = f->ids.emplace(uri, static_cast<LV2_URID>(f->uris.size() + 1));
                         if (added) f->uris.push_back(uri);
                         return it->second;
                     }};
    LV2_Log_Log log_feature{this, log_printf, log_vprintf};

    LV2_URID id(const char* uri) { return map.map(map.handle, uri); }
};

Features features;

int log_vprintf(LV2_Log_Handle h, LV2_URID, const char* fmt, va_list args) {
    char buffer[8192];
    int n = std::vsnprintf(buffer, sizeof buffer, fmt, args);
    static_cast<Features*>(h)->log += buffer;
    return n;
}

LilvWorld* world;  // the Lilv world below

// One plugin instance with its port buffers and a synchronous worker
class Host {
public:
    Host(const LilvPlugin* plugin, double rate) {
        schedule_ = {this, [](LV2_Worker_Schedule_Handle h, std::uint32_t size, const void* data) {
                         auto* bytes = static_cast<const std::uint8_t*>(data);
                         Host* host = static_cast<Host*>(h);
                         long before = allocations;
                         host->requests_.emplace_back(bytes, bytes + size);
                         host->host_allocations_ += allocations - before;  // the host's, not the plugin's
                         return LV2_WORKER_SUCCESS;
                     }};
        map_feature_ = {LV2_URID__map, &features.map};
        log_feature_ = {LV2_LOG__log, &features.log_feature};
        schedule_feature_ = {LV2_WORKER__schedule, &schedule_};
        options_[0] = {LV2_OPTIONS_INSTANCE, 0, features.id(LV2_BUF_SIZE__maxBlockLength), sizeof(std::int32_t),
                       features.id(LV2_ATOM__Int), &max_block_};
        options_feature_ = {LV2_OPTIONS__options, options_};
        feature_list_[0] = &map_feature_;
        feature_list_[1] = &log_feature_;
        feature_list_[2] = &schedule_feature_;
        feature_list_[3] = &options_feature_;
        feature_list_[4] = nullptr;
        instance_ = lilv_plugin_instantiate(plugin, rate, feature_list_);
        CHECK(instance_);
        if (!instance_) return;
        worker_ = static_cast<const LV2_Worker_Interface*>(lilv_instance_get_extension_data(instance_, LV2_WORKER__interface));
        CHECK(worker_);
        state_ = static_cast<const LV2_State_Interface*>(lilv_instance_get_extension_data(instance_, LV2_STATE__interface));
        CHECK(state_);
        connect(plugin, "lv2_events_in", control_.data());
        connect(plugin, "lv2_in", in_.data());
        connect(plugin, "lv2_out", out_.data());
        connect(plugin, "lv2_events_out", midi_.data());
        connect(plugin, "input_gain", &gain_db);
        connect(plugin, "midi_channel", &channel);
        connect(plugin, "onset_threshold", &onset_threshold);
        connect(plugin, "input_level", &input_level);
        connect(plugin, "note_count", &note_count);
        for (int slot = 0; slot < 6; ++slot) connect(plugin, ("note_" + std::to_string(slot + 1)).c_str(), &notes[slot]);
        connect(plugin, "output_mode", &output_mode);
        connect(plugin, "bend_range", &bend_range);
        connect(plugin, "model_choice", &model_choice);
        connect(plugin, "gate_threshold", &gate_threshold);
        connect(plugin, "gate_release", &gate_release);
        connect(plugin, "output_level", &output_level);
        connect(plugin, "output_mute", &output_mute);
        connect(plugin, "sustain_mode", &sustain_mode);
        connect(plugin, "sustain_cc", &sustain_cc);
        connect(plugin, "sustain", &sustain);
        connect(plugin, "sustain_state", &sustain_state);
        connect(plugin, "bend_mode", &bend_mode);
        connect(plugin, "model_loaded", &model_loaded);
        connect(plugin, "model_format", &model_format);
        connect(plugin, "model_error", &model_error);
        lilv_instance_activate(instance_);
        lv2_atom_forge_init(&forge_, &features.map);
    }

    ~Host() {
        if (!instance_) return;
        lilv_instance_deactivate(instance_);
        lilv_instance_free(instance_);
    }

    LilvInstance* instance() { return instance_; }

    // A port's buffer, by symbol (control ports)
    float& control(const std::string& symbol) { return *controls_.at(symbol); }

    void reactivate() {
        lilv_instance_deactivate(instance_);
        lilv_instance_activate(instance_);
    }

    // Sends patch:Set model = path with the next run().
    void set_model(const std::string& path) { pending_model_ = path; }

    // Sends a MIDI message at `frame` of the next run().
    void send_midi(std::uint32_t frame, std::uint8_t status, std::uint8_t data1, std::uint8_t data2) {
        pending_midi_.push_back({frame, status, data1, data2});
    }

    // The model path in the plugin's saved state ("" if none).
    std::string saved_model() {
        std::string model;
        const LV2_Feature* no_features[] = {nullptr};
        state_->save(lilv_instance_get_handle(instance_),
                     [](LV2_State_Handle h, std::uint32_t key, const void* value, std::size_t size, std::uint32_t,
                        std::uint32_t) {
                         if (key == features.id(G2M_MODEL_URI))
                             static_cast<std::string*>(h)->assign(static_cast<const char*>(value), strnlen(static_cast<const char*>(value), size));
                         return LV2_STATE_SUCCESS;
                     },
                     &model, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE, no_features);
        return model;
    }

    // Runs one block; collects MIDI (timed from `audio_start`) and the
    // audio output, and checks the audio passes through unless told not to.
    void run(const float* audio, std::uint32_t n) {
        write_control();
        prepare_output(midi_);
        std::copy(audio, audio + n, in_.begin());
        long before = allocations;
        host_allocations_ = 0;
        lilv_instance_run(instance_, n);
        run_allocations += allocations - before - host_allocations_;
        if (check_through) {
            bool through = std::memcmp(audio, out_.data(), n * sizeof(float)) == 0;  // bitwise: NaN passes through too
            CHECK(through);
        }
        output.insert(output.end(), out_.begin(), out_.begin() + n);
        read_midi();
        position_ += n;
        // the worker, synchronously, then its replies (which may schedule more work)
        while (!requests_.empty()) {
            std::vector<std::vector<std::uint8_t>> requests;
            requests.swap(requests_);
            for (auto& r : requests) {
                worker_->work(lilv_instance_get_handle(instance_), respond, this, static_cast<std::uint32_t>(r.size()),
                              r.data());
            }
            std::vector<std::vector<std::uint8_t>> responses;
            responses.swap(responses_);
            for (auto& r : responses)
                worker_->work_response(lilv_instance_get_handle(instance_), static_cast<std::uint32_t>(r.size()), r.data());
        }
    }

    // Runs `audio` in blocks of random size; MIDI times count from its start.
    void run_audio(const std::vector<float>& audio, std::mt19937& rng) {
        audio_start_ = position_;
        std::uniform_int_distribution<std::uint32_t> size(1, kMaxBlock);
        for (std::size_t i = 0; i < audio.size();) {
            std::uint32_t n = std::min<std::uint32_t>(size(rng), static_cast<std::uint32_t>(audio.size() - i));
            run(audio.data() + i, n);
            i += n;
        }
    }

    // MIDI times count from the next sample.
    void mark_audio_start() { audio_start_ = position_; }

    void run_silence(std::uint32_t n) {
        std::vector<float> zeros(n);
        audio_start_ = position_;
        run(zeros.data(), n);
    }

    std::vector<Midi> midi;
    float gain_db = 0;
    float channel = 1;
    float onset_threshold = 0;  // the model's default
    float input_level = 0;  // output
    float note_count = 0;  // output
    float notes[6] = {};  // output: the lowest held notes, -1 for none
    float output_mode = 0;  // 0 plain, 1 MPE
    float bend_range = 12;
    float model_choice = 0;  // the model file, unless a test chooses a bundled model
    float gate_threshold = -96;  // off
    float gate_release = 50;
    float output_level = 0;
    float output_mute = 0;
    float sustain_mode = 1;  // piano
    float sustain_cc = 64;
    float sustain = 0;
    float sustain_state = 0;  // output
    float bend_mode = 0;  // natural
    float model_loaded = 0, model_format = 0, model_error = 0;  // outputs
    bool check_through = true;  // the gate is off and the output level 0 dB
    std::vector<float> output;  // the audio output of every run
    long run_allocations = 0;  // made inside the plugin's run()

private:
    void connect(const LilvPlugin* plugin, const char* symbol, void* data) {
        LilvNode* name = lilv_new_string(world, symbol);
        const LilvPort* port = lilv_plugin_get_port_by_symbol(plugin, name);
        lilv_node_free(name);
        CHECK_MSG(port, "no port %s", symbol);
        if (port) lilv_instance_connect_port(instance_, lilv_port_get_index(plugin, port), data);
        controls_[symbol] = static_cast<float*>(data);
    }

    static LV2_Worker_Status respond(LV2_Worker_Respond_Handle h, std::uint32_t size, const void* data) {
        auto* bytes = static_cast<const std::uint8_t*>(data);
        static_cast<Host*>(h)->responses_.emplace_back(bytes, bytes + size);
        return LV2_WORKER_SUCCESS;
    }

    void write_control() {
        lv2_atom_forge_set_buffer(&forge_, reinterpret_cast<std::uint8_t*>(control_.data()), kAtomCapacity);
        LV2_Atom_Forge_Frame sequence;
        lv2_atom_forge_sequence_head(&forge_, &sequence, 0);
        if (!pending_model_.empty()) {
            LV2_Atom_Forge_Frame object;
            lv2_atom_forge_frame_time(&forge_, 0);
            lv2_atom_forge_object(&forge_, &object, 0, features.id(LV2_PATCH__Set));
            lv2_atom_forge_key(&forge_, features.id(LV2_PATCH__property));
            lv2_atom_forge_urid(&forge_, features.id(G2M_MODEL_URI));
            lv2_atom_forge_key(&forge_, features.id(LV2_PATCH__value));
            lv2_atom_forge_path(&forge_, pending_model_.c_str(), static_cast<std::uint32_t>(pending_model_.size()));
            lv2_atom_forge_pop(&forge_, &object);
            pending_model_.clear();
        }
        for (const Midi& m : pending_midi_) {
            std::uint8_t bytes[3] = {static_cast<std::uint8_t>(m.status), static_cast<std::uint8_t>(m.pitch),
                                     static_cast<std::uint8_t>(m.velocity)};
            lv2_atom_forge_frame_time(&forge_, static_cast<std::int64_t>(m.time));
            lv2_atom_forge_atom(&forge_, 3, features.id(LV2_MIDI__MidiEvent));
            lv2_atom_forge_write(&forge_, bytes, 3);
        }
        pending_midi_.clear();
        lv2_atom_forge_pop(&forge_, &sequence);
    }

    // An output sequence's size is its capacity until the plugin writes it.
    static void prepare_output(std::vector<std::uint64_t>& buffer) {
        auto* seq = reinterpret_cast<LV2_Atom_Sequence*>(buffer.data());
        seq->atom.type = features.id(LV2_ATOM__Chunk);
        seq->atom.size = kAtomCapacity - sizeof(LV2_Atom);
    }

    void read_midi() {
        auto* seq = reinterpret_cast<const LV2_Atom_Sequence*>(midi_.data());
        CHECK(seq->atom.type == features.id(LV2_ATOM__Sequence));
        long last = -1;
        LV2_ATOM_SEQUENCE_FOREACH(seq, ev) {
            if (ev->body.type != features.id(LV2_MIDI__MidiEvent)) continue;  // DPF reports a restored state as patch:Set
            CHECK(ev->body.size == 3);
            CHECK(ev->time.frames >= last && ev->time.frames < static_cast<long>(kMaxBlock));
            last = ev->time.frames;
            const auto* bytes = static_cast<const std::uint8_t*>(LV2_ATOM_BODY_CONST(&ev->body));
            midi.push_back({position_ + ev->time.frames - audio_start_, bytes[0], bytes[1], bytes[2]});
        }
    }

    LilvInstance* instance_ = nullptr;
    const LV2_Worker_Interface* worker_ = nullptr;
    LV2_Worker_Schedule schedule_;
    const LV2_State_Interface* state_ = nullptr;
    LV2_Feature map_feature_, log_feature_, schedule_feature_, options_feature_;
    std::int32_t max_block_ = kMaxBlock;
    LV2_Options_Option options_[2] = {};
    const LV2_Feature* feature_list_[5];
    LV2_Atom_Forge forge_;
    std::vector<std::uint64_t> control_ = std::vector<std::uint64_t>(kAtomCapacity / 8);
    std::vector<std::uint64_t> midi_ = std::vector<std::uint64_t>(kAtomCapacity / 8);
    std::vector<float> in_ = std::vector<float>(kMaxBlock);
    std::vector<float> out_ = std::vector<float>(kMaxBlock);
    std::vector<std::vector<std::uint8_t>> requests_, responses_;
    std::string pending_model_;
    std::unordered_map<std::string, float*> controls_;  // by symbol: the port buffers
    std::vector<Midi> pending_midi_;  // time: the frame in the next run
    std::uint64_t position_ = 0;
    std::uint64_t audio_start_ = 0;
    long host_allocations_ = 0;  // made by schedule_work during run()
};

struct Lilv {
    LilvWorld* world = lilv_world_new();
    const LilvPlugin* plugin = nullptr;

    Lilv() {
        ::world = world;
        LilvNode* bundle = lilv_new_file_uri(world, nullptr, G2M_LV2_BUNDLE);
        lilv_world_load_bundle(world, bundle);
        lilv_node_free(bundle);
        LilvNode* uri = lilv_new_uri(world, G2M_LV2_URI);
        plugin = lilv_plugins_get_by_uri(lilv_world_get_all_plugins(world), uri);
        lilv_node_free(uri);
    }
    ~Lilv() { lilv_world_free(world); }
};

Lilv* lv2;

std::vector<float> test_audio() {
    g2m::tools::Audio audio;
    std::string error;
    CHECK_MSG(g2m::tools::read_wav(kData + "/small.wav", audio, error), "%s", error.c_str());
    return audio.samples;
}

// What the plugin should send for `audio`: the core library's events, each
// on its frame's last sample, on MIDI channel `channel` (1-16). At a host
// rate other than the model's, the audio goes through g2m::Resampler and
// each event on the input sample that completed its frame's last sample.
std::vector<Midi> expected_midi(const std::vector<float>& audio, int channel = 1, float onset_threshold = 0,
                                int rate = 48000) {
    struct Collect : g2m::EventSink {
        std::vector<Midi> events;
        int channel;
        std::vector<std::uint32_t> source;  // empty: the model's rate
        void note(const g2m::NoteEvent& e) override {
            std::uint64_t last = source.empty() ? e.sample - 1 : source[e.sample - 1];
            events.push_back({last, (e.velocity ? 0x90 : 0x80) | (channel - 1), e.pitch, e.velocity});
        }
    } sink;
    sink.channel = channel;
    g2m::Transcriber transcriber(g2m::load_model(kModel).model);
    if (onset_threshold > 0) {
        g2m::DecoderSettings settings = transcriber.decoder_settings();
        settings.onset_threshold = onset_threshold;
        transcriber.set_decoder_settings(settings);
    }
    int model_rate = transcriber.model().config.sample_rate;
    if (rate == model_rate) {
        transcriber.process(audio.data(), audio.size(), sink);
        return sink.events;
    }
    g2m::Resampler resampler(rate, model_rate);
    std::vector<float> resampled(resampler.max_output(audio.size()));
    sink.source.resize(resampled.size());
    resampled.resize(resampler.process(audio.data(), audio.size(), resampled.data(), sink.source.data()));
    transcriber.process(resampled.data(), resampled.size(), sink);
    return sink.events;
}

// What the plugin should send in MPE mode for `audio`, after the
// configuration: the core library's events through g2m::MpeOutput
// (bend_range: the decoder's, as the MPE Bend Range port sets it)
std::vector<Midi> expected_mpe(const std::vector<float>& audio, const std::string& model = kModel, int bend_range = 12) {
    struct Collect : g2m::EventSink, g2m::MidiWriter {
        std::vector<Midi> events;
        g2m::MpeOutput mpe;
        std::uint64_t time = 0;
        void note(const g2m::NoteEvent& e) override {
            time = e.sample - 1;
            mpe.note(e.pitch, e.velocity, *this, e.has_strings ? e.strings : nullptr, e.onset_type,
                     e.has_bend ? e.bend : -1);
        }
        void bend(const g2m::BendEvent& e) override {
            time = e.sample - 1;
            mpe.bend(e.pitch, e.bend, *this);
        }
        void midi(std::uint8_t s, std::uint8_t d1, std::uint8_t d2) override { events.push_back({time, s, d1, d2}); }
    } sink;
    g2m::Transcriber transcriber(g2m::load_model(model).model);
    g2m::DecoderSettings settings = transcriber.decoder_settings();
    settings.bend_range = bend_range;
    transcriber.set_decoder_settings(settings);
    transcriber.process(audio.data(), audio.size(), sink);
    return sink.events;
}

// The configuration messages, at `time`
std::vector<Midi> mpe_configuration(std::uint64_t time, int bend_range = 12) {
    struct Collect : g2m::MidiWriter {
        std::vector<Midi> events;
        std::uint64_t time;
        void midi(std::uint8_t s, std::uint8_t d1, std::uint8_t d2) override { events.push_back({time, s, d1, d2}); }
    } out;
    out.time = time;
    g2m::MpeOutput::configure(bend_range, out);
    return out.events;
}

bool is_note(const Midi& m) {
    return (m.status & 0xE0) == 0x80;
}

// Notes only on MPE member channels 2-7, at most one at a time on each,
// except a legato overlap: a note-on on a sounding channel followed at once
// by the old note's note-off there
bool valid_mpe_notes(const std::vector<Midi>& events, int* overlaps = nullptr) {
    int on[16];
    std::fill(on, on + 16, -1);
    int ended = -1, channel_ended = -1, count = 0;  // an overlap's note awaiting its note-off
    for (const Midi& m : events) {
        if (!is_note(m)) continue;
        int channel = m.status & 0x0F;
        if (channel < 1 || channel > 6) return false;
        bool note_on = (m.status & 0xF0) == 0x90;
        if (ended >= 0) {
            if (note_on || channel != channel_ended || m.pitch != ended) return false;
            ended = -1;
            ++count;
            continue;
        }
        if (note_on && on[channel] >= 0) {
            ended = on[channel];
            channel_ended = channel;
        } else if (!note_on && on[channel] != m.pitch) {
            return false;
        }
        on[channel] = note_on ? m.pitch : -1;
    }
    if (overlaps) *overlaps = count;
    return ended < 0;
}

// Pitches left on by `events`
std::vector<int> held(const std::vector<Midi>& events) {
    bool on[128] = {};
    for (const Midi& m : events)
        if (is_note(m)) on[m.pitch] = (m.status & 0xF0) == 0x90;
    std::vector<int> out;
    for (int p = 0; p < 128; ++p)
        if (on[p]) out.push_back(p);
    return out;
}

// Loads the test model: one 64-sample silent run sends patch:Set, and the
// model is swapped in after it, so it has heard nothing yet. MIDI times
// then count from the next sample.
void load(Host& host) {
    host.set_model(kModel);
    host.run_silence(64);
    host.midi.clear();
    host.mark_audio_start();
}

}  // namespace

TEST(bundle_ports) {
    CHECK(lv2->plugin);
    if (!lv2->plugin) return;
    // DPF's audio and event ports, then the parameters with the old LV2 plugin's symbols
    const char* symbols[] = {"lv2_in",      "lv2_out",    "lv2_events_in", "lv2_events_out", "input_gain",
                             "midi_channel", "onset_threshold", "input_level", "note_count",  "note_1",
                             "note_2",      "note_3",     "note_4",     "note_5",      "note_6",
                             "output_mode", "bend_range", "model_choice", "gate_threshold", "gate_release",
                             "output_level", "output_mute", "sustain_mode", "sustain_cc", "sustain",
                             "sustain_state", "bend_mode", "model_loaded", "model_format", "model_error"};
    CHECK(lilv_plugin_get_num_ports(lv2->plugin) == 30);
    for (std::uint32_t i = 0; i < 30; ++i) {
        const LilvPort* port = lilv_plugin_get_port_by_index(lv2->plugin, i);
        CHECK(port && std::strcmp(lilv_node_as_string(lilv_port_get_symbol(lv2->plugin, port)), symbols[i]) == 0);
    }
}

TEST(matches_core_library) {
    if (!lv2->plugin || kBundledModelCount) return;  // a bundled default model would already be loaded
    Host host(lv2->plugin, 48000);
    load(host);
    std::vector<float> audio = test_audio();
    std::mt19937 rng(1);
    host.run_audio(audio, rng);
    CHECK(host.saved_model() == kModel);
    std::vector<Midi> want = expected_midi(audio);
    CHECK(want.size() > 50);
    CHECK_MSG(host.midi == want, "plugin %zu events, core %zu", host.midi.size(), want.size());
    CHECK(host.run_allocations == 0);
}

TEST(input_gain) {
    if (!lv2->plugin || kBundledModelCount) return;  // a bundled default model would already be loaded
    Host host(lv2->plugin, 48000);
    host.gain_db = -6;
    load(host);  // the gain settles during this run, before the model exists
    std::vector<float> audio = test_audio();
    std::mt19937 rng(2);
    host.run_audio(audio, rng);
    std::vector<float> scaled(audio);
    float gain = std::pow(10.0f, -6.0f / 20.0f);
    for (float& x : scaled) x *= gain;
    std::vector<Midi> want = expected_midi(scaled);
    CHECK(want != expected_midi(audio));
    CHECK(host.midi == want);
}

// The gate's output, as the core's NoiseGate makes it at 48 kHz
std::vector<float> gated(const std::vector<float>& audio, float threshold_db, float release_ms) {
    g2m::NoiseGate gate;
    gate.set(threshold_db, release_ms, 48000);
    gate.reset();
    std::vector<float> out(audio.size());
    gate.process(audio.data(), out.data(), static_cast<std::uint32_t>(audio.size()));
    return out;
}

// The model hears the gated audio after the input gain, and the audio
// output is the gated audio at the Output Level
TEST(gate_and_output_level) {
    if (!lv2->plugin || kBundledModelCount) return;  // a bundled default model would already be loaded
    Host host(lv2->plugin, 48000);
    host.check_through = false;
    host.gate_threshold = -30;
    host.gate_release = 20;
    host.output_level = -6;
    host.gain_db = 24;  // the quiet stretch loud enough for the model to notice the gate
    host.run_silence(512);  // the output level's ramp; the gate stays closed
    load(host);
    host.output.clear();
    std::vector<float> audio = test_audio();  // loud throughout: a quiet stretch for the gate to close on
    for (std::size_t i = audio.size() * 2 / 5; i < audio.size() * 7 / 10; ++i) audio[i] *= 0.01f;
    std::mt19937 rng(3);
    host.run_audio(audio, rng);
    std::vector<float> want = gated(audio, -30, 20);
    std::size_t closed = std::count(want.begin(), want.end(), 0.0f) - std::count(audio.begin(), audio.end(), 0.0f);
    CHECK_MSG(closed > 1000 && closed < audio.size() / 2, "%zu samples gated", closed);
    float gain = std::pow(10.0f, -6.0f / 20.0f);
    CHECK(host.output.size() == audio.size());
    bool same = host.output.size() == audio.size();
    for (std::size_t i = 0; same && i < want.size(); ++i) same = host.output[i] == want[i] * gain;
    CHECK(same);
    float input_gain = std::pow(10.0f, 24.0f / 20.0f);
    std::vector<float> heard(want), ungated(audio);
    for (float& x : heard) x *= input_gain;
    for (float& x : ungated) x *= input_gain;
    std::vector<Midi> want_midi = expected_midi(heard);
    CHECK(want_midi != expected_midi(ungated));
    CHECK_MSG(host.midi == want_midi, "plugin %zu events, core %zu", host.midi.size(), want_midi.size());
    CHECK(host.run_allocations == 0);
}

// Mute silences the audio output, after a short ramp, and not the MIDI;
// unmuting ramps back to the input bit for bit
TEST(mute) {
    if (!lv2->plugin || kBundledModelCount) return;  // a bundled default model would already be loaded
    Host host(lv2->plugin, 48000);
    host.check_through = false;
    load(host);
    std::vector<float> audio = test_audio();
    host.output_mute = 1;
    std::mt19937 rng(4);
    host.output.clear();
    host.run_audio(audio, rng);
    CHECK(host.midi == expected_midi(audio));
    std::size_t ramp = 240;  // 5 ms
    bool fading = true, silent = true;
    for (std::size_t i = 0; i < ramp - 1; ++i)
        fading = fading && std::fabs(host.output[i]) <= std::fabs(audio[i]) * (1.0f - i / 240.0f) + 1e-6f;
    for (std::size_t i = ramp - 1; i < audio.size(); ++i) silent = silent && host.output[i] == 0;
    CHECK(fading);
    CHECK(silent);
    host.output_mute = 0;
    host.output.clear();
    host.run_audio(audio, rng);
    bool through = true;
    for (std::size_t i = ramp; i < audio.size(); ++i) through = through && host.output[i] == audio[i];
    CHECK(through);
    CHECK(host.output[ramp / 2] != audio[ramp / 2] || audio[ramp / 2] == 0);
}

// A pedal change: down at `sample`, counted from the start of the audio
struct Pedal {
    std::uint64_t sample;
    bool down;
};

// What the plugin should send with the sustain pedal: the core library's
// events through g2m::Sustain, the pedal moving at the given samples
std::vector<Midi> expected_sustain(const std::vector<float>& audio, g2m::SustainMode mode,
                                   const std::vector<Pedal>& pedals, bool down_at_start = false) {
    struct Collect : g2m::EventSink {
        std::vector<Midi> events;
        void note(const g2m::NoteEvent& e) override {
            events.push_back({e.sample - 1, e.velocity ? 0x90 : 0x80, e.pitch, e.velocity});
        }
    } sink;
    struct Through : g2m::EventSink {
        g2m::Sustain* sustain;
        g2m::EventSink* out;
        void note(const g2m::NoteEvent& e) override { sustain->note(e, *out); }
    } through;
    g2m::Sustain sustain;
    sustain.set_mode(mode, 0, sink);
    sustain.set_pedal(down_at_start, 0, sink);
    through.sustain = &sustain;
    through.out = &sink;
    g2m::Transcriber transcriber(g2m::load_model(kModel).model);
    std::uint64_t done = 0;
    for (const Pedal& p : pedals) {
        transcriber.process(audio.data() + done, p.sample - done, through);
        done = p.sample;
        sustain.set_pedal(p.down, p.sample + 1, sink);
    }
    transcriber.process(audio.data() + done, audio.size() - done, through);
    return sink.events;
}

// Runs `audio` in blocks of random size, with the pedal's CC (value 127 or
// 0, on MIDI channel `channel`) at each change's sample, and extra values
// that change nothing: 64 while down, 63 while up, and other controllers.
void run_with_pedal(Host& host, const std::vector<float>& audio, const std::vector<Pedal>& pedals, std::mt19937& rng,
                    int cc = 64, int channel = 0) {
    host.mark_audio_start();
    std::uniform_int_distribution<std::uint32_t> size(1, kMaxBlock);
    std::size_t next = 0;
    bool down = false;
    for (std::size_t i = 0; i < audio.size();) {
        std::uint32_t n = std::min<std::uint32_t>(size(rng), static_cast<std::uint32_t>(audio.size() - i));
        for (; next < pedals.size() && pedals[next].sample < i + n; ++next) {
            auto frame = static_cast<std::uint32_t>(pedals[next].sample - i);
            down = pedals[next].down;
            host.send_midi(frame, static_cast<std::uint8_t>(0xB0 | channel), static_cast<std::uint8_t>(cc), down ? 127 : 0);
            host.send_midi(frame, static_cast<std::uint8_t>(0xB0 | channel), static_cast<std::uint8_t>(cc), down ? 64 : 63);
            host.send_midi(frame, 0xB0, static_cast<std::uint8_t>(cc ^ 1), down ? 0 : 127);
        }
        host.run(audio.data() + i, n);
        CHECK(host.sustain_state == (down ? 1.0f : 0.0f));
        i += n;
    }
}

const std::vector<Pedal> kPedals = {{3000, true}, {9000, false}, {9001, true}, {30000, false}, {40000, true},
                                    {52000, false}};

// Each mode through the plugin is the core's Sustain on the core's events,
// with the pedal at the CC's sample
TEST(sustain_modes_match_core_library) {
    if (!lv2->plugin || kBundledModelCount) return;  // a bundled default model would already be loaded
    std::vector<float> audio = test_audio();
    std::vector<Midi> plain = expected_midi(audio);
    for (int mode = 0; mode < 3; ++mode) {
        Host host(lv2->plugin, 48000);
        host.sustain_mode = static_cast<float>(mode);
        host.sustain_cc = mode == 2 ? 66 : 64;
        load(host);
        std::mt19937 rng(10 + mode);
        run_with_pedal(host, audio, kPedals, rng, mode == 2 ? 66 : 64, mode);
        std::vector<Midi> want = expected_sustain(audio, static_cast<g2m::SustainMode>(mode), kPedals);
        if (mode == 0) CHECK(want == plain);
        else CHECK_MSG(want != plain, "mode %d", mode);
        CHECK_MSG(host.midi == want, "mode %d: plugin %zu events, core %zu", mode, host.midi.size(), want.size());
        CHECK(host.run_allocations == 0);
    }
}

// The Sustain parameter holds the pedal down too, from the next block
TEST(sustain_parameter) {
    if (!lv2->plugin || kBundledModelCount) return;  // a bundled default model would already be loaded
    std::vector<float> audio = test_audio();
    Host host(lv2->plugin, 48000);
    host.sustain = 1;
    load(host);
    CHECK(host.sustain_state == 1);
    std::mt19937 rng(20);
    host.run_audio(audio, rng);
    host.sustain = 0;
    std::vector<float> silence(64);
    host.run(silence.data(), 64);  // the pedal comes up at its start
    std::vector<float> both(audio);
    both.insert(both.end(), silence.begin(), silence.end());
    std::vector<Midi> want = expected_sustain(both, g2m::SustainMode::piano, {{audio.size(), false}}, true);
    CHECK(want != expected_midi(both));
    CHECK_MSG(host.midi == want, "plugin %zu events, core %zu", host.midi.size(), want.size());
    CHECK(host.sustain_state == 0);
}

// Notes the plugin ends itself (here a channel change) aren't ended again
// at pedal up
TEST(sustained_notes_ended_once) {
    if (!lv2->plugin || kBundledModelCount) return;  // a bundled default model would already be loaded
    std::vector<float> audio = test_audio();
    Host host(lv2->plugin, 48000);
    host.sustain = 1;
    load(host);
    std::mt19937 rng(21);
    host.run_audio(audio, rng);
    CHECK(held(host.midi).size() > 1);
    host.channel = 2;
    host.run_silence(64);  // ends them on channel 1
    host.sustain = 0;
    host.run_silence(64);
    bool on[128] = {};
    bool paired = true;
    for (const Midi& m : host.midi) {
        if (m.status == 0x91) on[m.pitch] = true;
        if (m.status == 0x81) paired = paired && on[m.pitch];
        if (m.status == 0x81) on[m.pitch] = false;
    }
    CHECK(paired);
    CHECK(held(host.midi).empty());
}

TEST(onset_threshold) {
    if (!lv2->plugin || kBundledModelCount) return;  // a bundled default model would already be loaded
    std::vector<float> audio = test_audio();
    std::vector<Midi> at_default;
    for (float threshold : {0.0f, 0.3f, 0.6f, 0.95f}) {
        Host host(lv2->plugin, 48000);
        host.onset_threshold = threshold;
        load(host);
        std::mt19937 rng(7);
        host.run_audio(audio, rng);
        CHECK_MSG(host.midi == expected_midi(audio, 1, threshold), "threshold %g", threshold);
        // each threshold changes the notes (not monotonically: with random
        // weights the onset probability hovers near 0.5, so a low threshold
        // gives fewer rising edges)
        if (threshold == 0) at_default = host.midi;  // 0.45, the model's
        else CHECK_MSG(host.midi != at_default, "threshold %g", threshold);
    }
    // the model's own value, set explicitly, is the same as the default
    Host host(lv2->plugin, 48000);
    host.onset_threshold = 0.45f;
    load(host);
    std::mt19937 rng(7);
    host.run_audio(audio, rng);
    CHECK(host.midi == expected_midi(audio));
}

TEST(input_level_meter) {
    if (!lv2->plugin || kBundledModelCount) return;  // a bundled default model would already be loaded
    Host host(lv2->plugin, 48000);  // no model needed
    std::vector<float> sine(24000);
    for (std::size_t i = 0; i < sine.size(); ++i) sine[i] = 0.25f * std::sin(2 * M_PI * 220 * i / 48000.0);
    host.run(sine.data(), 500);
    CHECK(std::fabs(host.input_level - 20 * std::log10(0.25f)) < 0.05f);  // -12.04 dB
    host.gain_db = 6;
    for (int k = 0; k < 4; ++k) host.run(sine.data() + 500 * (k + 1), 500);  // the gain ramp settles
    CHECK(std::fabs(host.input_level - (20 * std::log10(0.25f) + 6)) < 0.05f);
    std::vector<float> silence(500);
    for (int k = 0; k < 96; ++k) host.run(silence.data(), 500);  // 1 s: falls with a 0.3 s time constant
    float fallen = 20 * std::log10(0.25f) + 6 - 20 * std::log10(std::exp(1.0f / 0.3f));
    CHECK(std::fabs(host.input_level - fallen) < 0.5f);
    for (int k = 0; k < 2000; ++k) host.run(silence.data(), 500);
    CHECK(host.input_level == -90.0f);
}

TEST(non_finite_input_is_silence) {
    if (!lv2->plugin || kBundledModelCount) return;  // a bundled default model would already be loaded
    std::vector<float> audio = test_audio();
    Host host(lv2->plugin, 48000);
    load(host);
    std::vector<float> bad(audio.begin(), audio.begin() + 640);
    bad[100] = NAN;
    bad[200] = INFINITY;
    host.run(bad.data(), 640);
    for (std::size_t i = 640; i < audio.size(); i += 300)
        host.run(audio.data() + i, static_cast<std::uint32_t>(std::min<std::size_t>(300, audio.size() - i)));
    std::vector<float> zeroed(audio);
    zeroed[100] = zeroed[200] = 0;
    CHECK(!host.midi.empty());
    CHECK(host.midi == expected_midi(zeroed));
}

// The note ports and the Notes string follow the MIDI sent, after every run.
TEST(note_display) {
    if (!lv2->plugin || kBundledModelCount) return;  // a bundled default model would already be loaded
    std::vector<float> audio = test_audio();
    Host host(lv2->plugin, 48000);
    load(host);
    std::mt19937 rng(9);
    std::uniform_int_distribution<std::uint32_t> size(1, kMaxBlock);
    int checked = 0, with_notes = 0;
    for (std::size_t i = 0; i < audio.size();) {
        std::uint32_t n = std::min<std::uint32_t>(size(rng), static_cast<std::uint32_t>(audio.size() - i));
        host.run(audio.data() + i, n);
        i += n;
        std::vector<int> on = held(host.midi);
        bool ports_ok = host.note_count == static_cast<float>(on.size());
        for (std::size_t slot = 0; slot < 6; ++slot)
            ports_ok = ports_ok && host.notes[slot] == (slot < on.size() ? static_cast<float>(on[slot]) : -1.0f);
        CHECK_MSG(ports_ok, "after sample %zu", i);
        ++checked;
        with_notes += !on.empty();
    }
    CHECK(with_notes > 10 && with_notes < checked);  // both states were seen
}

TEST(channel_change_ends_held_notes) {
    if (!lv2->plugin || kBundledModelCount) return;  // a bundled default model would already be loaded
    Host host(lv2->plugin, 48000);
    load(host);
    std::vector<float> audio = test_audio();
    std::mt19937 rng(3);
    // run until some notes are held
    std::size_t at = 0;
    for (; at + 256 <= audio.size() && held(host.midi).empty(); at += 256) host.run(audio.data() + at, 256);
    std::vector<int> on = held(host.midi);
    CHECK(!on.empty());
    std::size_t before = host.midi.size();
    host.channel = 3;
    host.run(audio.data() + at, 1);
    // note-offs on channel 1 first, at the start of the block
    CHECK(host.midi.size() >= before + on.size());
    for (std::size_t i = 0; i < on.size() && before + i < host.midi.size(); ++i) {
        const Midi& m = host.midi[before + i];
        CHECK(m.status == 0x80 && m.pitch == on[i] && m.velocity == 0);
    }
    std::vector<float> rest(audio.begin() + at + 1, audio.end());
    host.midi.clear();
    host.run_audio(rest, rng);
    bool any = false;
    for (const Midi& m : host.midi) {
        CHECK((m.status & 0x0F) == 2);
        any = true;
    }
    CHECK(any);
}

TEST(model_swap_ends_held_notes) {
    if (!lv2->plugin || kBundledModelCount) return;  // a bundled default model would already be loaded
    Host host(lv2->plugin, 48000);
    load(host);
    std::vector<float> audio = test_audio();
    std::size_t at = 0;
    for (; at + 256 <= audio.size() && held(host.midi).empty(); at += 256) host.run(audio.data() + at, 256);
    std::vector<int> on = held(host.midi);
    CHECK(!on.empty());
    host.set_model(kModel);  // the same file again: a new Transcriber
    host.run(audio.data() + at, 64);  // loads after this run
    std::size_t before = host.midi.size();
    host.run(audio.data() + at + 64, 64);
    std::vector<Midi> offs(host.midi.begin() + before, host.midi.end());
    CHECK(offs.size() >= on.size());
    for (std::size_t i = 0; i < on.size() && i < offs.size(); ++i)
        CHECK(offs[i].status == 0x80 && offs[i].pitch == on[i] && offs[i].time == at + 64);
}

// The LV2 presets are the factory presets: each sets every preset parameter
// (the factory value, or the default), not the Model or the Sustain switch,
// and restoring one keeps the model.
TEST(lv2_presets_are_the_factory_presets) {
    if (!lv2->plugin) return;
    std::vector<g2m::presets::ParameterInfo> info;
    for (std::uint32_t i = 0; i < lilv_plugin_get_num_ports(lv2->plugin); ++i) {
        const LilvPort* port = lilv_plugin_get_port_by_index(lv2->plugin, i);
        std::string symbol = lilv_node_as_string(lilv_port_get_symbol(lv2->plugin, port));
        LilvNode* control = lilv_new_uri(lv2->world, LV2_CORE__ControlPort);
        LilvNode* input = lilv_new_uri(lv2->world, LV2_CORE__InputPort);
        bool preset_port = lilv_port_is_a(lv2->plugin, port, control) && lilv_port_is_a(lv2->plugin, port, input) &&
                           symbol != "model_choice" && symbol != "sustain";
        lilv_node_free(control);
        lilv_node_free(input);
        if (!preset_port) continue;
        LilvNode *def, *min, *max;
        lilv_port_get_range(lv2->plugin, port, &def, &min, &max);
        info.push_back({symbol, lilv_node_as_float(def), lilv_node_as_float(min), lilv_node_as_float(max)});
        lilv_node_free(def);
        lilv_node_free(min);
        lilv_node_free(max);
    }
    CHECK(info.size() == 12);

    LilvNode* preset_class = lilv_new_uri(lv2->world, LV2_PRESETS__Preset);
    LilvNode* label = lilv_new_uri(lv2->world, LILV_NS_RDFS "label");
    LilvNodes* presets = lilv_plugin_get_related(lv2->plugin, preset_class);
    const auto& factory = g2m::presets::factory_presets();
    CHECK(lilv_nodes_size(presets) == factory.size());
    std::vector<bool> seen(factory.size());
    LILV_FOREACH (nodes, it, presets) {
        const LilvNode* uri = lilv_nodes_get(presets, it);
        lilv_world_load_resource(lv2->world, uri);
        LilvNodes* labels = lilv_world_find_nodes(lv2->world, uri, label, nullptr);
        std::string name = labels ? lilv_node_as_string(lilv_nodes_get_first(labels)) : "";
        lilv_nodes_free(labels);
        std::size_t k = 0;
        while (k < factory.size() && factory[k].name != name) ++k;
        CHECK_MSG(k < factory.size(), "LV2 preset '%s' isn't a factory preset", name.c_str());
        if (k == factory.size()) continue;
        seen[k] = true;
        LilvState* state = lilv_state_new_from_world(lv2->world, &features.map, uri);
        CHECK_MSG(state, "%s", name.c_str());
        if (!state) continue;
        std::unordered_map<std::string, float> got;
        Host host(lv2->plugin, 48000);
        if (!kBundledModelCount) load(host);
        std::string model = host.saved_model();
        lilv_state_restore(
            state, host.instance(),
            [](const char* symbol, void* data, const void* value, std::uint32_t size, std::uint32_t type) {
                // lilv gives each value the type its Turtle literal has
                float v = NAN;
                if (type == features.id(LV2_ATOM__Float) && size == sizeof(float)) v = *static_cast<const float*>(value);
                if (type == features.id(LV2_ATOM__Double) && size == sizeof(double)) v = static_cast<float>(*static_cast<const double*>(value));
                if (type == features.id(LV2_ATOM__Int) && size == sizeof(std::int32_t)) v = static_cast<float>(*static_cast<const std::int32_t*>(value));
                if (type == features.id(LV2_ATOM__Long) && size == sizeof(std::int64_t)) v = static_cast<float>(*static_cast<const std::int64_t*>(value));
                CHECK_MSG(!std::isnan(v), "%s: a value of type %u", symbol, type);
                (*static_cast<std::unordered_map<std::string, float>*>(data))[symbol] = v;
            },
            &got, 0, nullptr);
        lilv_state_free(state);
        CHECK_MSG(host.saved_model() == model, "%s: the model changed", name.c_str());
        std::vector<float> want = g2m::presets::values(factory[k], info);
        CHECK_MSG(got.size() == info.size(), "%s: %zu ports", name.c_str(), got.size());
        for (std::size_t i = 0; i < info.size(); ++i) {
            auto found = got.find(info[i].symbol);
            CHECK_MSG(found != got.end() && found->second == want[i], "%s: %s", name.c_str(), info[i].symbol.c_str());
        }
        for (const auto& [symbol, value] : factory[k].parameters) {  // the factory table names real parameters
            bool known = false;
            for (const auto& p : info) known = known || p.symbol == symbol;
            CHECK_MSG(known, "%s: no parameter %s", name.c_str(), symbol.c_str());
            (void)value;
        }
    }
    for (std::size_t k = 0; k < factory.size(); ++k) CHECK_MSG(seen[k], "no LV2 preset %s", factory[k].name.c_str());
    lilv_nodes_free(presets);
    lilv_node_free(label);
    lilv_node_free(preset_class);
}

// The LV2 programs extension (DPF's lv2_programs.h)
struct ProgramsInterface {
    const void* (*get_program)(LV2_Handle, std::uint32_t);
    void (*select_program)(LV2_Handle, std::uint32_t bank, std::uint32_t program);
};

// Selecting a program (the plugin's loadProgram, as VST3 and the
// standalone use it) sets the preset parameters and leaves the Model and
// the Sustain switch as they were.
TEST(programs_keep_model_and_sustain) {
    if (!lv2->plugin) return;
    const auto& factory = g2m::presets::factory_presets();
    for (std::uint32_t k = 0; k < factory.size(); ++k) {
        Host host(lv2->plugin, 48000);
        auto* programs = static_cast<const ProgramsInterface*>(
            lilv_instance_get_extension_data(host.instance(), "http://kxstudio.sf.net/ns/lv2ext/programs#Interface"));
        CHECK(programs);
        if (!programs) return;
        float choice = static_cast<float>(kBundledModelCount);  // the last, not the default
        host.model_choice = choice;
        host.sustain = 1;
        host.bend_mode = 2;  // not the default
        host.run_silence(64);
        programs->select_program(lilv_instance_get_handle(host.instance()), 0, k);
        CHECK_MSG(host.model_choice == choice && host.sustain == 1, "%s", factory[k].name.c_str());
        Host defaults(lv2->plugin, 48000);
        for (const char* symbol : {"input_gain", "midi_channel", "onset_threshold", "output_mode", "bend_range",
                                   "gate_threshold", "gate_release", "output_level", "output_mute", "sustain_mode",
                                   "sustain_cc", "bend_mode"}) {
            float want = defaults.control(symbol);  // the Host starts each port at its default
            for (const auto& [name, value] : factory[k].parameters)
                if (name == symbol) want = value;
            CHECK_MSG(host.control(symbol) == want, "%s: %s", factory[k].name.c_str(), symbol);
        }
    }
}

TEST(state_restores_the_model) {
    if (!lv2->plugin || kBundledModelCount) return;  // a bundled default model would already be loaded
    Host saved(lv2->plugin, 48000);
    load(saved);
    LilvState* state = lilv_state_new_from_instance(
        lv2->plugin, saved.instance(), &features.map, nullptr, nullptr, nullptr, nullptr,
        [](const char*, void*, std::uint32_t* size, std::uint32_t* type) -> const void* {
            *size = 0;
            *type = 0;
            return nullptr;
        },
        nullptr, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE, nullptr);
    CHECK(state);
    if (!state) return;

    Host restored(lv2->plugin, 48000);
    lilv_state_restore(state, restored.instance(), nullptr, nullptr, 0, nullptr);
    lilv_state_free(state);
    restored.run_silence(1);  // then the audio from the model's start
    CHECK(restored.saved_model() == kModel);
    // the silent sample above went through the model: compare with the same input
    std::vector<float> audio = test_audio();
    std::vector<float> with_silence(1, 0.0f);
    with_silence.insert(with_silence.end(), audio.begin(), audio.end());
    std::mt19937 rng(4);
    restored.midi.clear();
    restored.run_audio(audio, rng);
    std::vector<Midi> want = expected_midi(with_silence);
    for (Midi& m : want) m.time -= 1;
    CHECK(!restored.midi.empty());
    CHECK(restored.midi == want);
}

TEST(other_sample_rates_resampled) {
    if (!lv2->plugin || kBundledModelCount) return;  // a bundled default model would already be loaded
    std::vector<float> audio = test_audio();
    for (int rate : {44100, 96000}) {
        Host host(lv2->plugin, rate);
        load(host);
        std::mt19937 rng(5);
        host.run_audio(audio, rng);  // audio still passes through (checked in run)
        std::vector<Midi> want = expected_midi(audio, 1, 0, rate);
        CHECK_MSG(want.size() > 5, "%d Hz: %zu events", rate, want.size());  // fewer at 96 kHz: an octave up
        CHECK_MSG(host.midi == want, "%d Hz: plugin %zu events, core %zu", rate, host.midi.size(), want.size());
        CHECK(host.model_loaded == 0 && host.model_format == 2 && host.model_error == 0);
        CHECK(host.run_allocations == 0);
        // re-activated: the resampler starts again too
        host.reactivate();
        host.midi.clear();
        host.mark_audio_start();
        host.run_audio(audio, rng);
        std::vector<Midi> after;  // without the held notes' note-offs, first
        for (const Midi& m : host.midi)
            if (!(m.time == 0 && (m.status & 0xF0) == 0x80)) after.push_back(m);
        CHECK_MSG(after == want, "%d Hz, re-activated", rate);
    }
}

TEST(unsupported_sample_rate_gives_no_midi) {
    if (!lv2->plugin || kBundledModelCount) return;  // a bundled default model would already be loaded
    Host host(lv2->plugin, 6000);
    load(host);
    std::vector<float> audio = test_audio();
    audio.resize(audio.size() / 4);
    std::mt19937 rng(5);
    host.run_audio(audio, rng);  // audio still passes through (checked in run)
    CHECK(host.midi.empty());
    CHECK(host.saved_model() == kModel);  // kept for the session, should the rate change
    CHECK(host.model_loaded == 0 && host.model_format == 2 && host.model_error == 2);  // the file, for another rate
}

// Runs silent blocks until the plugin's state names `model` (the loader
// thread works in the background); false after 2 s.
bool wait_for_model(Host& host, const std::string& model) {
    for (int i = 0; i < 200; ++i) {
        host.run_silence(64);
        if (host.saved_model() == model) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

bool bundled_test_models() {
    return kBundledModelCount == 2 && std::strcmp(kBundledModels[0], "small") == 0 &&
           std::strcmp(kBundledModels[1], "mpe") == 0;
}

TEST(bundled_default_model) {
    if (!lv2->plugin || !bundled_test_models()) return;
    Host host(lv2->plugin, 48000);
    host.model_choice = 1;  // the default: loaded when the plugin is created
    CHECK(host.saved_model() == "bundled:small");
    std::vector<float> audio = test_audio();
    std::mt19937 rng(11);
    host.run_audio(audio, rng);
    CHECK(host.model_loaded == 1 && host.model_format == 2 && host.model_error == 0);
    CHECK(!host.midi.empty());
    CHECK(host.midi == expected_midi(audio));
    CHECK(host.run_allocations == 0);
}

TEST(bundled_choice_and_state) {
    if (!lv2->plugin || !bundled_test_models()) return;
    Host host(lv2->plugin, 48000);
    host.model_choice = 1;
    host.run_silence(64);
    host.model_choice = 2;  // loaded by the loader thread, not in run()
    CHECK(wait_for_model(host, "bundled:mpe"));
    host.run_silence(64);
    CHECK(host.model_loaded == 2 && host.model_format == 3);
    host.model_choice = 0;  // "Model file": keeps the current model
    host.run_silence(64);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(host.saved_model() == "bundled:mpe");

    // the state names the bundled model, and restores it
    LilvState* state = lilv_state_new_from_instance(
        lv2->plugin, host.instance(), &features.map, nullptr, nullptr, nullptr, nullptr,
        [](const char*, void*, std::uint32_t* size, std::uint32_t* type) -> const void* {
            *size = 0;
            *type = 0;
            return nullptr;
        },
        nullptr, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE, nullptr);
    CHECK(state);
    if (!state) return;
    Host restored(lv2->plugin, 48000);
    lilv_state_restore(state, restored.instance(), nullptr, nullptr, 0, nullptr);
    lilv_state_free(state);
    CHECK(restored.saved_model() == "bundled:mpe");
    restored.run_silence(64);
    CHECK(restored.model_loaded == 2);

    host.set_model(kModel);  // a file
    host.run_silence(64);
    host.run_silence(64);
    CHECK(host.saved_model() == kModel);
    CHECK(host.model_loaded == 0 && host.model_format == 2);
}

TEST(bad_model_file_keeps_the_current_one) {
    if (!lv2->plugin || kBundledModelCount) return;  // a bundled default model would already be loaded
    Host host(lv2->plugin, 48000);
    load(host);
    host.run_silence(64);
    CHECK(host.model_loaded == 0 && host.model_format == 2 && host.model_error == 0);
    host.set_model(kData + "/small.input.f32");  // not a model
    host.run_silence(64);
    host.run_silence(64);
    CHECK(host.saved_model() == kModel);
    CHECK(host.model_loaded == 0 && host.model_format == 2 && host.model_error == 1);  // the file before plays on
    std::vector<float> audio = test_audio();
    std::mt19937 rng(6);
    host.midi.clear();
    host.run_audio(audio, rng);
    CHECK(!host.midi.empty());
}

TEST(mpe_matches_core_library) {
    if (!lv2->plugin || kBundledModelCount) return;  // a bundled default model would already be loaded
    Host host(lv2->plugin, 48000);
    host.output_mode = 1;
    host.set_model(kModel);
    host.run_silence(64);
    CHECK(host.midi == mpe_configuration(0));  // at the start of the first run
    host.midi.clear();
    host.mark_audio_start();
    std::vector<float> audio = test_audio();
    std::mt19937 rng(12);
    host.run_audio(audio, rng);
    std::vector<Midi> want = expected_mpe(audio);
    CHECK(want.size() > 50);
    CHECK_MSG(host.midi == want, "plugin %zu events, core %zu", host.midi.size(), want.size());
    CHECK(valid_mpe_notes(host.midi));
    CHECK(host.run_allocations == 0);
    // the display shows the same notes as in plain mode
    std::vector<int> on = held(host.midi);
    CHECK(host.note_count == static_cast<float>(on.size()));
}

TEST(mpe_configuration_on_activate_and_range_change) {
    if (!lv2->plugin || kBundledModelCount) return;  // a bundled default model would already be loaded
    Host host(lv2->plugin, 48000);
    host.output_mode = 1;
    host.run_silence(10);
    CHECK(host.midi == mpe_configuration(0));
    host.midi.clear();
    host.run_silence(10);
    CHECK(host.midi.empty());  // once
    host.channel = 5;  // unused in MPE mode
    host.run_silence(10);
    CHECK(host.midi.empty());
    host.bend_range = 48;  // RPN 0 again, without the MCM
    host.run_silence(10);
    std::vector<Midi> want = mpe_configuration(0, 48);
    want.erase(want.begin(), want.begin() + 5);
    CHECK(host.midi == want);
    host.midi.clear();
    host.reactivate();
    host.run_silence(10);
    CHECK(host.midi == mpe_configuration(0, 48));
    CHECK(host.run_allocations == 0);
}

// Plain → MPE → plain while notes are held: each switch ends them as they
// were sent, at the start of the block
TEST(mode_change_ends_held_notes) {
    if (!lv2->plugin || kBundledModelCount) return;  // a bundled default model would already be loaded
    Host host(lv2->plugin, 48000);
    host.channel = 3;
    load(host);
    std::vector<float> audio = test_audio();
    std::size_t at = 0;
    for (; at + 256 <= audio.size() && held(host.midi).empty(); at += 256) host.run(audio.data() + at, 256);
    std::vector<int> on = held(host.midi);
    CHECK(!on.empty());
    std::size_t before = host.midi.size();
    host.output_mode = 1;
    host.run(audio.data() + at, 1);
    std::vector<Midi> want;
    for (int pitch : on) want.push_back({at, 0x82, pitch, 0});
    std::vector<Midi> config = mpe_configuration(at);
    want.insert(want.end(), config.begin(), config.end());
    CHECK(std::vector<Midi>(host.midi.begin() + before, host.midi.end()) == want);
    ++at;

    // MPE notes until some are held
    before = host.midi.size();
    for (; at + 256 <= audio.size() && held(host.midi).empty(); at += 256) host.run(audio.data() + at, 256);
    std::vector<Midi> mpe(host.midi.begin() + before, host.midi.end());
    CHECK(valid_mpe_notes(mpe));
    for (; at + 256 <= audio.size() && held(host.midi).empty(); at += 256) host.run(audio.data() + at, 256);
    std::vector<int> on_mpe = held(host.midi);
    CHECK(!on_mpe.empty());
    before = host.midi.size();
    host.output_mode = 0;
    host.run(audio.data() + at, 1);
    std::vector<Midi> offs(host.midi.begin() + before, host.midi.end());
    CHECK(offs.size() == on_mpe.size() + 5);  // the note-offs, then the MCM with no members
    for (std::size_t i = 0; i < on_mpe.size() && i < offs.size(); ++i)
        CHECK((offs[i].status & 0xF0) == 0x80 && (offs[i].status & 0x0F) >= 1 && (offs[i].status & 0x0F) <= 6 &&
              offs[i].pitch == on_mpe[i] && offs[i].time == at);
    CHECK(offs.size() >= 5 && offs[offs.size() - 3].status == 0xB0 && offs[offs.size() - 3].pitch == 6 &&
          offs[offs.size() - 3].velocity == 0);
    CHECK(held(host.midi).empty());
    host.midi.clear();
    std::vector<float> rest(audio.begin() + at + 1, audio.end());
    std::mt19937 rng(13);
    host.run_audio(rest, rng);
    CHECK(!host.midi.empty());
    for (const Midi& m : host.midi) CHECK((m.status & 0x0F) == 2);  // plain again, on channel 3
}

TEST(mpe_model_swap_ends_held_notes) {
    if (!lv2->plugin || kBundledModelCount) return;  // a bundled default model would already be loaded
    Host host(lv2->plugin, 48000);
    host.output_mode = 1;
    load(host);
    std::vector<float> audio = test_audio();
    std::size_t at = 0;
    for (; at + 256 <= audio.size() && held(host.midi).empty(); at += 256) host.run(audio.data() + at, 256);
    std::vector<int> on = held(host.midi);
    CHECK(!on.empty());
    host.set_model(kModel);
    host.run(audio.data() + at, 64);
    std::size_t before = host.midi.size();
    host.run(audio.data() + at + 64, 64);
    std::vector<Midi> offs(host.midi.begin() + before, host.midi.end());
    CHECK(offs.size() >= on.size());
    for (std::size_t i = 0; i < on.size() && i < offs.size(); ++i)
        CHECK((offs[i].status & 0xF0) == 0x80 && offs[i].pitch == on[i] && offs[i].time == at + 64);
    CHECK(valid_mpe_notes(host.midi));
}

// The version 3 test model (string and onset-type heads)
const std::string kMpeModel = kData + "/mpe.g2m";

std::vector<float> mpe_audio() {
    std::ifstream file(kData + "/mpe.input.f32", std::ios::binary);
    std::vector<char> bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    CHECK(!bytes.empty() && bytes.size() % 4 == 0);
    std::vector<float> audio(bytes.size() / 4);
    std::memcpy(audio.data(), bytes.data(), audio.size() * 4);  // little-endian host assumed
    return audio;
}

// With the heads, MPE mode places notes with the string probabilities and
// overlaps legato notes; plain mode sends the core library's events as
// ever, legato notes as ordinary note-ons.
TEST(heads_model_in_both_modes) {
    if (!lv2->plugin || kBundledModelCount) return;  // a bundled default model would already be loaded
    std::vector<float> audio = mpe_audio();
    for (int mode : {0, 1}) {
        Host host(lv2->plugin, 48000);
        host.output_mode = static_cast<float>(mode);
        host.set_model(kMpeModel);
        host.run_silence(64);
        host.midi.clear();
        host.mark_audio_start();
        std::mt19937 rng(20 + mode);
        host.run_audio(audio, rng);
        CHECK(host.saved_model() == kMpeModel);
        std::vector<Midi> want;
        if (mode) {
            want = expected_mpe(audio, kMpeModel);
        } else {
            struct Collect : g2m::EventSink {
                std::vector<Midi> events;
                void note(const g2m::NoteEvent& e) override {
                    events.push_back({e.sample - 1, e.velocity ? 0x90 : 0x80, e.pitch, e.velocity});
                }
            } sink;
            g2m::Transcriber transcriber(g2m::load_model(kMpeModel).model);
            transcriber.process(audio.data(), audio.size(), sink);
            want = sink.events;
        }
        CHECK(want.size() > 100);
        CHECK_MSG(host.midi == want, "mode %d: plugin %zu events, core %zu", mode, host.midi.size(), want.size());
        CHECK(host.run_allocations == 0);
        if (mode) {
            int overlaps = 0;
            CHECK(valid_mpe_notes(host.midi, &overlaps));
            CHECK(overlaps > 10);
        }
    }
}

// The version 4 test model (pitch-offset head, bend_range 24 in the file)
const std::string kBendModel = kData + "/bend.g2m";

std::vector<float> raw_audio(const std::string& name) {
    std::ifstream file(kData + "/" + name + ".input.f32", std::ios::binary);
    std::vector<char> bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    std::vector<float> audio(bytes.size() / 4);
    std::memcpy(audio.data(), bytes.data(), audio.size() * 4);
    return audio;
}

// MPE mode sends each note's bends on its channel, scaled to the MPE Bend
// Range port (not the file's range); plain mode sends no bends at all.
TEST(bends_in_both_modes) {
    if (!lv2->plugin || kBundledModelCount) return;  // a bundled default model would already be loaded
    std::vector<float> audio = raw_audio("bend");
    struct Case {
        int mode, range;
    };
    std::vector<Midi> at_12;
    for (Case c : {Case{1, 12}, Case{1, 48}, Case{0, 12}}) {
        Host host(lv2->plugin, 48000);
        host.output_mode = static_cast<float>(c.mode);
        host.bend_range = static_cast<float>(c.range);
        host.set_model(kBendModel);
        host.run_silence(64);
        if (c.mode) CHECK(host.midi == mpe_configuration(0, c.range));
        host.midi.clear();
        host.mark_audio_start();
        std::mt19937 rng(30 + c.range);
        host.run_audio(audio, rng);
        long bends = 0;
        for (const Midi& m : host.midi) bends += (m.status & 0xF0) == 0xE0;
        if (c.mode) {
            std::vector<Midi> want = expected_mpe(audio, kBendModel, c.range);
            CHECK_MSG(host.midi == want, "range %d: plugin %zu events, core %zu", c.range, host.midi.size(), want.size());
            CHECK(bends > 1000 && valid_mpe_notes(host.midi));
            if (c.range == 12) at_12 = host.midi;
            else CHECK(host.midi != at_12);  // the same notes, bends scaled to 48
            // re-activated, after the notes have ended: the configuration again, and the channels' bends
            // counted as reset with it, so the output is a fresh start's
            for (int k = 0; k < 100; ++k) host.run_silence(480);  // 1 s, within the port buffers
            CHECK(held(host.midi).empty());
            host.reactivate();
            host.midi.clear();
            host.mark_audio_start();
            host.run_audio(audio, rng);
            std::vector<Midi> again = mpe_configuration(0, c.range);
            again.insert(again.end(), want.begin(), want.end());
            CHECK(host.midi == again);
        } else {
            CHECK(bends == 0 && !host.midi.empty());
        }
        CHECK(host.run_allocations == 0);
    }
}

// What the plugin should send with a Bend Mode: the core library's events
// through g2m::BendModeFilter, then g2m::MpeOutput (mpe) or plain MIDI on
// channel 1, retriggers included
std::vector<Midi> expected_bend_mode(const std::vector<float>& audio, g2m::BendMode mode, bool mpe, int bend_range) {
    struct Collect : g2m::NoteSink, g2m::MidiWriter {
        std::vector<Midi> events;
        g2m::MpeOutput out;
        bool mpe;
        std::uint64_t time = 0;
        void note(const g2m::NoteEvent& e) override {
            time = e.sample - 1;
            if (mpe) out.note(e.pitch, e.velocity, *this, e.has_strings ? e.strings : nullptr, e.onset_type,
                              e.has_bend ? e.bend : -1);
            else midi(e.velocity ? 0x90 : 0x80, static_cast<std::uint8_t>(e.pitch), static_cast<std::uint8_t>(e.velocity));
        }
        void bend(const g2m::BendEvent& e) override {
            time = e.sample - 1;
            if (mpe) out.bend(e.pitch, e.bend, *this);
        }
        void retrigger(std::uint64_t sample, int from, int to, int velocity) override {
            time = sample - 1;
            if (mpe) {
                out.retrigger(from, to, velocity, *this);
            } else {
                midi(0x80, static_cast<std::uint8_t>(from), 0);
                midi(0x90, static_cast<std::uint8_t>(to), static_cast<std::uint8_t>(velocity));
            }
        }
        void midi(std::uint8_t s, std::uint8_t d1, std::uint8_t d2) override { events.push_back({time, s, d1, d2}); }
    } sink;
    struct Through : g2m::EventSink {
        g2m::BendModeFilter filter;
        Collect* out;
        void note(const g2m::NoteEvent& e) override { filter.note(e, *out); }
        void bend(const g2m::BendEvent& e) override { filter.bend(e, *out); }
    } through;
    sink.mpe = mpe;
    through.out = &sink;
    through.filter.set_mode(mode);
    g2m::Transcriber transcriber(g2m::load_model(kBendModel).model);
    g2m::DecoderSettings settings = transcriber.decoder_settings();
    settings.bend_range = bend_range;
    transcriber.set_decoder_settings(settings);
    transcriber.process(audio.data(), audio.size(), through);
    return sink.events;
}

// Every Bend Mode in both output modes is the core's BendModeFilter on the
// core's events
TEST(bend_modes_match_core_library) {
    if (!lv2->plugin || kBundledModelCount) return;  // a bundled default model would already be loaded
    std::vector<float> audio = raw_audio("bend");
    for (int output = 0; output < 2; ++output) {
        std::vector<Midi> natural;
        for (int mode = 0; mode < 3; ++mode) {
            Host host(lv2->plugin, 48000);
            host.output_mode = static_cast<float>(output);
            host.bend_mode = static_cast<float>(mode);
            host.set_model(kBendModel);
            host.run_silence(64);
            host.midi.clear();
            host.mark_audio_start();
            std::mt19937 rng(40 + mode);
            host.run_audio(audio, rng);
            std::vector<Midi> want = expected_bend_mode(audio, static_cast<g2m::BendMode>(mode), output == 1, 12);
            CHECK_MSG(host.midi == want, "output %d mode %d: plugin %zu events, core %zu", output, mode,
                      host.midi.size(), want.size());
            long bends = 0, retriggers = 0;
            for (std::size_t i = 0; i + 1 < host.midi.size(); ++i) {
                bends += (host.midi[i].status & 0xF0) == 0xE0;
                const Midi& a = host.midi[i];
                const Midi& b = host.midi[i + 1];
                bool on_after_off = (a.status & 0xF0) == 0x80 && (b.status & 0xF0) == 0x90 && a.time == b.time;
                retriggers += on_after_off && (a.status & 0x0F) == (b.status & 0x0F) && std::abs(a.pitch - b.pitch) == 1;
            }
            if (mode == 0) natural = host.midi;
            else if (output == 1 || mode == 1) CHECK_MSG(host.midi != natural, "output %d mode %d", output, mode);
            if (output == 1) CHECK_MSG(valid_mpe_notes(host.midi), "mode %d", mode);
            if (mode == 1) CHECK_MSG(retriggers > 10, "output %d: %ld retriggers", output, retriggers);
            if (mode >= 1 && output == 1) CHECK(bends < 10);
            if (output == 0) CHECK(bends == 0);
            CHECK(host.run_allocations == 0);
        }
    }
}

// Retriggered notes the plugin ends itself (here channel changes) aren't
// ended again later
TEST(retriggered_notes_ended_once) {
    if (!lv2->plugin || kBundledModelCount) return;  // a bundled default model would already be loaded
    std::vector<float> audio = raw_audio("bend");
    Host host(lv2->plugin, 48000);
    host.bend_mode = 1;  // retrigger
    host.set_model(kBendModel);
    host.run_silence(64);
    host.midi.clear();
    std::mt19937 rng(51);
    std::size_t part = audio.size() / 8;
    for (int k = 0; k < 16; ++k) {  // the audio twice, the channel changed eight times each
        std::vector<float> piece(audio.begin() + (k % 8) * part, audio.begin() + (k % 8 + 1) * part);
        host.run_audio(piece, rng);
        host.channel = static_cast<float>(1 + (k + 1) % 2);
    }
    int retriggers = 0;
    bool on[2][128] = {};
    bool paired = true;
    for (std::size_t i = 0; i < host.midi.size(); ++i) {
        const Midi& m = host.midi[i];
        int c = m.status & 0x0F;
        if ((m.status & 0xF0) == 0x90) on[c][m.pitch] = true;
        if ((m.status & 0xF0) != 0x80) continue;
        paired = paired && on[c][m.pitch];
        on[c][m.pitch] = false;
        if (i + 1 < host.midi.size() && host.midi[i + 1].time == m.time && std::abs(host.midi[i + 1].pitch - m.pitch) == 1)
            ++retriggers;
    }
    CHECK(retriggers > 10);
    CHECK(paired);
}

// lilv's world must exist before the tests run and after they finish
static struct Setup {
    Setup() { lv2 = new Lilv; }
    ~Setup() { delete lv2; }
} setup;
