#include "audio_io.h"

#include <cstring>
#include <fstream>
#include <iterator>

namespace g2m::tools {

namespace {

std::uint32_t u32(const std::uint8_t* p) {
    return std::uint32_t(p[0]) | std::uint32_t(p[1]) << 8 | std::uint32_t(p[2]) << 16 | std::uint32_t(p[3]) << 24;
}

std::uint16_t u16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | p[1] << 8);
}

constexpr std::uint16_t kPcm = 1;
constexpr std::uint16_t kFloat = 3;
constexpr std::uint16_t kExtensible = 0xFFFE;

// One sample → float, as reference.py converts integers (int → float32,
// then divided by 2^(bits - 1) in float32).
float sample(const std::uint8_t* p, int bytes, bool is_float) {
    if (is_float) {
        if (bytes == 4) {
            std::uint32_t bits = u32(p);
            float f;
            std::memcpy(&f, &bits, 4);
            return f;
        }
        std::uint64_t bits = u32(p) | std::uint64_t(u32(p + 4)) << 32;
        double d;
        std::memcpy(&d, &bits, 8);
        return static_cast<float>(d);
    }
    switch (bytes) {
        case 1: return (static_cast<float>(p[0]) - 128.0f) / 128.0f;
        case 2: return static_cast<float>(static_cast<std::int16_t>(u16(p))) / 32768.0f;
        case 3: {
            std::uint32_t bits = std::uint32_t(p[0]) << 8 | std::uint32_t(p[1]) << 16 | std::uint32_t(p[2]) << 24;
            return static_cast<float>(static_cast<std::int32_t>(bits) >> 8) / 8388608.0f;  // sign-extended
        }
        default: return static_cast<float>(static_cast<std::int32_t>(u32(p))) / 2147483648.0f;
    }
}

void append_varlen(std::vector<std::uint8_t>& out, std::uint32_t n) {
    std::uint8_t bytes[5];
    int count = 0;
    bytes[count++] = n & 0x7F;
    while (n > 0x7F) {
        n >>= 7;
        bytes[count++] = 0x80 | (n & 0x7F);
    }
    while (count > 0) out.push_back(bytes[--count]);
}

void append_be32(std::vector<std::uint8_t>& out, std::uint32_t n) {
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<std::uint8_t>(n >> shift));
}

}  // namespace

bool parse_wav(const std::vector<std::uint8_t>& bytes, Audio& out, std::string& error) {
    if (bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) != 0 || std::memcmp(bytes.data() + 8, "WAVE", 4) != 0) {
        error = "not a RIFF/WAVE file";
        return false;
    }
    int channels = 0, bytes_per_sample = 0;
    bool is_float = false, have_format = false;
    const std::uint8_t* data = nullptr;
    std::size_t data_size = 0;
    for (std::size_t pos = 12; pos + 8 <= bytes.size();) {
        const std::uint8_t* chunk = bytes.data() + pos;
        std::size_t size = u32(chunk + 4);
        std::size_t available = bytes.size() - pos - 8;
        if (std::memcmp(chunk, "fmt ", 4) == 0) {
            if (size < 16 || size > available) {
                error = "bad fmt chunk";
                return false;
            }
            std::uint16_t format = u16(chunk + 8);
            channels = u16(chunk + 10);
            out.sample_rate = static_cast<int>(u32(chunk + 12));
            int bits = u16(chunk + 22);
            if (format == kExtensible && size >= 40) format = u16(chunk + 32);  // the subformat GUID's first two bytes
            is_float = format == kFloat;
            if ((format != kPcm && format != kFloat) || bits % 8 != 0 ||
                (is_float ? bits != 32 && bits != 64 : bits < 8 || bits > 32) || channels < 1) {
                error = "unsupported WAV format " + std::to_string(format) + ", " + std::to_string(bits) + " bits";
                return false;
            }
            bytes_per_sample = bits / 8;
            have_format = true;
        } else if (std::memcmp(chunk, "data", 4) == 0) {
            data = chunk + 8;
            data_size = size > available ? available : size;  // tolerate a truncated last chunk
        }
        pos += 8 + size + (size & 1);
    }
    if (!have_format || !data) {
        error = "no fmt or data chunk";
        return false;
    }
    std::size_t frame_bytes = static_cast<std::size_t>(channels) * bytes_per_sample;
    std::size_t frames = data_size / frame_bytes;
    out.samples.resize(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        const std::uint8_t* frame = data + i * frame_bytes;
        float sum = 0;
        for (int c = 0; c < channels; ++c) sum += sample(frame + c * bytes_per_sample, bytes_per_sample, is_float);
        out.samples[i] = channels == 1 ? sum : sum / static_cast<float>(channels);
    }
    return true;
}

bool read_wav(const std::string& path, Audio& out, std::string& error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "can't open " + path;
        return false;
    }
    std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    if (!parse_wav(bytes, out, error)) {
        error = path + ": " + error;
        return false;
    }
    return true;
}

std::vector<std::uint8_t> midi_file(const std::vector<NoteEvent>& events, int sample_rate) {
    std::vector<MidiMessage> messages;
    for (const NoteEvent& e : events)
        messages.push_back({e.sample, static_cast<std::uint8_t>(e.velocity > 0 ? 0x90 : 0x80),
                            static_cast<std::uint8_t>(e.pitch), static_cast<std::uint8_t>(e.velocity)});
    return midi_file(messages, sample_rate);
}

std::vector<std::uint8_t> midi_file(const std::vector<MidiMessage>& messages, int sample_rate) {
    constexpr std::uint64_t kTicksPerSecond = 1920;  // 960 PPQ at 120 BPM
    std::vector<std::uint8_t> track = {0x00, 0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20};  // tempo: 500,000 us per beat
    std::uint64_t now = 0;
    for (const MidiMessage& m : messages) {
        std::uint64_t tick = (m.sample * kTicksPerSecond * 2 + sample_rate) / (2 * static_cast<std::uint64_t>(sample_rate));
        append_varlen(track, static_cast<std::uint32_t>(tick - now));
        track.insert(track.end(), {m.status, m.data1, m.data2});
        now = tick;
    }
    track.insert(track.end(), {0x00, 0xFF, 0x2F, 0x00});

    std::vector<std::uint8_t> out = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0x03, 0xC0};  // format 0, 1 track, 960 PPQ
    out.insert(out.end(), {'M', 'T', 'r', 'k'});
    append_be32(out, static_cast<std::uint32_t>(track.size()));
    out.insert(out.end(), track.begin(), track.end());
    return out;
}

}  // namespace g2m::tools
