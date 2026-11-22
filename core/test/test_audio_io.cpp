#include "audio_io.h"

#include <cstring>

#include "check.h"

using g2m::tools::Audio;

// A WAV file: fmt chunk fields, then the data bytes.
static std::vector<std::uint8_t> wav(std::uint16_t format, int channels, int bits, const std::vector<std::uint8_t>& data,
                                     bool extensible = false) {
    std::vector<std::uint8_t> out;
    auto u16 = [&](unsigned v) { out.push_back(v & 0xFF); out.push_back((v >> 8) & 0xFF); };
    auto u32 = [&](unsigned v) { u16(v & 0xFFFF); u16(v >> 16); };
    auto tag = [&](const char* t) { out.insert(out.end(), t, t + 4); };
    tag("RIFF");
    u32(0);  // not checked
    tag("WAVE");
    tag("LIST");  // an unrelated chunk first, odd-sized, to check padding
    u32(3);
    out.insert(out.end(), {1, 2, 3, 0});
    tag("fmt ");
    u32(extensible ? 40 : 16);
    u16(extensible ? 0xFFFE : format);
    u16(channels);
    u32(48000);
    u32(48000 * channels * bits / 8);
    u16(channels * bits / 8);
    u16(bits);
    if (extensible) {
        u16(22);
        u16(bits);
        u32(0);
        u16(format);  // the subformat GUID starts with the format code
        out.insert(out.end(), 14, 0);
    }
    tag("data");
    u32(static_cast<unsigned>(data.size()));
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

static Audio parse_ok(const std::vector<std::uint8_t>& bytes) {
    Audio a;
    std::string error;
    CHECK_MSG(g2m::tools::parse_wav(bytes, a, error), "%s", error.c_str());
    CHECK(a.sample_rate == 48000);
    return a;
}

TEST(pcm_formats) {
    // 16-bit stereo: frames (16384, -16384) and (32767, -32768), averaged
    Audio a = parse_ok(wav(1, 2, 16, {0x00, 0x40, 0x00, 0xC0, 0xFF, 0x7F, 0x00, 0x80}));
    CHECK(a.samples.size() == 2 && a.samples[0] == 0.0f && a.samples[1] == (32767.0f / 32768 - 1) / 2);
    a = parse_ok(wav(1, 1, 8, {0, 128, 255}));
    CHECK(a.samples.size() == 3 && a.samples[0] == -1.0f && a.samples[1] == 0.0f && a.samples[2] == 127.0f / 128);
    a = parse_ok(wav(1, 1, 24, {0x00, 0x00, 0x40, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x80}));
    CHECK(a.samples.size() == 3 && a.samples[0] == 0.5f && a.samples[1] == -1.0f / 8388608 && a.samples[2] == -1.0f);
    a = parse_ok(wav(1, 1, 24, {0x00, 0x00, 0x40}, true));
    CHECK(a.samples.size() == 1 && a.samples[0] == 0.5f);
    a = parse_ok(wav(1, 1, 32, {0x00, 0x00, 0x00, 0xC0}));
    CHECK(a.samples.size() == 1 && a.samples[0] == -0.5f);
}

TEST(float_formats) {
    float f[2] = {0.25f, -1.5f};
    std::vector<std::uint8_t> data(8);
    std::memcpy(data.data(), f, 8);
    Audio a = parse_ok(wav(3, 1, 32, data));
    CHECK(a.samples.size() == 2 && a.samples[0] == 0.25f && a.samples[1] == -1.5f);
    double d = 0.125;
    std::vector<std::uint8_t> data64(8);
    std::memcpy(data64.data(), &d, 8);
    a = parse_ok(wav(3, 1, 64, data64, true));
    CHECK(a.samples.size() == 1 && a.samples[0] == 0.125f);
}

TEST(partial_frame_ignored) {
    Audio a = parse_ok(wav(1, 2, 16, {0, 0, 0, 0, 1, 2}));
    CHECK(a.samples.size() == 1);
}

TEST(rejects) {
    Audio a;
    std::string error;
    CHECK(!g2m::tools::parse_wav({'R', 'I', 'F', 'F'}, a, error));
    CHECK(!g2m::tools::parse_wav(wav(2, 1, 16, {0, 0}), a, error) && check::contains(error, "unsupported"));  // ADPCM
    CHECK(!g2m::tools::parse_wav(wav(3, 1, 16, {0, 0}), a, error));  // 16-bit float
    std::vector<std::uint8_t> no_data = wav(1, 1, 16, {});
    no_data.resize(no_data.size() - 8);
    CHECK(!g2m::tools::parse_wav(no_data, a, error) && check::contains(error, "no fmt or data"));
    CHECK(!g2m::tools::read_wav("/no/such/file.wav", a, error) && check::contains(error, "can't open"));
}

TEST(midi_file) {
    // note-on 40 at 0.5 s, note-off at 1 s (frame ends, at 48 kHz)
    std::vector<g2m::NoteEvent> events = {{24000, 40, 100}, {48000, 40, 0}};
    std::vector<std::uint8_t> expected = {
        'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0x03, 0xC0,  // format 0, 1 track, 960 PPQ
        'M', 'T', 'r', 'k', 0, 0, 0, 21,
        0x00, 0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20,  // tempo 120 BPM
        0x87, 0x40, 0x90, 40, 100,  // 960 ticks later: on
        0x87, 0x40, 0x80, 40, 0,  // 960 ticks later: off
        0x00, 0xFF, 0x2F, 0x00,
    };
    CHECK(g2m::tools::midi_file(events, 48000) == expected);
    // a frame end that isn't a whole tick: 64 samples = 2.56 ticks → 3
    std::vector<std::uint8_t> one = g2m::tools::midi_file(std::vector<g2m::NoteEvent>{{64, 38, 1}}, 48000);
    CHECK(one[29] == 3);
    // any messages: here on channel 3, with a controller
    std::vector<g2m::tools::MidiMessage> messages = {{0, 0xB2, 101, 0}, {24000, 0x92, 40, 100}, {48000, 0x82, 40, 0}};
    std::vector<std::uint8_t> mpe = g2m::tools::midi_file(messages, 48000);
    std::vector<std::uint8_t> want(expected);
    want[21] += 4;  // the track is 4 bytes longer
    want[31] = 0x92;
    want[36] = 0x82;
    want.insert(want.begin() + 29, {0x00, 0xB2, 101, 0});  // after the tempo
    CHECK(mpe == want);
}
