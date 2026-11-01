// TranslateOS — Waveshare ESP32-S3-Touch-LCD-3.49 (V1 board).
// Tap REC, speak, tap REC again: Gemini transcribes + translates, Google TTS speaks the result.
// Architecture: app_api.h is the contract between UI and app.
//
// Arduino IDE settings: ESP32S3 Dev Module, USB CDC On Boot Enabled,
// Flash QIO 80 MHz 16 MB, Partition "16M Flash (3MB APP/9.9MB FATFS)", PSRAM OPI, core esp32 3.3.0,
// LVGL 9.3.0 with vendor_patches/lv_conf.h (see top-level README, "Libraries").

#include <Arduino.h>
#include <time.h>
#include <sys/time.h>
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_core_dump.h"

#include "user_config.h"
#include "lvgl_port.h"
#include "i2c_bsp.h"
#include "src/rtc_bsp/rtc_bsp.h"
#include "src/qmi_bsp/qmi_bsp.h"
#include "src/lcd_bl_bsp/lcd_bl_pwm_bsp.h"

#include "tos_config.h"
#include "app_api.h"
#include "tos_settings.h"
#include "tos_audio.h"
#include "tos_net.h"
#include "tos_wifi.h"
#include "tos_power.h"

// ------------------------------------------------------------------ app state
static volatile uint32_t s_lastActivity = 0;
static volatile bool s_screensaver = false;
static volatile bool s_wifiEnablePending = false, s_wifiEnableValue = true;
static volatile bool s_wifiResetPending = false;
static uint32_t s_lastStatus = 0, s_lastMemLog = 0;
static uint32_t s_translations = 0;

static void log_memory(const char* why) {
  Serial.printf("[MEM] %s int free=%u min=%u big=%u | psram free=%u big=%u | n=%u\n", why,
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM), (unsigned)s_translations);
}

static const char* reset_reason_str(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON: return "power on";   case ESP_RST_SW: return "software restart";
    case ESP_RST_PANIC: return "crash";        case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog"; case ESP_RST_WDT: return "watchdog";
    case ESP_RST_BROWNOUT: return "brownout";  case ESP_RST_USB: return "USB reset";
    default: return "other";
  }
}

// Report the previous crash (if any) once, then clear it so the next one is fresh evidence.
static void report_previous_crash() {
  esp_reset_reason_t rr = esp_reset_reason();
  Serial.printf("[BOOT] reset reason: %s (%d)\n", reset_reason_str(rr), (int)rr);
  if (esp_core_dump_image_check() == ESP_OK) {
    esp_core_dump_summary_t sum;
    if (esp_core_dump_get_summary(&sum) == ESP_OK) {
      char reason[160] = "";
      esp_core_dump_get_panic_reason(reason, sizeof reason);
      Serial.printf("[BOOT] coredump: task=%s pc=0x%08lx reason=%s\n", sum.exc_task, (unsigned long)sum.exc_pc, reason);
      Serial.print("[BOOT] backtrace:");
      for (int i = 0; i < (int)sum.exc_bt_info.depth; i++) Serial.printf(" 0x%08lx", (unsigned long)sum.exc_bt_info.bt[i]);
      Serial.println();
      char t[96]; snprintf(t, sizeof t, "Recovered from a crash (%s)", sum.exc_task);
      ui_post_toast(TOAST_WARN, t, 5000);
    }
    esp_core_dump_image_erase();
  }
}

// ------------------------------------------------------------------ UI -> app (LVGL task, must be quick)
extern "C" void app_on_rec_tap(void) {
  s_lastActivity = millis();
  if (net_busy()) { net_cancel(); return; }                 // tap while translating/speaking = cancel
  if (audio_play_active()) { audio_play_cancel(); return; }
  if (audio_rec_active()) { audio_rec_stop(); ui_post_state(UI_PROCESSING); return; }
  if (!audio_rec_start()) ui_post_toast(TOAST_WARN, "Busy - try again", 1500);
}

extern "C" void app_on_bubble_tap(int pairIndex) {
  s_lastActivity = millis();
  if (net_busy() || audio_rec_active() || audio_play_active()) return;
  net_submit_speak_pair(pairIndex);
}

extern "C" void app_on_swap_langs(void) {
  int a = g_settings.langA; g_settings.langA = g_settings.langB; g_settings.langB = a;
  settings_mark_dirty();
}

extern "C" void app_on_setting_changed(SettingId id, int v) {
  s_lastActivity = millis();
  switch (id) {
    case SET_LANG_A:         if (v >= 0 && v < LANG_COUNT) g_settings.langA = v; break;
    case SET_LANG_B:         if (v >= 0 && v < LANG_COUNT) g_settings.langB = v; break;
    case SET_VOLUME:         g_settings.volume = v; audio_set_volume(settings_codec_volume()); break;
    case SET_MIC_GAIN:       g_settings.micGain = v; audio_set_gain_db(settings_codec_gain_db()); break;
    case SET_AI_CREATIVITY:  g_settings.aiCreativity = v; break;
    case SET_NIGHT:          g_settings.night = v; break;
    case SET_NOISE:          g_settings.noise = v; break;
    case SET_BRIGHTNESS:     g_settings.brightness = v; power_set_backlight_level(v); break;
    case SET_SCREEN_TIMEOUT: g_settings.screenTimeoutS = v; break;
    case SET_FONT_SIZE:      g_settings.fontSize = v; break;
    case SET_TOUCH_SENS:     g_settings.touchSens = v; break;
    case SET_HIST_PAIRS:     g_settings.histPairs = v; break;
    case SET_SAVE_HISTORY:   g_settings.saveHistory = v; break;
    case SET_LEFT_HAND:      g_settings.leftHand = v; break;
    case SET_FLIP_ENABLED:   g_settings.flipEnabled = v; if (!v) ui_post_flip(false); break;
    case SET_MIC_AGC:        g_settings.micAGC = v; break;
    case SET_RMS_THRESHOLD:  g_settings.rmsThreshold = v; break;
    case SET_WIFI_ENABLED:   g_settings.wifiEnabled = v; s_wifiEnableValue = v; s_wifiEnablePending = true; return;
    case SET_BLUETOOTH:      return;                         // placeholder
    default: return;
  }
  settings_mark_dirty();
}

extern "C" void app_on_wifi_reset(void)     { s_wifiResetPending = true; }
extern "C" void app_on_clear_history(void)  { if (!net_busy()) net_history_clear(); ui_post_clear_history(); }

extern "C" void app_on_user_activity(void) {
  s_lastActivity = millis();
  if (s_screensaver) {
    Serial.println("[SAVER] off (touch)");
    s_screensaver = false;
    power_set_backlight_level(g_settings.brightness);
    ui_post_screensaver(false);
  }
}

// ------------------------------------------------------------------ hardware -> app
void app_on_power_click() {
  if (s_screensaver) app_on_user_activity();
  else { Serial.println("[SAVER] on (power button)"); s_screensaver = true; power_set_backlight_dim(); ui_post_screensaver(true); }
}
void app_before_power_off() { settings_flush(); ui_post_toast(TOAST_INFO, "Power off", 1000); delay(300); }

void app_net_done(bool ok) {
  if (ok) s_translations++;
  s_lastActivity = millis();
  log_memory(ok ? "after job" : "after failed job");
}

// ------------------------------------------------------------------ setup
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.printf("\n[BOOT] TranslateOS %s\n", FW_VERSION);

  i2c_master_Init();
  power_init();                     // latch + PA first (vendor 07/08 order)
  settings_load();

  // Display first: the screen is alive before anything touches the network (no more "trash screen").
  lvgl_port_init();                 // starts the LVGL task; ui_init() shows the splash
  lcd_bl_pwm_bsp_init(LCD_PWM_MODE_255);
  power_set_backlight_level(g_settings.brightness);
  report_previous_crash();

  // Clock from the RTC until the network time sync (if any) runs. Never blocks if the chip is missing.
  rtc_init();
  RtcDateTime_t rt = i2c_rtc_get();
  if (rt.year >= 2024) {
    struct tm tm = {}; tm.tm_year = rt.year - 1900; tm.tm_mon = rt.month - 1; tm.tm_mday = rt.day;
    tm.tm_hour = rt.hour; tm.tm_min = rt.minute; tm.tm_sec = rt.second; tm.tm_isdst = -1;
    setenv("TZ", "UTC0", 1); tzset();
    struct timeval tv = { mktime(&tm), 0 }; settimeofday(&tv, NULL);
    Serial.printf("[BOOT] RTC %04d-%02d-%02d %02d:%02d\n", rt.year, rt.month, rt.day, rt.hour, rt.minute);
  } else {
    Serial.println("[BOOT] RTC time not valid yet (will be set from the network)");
  }

  if (!audio_init()) ui_post_toast(TOAST_ERROR, "Audio codec failed - restart the device", 5000);
  net_init();

  qmi_init();
  xTaskCreatePinnedToCoreWithCaps(i2c_qmi_loop_task, "qmi_task", 4 * 1024, NULL, 2, NULL, 1, MALLOC_CAP_SPIRAM);
  power_start_tasks();

  wifi_begin();                     // returns immediately; connects in the background
  s_lastActivity = millis();
  log_memory("boot");
  Serial.println("[BOOT] ready");
}

// ------------------------------------------------------------------ loop (Arduino task, core 1)
void loop() {
  uint32_t now = millis();
  wifi_service();
  settings_service();

  if (s_wifiEnablePending) { s_wifiEnablePending = false; wifi_set_enabled(s_wifiEnableValue); }
  if (s_wifiResetPending)  { s_wifiResetPending = false; settings_flush(); wifi_forget_all_and_restart(); }

  // A recording finished: decide locally first (no data used for silence / too short / no WiFi).
  RecResult r;
  if (audio_rec_take_result(&r)) {
    if (r.silent && r.peak < 1500) { ui_post_toast(TOAST_INFO, "Didn't hear anything - speak closer", 2500); ui_post_state(UI_IDLE); }
    else if (r.speechMs < MIN_SPEECH_MS) { ui_post_toast(TOAST_INFO, "Too short - speak a bit longer", 2500); ui_post_state(UI_IDLE); }
    else if (!wifi_connected()) { ui_post_toast(TOAST_ERROR, "No WiFi - check hotspot / WiFi", 4000); ui_post_state(UI_IDLE); }
    else if (!net_submit_translate(r.wav, r.wavLen, g_settings.langA, g_settings.langB)) {
      ui_post_toast(TOAST_WARN, "Busy - try again", 1500); ui_post_state(UI_IDLE);
    }
  }

  // IMU flip (only when "Flip for others" is on)
  if (g_flipChanged) {
    g_flipChanged = false;
    Serial.printf("[FLIP] sensor says flipped=%d, feature %s\n", (int)g_isFlipped, g_settings.flipEnabled ? "ON" : "off");
    if (g_settings.flipEnabled) ui_post_flip(g_isFlipped);
  }

  // Screensaver (never while recording, translating or speaking)
  bool busy = audio_rec_active() || net_busy() || audio_play_active();
  if (busy) s_lastActivity = now;
  if (!s_screensaver && g_settings.screenTimeoutS > 0 && now - s_lastActivity > (uint32_t)g_settings.screenTimeoutS * 1000) {
    Serial.printf("[SAVER] on (idle %us, timeout %ds)\n", (unsigned)((now - s_lastActivity) / 1000), g_settings.screenTimeoutS);
    s_screensaver = true; power_set_backlight_dim(); ui_post_screensaver(true);
  }

  if (now - s_lastStatus > 5000) {
    s_lastStatus = now;
    int rssi = 0; NetQuality q = wifi_quality(&rssi);
    int batt = power_battery_pct();
    ui_post_status(wifi_connected(), rssi, batt, q, wifi_ssid());
    // Low-battery popups: once at 20 %, once at 10 %, re-armed above 30 %. Two readings in a row (10 s) so a
    // brief dip while the speaker plays doesn't trigger it.
    static int s_lowStep = 0, s_lowSeen = 0;
    if (batt >= 30) { s_lowStep = 0; s_lowSeen = 0; }
    else if (batt <= 20) {
      if (++s_lowSeen >= 2) {
        if (batt <= 10 && s_lowStep < 2) { s_lowStep = 2; ui_post_toast(TOAST_ERROR, "Battery very low - charge now", 5000); Serial.printf("[BATT] %d%% critical\n", batt); }
        else if (batt > 10 && s_lowStep < 1) { s_lowStep = 1; ui_post_toast(TOAST_WARN, "Battery low - 20%", 5000); Serial.printf("[BATT] %d%% low\n", batt); }
      }
    } else s_lowSeen = 0;
  }
  if (now - s_lastMemLog > 60000) { s_lastMemLog = now; log_memory("periodic"); }
  delay(10);
}
