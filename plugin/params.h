// The plugin's parameters, shared by the plugin and its UI: their indices
// and their definitions (DPF's Parameter), so the UI has the same ranges,
// labels and symbols (for preset files) as the host.
#pragma once

#include "DistrhoDetails.hpp"
#include "g2m_bundled_models.h"

#include <g2m/mpe.h>
#include <g2m/noise_gate.h>
#include <g2m/note_name.h>

#include <cstdint>

namespace g2m::param {

using namespace DISTRHO_NAMESPACE;

constexpr int kNoteSlots = 6;  // notes shown on parameters: the lowest held
constexpr float kMeterFloorDb = -90.0f;  // the Input Level's floor

// Parameters, in LV2 port order after the audio and event ports. Their
// symbols are the old LV2 plugin's port symbols. New ones are appended.
enum Param : std::uint32_t {
    kInputGain, kMidiChannel, kOnsetThreshold, kInputLevel, kNoteCount,
    kNote1,  // kNote1 + slot, for kNoteSlots slots
    kOutputMode = kNote1 + kNoteSlots, kBendRange, kModelChoice,
    kGateThreshold, kGateRelease, kOutputLevel, kOutputMute,
    kSustainMode, kSustainCc, kSustain, kSustainState, kBendMode,
    kModelLoaded, kModelFormat, kModelError, kParamCount
};

// kModelError's values
enum ModelError { kModelOk, kModelUnloadable, kModelWrongRate };

inline void init_note_parameter(int slot, Parameter& p) {
    static const char* const symbols[kNoteSlots] = {"note_1", "note_2", "note_3", "note_4", "note_5", "note_6"};
    static const char* const names[kNoteSlots] = {"Note 1", "Note 2", "Note 3", "Note 4", "Note 5", "Note 6"};
    p.hints = kParameterIsOutput | kParameterIsInteger;
    p.symbol = symbols[slot];
    p.name = names[slot];
    p.description = "The notes on, lowest first: Note 1 is the lowest (- when fewer are on).";
    p.ranges = ParameterRanges(-1.0f, -1.0f, 127.0f);
    p.enumValues.count = 129;
    p.enumValues.restrictedMode = true;
    p.enumValues.values = new ParameterEnumerationValue[129];
    p.enumValues.values[0] = ParameterEnumerationValue(-1.0f, "-");
    for (int pitch = 0; pitch < 128; ++pitch) {
        char name[g2m::kNoteNameSize];
        g2m::note_name(pitch, name);
        p.enumValues.values[pitch + 1] = ParameterEnumerationValue(static_cast<float>(pitch), name);
    }
}

// Parameter `index`'s definition
inline void init(std::uint32_t index, Parameter& p) {
    switch (index) {
        case kInputGain:
            p.hints = kParameterIsAutomatable;
            p.symbol = "input_gain";
            p.name = "Input Gain";
            p.description = "Gain applied to the signal the model hears (the audio output is unaffected).";
            p.unit = "dB";
            p.ranges = ParameterRanges(0.0f, -24.0f, 24.0f);
            break;
        case kMidiChannel:
            p.hints = kParameterIsAutomatable | kParameterIsInteger;
            p.symbol = "midi_channel";
            p.name = "MIDI Channel";
            p.ranges = ParameterRanges(1.0f, 1.0f, 16.0f);
            break;
        case kOnsetThreshold:
            p.hints = kParameterIsAutomatable;
            p.symbol = "onset_threshold";
            p.name = "Onset Threshold";
            p.description = "Onset probability a note needs to start: lower gives more notes (and more false "
                            "ones). 0 uses the model's own tuned threshold.";
            p.ranges = ParameterRanges(0.0f, 0.0f, 0.95f);
            p.enumValues.count = 1;
            p.enumValues.restrictedMode = false;
            p.enumValues.values = new ParameterEnumerationValue[1]{ParameterEnumerationValue(0.0f, "Model default")};
            break;
        case kInputLevel:
            p.hints = kParameterIsOutput;
            p.symbol = "input_level";
            p.name = "Input Level";
            p.description = "Peak level the model hears (after Input Gain). The training audio mostly peaks "
                            "between -17 and -1 dBFS.";
            p.unit = "dB";
            p.ranges = ParameterRanges(kMeterFloorDb, kMeterFloorDb, 6.0f);
            break;
        case kNoteCount:
            p.hints = kParameterIsOutput | kParameterIsInteger;
            p.symbol = "note_count";
            p.name = "Notes On";
            p.description = "How many notes are on (the Note parameters show the lowest 6).";
            p.ranges = ParameterRanges(0.0f, 0.0f, 128.0f);
            break;
        case kOutputMode:
            p.hints = kParameterIsAutomatable | kParameterIsInteger;
            p.symbol = "output_mode";
            p.name = "Output Mode";
            p.description = "Plain: every note on the MIDI Channel. MPE: a lower zone (manager channel 1), each "
                            "note on its guitar string's channel, string 1 (high E) on channel 2 to string 6 on "
                            "channel 7; the MIDI Channel is unused. Changing it ends the notes that are on.";
            p.ranges = ParameterRanges(0.0f, 0.0f, 1.0f);
            p.enumValues.count = 2;
            p.enumValues.restrictedMode = true;
            p.enumValues.values = new ParameterEnumerationValue[2]{ParameterEnumerationValue(0.0f, "Plain"),
                                                                   ParameterEnumerationValue(1.0f, "MPE")};
            break;
        case kBendRange:
            p.hints = kParameterIsAutomatable | kParameterIsInteger;
            p.symbol = "bend_range";
            p.name = "Bend Range";
            p.description = "Pitch-bend range in semitones: the model's bends are scaled to it, and it is sent as "
                            "RPN 0 (to the MPE member channels, or the plain MIDI Channel in Natural and Auto). "
                            "Match it to the synth patch's bend range for the right pitch (often 2 in plain "
                            "synths, 48 in MPE synths that ignore RPN 0).";
            p.unit = "semitones";
            p.ranges = ParameterRanges(12.0f, 1.0f, static_cast<float>(g2m::kMaxBendRange));
            break;
        case kModelChoice:
            p.hints = kParameterIsAutomatable | kParameterIsInteger;
            p.symbol = "model_choice";
            p.name = "Model";
            p.description = "A model bundled with the plugin, or \"Model file\": the file chosen with the Model "
                            "setting (where the host has a file chooser).";
            p.ranges = ParameterRanges(kBundledModelCount > 0 ? 1.0f : 0.0f, 0.0f,
                                       static_cast<float>(kBundledModelCount));
            p.enumValues.count = static_cast<std::uint8_t>(kBundledModelCount + 1);
            p.enumValues.restrictedMode = true;
            p.enumValues.values = new ParameterEnumerationValue[kBundledModelCount + 1];
            p.enumValues.values[0] = ParameterEnumerationValue(0.0f, "Model file");
            for (int i = 0; i < kBundledModelCount; ++i)
                p.enumValues.values[i + 1] = ParameterEnumerationValue(static_cast<float>(i + 1), kBundledModels[i]);
            break;
        case kGateThreshold:
            p.hints = kParameterIsAutomatable;
            p.symbol = "gate_threshold";
            p.name = "Gate Threshold";
            p.description = "Noise gate before the Input Gain: it opens at once when the input peak reaches this "
                            "level and closes 6 dB below it, after a 10 ms hold and the Gate Release. Its output "
                            "is what the model hears and the audio output. -96 turns it off.";
            p.unit = "dB";
            p.ranges = ParameterRanges(g2m::kGateOff, g2m::kGateOff, -20.0f);
            p.enumValues.count = 1;
            p.enumValues.restrictedMode = false;
            p.enumValues.values = new ParameterEnumerationValue[1]{ParameterEnumerationValue(g2m::kGateOff, "Off")};
            break;
        case kGateRelease:
            p.hints = kParameterIsAutomatable | kParameterIsLogarithmic;
            p.symbol = "gate_release";
            p.name = "Gate Release";
            p.description = "How long the noise gate takes to fade out once it closes.";
            p.unit = "ms";
            p.ranges = ParameterRanges(50.0f, 5.0f, 500.0f);
            break;
        case kOutputLevel:
            p.hints = kParameterIsAutomatable;
            p.symbol = "output_level";
            p.name = "Output Level";
            p.description = "Level of the audio output (the gated input); the model doesn't hear it.";
            p.unit = "dB";
            p.ranges = ParameterRanges(0.0f, -60.0f, 12.0f);
            break;
        case kOutputMute:
            p.hints = kParameterIsAutomatable | kParameterIsBoolean | kParameterIsInteger;
            p.symbol = "output_mute";
            p.name = "Mute";
            p.description = "Silences the audio output; the MIDI output carries on.";
            p.ranges = ParameterRanges(0.0f, 0.0f, 1.0f);
            break;
        case kSustainMode:
            p.hints = kParameterIsAutomatable | kParameterIsInteger;
            p.symbol = "sustain_mode";
            p.name = "Sustain Mode";
            p.description = "What the sustain pedal does. Piano: notes ended while it is down sound on until it "
                            "is up. Sostenuto: only the notes sounding when it goes down sound on. Freeze: the "
                            "notes sounding when it goes down sound on, unbent, and no new notes start until it "
                            "is up.";
            p.ranges = ParameterRanges(1.0f, 0.0f, 3.0f);
            p.enumValues.count = 4;
            p.enumValues.restrictedMode = true;
            p.enumValues.values = new ParameterEnumerationValue[4]{
                ParameterEnumerationValue(0.0f, "Off"), ParameterEnumerationValue(1.0f, "Piano"),
                ParameterEnumerationValue(2.0f, "Sostenuto"), ParameterEnumerationValue(3.0f, "Freeze")};
            break;
        case kSustainCc:
            p.hints = kParameterIsAutomatable | kParameterIsInteger;
            p.symbol = "sustain_cc";
            p.name = "Sustain CC";
            p.description = "The MIDI input's controller that works the pedal, on any channel: down at 64 and "
                            "above. 64 is the usual sustain pedal, 66 sostenuto.";
            p.ranges = ParameterRanges(64.0f, 0.0f, 127.0f);
            break;
        case kSustain:
            p.hints = kParameterIsAutomatable | kParameterIsBoolean | kParameterIsInteger;
            p.symbol = "sustain";
            p.name = "Sustain";
            p.description = "Holds the pedal down, as the Sustain CC does: for automation, or hosts that can't "
                            "send MIDI to an audio effect.";
            p.ranges = ParameterRanges(0.0f, 0.0f, 1.0f);
            break;
        case kSustainState:
            p.hints = kParameterIsOutput | kParameterIsBoolean | kParameterIsInteger;
            p.symbol = "sustain_state";
            p.name = "Pedal";
            p.description = "The pedal is down (the Sustain CC or the Sustain parameter).";
            p.ranges = ParameterRanges(0.0f, 0.0f, 1.0f);
            break;
        case kBendMode:
            p.hints = kParameterIsAutomatable | kParameterIsInteger;
            p.symbol = "bend_mode";
            p.name = "Bend Mode";
            p.description = "What becomes of the model's pitch bends (models with bends only). Natural: sent as "
                            "they are; in plain output the channel bend follows the newest note. Retrigger: a new "
                            "note each time a bend reaches another semitone. Off: none. Auto: in plain output, bends "
                            "smooth while one note sounds and retriggered while more do; in MPE, Natural.";
            p.ranges = ParameterRanges(0.0f, 0.0f, 3.0f);
            p.enumValues.count = 4;
            p.enumValues.restrictedMode = true;
            p.enumValues.values = new ParameterEnumerationValue[4]{ParameterEnumerationValue(0.0f, "Natural"),
                                                                   ParameterEnumerationValue(1.0f, "Retrigger"),
                                                                   ParameterEnumerationValue(2.0f, "Off"),
                                                                   ParameterEnumerationValue(3.0f, "Auto")};
            break;
        case kModelLoaded:
            p.hints = kParameterIsOutput | kParameterIsInteger;
            p.symbol = "model_loaded";
            p.name = "Model Loaded";
            p.description = "The model playing: a bundled model, the model file, or none.";
            p.ranges = ParameterRanges(-1.0f, -1.0f, static_cast<float>(kBundledModelCount));
            p.enumValues.count = static_cast<std::uint8_t>(kBundledModelCount + 2);
            p.enumValues.restrictedMode = true;
            p.enumValues.values = new ParameterEnumerationValue[kBundledModelCount + 2];
            p.enumValues.values[0] = ParameterEnumerationValue(-1.0f, "None");
            p.enumValues.values[1] = ParameterEnumerationValue(0.0f, "Model file");
            for (int i = 0; i < kBundledModelCount; ++i)
                p.enumValues.values[i + 2] = ParameterEnumerationValue(static_cast<float>(i + 1), kBundledModels[i]);
            break;
        case kModelFormat:
            p.hints = kParameterIsOutput | kParameterIsInteger;
            p.symbol = "model_format";
            p.name = "Model Format";
            p.description = "The loaded model file's format version (0: no model).";
            p.ranges = ParameterRanges(0.0f, 0.0f, 255.0f);
            break;
        case kModelError:
            p.hints = kParameterIsOutput | kParameterIsInteger;
            p.symbol = "model_error";
            p.name = "Model Error";
            p.description = "Why the last model chosen isn't playing: it couldn't be loaded (the model before it "
                            "plays on), or it is for another sample rate (no MIDI).";
            p.ranges = ParameterRanges(0.0f, 0.0f, 2.0f);
            p.enumValues.count = 3;
            p.enumValues.restrictedMode = true;
            p.enumValues.values = new ParameterEnumerationValue[3]{ParameterEnumerationValue(0.0f, "None"),
                                                                   ParameterEnumerationValue(1.0f, "Couldn't load"),
                                                                   ParameterEnumerationValue(2.0f, "Wrong sample rate")};
            break;
        default:
            if (index >= kNote1 && index < kNote1 + kNoteSlots) init_note_parameter(index - kNote1, p);
    }
}

inline bool in_presets(std::uint32_t index, const Parameter& p) {
    return !(p.hints & kParameterIsOutput) && index != kModelChoice && index != kSustain;
}

}  // namespace g2m::param
