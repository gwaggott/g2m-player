// The plugin's UI, drawn with LVGL (plugin/ui/LVGL.hpp; docs/release-plan.md,
// "UI"): presets, the model, and a card each for the input, the MIDI output
// and the audio output, over a strip of the notes on.
//
// Every control is a host parameter (their definitions are shared with the
// plugin in params.h), so the UI only reads and sets parameters, and the
// "model" state when a file is loaded. The model's name, format and load
// error come from the plugin's Model Loaded, Model Format and Model Error
// outputs.
//
// Presets: the factory presets (as the plugin's programs, which only LV2
// hosts list) and the user's preset files (presets.h), applied by setting
// the parameters and the model they name.
//
// LVGL's state is per instance: makeCurrent() before LVGL calls made outside
// the wrapper's callbacks (parameterChanged, stateChanged, uiFileBrowserSelected).

#include "DistrhoUI.hpp"
#include "factory_presets.h"
#include "params.h"
#include "presets.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

START_NAMESPACE_DISTRHO

using namespace g2m::param;

namespace {

constexpr char kBundledPrefix[] = "bundled:";

constexpr float kMeterMinDb = -60.0f, kMeterMaxDb = 6.0f;  // the meter's scale
constexpr float kTrainingLowDb = -17.0f, kTrainingHighDb = -1.0f;  // where the training audio mostly peaks
constexpr int kStripLow = 40, kStripHigh = 88;  // the notes strip: E2 to E6
constexpr int kLogSteps = 1000;  // a logarithmic slider's positions

bool is_black_key(int pitch) {
    int k = pitch % 12;
    return k == 1 || k == 3 || k == 6 || k == 8 || k == 10;
}

// The meter's x for `db`, in percent of its width
int meter_pct(float db) {
    float x = (std::clamp(db, kMeterMinDb, kMeterMaxDb) - kMeterMinDb) / (kMeterMaxDb - kMeterMinDb);
    return static_cast<int>(std::lround(x * 100));
}

}  // namespace

class G2mUI : public UI {
public:
    G2mUI() : UI(DISTRHO_UI_DEFAULT_WIDTH, DISTRHO_UI_DEFAULT_HEIGHT) {
        const double scale = getScaleFactor();
        if (d_isNotEqual(scale, 1.0)) {
            setGeometryConstraints(DISTRHO_UI_DEFAULT_WIDTH * scale, DISTRHO_UI_DEFAULT_HEIGHT * scale);
            setSize(DISTRHO_UI_DEFAULT_WIDTH * scale, DISTRHO_UI_DEFAULT_HEIGHT * scale);
        } else {
            setGeometryConstraints(DISTRHO_UI_DEFAULT_WIDTH, DISTRHO_UI_DEFAULT_HEIGHT);
        }
        for (std::uint32_t i = 0; i < kParamCount; ++i) {
            init(i, info_[i]);
            values_[i] = info_[i].ranges.def;
            if (in_presets(i, info_[i])) {
                preset_params_.push_back(i);
                preset_info_.push_back({info_[i].symbol.buffer(), info_[i].ranges.def, info_[i].ranges.min,
                                        info_[i].ranges.max});
            }
        }
        makeCurrent();
        build();
        for (std::uint32_t i = 0; i < kParamCount; ++i) show(i);
        refresh_presets();
    }

protected:
    // The host's value, shown without sending it back
    void parameterChanged(uint32_t index, float value) override {
        if (index >= kParamCount) return;
        makeCurrent();
        bool changed = values_[index] != value;
        values_[index] = value;
        if (changed && preset_ && in_presets(index, info_[index])) mark_changed();
        show(index);
    }

    void stateChanged(const char* key, const char* value) override {
        if (std::strcmp(key, "model") != 0) return;
        makeCurrent();
        if (std::strncmp(value, kBundledPrefix, sizeof kBundledPrefix - 1) != 0) model_file_ = value;
        show_model();
    }

    void uiFileBrowserSelected(const char* filename) override {
        if (!filename) return;
        makeCurrent();
        load_file(filename);
    }

private:
    // A control for parameter `index`, found again by its LVGL objects' user data
    struct Control {
        enum Kind { slider, log_slider, dropdown, stepper, toggle } kind;
        std::uint32_t index;
        float steps = 1;  // a linear slider's positions per unit
        lv_obj_t* obj = nullptr;
        lv_obj_t* value = nullptr;  // the value's label, if any
        lv_obj_t* row = nullptr;  // greyed out when the control is unused
        G2mUI* ui = nullptr;
    };

    // A preset in the menu: a factory one (`path` empty) or a user file
    struct Item {
        std::string name;
        std::string path;
        int factory = -1;
    };

    enum class Dialog { none, save_as, replace, remove };

    // ---- building

    void build() {
        lv_display_t* display = lv_display_get_default();
        lv_display_set_theme(display, lv_theme_default_init(display, lv_palette_main(LV_PALETTE_BLUE),
                                                            lv_palette_main(LV_PALETTE_ORANGE), true,
                                                            LV_FONT_DEFAULT));
        lv_obj_t* screen = lv_screen_active();
        lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
        column(screen, 12, 8);

        // presets
        lv_obj_t* top = row(screen);
        lv_obj_t* title = lv_label_create(top);
        lv_label_set_text(title, "Guitar to MIDI");
        lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
        lv_obj_set_flex_grow(title, 1);
        caption(top, "Preset");
        presets_ = lv_dropdown_create(top);
        lv_obj_set_width(presets_, 220);
        lv_obj_add_event_cb(presets_, on_presets, LV_EVENT_ALL, this);
        lv_obj_add_event_cb(lv_dropdown_get_list(presets_), on_preset_list, LV_EVENT_RELEASED, this);
        save_ = button(top, "Save", on_save);
        button(top, "Save as", on_save_as);
        delete_ = button(top, "Delete", on_delete);

        // the model
        lv_obj_t* model = row(screen);
        caption(model, "Model");
        models_ = lv_dropdown_create(model);
        lv_obj_set_width(models_, 220);
        std::string options;
        for (int i = 0; i < kBundledModelCount; ++i) options += std::string(i ? "\n" : "") + kBundledModels[i];
        lv_dropdown_set_options(models_, options.c_str());
        lv_obj_add_event_cb(models_, on_model_choice, LV_EVENT_VALUE_CHANGED, this);
        if (kBundledModelCount == 0) lv_obj_add_state(models_, LV_STATE_DISABLED);
        button(model, "Load file...", on_load_file);
        model_info_ = lv_label_create(model);
        lv_obj_set_flex_grow(model_info_, 1);
        lv_label_set_long_mode(model_info_, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_color(model_info_, lv_palette_main(LV_PALETTE_GREY), 0);

        // the cards
        lv_obj_t* cards = row(screen);
        lv_obj_set_style_flex_cross_place(cards, LV_FLEX_ALIGN_START, 0);
        lv_obj_set_style_flex_track_place(cards, LV_FLEX_ALIGN_START, 0);
        lv_obj_t* input = card(cards, "Input", 220);
        build_meter(input);
        slider(input, kGateThreshold, 1);
        slider(input, kGateRelease, 0, true);
        slider(input, kInputGain, 10);
        slider(input, kOnsetThreshold, 100);

        lv_obj_t* midi = card(cards, "MIDI", 256);
        dropdown(midi, kOutputMode);
        stepper(midi, kMidiChannel);
        stepper(midi, kBendRange);
        dropdown(midi, kBendMode);

        lv_obj_t* right = lv_obj_create(cards);
        lv_obj_remove_style_all(right);
        lv_obj_set_flex_grow(right, 1);
        lv_obj_set_height(right, LV_SIZE_CONTENT);
        column(right, 0, 10);
        lv_obj_t* sustain = card(right, "Sustain");
        dropdown(sustain, kSustainMode);
        stepper(sustain, kSustainCc);
        lv_obj_t* pedal = row(sustain);
        caption(pedal, "Pedal");
        lv_obj_set_flex_grow(lv_obj_get_child(pedal, 0), 1);
        pedal_ = lv_led_create(pedal);
        lv_obj_set_size(pedal_, 14, 14);
        lv_led_set_color(pedal_, lv_palette_main(LV_PALETTE_ORANGE));
        lv_obj_t* output = card(right, "Audio output");
        slider(output, kOutputLevel, 10);
        toggle(output, kOutputMute);

        build_strip(screen);

        lv_obj_t* bottom = row(screen);
        model_error_ = lv_label_create(bottom);
        lv_obj_set_style_text_color(model_error_, lv_palette_main(LV_PALETTE_RED), 0);
        lv_label_set_text(model_error_, "");
        message_ = lv_label_create(bottom);
        lv_obj_set_flex_grow(message_, 1);
        lv_label_set_long_mode(message_, LV_LABEL_LONG_MODE_DOTS);
        lv_label_set_text(message_, "");
    }

    static void column(lv_obj_t* obj, int pad, int gap) {
        lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_all(obj, pad, 0);
        lv_obj_set_style_pad_row(obj, gap, 0);
    }

    static lv_obj_t* row(lv_obj_t* parent) {
        lv_obj_t* r = lv_obj_create(parent);
        lv_obj_remove_style_all(r);
        lv_obj_set_size(r, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(r, 8, 0);
        return r;
    }

    static lv_obj_t* caption(lv_obj_t* parent, const char* text) {
        lv_obj_t* label = lv_label_create(parent);
        lv_label_set_text(label, text);
        return label;
    }

    static lv_obj_t* card(lv_obj_t* parent, const char* title, int width = 0) {
        lv_obj_t* c = lv_obj_create(parent);
        lv_obj_set_width(c, width ? width : lv_pct(100));
        lv_obj_set_height(c, LV_SIZE_CONTENT);
        lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
        column(c, 10, 6);
        lv_obj_t* label = caption(c, title);
        lv_obj_set_style_text_color(label, lv_palette_main(LV_PALETTE_BLUE), 0);
        return c;
    }

    lv_obj_t* button(lv_obj_t* parent, const char* text, lv_event_cb_t cb) {
        lv_obj_t* b = lv_button_create(parent);
        lv_obj_set_style_pad_hor(b, 10, 0);
        lv_obj_set_style_pad_ver(b, 8, 0);
        caption(b, text);
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, this);
        return b;
    }

    // The parameter's name on the left and its value on the right
    Control& labelled(lv_obj_t* parent, std::uint32_t index, Control::Kind kind) {
        controls_.push_back(std::make_unique<Control>());
        Control& c = *controls_.back();
        c.kind = kind;
        c.index = index;
        c.ui = this;
        c.row = row(parent);
        lv_obj_t* name = caption(c.row, info_[index].name.buffer());
        lv_obj_set_flex_grow(name, 1);
        return c;
    }

    void slider(lv_obj_t* parent, std::uint32_t index, float steps, bool log = false) {
        lv_obj_t* box = lv_obj_create(parent);
        lv_obj_remove_style_all(box);
        lv_obj_set_size(box, lv_pct(100), LV_SIZE_CONTENT);
        // The knob is larger than the bar: let it draw past the box
        lv_obj_add_flag(box, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
        lv_obj_add_event_cb(box, [](lv_event_t* e) {
            auto* s = static_cast<int32_t*>(lv_event_get_param(e));
            *s = LV_MAX(*s, 10);
        }, LV_EVENT_REFR_EXT_DRAW_SIZE, nullptr);
        column(box, 0, 6);
        Control& c = labelled(box, index, log ? Control::log_slider : Control::slider);
        c.value = caption(c.row, "");
        c.row = box;
        c.steps = steps;
        c.obj = lv_slider_create(box);
        lv_obj_set_width(c.obj, lv_pct(96));
        lv_obj_set_height(c.obj, 6);
        lv_obj_set_style_margin_hor(c.obj, 4, 0);
        const ParameterRanges& r = info_[index].ranges;
        lv_slider_set_range(c.obj, 0, log ? kLogSteps : static_cast<int32_t>(std::lround((r.max - r.min) * steps)));
        lv_obj_set_user_data(c.obj, &c);
        lv_obj_add_event_cb(c.obj, on_slider, LV_EVENT_ALL, &c);
    }

    void dropdown(lv_obj_t* parent, std::uint32_t index) {
        Control& c = labelled(parent, index, Control::dropdown);
        c.value = nullptr;
        c.obj = lv_dropdown_create(c.row);
        lv_obj_set_width(c.obj, 108);
        std::string options;
        const ParameterEnumerationValues& e = info_[index].enumValues;
        for (int k = 0; k < e.count; ++k) options += std::string(k ? "\n" : "") + e.values[k].label.buffer();
        lv_dropdown_set_options(c.obj, options.c_str());
        lv_obj_add_event_cb(c.obj, on_dropdown, LV_EVENT_VALUE_CHANGED, &c);
    }

    void stepper(lv_obj_t* parent, std::uint32_t index) {
        Control& c = labelled(parent, index, Control::stepper);
        lv_obj_t* minus = lv_button_create(c.row);
        caption(minus, "-");
        c.value = caption(c.row, "");
        lv_obj_set_width(c.value, 44);
        lv_obj_set_style_text_align(c.value, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_t* plus = lv_button_create(c.row);
        caption(plus, "+");
        for (lv_obj_t* b : {minus, plus}) {
            lv_obj_set_size(b, 26, 26);
            lv_obj_set_style_pad_all(b, 0, 0);
            lv_obj_center(lv_obj_get_child(b, 0));
            lv_obj_set_user_data(b, reinterpret_cast<void*>(static_cast<intptr_t>(b == plus ? 1 : -1)));
            lv_obj_add_event_cb(b, on_step, LV_EVENT_CLICKED, &c);
            lv_obj_add_event_cb(b, on_step, LV_EVENT_LONG_PRESSED_REPEAT, &c);
        }
        c.obj = minus;
    }

    void toggle(lv_obj_t* parent, std::uint32_t index) {
        Control& c = labelled(parent, index, Control::toggle);
        c.obj = lv_switch_create(c.row);
        lv_obj_set_size(c.obj, 40, 20);
        lv_obj_add_event_cb(c.obj, on_toggle, LV_EVENT_VALUE_CHANGED, &c);
    }

    // The input meter: the level the model hears, the training range under
    // it and the gate threshold (at the model's level: plus the Input Gain)
    void build_meter(lv_obj_t* parent) {
        lv_obj_t* labels = row(parent);
        lv_obj_set_flex_grow(caption(labels, "Level"), 1);
        level_text_ = caption(labels, "");
        meter_ = lv_obj_create(parent);
        lv_obj_remove_style_all(meter_);
        lv_obj_set_size(meter_, lv_pct(100), 18);
        lv_obj_remove_flag(meter_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t* band = lv_obj_create(meter_);
        lv_obj_remove_style_all(band);
        lv_obj_set_style_bg_color(band, lv_palette_main(LV_PALETTE_GREEN), 0);
        lv_obj_set_style_bg_opa(band, LV_OPA_40, 0);
        lv_obj_set_pos(band, lv_pct(meter_pct(kTrainingLowDb)), 12);
        lv_obj_set_size(band, lv_pct(meter_pct(kTrainingHighDb) - meter_pct(kTrainingLowDb)), 6);
        level_ = lv_bar_create(meter_);
        lv_obj_set_size(level_, lv_pct(100), 10);
        lv_bar_set_range(level_, static_cast<int32_t>(kMeterMinDb), static_cast<int32_t>(kMeterMaxDb));
        gate_mark_ = lv_obj_create(meter_);
        lv_obj_remove_style_all(gate_mark_);
        lv_obj_set_style_bg_color(gate_mark_, lv_palette_main(LV_PALETTE_ORANGE), 0);
        lv_obj_set_style_bg_opa(gate_mark_, LV_OPA_COVER, 0);
        lv_obj_set_size(gate_mark_, 2, 18);
        lv_obj_t* legend = caption(parent, "Green: training. Orange: gate.");
        lv_obj_set_width(legend, lv_pct(100));
        lv_label_set_long_mode(legend, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_font(legend, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(legend, lv_palette_main(LV_PALETTE_GREY), 0);
    }

    // The notes on: a key for each note from E2 to E6, and their names
    void build_strip(lv_obj_t* parent) {
        lv_obj_t* labels = row(parent);
        lv_obj_set_flex_grow(caption(labels, "Notes on"), 1);
        notes_text_ = caption(labels, "");
        lv_obj_t* strip = row(parent);
        lv_obj_set_style_pad_column(strip, 2, 0);
        for (int pitch = kStripLow; pitch <= kStripHigh; ++pitch) {
            lv_obj_t* key = lv_obj_create(strip);
            lv_obj_remove_style_all(key);
            lv_obj_set_height(key, is_black_key(pitch) ? 16 : 24);
            lv_obj_set_flex_grow(key, 1);
            lv_obj_set_style_bg_opa(key, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(key, 2, 0);
            keys_.push_back(key);
        }
        lv_obj_set_flex_align(strip, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    }

    // ---- showing values

    void show(std::uint32_t index) {
        for (auto& c : controls_)
            if (c->index == index) show(*c);
        switch (index) {
            case kInputLevel:
            case kInputGain:
            case kGateThreshold:
                show_meter();
                break;
            case kOutputMode:
                show_used();
                break;
            case kSustainState:
                if (values_[kSustainState] >= 0.5f) lv_led_on(pedal_);
                else lv_led_off(pedal_);
                break;
            case kModelLoaded:
            case kModelFormat:
            case kModelError:
                show_model();
                break;
            default:
                if (index == kNoteCount || (index >= kNote1 && index < kNote1 + kNoteSlots)) show_notes();
        }
    }

    void show(Control& c) {
        float v = values_[c.index];
        const ParameterRanges& r = info_[c.index].ranges;
        switch (c.kind) {
            case Control::slider:
                lv_slider_set_value(c.obj, static_cast<int32_t>(std::lround((v - r.min) * c.steps)), LV_ANIM_OFF);
                break;
            case Control::log_slider:
                lv_slider_set_value(c.obj, static_cast<int32_t>(std::lround(std::log(v / r.min) / std::log(r.max / r.min) * kLogSteps)),
                                    LV_ANIM_OFF);
                break;
            case Control::dropdown:
                lv_dropdown_set_selected(c.obj, static_cast<uint32_t>(std::max(0L, std::lround(v - r.min))));
                break;
            case Control::toggle:
                if (v >= 0.5f) lv_obj_add_state(c.obj, LV_STATE_CHECKED);
                else lv_obj_remove_state(c.obj, LV_STATE_CHECKED);
                break;
            case Control::stepper:
                break;
        }
        if (c.value) lv_label_set_text(c.value, text(c.index, v).c_str());
    }

    // A value as the host shows it: its label, or the number and unit
    std::string text(std::uint32_t index, float v) const {
        const Parameter& p = info_[index];
        for (int k = 0; k < p.enumValues.count; ++k)
            if (std::fabs(p.enumValues.values[k].value - v) < 1e-4f) return p.enumValues.values[k].label.buffer();
        char buf[48];
        const char* unit = p.unit.buffer();
        bool whole = (p.hints & kParameterIsInteger) || std::strcmp(unit, "ms") == 0;
        std::snprintf(buf, sizeof buf, whole ? "%.0f" : index == kOnsetThreshold ? "%.2f" : "%.1f", v);
        std::string out = buf;
        if (*unit && std::strcmp(unit, "semitones") != 0) out += std::string(" ") + unit;
        return out;
    }

    void show_meter() {
        float db = values_[kInputLevel];
        lv_bar_set_value(level_, static_cast<int32_t>(std::lround(std::max(db, kMeterMinDb))), LV_ANIM_OFF);
        char buf[32];
        if (db < kMeterMinDb) std::snprintf(buf, sizeof buf, "below %.0f dB", kMeterMinDb);
        else std::snprintf(buf, sizeof buf, "%.1f dB", db);
        lv_label_set_text(level_text_, buf);
        float gate = values_[kGateThreshold] + values_[kInputGain];
        bool shown = values_[kGateThreshold] > g2m::kGateOff && gate >= kMeterMinDb && gate <= kMeterMaxDb;
        if (shown) lv_obj_remove_flag(gate_mark_, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(gate_mark_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_x(gate_mark_, lv_pct(meter_pct(gate)));
    }

    // The MIDI Channel is for plain output only (the Bend Range is for both)
    void show_used() {
        bool mpe = values_[kOutputMode] >= 0.5f;
        for (auto& c : controls_) {
            if (c->index != kMidiChannel) continue;
            bool unused = mpe;
            if (unused) lv_obj_add_state(c->row, LV_STATE_DISABLED);
            else lv_obj_remove_state(c->row, LV_STATE_DISABLED);
            lv_obj_set_style_opa(c->row, unused ? LV_OPA_50 : LV_OPA_COVER, 0);
            for (uint32_t k = 0; k < lv_obj_get_child_count(c->row); ++k) {
                lv_obj_t* child = lv_obj_get_child(c->row, k);
                if (unused) lv_obj_add_state(child, LV_STATE_DISABLED);
                else lv_obj_remove_state(child, LV_STATE_DISABLED);
            }
        }
    }

    void show_notes() {
        bool on[128] = {};
        std::string names;
        for (int slot = 0; slot < kNoteSlots; ++slot) {
            int pitch = static_cast<int>(std::lround(values_[kNote1 + slot]));
            if (pitch < 0 || pitch > 127) continue;
            on[pitch] = true;
            char name[g2m::kNoteNameSize];
            g2m::note_name(pitch, name);
            names += (names.empty() ? "" : " ") + std::string(name);
        }
        int count = static_cast<int>(std::lround(values_[kNoteCount]));
        if (count > kNoteSlots) names += " and " + std::to_string(count - kNoteSlots) + " more";
        lv_label_set_text(notes_text_, names.empty() ? "none" : names.c_str());
        for (int pitch = kStripLow; pitch <= kStripHigh; ++pitch) {
            lv_obj_t* key = keys_[pitch - kStripLow];
            lv_color_t off = is_black_key(pitch) ? lv_color_hex(0x30343a) : lv_color_hex(0x5a6068);
            lv_obj_set_style_bg_color(key, on[pitch] ? lv_palette_main(LV_PALETTE_ORANGE) : off, 0);
        }
    }

    void show_model() {
        int loaded = static_cast<int>(std::lround(values_[kModelLoaded]));
        if (loaded >= 1 && loaded <= kBundledModelCount) {
            lv_dropdown_set_text(models_, nullptr);
            lv_dropdown_set_selected(models_, static_cast<uint32_t>(loaded - 1));
            model_name_ = kBundledModels[loaded - 1];
        } else {
            model_name_ = loaded == 0 ? std::filesystem::path(model_file_).filename().string() : "No model";
            if (model_name_.empty()) model_name_ = "Model file";
            lv_dropdown_set_text(models_, model_name_.c_str());  // kept: LVGL holds the pointer
        }
        char info[64] = "";
        int format = static_cast<int>(std::lround(values_[kModelFormat]));
        if (loaded >= 0 && format > 0) std::snprintf(info, sizeof info, "format %d", format);
        lv_label_set_text(model_info_, info);
        int error = static_cast<int>(std::lround(values_[kModelError]));
        lv_label_set_text(model_error_, error == kModelUnloadable ? "Couldn't load the model chosen; the one above plays on."
                                        : error == kModelWrongRate ? "The host isn't at 48 kHz: no MIDI."
                                                                   : "");
        if (error == kModelOk) lv_obj_add_flag(model_error_, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(model_error_, LV_OBJ_FLAG_HIDDEN);
    }

    // ---- changing values

    void begin(std::uint32_t index) { editParameter(index, true); }
    void end(std::uint32_t index) { editParameter(index, false); }

    void set(std::uint32_t index, float value) {
        const ParameterRanges& r = info_[index].ranges;
        value = std::clamp(value, r.min, r.max);
        if (value != values_[index] && preset_ && in_presets(index, info_[index])) mark_changed();
        values_[index] = value;
        setParameterValue(index, value);
        show(index);
    }

    // One host gesture
    void change(std::uint32_t index, float value) {
        begin(index);
        set(index, value);
        end(index);
    }

    void load_file(const std::string& path) {
        change(kModelChoice, 0);  // "Model file", so the loader leaves it be
        model_file_ = path;
        setState("model", path.c_str());
        show_model();
    }

    // A preset's model: "bundled:<name>" or a file. A missing one keeps the
    // current model, with a warning.
    std::string apply_model(const std::string& model) {
        if (model.compare(0, sizeof kBundledPrefix - 1, kBundledPrefix) == 0) {
            std::string name = model.substr(sizeof kBundledPrefix - 1);
            for (int i = 0; i < kBundledModelCount; ++i)
                if (name == kBundledModels[i]) {
                    change(kModelChoice, static_cast<float>(i + 1));
                    return {};
                }
            return "its model " + name + " isn't bundled; the current model plays on";
        }
        std::error_code ec;
        if (!std::filesystem::is_regular_file(model, ec)) return "its model file is missing; the current model plays on";
        load_file(model);
        return {};
    }

    static void on_slider(lv_event_t* e) {
        auto* c = static_cast<Control*>(lv_event_get_user_data(e));
        G2mUI* ui = c->ui;
        const ParameterRanges& r = ui->info_[c->index].ranges;
        switch (lv_event_get_code(e)) {
            case LV_EVENT_PRESSED:
                ui->begin(c->index);
                break;
            case LV_EVENT_VALUE_CHANGED: {
                float pos = static_cast<float>(lv_slider_get_value(c->obj));
                float v = c->kind == Control::log_slider ? r.min * std::pow(r.max / r.min, pos / kLogSteps)
                                                         : r.min + pos / c->steps;
                ui->set(c->index, v);
                break;
            }
            case LV_EVENT_RELEASED:
            case LV_EVENT_PRESS_LOST:
                ui->end(c->index);
                break;
            case LV_EVENT_DOUBLE_CLICKED:  // back to the default
                ui->change(c->index, r.def);
                break;
            default:
                break;
        }
    }

    static void on_dropdown(lv_event_t* e) {
        auto* c = static_cast<Control*>(lv_event_get_user_data(e));
        c->ui->change(c->index, c->ui->info_[c->index].ranges.min + static_cast<float>(lv_dropdown_get_selected(c->obj)));
    }

    static void on_step(lv_event_t* e) {
        auto* c = static_cast<Control*>(lv_event_get_user_data(e));
        auto* b = static_cast<lv_obj_t*>(lv_event_get_target(e));
        auto step = static_cast<float>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(b)));
        c->ui->change(c->index, c->ui->values_[c->index] + step);
    }

    static void on_toggle(lv_event_t* e) {
        auto* c = static_cast<Control*>(lv_event_get_user_data(e));
        c->ui->change(c->index, lv_obj_has_state(c->obj, LV_STATE_CHECKED) ? 1.0f : 0.0f);
    }

    static void on_model_choice(lv_event_t* e) {
        auto* ui = static_cast<G2mUI*>(lv_event_get_user_data(e));
        ui->change(kModelChoice, static_cast<float>(lv_dropdown_get_selected(ui->models_) + 1));
    }

    static void on_load_file(lv_event_t* e) {
        auto* ui = static_cast<G2mUI*>(lv_event_get_user_data(e));
        FileBrowserOptions options;
        options.title = "Load a model (.g2m)";
        std::string dir;
        if (!ui->model_file_.empty()) dir = std::filesystem::path(ui->model_file_).parent_path().string();
        if (!dir.empty()) options.startDir = dir.c_str();
        if (!ui->openFileBrowser(options)) ui->message("This host has no file browser for plugins.");
    }

    // ---- presets

    void refresh_presets() {
        items_.clear();
        const auto& factory = g2m::presets::factory_presets();
        for (std::size_t i = 0; i < factory.size(); ++i) items_.push_back({factory[i].name, {}, static_cast<int>(i)});
        dir_ = g2m::presets::user_dir();
        if (!dir_.empty())
            for (const auto& entry : g2m::presets::list(dir_)) items_.push_back({entry.name, entry.path, -1});
        std::string options;
        for (std::size_t i = 0; i < items_.size(); ++i) {
            std::string name = items_[i].name;
            std::replace(name.begin(), name.end(), '\n', ' ');
            options += (i ? "\n" : "") + name + (items_[i].factory >= 0 ? " (factory)" : "");
        }
        lv_dropdown_set_options(presets_, options.c_str());
        show_preset();
    }

    void show_preset() {
        preset_text_ = preset_ ? preset_name_ + (preset_changed_ ? " *" : "") : "Choose a preset";
        lv_dropdown_set_text(presets_, preset_text_.c_str());  // kept: LVGL holds the pointer
        for (std::size_t k = 0; preset_ && k < items_.size(); ++k)
            if (items_[k].path == preset_path_ && items_[k].name == preset_name_)
                lv_dropdown_set_selected(presets_, static_cast<uint32_t>(k));
        bool user = preset_ && !preset_path_.empty();
        for (lv_obj_t* b : {save_, delete_}) {
            if (user) lv_obj_remove_state(b, LV_STATE_DISABLED);
            else lv_obj_add_state(b, LV_STATE_DISABLED);
        }
    }

    void mark_changed() {
        if (preset_changed_) return;
        preset_changed_ = true;
        show_preset();
    }

    void apply(const Item& item) {
        g2m::presets::Preset preset;
        std::string error;
        if (item.factory >= 0) {
            preset = g2m::presets::factory_presets()[static_cast<std::size_t>(item.factory)];
        } else if (!g2m::presets::load(item.path, preset, error)) {
            message("Couldn't load " + item.name + ": " + error);
            return;
        }
        std::vector<float> v = g2m::presets::values(preset, preset_info_);
        for (std::size_t i = 0; i < v.size(); ++i) change(preset_params_[i], v[i]);
        std::string warning = preset.model.empty() ? std::string() : apply_model(preset.model);
        preset_ = true;
        preset_changed_ = false;
        preset_name_ = item.name;
        preset_path_ = item.path;
        show_preset();
        message(warning.empty() ? "Loaded " + item.name : "Loaded " + item.name + ", but " + warning + ".");
    }

    // The current settings as a preset named `name`
    g2m::presets::Preset current(const std::string& name) const {
        g2m::presets::Preset p;
        p.name = name;
        for (std::uint32_t i : preset_params_) p.parameters.emplace_back(info_[i].symbol.buffer(), values_[i]);
        int loaded = static_cast<int>(std::lround(values_[kModelLoaded]));
        if (loaded >= 1 && loaded <= kBundledModelCount) p.model = std::string(kBundledPrefix) + kBundledModels[loaded - 1];
        else if (loaded == 0) p.model = model_file_;
        return p;
    }

    void save(const std::string& name) {
        std::string error;
        if (!g2m::presets::save(dir_, current(name), error)) {
            message("Couldn't save: " + error);
            return;
        }
        refresh_presets();
        preset_ = true;
        preset_changed_ = false;
        preset_name_ = name;
        preset_path_ = (std::filesystem::path(dir_) / g2m::presets::file_name(name)).string();
        show_preset();
        message("Saved " + name);
    }

    static void on_presets(lv_event_t* e) {
        auto* ui = static_cast<G2mUI*>(lv_event_get_user_data(e));
        lv_event_code_t code = lv_event_get_code(e);
        if (code == LV_EVENT_PRESSED && !lv_dropdown_is_open(ui->presets_)) {
            ui->refresh_presets();  // files saved elsewhere
            ui->picked_before_ = lv_dropdown_get_selected(ui->presets_);
        }
        if (code != LV_EVENT_VALUE_CHANGED) return;
        std::size_t k = lv_dropdown_get_selected(ui->presets_);
        if (k < ui->items_.size()) ui->apply(ui->items_[k]);
    }

    // The same preset picked again (no VALUE_CHANGED): applied again
    static void on_preset_list(lv_event_t* e) {
        auto* ui = static_cast<G2mUI*>(lv_event_get_user_data(e));
        std::size_t k = lv_dropdown_get_selected(ui->presets_);
        if (!lv_dropdown_is_open(ui->presets_) && k == ui->picked_before_ && k < ui->items_.size())
            ui->apply(ui->items_[k]);
    }

    static void on_save(lv_event_t* e) {
        auto* ui = static_cast<G2mUI*>(lv_event_get_user_data(e));
        if (ui->preset_ && !ui->preset_path_.empty()) ui->save(ui->preset_name_);
    }

    static void on_save_as(lv_event_t* e) {
        auto* ui = static_cast<G2mUI*>(lv_event_get_user_data(e));
        if (ui->dir_.empty()) {
            ui->message("No folder for user presets (HOME isn't set).");
            return;
        }
        lv_obj_t* box = ui->dialog(Dialog::save_as, "Save preset as", "Ok");
        ui->name_ = lv_textarea_create(lv_msgbox_get_content(box));
        lv_textarea_set_one_line(ui->name_, true);
        lv_obj_set_width(ui->name_, lv_pct(100));
        lv_textarea_set_placeholder_text(ui->name_, "Preset name");
        if (ui->preset_ && !ui->preset_path_.empty()) lv_textarea_set_text(ui->name_, ui->preset_name_.c_str());
        lv_obj_add_event_cb(ui->name_, on_dialog, LV_EVENT_READY, ui);  // Enter
        lv_group_focus_obj(ui->name_);
        lv_obj_add_state(ui->name_, LV_STATE_FOCUSED);
    }

    static void on_delete(lv_event_t* e) {
        auto* ui = static_cast<G2mUI*>(lv_event_get_user_data(e));
        if (!ui->preset_ || ui->preset_path_.empty()) return;
        lv_obj_t* box = ui->dialog(Dialog::remove, "Delete preset", "Delete");
        lv_msgbox_add_text(box, ("Delete the preset \"" + ui->preset_name_ + "\"? Its file is removed.").c_str());
    }

    // A modal dialog with Cancel and `ok`; on_dialog does what `kind` says
    lv_obj_t* dialog(Dialog kind, const char* title, const char* ok) {
        dialog_kind_ = kind;
        dialog_ = lv_msgbox_create(nullptr);
        lv_obj_set_width(dialog_, 360);
        lv_msgbox_add_title(dialog_, title);
        lv_obj_add_event_cb(lv_msgbox_add_footer_button(dialog_, "Cancel"), on_dialog_cancel, LV_EVENT_CLICKED, this);
        lv_obj_add_event_cb(lv_msgbox_add_footer_button(dialog_, ok), on_dialog, LV_EVENT_CLICKED, this);
        return dialog_;
    }

    void close_dialog() {
        if (dialog_) lv_msgbox_close_async(dialog_);
        dialog_ = nullptr;
        name_ = nullptr;
        dialog_kind_ = Dialog::none;
    }

    static void on_dialog_cancel(lv_event_t* e) { static_cast<G2mUI*>(lv_event_get_user_data(e))->close_dialog(); }

    static void on_dialog(lv_event_t* e) {
        auto* ui = static_cast<G2mUI*>(lv_event_get_user_data(e));
        switch (ui->dialog_kind_) {
            case Dialog::save_as: {
                std::string name = lv_textarea_get_text(ui->name_);
                if (g2m::presets::file_name(name).empty()) {
                    ui->message("The preset needs a name.");
                    return;
                }
                ui->close_dialog();
                std::error_code ec;
                if (std::filesystem::exists(std::filesystem::path(ui->dir_) / g2m::presets::file_name(name), ec)) {
                    ui->pending_name_ = name;
                    lv_obj_t* box = ui->dialog(Dialog::replace, "Replace preset", "Replace");
                    lv_msgbox_add_text(box, ("A preset \"" + name + "\" is saved already. Replace it?").c_str());
                    return;
                }
                ui->save(name);
                break;
            }
            case Dialog::replace:
                ui->close_dialog();
                ui->save(ui->pending_name_);
                break;
            case Dialog::remove: {
                ui->close_dialog();
                std::string name = ui->preset_name_;
                if (!g2m::presets::remove(ui->preset_path_)) {
                    ui->message("Couldn't delete " + name);
                    return;
                }
                ui->preset_ = false;
                ui->preset_path_.clear();
                ui->refresh_presets();
                ui->message("Deleted " + name);
                break;
            }
            case Dialog::none:
                break;
        }
    }

    void message(const std::string& text) { lv_label_set_text(message_, text.c_str()); }

    Parameter info_[kParamCount];
    float values_[kParamCount] = {};
    std::vector<std::uint32_t> preset_params_;  // the parameters presets set, with preset_info_
    std::vector<g2m::presets::ParameterInfo> preset_info_;
    std::vector<std::unique_ptr<Control>> controls_;
    std::string model_file_;  // the last model file named in the "model" state
    std::string model_name_;  // the models menu's text

    std::vector<Item> items_;  // the presets menu
    std::string dir_;  // the user's presets
    bool preset_ = false;  // a preset was chosen or saved
    bool preset_changed_ = false;  // and a parameter has changed since
    std::string preset_name_, preset_path_, preset_text_;
    std::size_t picked_before_ = 0;  // the menu's selection when it was opened
    Dialog dialog_kind_ = Dialog::none;
    lv_obj_t* dialog_ = nullptr;
    lv_obj_t* name_ = nullptr;  // the Save as dialog's text box
    std::string pending_name_;  // the name to save once replacing it is confirmed

    lv_obj_t* presets_ = nullptr;
    lv_obj_t* save_ = nullptr;
    lv_obj_t* delete_ = nullptr;
    lv_obj_t* models_ = nullptr;
    lv_obj_t* model_info_ = nullptr;
    lv_obj_t* model_error_ = nullptr;
    lv_obj_t* message_ = nullptr;
    lv_obj_t* meter_ = nullptr;
    lv_obj_t* level_ = nullptr;
    lv_obj_t* level_text_ = nullptr;
    lv_obj_t* gate_mark_ = nullptr;
    lv_obj_t* pedal_ = nullptr;
    lv_obj_t* notes_text_ = nullptr;
    std::vector<lv_obj_t*> keys_;

    DISTRHO_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(G2mUI)
};

UI* createUI() {
    return new G2mUI();
}

END_NAMESPACE_DISTRHO
