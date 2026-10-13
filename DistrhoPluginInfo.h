#ifndef DISTRHO_PLUGIN_INFO_H_INCLUDED
#define DISTRHO_PLUGIN_INFO_H_INCLUDED

#define DISTRHO_PLUGIN_BRAND   "plebdui"
#define DISTRHO_PLUGIN_BRAND_ID  Pleb

// The Mac build also makes "CRT Scope MIDI" (make MIDI_AU=true): an Audio
// Unit can't switch type at run time, and Logic only sends MIDI to a
// MIDI-controlled effect, which GarageBand doesn't list
#ifdef CRT_MIDI_AU
#define DISTRHO_PLUGIN_NAME    "CRT Scope MIDI"
#define DISTRHO_PLUGIN_URI     "https://github.com/plebdui/crtscope#midi"
#define DISTRHO_PLUGIN_CLAP_ID "io.github.plebdui.crtscope.midi"
#define DISTRHO_PLUGIN_UNIQUE_ID CrSM
#define DISTRHO_PLUGIN_AU_TYPE   aumf
#else
#define DISTRHO_PLUGIN_NAME    "CRT Scope"
#define DISTRHO_PLUGIN_URI     "https://github.com/plebdui/crtscope"
#define DISTRHO_PLUGIN_CLAP_ID "io.github.plebdui.crtscope"
#define DISTRHO_PLUGIN_UNIQUE_ID CrtS
#define DISTRHO_PLUGIN_AU_TYPE   aufx
#endif

#define DISTRHO_PLUGIN_HAS_UI             1
#define DISTRHO_PLUGIN_IS_RT_SAFE         1
#define DISTRHO_PLUGIN_NUM_INPUTS         2
#define DISTRHO_PLUGIN_NUM_OUTPUTS        2
#define DISTRHO_PLUGIN_WANT_DIRECT_ACCESS 1   // the window reads the audio straight from the plugin
#define DISTRHO_PLUGIN_WANT_STATE         1   // the image file path
#define DISTRHO_PLUGIN_WANT_MIDI_INPUT    1   // SHAPE can follow MIDI notes
#define DPF_VST3_NO_MIDI_CC_PARAMETERS    1   // keep VST3 parameter IDs as before (patches/dpf-vst3-no-midi-cc-params.patch)
#define DISTRHO_UI_USE_NANOVG             1
#define DISTRHO_UI_FILE_BROWSER           1   // click the tube in image mode to pick a file
#define DISTRHO_UI_USER_RESIZABLE         0
#define DISTRHO_PLUGIN_VST3_CATEGORIES    "Fx|Analyzer"
#define DISTRHO_PLUGIN_CLAP_FEATURES      "audio-effect", "analyzer", "stereo"

#endif
