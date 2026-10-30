// Shared contract between the UI module (ui.cpp, runs only in the LVGL task) and the app
// (recording, network, power, settings).
#ifndef TRANSLATEOS_APP_API_H_
#define TRANSLATEOS_APP_API_H_
#include <stdint.h>
#include <stdbool.h>

// ---------- shared enums ----------
typedef enum { UI_IDLE, UI_RECORDING, UI_PROCESSING, UI_SPEAKING } UiState;
typedef enum { TOAST_INFO, TOAST_WARN, TOAST_ERROR } ToastKind;
typedef enum { NETQ_NONE, NETQ_POOR, NETQ_OK, NETQ_GOOD } NetQuality;

typedef enum {
  SET_LANG_A, SET_LANG_B,          // value = language index (see LANG_* table)
  SET_VOLUME,                      // 1..10   (codec volume = v*10)
  SET_MIC_GAIN,                    // 1..8    (codec gain dB = v*3+9)
  SET_AI_CREATIVITY,               // 0..10   (temperature = v/10)
  SET_NIGHT,                       // 0/1
  SET_NOISE,                       // 0/1
  SET_BRIGHTNESS,                  // 1..10
  SET_SCREEN_TIMEOUT,              // seconds, 0 = never
  SET_FONT_SIZE,                   // 1..4
  SET_TOUCH_SENS,                  // 1..10
  SET_HIST_PAIRS,                  // 1..20   (chat pairs kept on screen)
  SET_SAVE_HISTORY,                // 0/1
  SET_LEFT_HAND,                   // 0/1
  SET_FLIP_ENABLED,                // 0/1     ("Flip for others" / Share Screen)
  SET_MIC_AGC,                     // 0/1
  SET_RMS_THRESHOLD,               // 0..3000 (0 = off)
  SET_WIFI_ENABLED,                // 0/1 (runtime only)
  SET_BLUETOOTH,                   // placeholder, not implemented
  SET__COUNT
} SettingId;

#define LANG_COUNT 10
// Index order used by SET_LANG_A/B. Names and 2-letter codes as shown in the UI.
extern const char* const LANG_NAMES[LANG_COUNT];   // "English","Italian",...
extern const char* const LANG_CODES[LANG_COUNT];   // "en","it",...

typedef struct {
  int  langA, langB;
  int  volume, micGain, aiCreativity;
  bool night, noise;
  int  brightness, screenTimeoutS, fontSize, touchSens, histPairs;
  bool saveHistory, leftHand, flipEnabled, micAGC;
  int  rmsThreshold;
  bool wifiEnabled;
} Settings;

// Owned by the app. The UI may read it at any time (int/bool fields, torn reads are harmless).
// The UI never writes it directly: it calls app_on_setting_changed().
extern Settings g_settings;

#ifdef __cplusplus
extern "C" {
#endif

// ---------- app -> UI (any task; non-blocking; copies its arguments) ----------
void ui_post_state(UiState s);
void ui_post_bubble(const char* orig, const char* trans);
void ui_post_toast(ToastKind k, const char* text, uint32_t ms);
void ui_post_status(bool wifi, int rssi, int battPct, NetQuality q, const char* ssid);
void ui_post_rec_progress(uint16_t ms_elapsed, uint16_t ms_max);
void ui_post_screensaver(bool on);
void ui_post_clear_history(void);
void ui_post_flip(bool flipped);           // IMU says device turned over (only if flipEnabled)

// Called by lvgl_port inside the LVGL task, with the LVGL lock held.
void ui_init(void);

// ---------- UI -> app (called from the LVGL task; must return immediately) ----------
void app_on_rec_tap(void);                 // REC button pressed (toggle start/stop, or cancel while processing)
void app_on_bubble_tap(int pairIndex);      // tap a translation bubble -> replay its speech
void app_on_swap_langs(void);
void app_on_setting_changed(SettingId id, int value);
void app_on_wifi_reset(void);              // "Reset WiFi": forget network, reboot to setup portal
void app_on_clear_history(void);
void app_on_user_activity(void);           // any touch: resets the screensaver timer

#ifdef __cplusplus
}
#endif
#endif
