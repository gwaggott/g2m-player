// The player plugin (DPF: LV2, VST3, CLAP, standalone): guitar audio in,
// MIDI notes out. The audio goes through a noise gate first; the model hears
// the gated audio after the Input Gain, and the audio output is the gated
// audio at the Output Level (or muted).
//
// A sustain pedal (g2m::Sustain: piano, sostenuto or freeze) sits between
// the model's notes and the MIDI output, worked by a CC on the MIDI input
// or by the Sustain parameter. Each buffer is split at the pedal's CCs, so
// the pedal acts at the sample the CC arrived on. Other MIDI input is
// ignored.
//
// Then the Bend Mode (g2m::BendModeFilter): the model's bends as they are,
// as retriggered notes (which work in plain mode too), or none.
//
// The model hears 48 kHz (its own rate): at another host rate the gated
// audio goes through a resampler (g2m::Resampler, built with the model's
// Transcriber) on its way to the model, and each event goes on the host's
// sample that completed its frame. The audio output isn't resampled.
//
// The model file is a state value ("model", a file path), set by the host
// or restored with the session. It is loaded off the audio thread (DPF calls
// setState from the LV2 worker or the host's main thread), together with the
// Transcriber that runs it, and swapped in under a lock that run() only
// tries: if a swap is in progress, that block passes the audio through and
// sends no MIDI. The plugin tracks the notes it has sent, so it can end them
// when the model changes, the plugin is (re)activated, the MIDI channel
// changes (plain output) or the output mode changes.
//
// Models can also be bundled with the plugin (G2M_BUNDLED_MODELS in
// CMake): the Model parameter chooses one, for hosts with no file chooser.
// A loader thread watches it, so a choice made on the audio thread loads
// off it; the model state is then "bundled:<name>".
//
// The Model Loaded, Model Format and Model Error outputs say which model
// plays and why the last one chosen doesn't, for the UI.
//
// The factory presets (factory_presets.h) are the plugin's programs. A
// preset sets every parameter but the Model and the Sustain switch (and the
// outputs) and keeps the current model.
//
// The notes currently on are shown, lowest first, on output parameters
// whose values are labelled with note names.
//
// Output modes: plain MIDI on the chosen channel, or MPE (docs/mpe-output.md):
// a lower zone with one member channel per string (chosen with the model's
// string probabilities when it has them; legato notes overlap the note they
// continue on its channel; with a pitch-offset head, each note's pitch
// bend on its channel, scaled to the MPE Bend Range), configured with the MPE
// Configuration Message and the pitch-bend range at the start of the first
// run after activation and whenever MPE is chosen; the MIDI channel is then
// unused. Leaving MPE turns the zone off.

#include "DistrhoPlugin.hpp"
#include "DistrhoPluginUtils.hpp"
#include "factory_presets.h"
#include "params.h"
#include "g2m_bundled_models.h"

#include <g2m/bend_mode.h>
#include <g2m/denormals.h>
#include <g2m/mpe.h>
#include <g2m/noise_gate.h>
#include <g2m/resampler.h>
#include <g2m/sustain.h>
#include <g2m/note_name.h>
#include <g2m/transcriber.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

START_NAMESPACE_DISTRHO

using namespace g2m::param;

namespace {

constexpr std::uint32_t kChunk = 256;  // samples gain-scaled at a time

constexpr char kBundledPrefix[] = "bundled:";
constexpr auto kLoaderPoll = std::chrono::milliseconds(20);

// Where the bundled models are, relative to the plugin's binary: an LV2
// bundle or the standalone (resources/models), a VST3 or macOS bundle
// (Contents/Resources/models), or next to a single-file CLAP (g2m-models).
std::string bundled_models_dir() {
    namespace fs = std::filesystem;
    const char* binary = getBinaryFilename();
    if (!binary || !*binary) return {};
    fs::path dir = fs::path(binary).parent_path();
    for (fs::path candidate : {dir / "resources" / "models", dir / ".." / "Resources" / "models", dir / "g2m-models"}) {
        std::error_code ec;
        if (fs::is_directory(candidate, ec)) return candidate.lexically_normal().string();
    }
    return {};
}

enum class OutputMode { plain, mpe };

constexpr double kMeterFallSeconds = 0.3;  // time constant of the level meter's fall
constexpr double kOutputRampSeconds = 0.005;  // Output Level and Mute changes

// A spin lock the audio thread only tries.
class SwapLock {
public:
    bool try_lock() { return !flag_.test_and_set(std::memory_order_acquire); }
    void lock() {
        while (!try_lock()) std::this_thread::yield();
    }
    void unlock() { flag_.clear(std::memory_order_release); }

private:
    std::atomic_flag flag_ = ATOMIC_FLAG_INIT;
};

}  // namespace

class G2mPlugin : public Plugin {
public:
    G2mPlugin() : Plugin(kParamCount, static_cast<uint32_t>(g2m::presets::factory_presets().size()), 1) {
        for (std::uint32_t i = 0; i < kParamCount; ++i) {
            Parameter p;
            initParameter(i, p);
            params_[i] = p.ranges.def;
            if (in_presets(i, p)) {
                preset_params_.push_back(i);
                preset_info_.push_back({p.symbol.buffer(), p.ranges.def, p.ranges.min, p.ranges.max});
            }
        }
        if (isDummyInstance() || kBundledModelCount == 0) return;
        models_dir_ = bundled_models_dir();
        // a new instance plays at once with the default (first) bundled model
        int choice = static_cast<int>(params_[kModelChoice]);
        load_choice(choice);
        loaded_choice_ = choice;
        requested_choice_ = choice;
        loader_ = std::thread([this] { watch_choice(); });
    }

    ~G2mPlugin() override {
        stop_loader_ = true;
        if (loader_.joinable()) loader_.join();
        delete transcriber_;
        delete resampling_;
    }

protected:
    const char* getLabel() const override { return "g2m"; }
    const char* getDescription() const override {
        return "Real-time guitar to MIDI: a DI electric guitar signal in (48 kHz, passed through a noise gate), MIDI "
               "note-on (with velocity) and note-off out, from a neural network note model, as plain MIDI on one "
               "channel or as MPE with one channel per string. Choose a .g2m model file with the Model setting.";
    }
    const char* getMaker() const override { return "guitar-2-midi"; }
    const char* getHomePage() const override { return "https://github.com/gwaggott/g2m-player"; }
    const char* getLicense() const override { return "GPL-3.0-or-later"; }
    uint32_t getVersion() const override { return d_version(G2M_VERSION_MAJOR, G2M_VERSION_MINOR, G2M_VERSION_PATCH); }

    void initAudioPort(bool input, uint32_t index, AudioPort& port) override {
        port.groupId = kPortGroupMono;
        Plugin::initAudioPort(input, index, port);
        port.symbol = input ? "in" : "out";
        port.name = input ? "In" : "Out";
    }

    void initParameter(uint32_t index, Parameter& p) override { init(index, p); }

    void initState(uint32_t, State& state) override {
        state.key = "model";
        state.label = "Model";
        state.description = "A .g2m note model.";
        state.defaultValue = "";
        state.hints = kStateIsFilenamePath;
    }

    void initProgramName(uint32_t index, String& name) override {
        name = g2m::presets::factory_presets()[index].name.c_str();
    }

    // Not the audio thread
    void loadProgram(uint32_t index) override {
        std::vector<float> values = g2m::presets::values(g2m::presets::factory_presets()[index], preset_info_);
        for (std::size_t i = 0; i < values.size(); ++i) params_[preset_params_[i]] = values[i];
    }

    float getParameterValue(uint32_t index) const override { return index < kParamCount ? params_[index] : 0.0f; }

    void setParameterValue(uint32_t index, float value) override {
        if (index >= kParamCount) return;
        params_[index] = value;
        if (index == kModelChoice) requested_choice_ = static_cast<int>(std::lround(value));  // for the loader
    }

    String getState(const char* key) const override {
        if (std::strcmp(key, "model") != 0) return String();
        std::lock_guard<std::mutex> guard(path_mutex_);
        return String(model_path_.c_str());
    }

    // Not the audio thread: loads the model and swaps it in. A file that
    // can't be used keeps the current model.
    void setState(const char* key, const char* value) override {
        if (std::strcmp(key, "model") != 0) return;
        std::lock_guard<std::mutex> guard(path_mutex_);
        if (*value) load_value(value, bundled_choice(value));
    }

    void activate() override {
        swap_lock_.lock();
        notes_off_pending_ = true;  // whatever was held before deactivation
        configure_pending_ = true;
        gate_.reset();
        if (transcriber_) {
            NullSink none;
            transcriber_->reset(none);
        }
        if (resampling_) resampling_->resampler.reset();
        swap_lock_.unlock();
    }

    // Deactivated: rebuild the Transcriber for the new rate.
    void sampleRateChanged(double) override {
        std::lock_guard<std::mutex> guard(path_mutex_);
        if (!model_path_.empty()) load_value(model_path_, model_loaded_);
    }

    // The audio between the MIDI input's events, then each event. The audio a
    // chunk at a time: the gate, then the model and the audio output. The
    // output may be the input's buffer: each chunk is read before it's written.
    void run(const float** inputs, float** outputs, uint32_t frames, const MidiEvent* events,
             uint32_t event_count) override {
        const float* in = inputs[0];
        float* out = outputs[0];
        g2m::FlushDenormals flush;
        gate_.set(params_[kGateThreshold], params_[kGateRelease], getSampleRate());
        set_output_gain();
        bool locked = swap_lock_.try_lock();  // if not, a model is being swapped in: no MIDI this time
        if (locked) begin_midi(frames);
        std::uint32_t done = 0;
        for (std::uint32_t k = 0; k <= event_count; ++k) {
            std::uint32_t until = k < event_count ? std::min(events[k].frame, frames) : frames;
            for (std::uint32_t i = done; i < until; i += kChunk) {
                std::uint32_t m = std::min<std::uint32_t>(kChunk, until - i);
                if (gate_.off()) std::copy(in + i, in + i + m, gated_);
                else gate_.process(in + i, gated_, m);
                if (locked) transcribe(gated_, m);
                write_output(out + i, m);
            }
            done = std::max(done, until);
            if (k < event_count) read_midi(events[k], std::min(until, frames ? frames - 1 : 0), locked);
        }
        params_[kSustainState] = pedal_down() ? 1.0f : 0.0f;
        params_[kModelLoaded] = static_cast<float>(model_loaded_.load());
        params_[kModelFormat] = static_cast<float>(model_format_.load());
        params_[kModelError] = static_cast<float>(model_error_.load());
        if (locked) {
            end_midi(frames);
            show_notes();
            swap_lock_.unlock();
        }
    }

private:
    struct NullSink : g2m::EventSink {
        void note(const g2m::NoteEvent&) override {}
    };

    // The resampler for a host rate other than the model's, and its output
    // for one chunk
    struct Resampling {
        g2m::Resampler resampler;
        std::vector<float> out;
        std::vector<std::uint32_t> source;  // the chunk's sample that completed each output
        Resampling(int host_rate, int model_rate)
            : resampler(host_rate, model_rate), out(resampler.max_output(kChunk)), source(out.size()) {}
    };

    // The Transcriber's events, to the sustain pedal, on the host's samples
    struct MidiSink : g2m::EventSink {
        G2mPlugin* plugin;
        void note(const g2m::NoteEvent& e) override {
            g2m::NoteEvent host = e;
            host.sample = plugin->host_sample(e.sample);
            plugin->sustain_.note(host, plugin->bend_sink_);
        }
        void bend(const g2m::BendEvent& e) override {
            g2m::BendEvent host = e;
            host.sample = plugin->host_sample(e.sample);
            plugin->sustain_.bend(host, plugin->bend_sink_);
        }
    };

    // The sustain pedal's events, to the bend mode and on to write_sink_
    struct BendSink : g2m::EventSink {
        G2mPlugin* plugin;
        void note(const g2m::NoteEvent& e) override { plugin->bends_.note(e, plugin->write_sink_); }
        void bend(const g2m::BendEvent& e) override { plugin->bends_.bend(e, plugin->write_sink_); }
    };

    // Writes note events at the sample before each event's frame end,
    // within this run() call's buffer.
    struct WriteSink : g2m::NoteSink {
        G2mPlugin* plugin;
        std::uint64_t start;  // host_position_ at the start of the buffer
        void note(const g2m::NoteEvent& e) override { plugin->note(e, offset(e.sample)); }
        void bend(const g2m::BendEvent& e) override { plugin->bend(e, offset(e.sample)); }
        void retrigger(std::uint64_t sample, int from, int to, int velocity) override {
            plugin->retrigger(from, to, velocity, offset(sample));
        }
        void channel_bend(std::uint64_t sample, int value) override { plugin->channel_bend(value, offset(sample)); }
        // The event's frame ends at `sample`; it belongs on the frame's last
        // sample, which arrived in this buffer.
        std::uint32_t offset(std::uint64_t sample) const { return static_cast<std::uint32_t>(sample - start - 1); }
    };

    // Writes MPE messages at `offset` within this run() call's buffer.
    struct MpeSink : g2m::MidiWriter {
        G2mPlugin* plugin;
        std::uint32_t offset = 0;
        void midi(std::uint8_t status, std::uint8_t data1, std::uint8_t data2) override {
            plugin->send_raw(offset, status, data1, data2);
        }
    };

    // A state value as a file path: "bundled:<name>" is in the bundled
    // models' folder ("" if the plugin has none).
    std::string resolve(const std::string& value) const {
        if (value.compare(0, sizeof kBundledPrefix - 1, kBundledPrefix) != 0) return value;
        if (models_dir_.empty()) {
            d_stderr2("guitar-2-midi: no bundled models folder for %s", value.c_str());
            return {};
        }
        return models_dir_ + "/" + value.substr(sizeof kBundledPrefix - 1) + ".g2m";
    }

    // Not the audio thread: loads bundled model `choice` (1-based; 0, the
    // model file, keeps the current model).
    void load_choice(int choice) {
        if (choice < 1 || choice > kBundledModelCount) return;
        std::lock_guard<std::mutex> guard(path_mutex_);
        load_value(std::string(kBundledPrefix) + kBundledModels[choice - 1], choice);
    }

    // The bundled model a state value names, as a Model choice (0 if none)
    static int bundled_choice(const std::string& value) {
        for (int i = 0; i < kBundledModelCount; ++i)
            if (value == std::string(kBundledPrefix) + kBundledModels[i]) return i + 1;
        return 0;
    }

    // Under path_mutex_: loads a model state value and swaps it in; `which`
    // is what Model Loaded shows for it (0, the model file, or a bundled
    // model's choice). A model that can't be used keeps the current one.
    void load_value(const std::string& value, int which) {
        std::string path = resolve(value);
        g2m::Transcriber* transcriber = nullptr;
        int format = 0;
        Resampling* resampling = nullptr;
        if (path.empty() || !load(path.c_str(), transcriber, resampling, format)) {
            model_error_ = kModelUnloadable;
            return;
        }
        model_path_ = value;
        model_loaded_ = which;
        model_format_ = format;
        model_error_ = transcriber ? kModelOk : kModelWrongRate;
        install(transcriber, resampling);
    }

    // The loader thread: loads the bundled model the Model parameter asks for.
    void watch_choice() {
        while (!stop_loader_) {
            int choice = requested_choice_;
            if (choice != loaded_choice_) {
                load_choice(choice);
                loaded_choice_ = choice;
            }
            std::this_thread::sleep_for(kLoaderPoll);
        }
    }

    bool pedal_down() const { return midi_pedal_ || params_[kSustain] >= 0.5f; }

    // The pedal's CC moves the pedal at `offset` in the buffer.
    void read_midi(const MidiEvent& event, std::uint32_t offset, bool locked) {
        if (event.size != 3 || (event.data[0] & 0xF0) != 0xB0) return;
        if (event.data[1] != static_cast<std::uint8_t>(std::lround(params_[kSustainCc]))) return;
        midi_pedal_ = event.data[2] >= 64;
        if (locked) sustain_.set_pedal(pedal_down(), sample_at(offset), bend_sink_);
    }

    // The Sustain events' sample for `offset` in this buffer
    std::uint64_t sample_at(std::uint32_t offset) const { return write_sink_.start + offset + 1; }

    // Output Level and Mute: a linear ramp to the new gain.
    void set_output_gain() {
        float target = params_[kOutputMute] >= 0.5f ? 0.0f : std::pow(10.0f, params_[kOutputLevel] / 20.0f);
        if (target == out_target_) return;
        out_target_ = target;
        out_ramp_ = static_cast<std::uint32_t>(std::max(1L, std::lround(kOutputRampSeconds * getSampleRate())));
        out_step_ = (target - out_gain_) / static_cast<float>(out_ramp_);
    }

    void write_output(float* out, std::uint32_t n) {
        if (out_ramp_ == 0 && out_gain_ == 1.0f) {
            std::copy(gated_, gated_ + n, out);  // bit for bit
            return;
        }
        for (std::uint32_t j = 0; j < n; ++j) {
            if (out_ramp_ > 0 && --out_ramp_ == 0) out_gain_ = out_target_;
            else if (out_ramp_ > 0) out_gain_ += out_step_;
            out[j] = gated_[j] * out_gain_;
        }
    }

    // Before the buffer's audio: the output mode's and settings' changes.
    void begin_midi(std::uint32_t n) {
        int channel = std::clamp(static_cast<int>(std::lround(params_[kMidiChannel])), 1, 16) - 1;
        OutputMode mode = params_[kOutputMode] >= 0.5f ? OutputMode::mpe : OutputMode::plain;
        int range = std::clamp(static_cast<int>(std::lround(params_[kBendRange])), 1, g2m::kMaxBendRange);
        bool mode_changed = mode != mode_;
        if (mode_changed || (mode_ == OutputMode::plain && channel != channel_)) {
            notes_off_pending_ = true;  // end the held notes as they were sent
        }
        MpeSink mpe_sink;
        mpe_sink.plugin = this;
        if (notes_off_pending_) {
            if (mode_ == OutputMode::mpe) {
                mpe_.all_notes_off(mpe_sink);
            } else {
                for (int pitch = 0; pitch < 128; ++pitch)
                    if (held_[pitch]) send_midi(0, 0x80, pitch, 0);
            }
            sustain_.reset();
            bends_.reset();
            notes_off_pending_ = false;
        }
        if (mode_changed && mode_ == OutputMode::mpe) g2m::MpeOutput::release_zone(mpe_sink);
        mode_ = mode;
        channel_ = channel;
        if (mode_ == OutputMode::mpe) {
            if (mode_changed || configure_pending_) {
                g2m::MpeOutput::configure(range, mpe_sink);
                mpe_.reset_bends();
            }
            else if (range != bend_range_sent_) g2m::MpeOutput::send_bend_range(range, mpe_sink);
            bend_range_sent_ = range;
        }
        configure_pending_ = false;

        if (transcriber_) apply_settings(range);

        write_sink_.plugin = this;
        write_sink_.start = host_position_;
        auto sustain_mode = static_cast<g2m::SustainMode>(std::clamp(static_cast<int>(std::lround(params_[kSustainMode])), 0, 3));
        bend_sink_.plugin = this;
        bends_.set_mode(static_cast<g2m::BendMode>(std::clamp(static_cast<int>(std::lround(params_[kBendMode])), 0, 2)));
        sustain_.set_mode(sustain_mode, sample_at(0), bend_sink_);
        sustain_.set_pedal(pedal_down(), sample_at(0), bend_sink_);

        // the input gain, ramped linearly over the buffer
        gain_target_ = std::pow(10.0f, params_[kInputGain] / 20.0f);
        gain_step_ = (gain_target_ - gain_) / static_cast<float>(n ? n : 1);
        sink_.plugin = this;
        peak_ = 0;
    }

    // A chunk of gated audio, after the input gain, into a scratch chunk:
    // what the model hears, and what the level meter shows.
    void transcribe(const float* in, std::uint32_t m) {
        for (std::uint32_t j = 0; j < m; ++j) {
            gain_ += gain_step_;
            float x = in[j] * gain_;
            scaled_[j] = std::isfinite(x) ? x : 0.0f;  // a NaN would stay in the GRU's state for good
            peak_ = std::max(peak_, std::fabs(scaled_[j]));
        }
        chunk_start_ = host_position_;
        host_position_ += m;
        if (!transcriber_) return;
        model_start_ = transcriber_->position();
        if (resampling_) {
            Resampling& r = *resampling_;
            std::size_t k = r.resampler.process(scaled_, m, r.out.data(), r.source.data());
            chunk_source_ = r.source.data();
            transcriber_->process(r.out.data(), k, sink_);
        } else {
            chunk_source_ = nullptr;
            transcriber_->process(scaled_, m, sink_);
        }
    }

    // The host's sample (counted as host_position_) for the Transcriber's
    // sample `model`, both the end of a frame: the chunk's sample that
    // completed the frame, plus one. Only for events of transcribe()'s chunk.
    std::uint64_t host_sample(std::uint64_t model) const {
        std::uint64_t j = model - 1 - model_start_;  // the frame's last sample, in this chunk's output
        return chunk_start_ + (chunk_source_ ? chunk_source_[j] : j) + 1;
    }

    void end_midi(std::uint32_t n) {
        gain_ = gain_target_;
        float fall = static_cast<float>(std::exp(-static_cast<double>(n) / (kMeterFallSeconds * getSampleRate())));
        level_ = std::max(peak_, level_ * fall);
        params_[kInputLevel] = level_ > 0 ? std::max(20.0f * std::log10(level_), kMeterFloorDb) : kMeterFloorDb;
    }

    // Onset Threshold: 0 means the model's own (tuned) threshold. MPE Bend
    // Range: the range the bends are scaled to, whatever the model's (format
    // version 4), so they match the RPN 0 sent.
    void apply_settings(int bend_range) {
        float threshold = params_[kOnsetThreshold];
        float wanted = threshold > 0.005f ? threshold : transcriber_->model().decoder.onset_threshold;
        const g2m::DecoderSettings& now = transcriber_->decoder_settings();
        if (wanted == now.onset_threshold && bend_range == now.bend_range) return;
        g2m::DecoderSettings settings = now;
        settings.onset_threshold = wanted;
        settings.bend_range = bend_range;
        transcriber_->set_decoder_settings(settings);
    }

    void note(const g2m::NoteEvent& e, std::uint32_t offset) {
        if (mode_ == OutputMode::mpe) {
            MpeSink mpe;
            mpe.plugin = this;
            mpe.offset = offset;
            mpe_.note(e.pitch, e.velocity, mpe, e.has_strings ? e.strings : nullptr, e.onset_type,
                      e.has_bend ? e.bend : -1);
        } else {
            send_midi(offset, e.velocity > 0 ? 0x90 : 0x80, e.pitch, e.velocity);
        }
    }

    // MPE mode only: plain MIDI carries no bends
    void bend(const g2m::BendEvent& e, std::uint32_t offset) {
        if (mode_ != OutputMode::mpe) return;
        MpeSink mpe;
        mpe.plugin = this;
        mpe.offset = offset;
        mpe_.bend(e.pitch, e.bend, mpe);
    }

    // A note ends and a note at the new pitch starts on the same channel
    void retrigger(int from, int to, int velocity, std::uint32_t offset) {
        if (mode_ == OutputMode::mpe) {
            MpeSink mpe;
            mpe.plugin = this;
            mpe.offset = offset;
            mpe_.retrigger(from, to, velocity, mpe);
        } else {
            send_midi(offset, 0x80, from, 0);
            send_midi(offset, 0x90, to, velocity);
        }
    }

    // Plain output: the channel's pitch bend
    void channel_bend(int value, std::uint32_t offset) {
        send_raw(offset, static_cast<std::uint8_t>(0xE0 | channel_), static_cast<std::uint8_t>(value & 0x7F),
                 static_cast<std::uint8_t>(value >> 7));
    }

    // A note-on or note-off on the plain output's channel
    void send_midi(std::uint32_t offset, std::uint8_t status, int pitch, int velocity) {
        send_raw(offset, static_cast<std::uint8_t>(status | channel_), static_cast<std::uint8_t>(pitch),
                 static_cast<std::uint8_t>(velocity));
    }

    // Any three-byte message; note-ons and note-offs update the held notes.
    void send_raw(std::uint32_t offset, std::uint8_t status, std::uint8_t data1, std::uint8_t data2) {
        MidiEvent event;
        event.frame = offset;
        event.size = 3;
        event.data[0] = status;
        event.data[1] = data1;
        event.data[2] = data2;
        event.data[3] = 0;
        event.dataExt = nullptr;
        if (!writeMidiEvent(event)) return;  // buffer full
        std::uint8_t kind = status & 0xF0;
        if (kind == 0x80 || kind == 0x90) held_[data1] = kind == 0x90;
    }

    // Loads a model and builds its Transcriber, and a resampler if the host
    // runs at another rate (allocates: not on the audio thread). False if the
    // file can't be used; true with a null Transcriber if it's a valid model
    // for a rate the resampler can't convert from.
    bool load(const char* path, g2m::Transcriber*& out, Resampling*& resampling, int& format) {
        g2m::LoadResult r = g2m::load_model(path);
        if (!r.model) {
            d_stderr2("guitar-2-midi: %s", r.error.c_str());
            return false;
        }
        out = nullptr;
        resampling = nullptr;
        format = r.model->format_version;
        int rate = static_cast<int>(std::lround(getSampleRate()));
        int model_rate = r.model->config.sample_rate;
        if (rate != model_rate) {
            if (!g2m::Resampler::supported(rate, model_rate)) {
                d_stderr2("guitar-2-midi: %s is for %d Hz and can't be resampled from %d Hz: no MIDI output", path,
                          model_rate, rate);
                return true;
            }
            resampling = new Resampling(rate, model_rate);
        }
        out = new g2m::Transcriber(std::shared_ptr<const g2m::Model>(std::move(r.model)));
        d_stdout("guitar-2-midi: loaded %s", path);
        return true;
    }

    // Swaps in a Transcriber (null: no MIDI) and its resampler (null: none
    // needed), and frees the old ones. Ends the held notes at the start of
    // the next run.
    void install(g2m::Transcriber* transcriber, Resampling* resampling) {
        swap_lock_.lock();
        g2m::Transcriber* old = transcriber_;
        Resampling* old_resampling = resampling_;
        transcriber_ = transcriber;
        resampling_ = resampling;
        notes_off_pending_ = true;
        swap_lock_.unlock();
        delete old;
        delete old_resampling;
    }

    // The held notes on the note parameters, lowest first.
    void show_notes() {
        int count = 0;
        for (int pitch = 0; pitch < 128; ++pitch) {
            if (!held_[pitch]) continue;
            if (count < kNoteSlots) params_[kNote1 + count] = static_cast<float>(pitch);
            ++count;
        }
        for (int slot = count; slot < kNoteSlots; ++slot) params_[kNote1 + slot] = -1.0f;
        params_[kNoteCount] = static_cast<float>(count);
    }

    float params_[kParamCount] = {};
    std::vector<std::uint32_t> preset_params_;  // the parameters presets set, with preset_info_
    std::vector<g2m::presets::ParameterInfo> preset_info_;

    SwapLock swap_lock_;  // guards transcriber_, resampling_ and the pending flags against run()
    g2m::Transcriber* transcriber_ = nullptr;
    Resampling* resampling_ = nullptr;
    // transcribe()'s: the host samples given to it (a count that only goes
    // up), the chunk's first, the Transcriber's position at the chunk's
    // start, the resampler's sources for the chunk (null: none)
    std::uint64_t host_position_ = 0;
    std::uint64_t chunk_start_ = 0;
    std::uint64_t model_start_ = 0;
    const std::uint32_t* chunk_source_ = nullptr;
    bool notes_off_pending_ = false;
    bool configure_pending_ = true;  // send the MPE configuration with the next run, if in MPE mode

    mutable std::mutex path_mutex_;  // non-real-time callers: setState, getState, sampleRateChanged, the loader
    std::string model_path_;  // a file path or "bundled:<name>"
    // the Model Loaded, Model Format and Model Error outputs, set by load_value
    std::atomic<int> model_loaded_{-1}, model_format_{0}, model_error_{kModelOk};

    std::string models_dir_;  // the bundled models' folder
    std::thread loader_;
    std::atomic<bool> stop_loader_{false};
    std::atomic<int> requested_choice_{0};  // the Model parameter, as last set
    int loaded_choice_ = 0;  // the loader's: the choice last acted on

    g2m::NoiseGate gate_;
    float gated_[kChunk];
    float out_gain_ = 1.0f, out_target_ = 1.0f, out_step_ = 0;
    std::uint32_t out_ramp_ = 0;  // samples left in the output gain's ramp

    MidiSink sink_;
    BendSink bend_sink_;
    g2m::BendModeFilter bends_;
    WriteSink write_sink_;  // this buffer's
    g2m::Sustain sustain_;
    bool midi_pedal_ = false;  // the Sustain CC's last value was 64 or more
    float scaled_[kChunk];
    float gain_ = 1.0f, gain_target_ = 1.0f, gain_step_ = 0;
    float peak_ = 0;  // this buffer's, after the input gain
    float level_ = 0;  // the level meter's decaying peak (linear)
    int channel_ = 0;
    OutputMode mode_ = OutputMode::plain;
    g2m::MpeOutput mpe_;  // the strings' notes in MPE mode
    int bend_range_sent_ = 0;
    bool held_[128] = {};  // notes sent and not yet ended

    DISTRHO_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(G2mPlugin)
};

Plugin* createPlugin() {
    return new G2mPlugin();
}

END_NAMESPACE_DISTRHO
