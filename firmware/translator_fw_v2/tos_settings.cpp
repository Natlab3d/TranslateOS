#include "tos_settings.h"
#include <Arduino.h>
#include <Preferences.h>

Settings g_settings;

static Preferences s_prefs;
static volatile bool s_dirty = false;
static volatile uint32_t s_dirtyAt = 0;

static int lang_index(const String& name, int fallback) {
  for (int i = 0; i < LANG_COUNT; i++) if (name.equalsIgnoreCase(LANG_NAMES[i])) return i;
  return fallback;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

void settings_load() {
  s_prefs.begin("translator", true);
  // NVS value types: str / i32 / u8 (bool) / float (blob).
  g_settings.langA        = lang_index(s_prefs.getString("langA", "English"), 0);
  g_settings.langB        = lang_index(s_prefs.getString("langB", "Italian"), 1);
  g_settings.volume       = clampi(s_prefs.getInt("volume", 8), 1, 10);
  g_settings.micGain      = clampi(s_prefs.getInt("gain", 7), 1, 8);
  g_settings.aiCreativity = clampi((int)lroundf(s_prefs.getFloat("temp", 0.3f) * 10.0f), 0, 10);
  g_settings.night        = s_prefs.getBool("night", false);
  g_settings.noise        = s_prefs.getBool("noise", true);
  g_settings.brightness   = clampi(s_prefs.getInt("bright", 7), 1, 10);
  g_settings.screenTimeoutS = clampi(s_prefs.getInt("timeout", 30000) / 1000, 0, 3600);
  g_settings.fontSize     = clampi(s_prefs.getInt("fontSize", 2), 1, 4);
  g_settings.touchSens    = clampi(s_prefs.getInt("touchSens", 7), 1, 10);
  g_settings.histPairs    = clampi(s_prefs.getInt("histPairs", 10), 1, 20);
  g_settings.saveHistory  = s_prefs.getBool("saveHistory", false);
  g_settings.leftHand     = s_prefs.getBool("leftHand", false);
  g_settings.flipEnabled  = s_prefs.getBool("flipEnabled", false);
  g_settings.micAGC       = s_prefs.getBool("micAGC", true);
  g_settings.rmsThreshold = clampi(s_prefs.getInt("rmsThresh", 0), 0, 3000);
  g_settings.wifiEnabled  = true;
  s_prefs.end();
  Serial.printf("[SET] loaded: %s->%s vol=%d gain=%d temp=%d bright=%d timeout=%ds font=%d pairs=%d\n",
                LANG_NAMES[g_settings.langA], LANG_NAMES[g_settings.langB], g_settings.volume,
                g_settings.micGain, g_settings.aiCreativity, g_settings.brightness,
                g_settings.screenTimeoutS, g_settings.fontSize, g_settings.histPairs);
}

void settings_mark_dirty() {
  s_dirtyAt = millis();
  s_dirty = true;
}

static void settings_save();

void settings_service() {
  if (!s_dirty || millis() - s_dirtyAt < 2000) return;
  settings_save();
}

void settings_flush() { if (s_dirty) settings_save(); }

static void settings_save() {
  s_dirty = false;
  s_prefs.begin("translator", false);
  s_prefs.putString("langA", LANG_NAMES[g_settings.langA]);
  s_prefs.putString("langB", LANG_NAMES[g_settings.langB]);
  s_prefs.putInt("volume", g_settings.volume);
  s_prefs.putInt("gain", g_settings.micGain);
  s_prefs.putFloat("temp", g_settings.aiCreativity / 10.0f);
  s_prefs.putBool("night", g_settings.night);
  s_prefs.putBool("noise", g_settings.noise);
  s_prefs.putInt("bright", g_settings.brightness);
  s_prefs.putInt("timeout", g_settings.screenTimeoutS * 1000);
  s_prefs.putInt("fontSize", g_settings.fontSize);
  s_prefs.putInt("touchSens", g_settings.touchSens);
  s_prefs.putInt("histPairs", g_settings.histPairs);
  s_prefs.putBool("saveHistory", g_settings.saveHistory);
  s_prefs.putBool("leftHand", g_settings.leftHand);
  s_prefs.putBool("flipEnabled", g_settings.flipEnabled);
  s_prefs.putBool("micAGC", g_settings.micAGC);
  s_prefs.putInt("rmsThresh", g_settings.rmsThreshold);
  s_prefs.end();
  Serial.println("[SET] saved");
}

float settings_temperature()   { return g_settings.aiCreativity / 10.0f; }
int   settings_codec_volume()  { return g_settings.volume * 10; }
float settings_codec_gain_db() { return g_settings.micGain * 3 + 9; }   // UI 1-8 -> 12-33 dB
