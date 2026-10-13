#ifndef DISTRHO_PLUGIN_INFO_H_INCLUDED
#define DISTRHO_PLUGIN_INFO_H_INCLUDED

#define DISTRHO_PLUGIN_BRAND   "plebdui"
#define DISTRHO_PLUGIN_NAME    "CRT Scope"
#define DISTRHO_PLUGIN_URI     "https://github.com/plebdui/crtscope"
#define DISTRHO_PLUGIN_CLAP_ID "io.github.plebdui.crtscope"

#define DISTRHO_PLUGIN_BRAND_ID  Pleb
#define DISTRHO_PLUGIN_UNIQUE_ID CrtS

#define DISTRHO_PLUGIN_HAS_UI             1
#define DISTRHO_PLUGIN_IS_RT_SAFE         1
#define DISTRHO_PLUGIN_NUM_INPUTS         2
#define DISTRHO_PLUGIN_NUM_OUTPUTS        2
#define DISTRHO_PLUGIN_WANT_DIRECT_ACCESS 1   // the window reads the audio straight from the plugin
#define DISTRHO_PLUGIN_WANT_STATE         1   // the image file path
#define DISTRHO_UI_USE_NANOVG             1
#define DISTRHO_UI_FILE_BROWSER           1   // click the tube in image mode to pick a file
#define DISTRHO_UI_USER_RESIZABLE         0
#define DISTRHO_PLUGIN_VST3_CATEGORIES    "Fx|Analyzer"
#define DISTRHO_PLUGIN_CLAP_FEATURES      "audio-effect", "analyzer", "stereo"

#endif
