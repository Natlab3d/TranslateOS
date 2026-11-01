#include "tos_wifi.h"
#include "tos_config.h"
#include "secrets.h"
#include <esp_sntp.h>
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <ArduinoOTA.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <time.h>
#include <sys/time.h>
#include "src/rtc_bsp/rtc_bsp.h"
#include "esp_wifi.h"

typedef struct { char ssid[33]; char pass[65]; } KnownNet;
static KnownNet s_known[WIFI_MAX_KNOWN];
static int s_knownCount = 0;

typedef enum { W_OFF, W_SCAN, W_WAIT_SCAN, W_CONNECTING, W_CONNECTED, W_BACKOFF } WState;
static WState s_state = W_OFF;
static uint32_t s_stateAt = 0, s_backoffMs = 3000, s_lastSeenKnownAt = 0;
static bool s_enabled = true, s_otaStarted = false, s_timeSynced = false, s_portal = false;
static char s_ssid[33] = "";
static WiFiManager s_wm;

// ------------------------------------------------------------------ known networks (NVS "wifinets")
static void known_load() {
  Preferences p; p.begin("wifinets", true);
  s_knownCount = 0;
  for (int i = 0; i < WIFI_MAX_KNOWN; i++) {
    char k[8]; snprintf(k, sizeof k, "s%d", i);
    String s = p.getString(k, "");
    if (!s.length()) continue;
    snprintf(k, sizeof k, "p%d", i);
    String pw = p.getString(k, "");
    snprintf(s_known[s_knownCount].ssid, 33, "%s", s.c_str());
    snprintf(s_known[s_knownCount].pass, 65, "%s", pw.c_str());
    s_knownCount++;
  }
  p.end();
  // Adopt the network already saved in the WiFi driver's own config.
  wifi_config_t cfg;
  if (s_knownCount < WIFI_MAX_KNOWN && esp_wifi_get_config(WIFI_IF_STA, &cfg) == ESP_OK && cfg.sta.ssid[0]) {
    bool dup = false;
    for (int i = 0; i < s_knownCount; i++) if (!strcmp(s_known[i].ssid, (char*)cfg.sta.ssid)) dup = true;
    if (!dup) {
      snprintf(s_known[s_knownCount].ssid, 33, "%s", (char*)cfg.sta.ssid);
      snprintf(s_known[s_knownCount].pass, 65, "%s", (char*)cfg.sta.password);
      s_knownCount++;
    }
  }
  Serial.printf("[WIFI] %d known network(s)\n", s_knownCount);
}

static void known_save() {
  Preferences p; p.begin("wifinets", false);
  for (int i = 0; i < WIFI_MAX_KNOWN; i++) {
    char k[8];
    snprintf(k, sizeof k, "s%d", i); p.putString(k, i < s_knownCount ? s_known[i].ssid : "");
    snprintf(k, sizeof k, "p%d", i); p.putString(k, i < s_knownCount ? s_known[i].pass : "");
  }
  p.end();
}

static void known_add(const char* ssid, const char* pass) {
  if (!ssid || !ssid[0]) return;
  KnownNet n; snprintf(n.ssid, 33, "%s", ssid); snprintf(n.pass, 65, "%s", pass ? pass : "");
  int w = 1; KnownNet out[WIFI_MAX_KNOWN]; out[0] = n;        // newest first, no duplicates
  for (int i = 0; i < s_knownCount && w < WIFI_MAX_KNOWN; i++) if (strcmp(s_known[i].ssid, ssid)) out[w++] = s_known[i];
  memcpy(s_known, out, sizeof(KnownNet) * w); s_knownCount = w;
  known_save();
  Serial.printf("[WIFI] saved network '%s' (%d known)\n", ssid, s_knownCount);
}

// ------------------------------------------------------------------ portal (non-blocking)
static void portal_saved_cb() {
  known_add(s_wm.getWiFiSSID().c_str(), s_wm.getWiFiPass().c_str());
  ui_post_toast(TOAST_INFO, "WiFi saved", 2500);
}

static void portal_start(const char* why) {
  if (s_portal) return;
  s_wm.setConfigPortalBlocking(false);
  s_wm.setConfigPortalTimeout(WIFI_PORTAL_TIMEOUT_S);
  s_wm.setSaveConfigCallback(portal_saved_cb);
  s_wm.setBreakAfterConfig(true);
  s_wm.startConfigPortal("Translator");
  s_portal = true;
  char t[96]; snprintf(t, sizeof t, "%s - join WiFi 'Translator' to set up", why);
  ui_post_toast(TOAST_WARN, t, 5000);
  Serial.printf("[WIFI] portal started (%s)\n", why);
}

// ------------------------------------------------------------------ background time sync
static volatile bool s_sntpDone = false;
static void sntp_cb(struct timeval*) { s_sntpDone = true; }

// System time and RTC hold LOCAL time with TZ=UTC0 (project convention); utc is converted through TZ_POSIX.
static void time_set_from_utc(time_t utc, const char* how) {
  setenv("TZ", TZ_POSIX, 1); tzset();
  struct tm lt; localtime_r(&utc, &lt);
  lt.tm_isdst = 0;
  setenv("TZ", "UTC0", 1); tzset();
  struct timeval tv = { mktime(&lt), 0 }; settimeofday(&tv, NULL);
  i2c_rtc_setTime(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min, lt.tm_sec);
  Serial.printf("[TIME] %s: local %04d-%02d-%02d %02d:%02d, RTC updated\n",
                how, lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min);
  s_timeSynced = true;
}

static void time_sync_task(void*) {
  // 1. NTP: no key needed
  s_sntpDone = false;
  sntp_set_time_sync_notification_cb(sntp_cb);
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  for (int i = 0; i < 25 && !s_sntpDone; i++) vTaskDelay(pdMS_TO_TICKS(200));
  time_t u = time(NULL);
  esp_sntp_stop();                                       // it would keep rewriting the clock with UTC
  if (s_sntpDone) { time_set_from_utc(u, "NTP"); vTaskDelete(NULL); return; }
  // 2. HTTP Date header (works when NTP/UDP is blocked)
  {
    HTTPClient h;
    h.begin("http://connectivitycheck.gstatic.com/generate_204");
    h.setConnectTimeout(4000); h.setTimeout(4000);
    const char* keys[] = { "Date" }; h.collectHeaders(keys, 1);
    int c = h.GET();
    String d = (c == 204 && h.hasHeader("Date")) ? h.header("Date") : String();
    h.end();
    struct tm g = {};
    if (d.length() && strptime(d.c_str(), "%a, %d %b %Y %H:%M:%S GMT", &g)) {
      setenv("TZ", "UTC0", 1); tzset();
      g.tm_isdst = 0;
      time_set_from_utc(mktime(&g), "HTTP Date"); vTaskDelete(NULL); return;
    }
    Serial.printf("[TIME] NTP and HTTP Date failed (%d), trying Geolocation\n", c);
  }
  // 3. Geolocation + timeapi (needs a key with Geolocation enabled)
  int n = WiFi.scanComplete() > 0 ? WiFi.scanComplete() : WiFi.scanNetworks(false, true);
  if (n <= 0) { Serial.println("[GEO] no networks, skip time sync"); vTaskDelete(NULL); return; }
  DynamicJsonDocument req(2048);
  JsonArray aps = req.createNestedArray("wifiAccessPoints");
  req["considerIp"] = true;
  for (int i = 0; i < n && i < 10; i++) { JsonObject a = aps.createNestedObject(); a["macAddress"] = WiFi.BSSIDstr(i); a["signalStrength"] = WiFi.RSSI(i); }
  String body; serializeJson(req, body);
  WiFiClientSecure c; c.setInsecure();
  HTTPClient http;
  http.begin(c, "https://www.googleapis.com/geolocation/v1/geolocate");
  http.setConnectTimeout(8000); http.setTimeout(10000);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("x-goog-api-key", GEMINI_API_KEY);            // header, not in the URL
  int code = http.POST(body);
  if (code != 200) { Serial.printf("[GEO] HTTP %d: %.160s\n", code, http.getString().c_str()); http.end(); vTaskDelete(NULL); return; }
  DynamicJsonDocument g(1024);
  bool bad = deserializeJson(g, http.getString()) || !g["location"].is<JsonObject>();
  http.end();
  if (bad) { Serial.println("[GEO] parse failed"); vTaskDelete(NULL); return; }
  float lat = g["location"]["lat"] | 0.0f, lng = g["location"]["lng"] | 0.0f;
  char url[160];
  snprintf(url, sizeof url, "https://timeapi.io/api/Time/current/coordinate?latitude=%.5f&longitude=%.5f", lat, lng);
  http.begin(c, url);
  http.setConnectTimeout(8000); http.setTimeout(10000);
  code = http.GET();
  if (code != 200) { Serial.printf("[TIME] timeapi HTTP %d\n", code); http.end(); vTaskDelete(NULL); return; }
  DynamicJsonDocument t(768);
  bad = (deserializeJson(t, http.getString()) != DeserializationError::Ok);
  http.end();
  if (bad) { Serial.println("[TIME] parse failed"); vTaskDelete(NULL); return; }
  struct tm tm = {};
  tm.tm_year = (int)(t["year"] | 1970) - 1900; tm.tm_mon = (int)(t["month"] | 1) - 1; tm.tm_mday = t["day"] | 1;
  tm.tm_hour = t["hour"] | 0; tm.tm_min = t["minute"] | 0; tm.tm_sec = t["seconds"] | 0; tm.tm_isdst = -1;
  setenv("TZ", "UTC0", 1); tzset();                      // timeapi already gives local time
  time_t e = mktime(&tm); struct timeval tv = { e, 0 }; settimeofday(&tv, NULL);
  i2c_rtc_setTime(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
  Serial.printf("[TIME] set %04d-%02d-%02d %02d:%02d (lat %.3f lng %.3f), RTC updated\n",
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, lat, lng);
  s_timeSynced = true;
  vTaskDelete(NULL);
}

// ------------------------------------------------------------------ state machine
static void set_state(WState s) { s_state = s; s_stateAt = millis(); }

void wifi_begin() {
  WiFi.mode(WIFI_STA);
  known_load();                              // reads the driver's saved config too, so before persistent(false)
  WiFi.persistent(false);                    // we keep our own list; don't rewrite driver NVS each connect
  WiFi.setSleep(false);                      // modem sleep glitches GPIO on this board (project notes)
  WiFi.setAutoReconnect(false);              // reconnection is driven here
  s_lastSeenKnownAt = millis();
  if (s_knownCount == 0) portal_start("No WiFi saved");
  set_state(W_SCAN);
}

void wifi_service() {
  if (s_portal) {
    s_wm.process();
    if (!s_wm.getConfigPortalActive()) { s_portal = false; Serial.println("[WIFI] portal closed"); set_state(W_SCAN); }
  }
  if (s_otaStarted) ArduinoOTA.handle();
  if (!s_enabled) return;
  uint32_t now = millis();
  switch (s_state) {
    case W_OFF: break;
    case W_SCAN:
      if (s_knownCount == 0) { set_state(W_BACKOFF); break; }
      WiFi.scanDelete();
      WiFi.scanNetworks(true, false);          // async
      set_state(W_WAIT_SCAN);
      break;
    case W_WAIT_SCAN: {
      int n = WiFi.scanComplete();
      if (n == WIFI_SCAN_RUNNING) { if (now - s_stateAt > 12000) { WiFi.scanDelete(); set_state(W_BACKOFF); } break; }
      int best = -1, bestRssi = -1000;
      for (int i = 0; i < n; i++)
        for (int k = 0; k < s_knownCount; k++)
          if (WiFi.SSID(i) == s_known[k].ssid && WiFi.RSSI(i) > bestRssi) { best = k; bestRssi = WiFi.RSSI(i); }
      if (best < 0) {
        if (!s_portal && now - s_lastSeenKnownAt > 90000) portal_start("No known WiFi nearby");
        set_state(W_BACKOFF); break;
      }
      s_lastSeenKnownAt = now;
      snprintf(s_ssid, sizeof s_ssid, "%s", s_known[best].ssid);
      Serial.printf("[WIFI] connecting '%s' rssi=%d\n", s_ssid, bestRssi);
      WiFi.begin(s_known[best].ssid, s_known[best].pass);
      set_state(W_CONNECTING);
      break;
    }
    case W_CONNECTING:
      if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[WIFI] connected '%s' ip=%s rssi=%d in %u ms\n", s_ssid, WiFi.localIP().toString().c_str(),
                      WiFi.RSSI(), (unsigned)(now - s_stateAt));
        s_backoffMs = 3000;
        set_state(W_CONNECTED);
        ui_post_toast(TOAST_INFO, "WiFi connected", 1500);
        if (!s_timeSynced) xTaskCreatePinnedToCore(time_sync_task, "timesync", 8192, NULL, 1, NULL, 0);   // internal-RAM stack (TCP)
        if (!s_otaStarted) {
          ArduinoOTA.setHostname("translator");
          ArduinoOTA.setPassword(OTA_PASSWORD);
          ArduinoOTA.onStart([]() { Serial.println("[OTA] start"); });
          ArduinoOTA.onError([](ota_error_t e) { Serial.printf("[OTA] error %u\n", e); });
          ArduinoOTA.begin();
          s_otaStarted = true;
        }
      } else if (now - s_stateAt > 15000) {
        Serial.printf("[WIFI] connect timeout status=%d\n", WiFi.status());
        WiFi.disconnect(false, false);
        set_state(W_BACKOFF);
      }
      break;
    case W_CONNECTED:
      if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[WIFI] lost connection");
        ui_post_toast(TOAST_WARN, "WiFi lost - reconnecting", 3000);
        set_state(W_BACKOFF);
      }
      break;
    case W_BACKOFF:
      if (now - s_stateAt > s_backoffMs) { if (s_backoffMs < 30000) s_backoffMs *= 2; set_state(W_SCAN); }
      break;
  }
}

void wifi_set_enabled(bool on) {
  if (on == s_enabled) return;
  s_enabled = on;
  if (!on) { WiFi.disconnect(true, false); WiFi.mode(WIFI_OFF); set_state(W_OFF); }
  else { WiFi.mode(WIFI_STA); WiFi.setSleep(false); set_state(W_SCAN); }
}

void wifi_forget_all_and_restart() {
  s_knownCount = 0; known_save();
  s_wm.resetSettings();
  Serial.println("[WIFI] all networks forgotten, restarting into setup");
  delay(300);
  ESP.restart();
}

bool wifi_connected() { return s_state == W_CONNECTED && WiFi.status() == WL_CONNECTED; }
const char* wifi_ssid() { return wifi_connected() ? s_ssid : ""; }
NetQuality wifi_quality(int* rssiOut) {
  if (!wifi_connected()) { if (rssiOut) *rssiOut = 0; return NETQ_NONE; }
  int r = WiFi.RSSI(); if (rssiOut) *rssiOut = r;
  return r > -60 ? NETQ_GOOD : (r > -72 ? NETQ_OK : NETQ_POOR);
}
