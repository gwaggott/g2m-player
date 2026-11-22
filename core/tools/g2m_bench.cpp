// g2m-bench --model M.g2m [--wav IN.wav] [--seconds S] [--stages]
//
// Single-thread time per block (model + decoder), against the block's
// real-time budget (hop / sample rate). Runs noise, silence (where
// denormals appear if they aren't flushed) and optionally a WAV, each with
// and without flushing denormals to zero, S seconds of audio each
// (default 20; a WAV is run whole). --stages also prints the mean time per
// block of each stage (front end, gated layers, GRU, head, decoder) and its
// share of their sum; reading the clock between stages adds a little to the
// totals.

#include <g2m/denormals.h>
#include <g2m/transcriber.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "audio_io.h"

namespace {

struct Discard : g2m::EventSink {
    long notes = 0;
    void note(const g2m::NoteEvent& e) override { notes += e.velocity > 0; }
};

void run(const char* name, std::shared_ptr<const g2m::Model> model, const std::vector<float>& audio, bool flush,
         bool stages) {
    int hop = model->config.hop;
    std::size_t blocks = audio.size() / hop;
    double budget = 1e6 * hop / model->config.sample_rate;
    g2m::Transcriber transcriber(model);
    g2m::StageTimes stage_times;
    if (stages) transcriber.time_stages(&stage_times);
    Discard sink;
    std::vector<double> times(blocks);
    {
        std::optional<g2m::FlushDenormals> guard;
        if (flush) guard.emplace();
        for (std::size_t k = 0; k < blocks; ++k) {
            auto t0 = std::chrono::steady_clock::now();
            transcriber.process(audio.data() + k * hop, hop, sink);
            times[k] = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
        }
    }
    double total = 0;
    for (double t : times) total += t;
    std::sort(times.begin(), times.end());
    std::printf("%-8s %-9s %7zu blocks  mean %6.0f  median %6.0f  p99 %6.0f  max %6.0f us  (%3.0f%% of %.0f us)  %ld notes\n",
                name, flush ? "flushed" : "unflushed", blocks, total / blocks, times[blocks / 2],
                times[blocks * 99 / 100], times.back(), 100 * total / blocks / budget, budget, sink.notes);
    if (!stages) return;
    const g2m::StageTimes& s = stage_times;
    double sum = s.frontend + s.layers + s.gru + s.head + s.decoder;
    std::printf("  stages, mean us per block (share):");
    for (auto [label, t] : {std::pair{"front end", s.frontend}, {"layers", s.layers}, {"GRU", s.gru}, {"head", s.head},
                            {"decoder", s.decoder}})
        std::printf("  %s %.1f (%.0f%%)", label, t / blocks, 100 * t / sum);
    std::printf("\n");
}

int usage() {
    std::fprintf(stderr, "usage: g2m-bench --model M.g2m [--wav IN.wav] [--seconds S] [--stages]\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    std::string model_path, wav_path;
    double seconds = 20;
    bool stages = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        bool has_value = i + 1 < argc;
        if (arg == "--model" && has_value) model_path = argv[++i];
        else if (arg == "--wav" && has_value) wav_path = argv[++i];
        else if (arg == "--seconds" && has_value) seconds = std::strtod(argv[++i], nullptr);
        else if (arg == "--stages") stages = true;
        else return usage();
    }
    if (model_path.empty() || seconds <= 0) return usage();
    g2m::LoadResult loaded = g2m::load_model(model_path);
    if (!loaded.model) {
        std::fprintf(stderr, "%s\n", loaded.error.c_str());
        return 1;
    }
    std::shared_ptr<const g2m::Model> model = std::move(loaded.model);

    std::size_t n = static_cast<std::size_t>(seconds * model->config.sample_rate);
    std::vector<float> noise(n);
    std::mt19937 rng(1);
    std::normal_distribution<float> normal(0.0f, 0.1f);
    for (float& x : noise) x = normal(rng);
    std::vector<float> silence(n, 0.0f);

    for (bool flush : {true, false}) {
        run("noise", model, noise, flush, stages);
        run("silence", model, silence, flush, stages);
        if (!wav_path.empty()) {
            g2m::tools::Audio audio;
            std::string error;
            if (!g2m::tools::read_wav(wav_path, audio, error)) {
                std::fprintf(stderr, "%s\n", error.c_str());
                return 1;
            }
            run("wav", model, audio.samples, flush, stages);
        }
    }
    return 0;
}
