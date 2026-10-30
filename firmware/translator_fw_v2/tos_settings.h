// Settings persistence in NVS. Namespace, keys and value types are fixed so stored settings survive firmware updates.
#ifndef TRANSLATEOS_SETTINGS_H_
#define TRANSLATEOS_SETTINGS_H_
#include "app_api.h"

void settings_load();                 // fills g_settings (defaults for missing keys)
void settings_mark_dirty();           // schedule a save (debounced)
void settings_service();              // call from loop(): saves 2 s after the last change
void settings_flush();                // save now if anything changed (before power off)
float settings_temperature();         // AI creativity as Gemini temperature
int  settings_codec_volume();         // 0..100
float settings_codec_gain_db();       // dB
#endif
