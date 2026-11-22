// DPF's description of the plugin (read by DPF's format wrappers).

#ifndef DISTRHO_PLUGIN_INFO_H_INCLUDED
#define DISTRHO_PLUGIN_INFO_H_INCLUDED

#define DISTRHO_PLUGIN_BRAND "guitar-2-midi"
#define DISTRHO_PLUGIN_NAME "Guitar to MIDI"
#define DISTRHO_PLUGIN_URI "https://github.com/gwaggott/guitar-2-midi#player"  // the LV2 plugin's URI: never change it
#define DISTRHO_PLUGIN_CLAP_ID "com.github.gwaggott.guitar-2-midi.player"

#define DISTRHO_PLUGIN_BRAND_ID Gwag
#define DISTRHO_PLUGIN_UNIQUE_ID G2mP

#define DISTRHO_PLUGIN_HAS_UI 1
#define DISTRHO_UI_USE_CUSTOM 1  // LVGL, through plugin/ui/LVGL.hpp (docs/release-plan.md, "UI")
#define DISTRHO_UI_CUSTOM_INCLUDE_PATH "LVGL.hpp"
#define DISTRHO_UI_CUSTOM_WIDGET_TYPE DGL_NAMESPACE::LVGLTopLevelWidget
#define DISTRHO_UI_DEFAULT_WIDTH 760
#define DISTRHO_UI_DEFAULT_HEIGHT 480
#define DISTRHO_UI_USER_RESIZABLE 0
#define DISTRHO_UI_FILE_BROWSER 1  // "Load file..." for the model
#define DISTRHO_UI_URI "https://github.com/gwaggott/guitar-2-midi#player-ui"  // one fragment: the plugin URI has one already
#define DISTRHO_PLUGIN_IS_RT_SAFE 1
#define DISTRHO_PLUGIN_NUM_INPUTS 1
#define DISTRHO_PLUGIN_NUM_OUTPUTS 1
#define DISTRHO_PLUGIN_WANT_MIDI_INPUT 1  // for the sustain pedal (docs/release-plan.md); DPF's standalone needs it too
#define DISTRHO_PLUGIN_WANT_MIDI_OUTPUT 1
#define DISTRHO_PLUGIN_WANT_STATE 1
#define DISTRHO_PLUGIN_WANT_FULL_STATE 1  // getState reports the model actually loaded
#define DISTRHO_PLUGIN_WANT_PROGRAMS 1  // the factory presets (factory_presets.h)

#define DISTRHO_PLUGIN_LV2_CATEGORY "lv2:AnalyserPlugin"
#define DISTRHO_PLUGIN_VST3_CATEGORIES "Fx|Analyzer"
#define DISTRHO_PLUGIN_CLAP_FEATURES "audio-effect", "analyzer", "mono"

#endif
