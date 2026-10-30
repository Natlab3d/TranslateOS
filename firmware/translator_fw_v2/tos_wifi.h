// WiFi: non-blocking connection manager (never blocks boot or the UI), up to WIFI_MAX_KNOWN remembered
// networks, setup portal "Translator" only when needed, background time sync, OTA.
#ifndef TRANSLATEOS_WIFI_H_
#define TRANSLATEOS_WIFI_H_
#include "app_api.h"

void wifi_begin();                       // call once from setup(); returns immediately
void wifi_service();                     // call from loop()
void wifi_set_enabled(bool on);
void wifi_forget_all_and_restart();      // "Reset WiFi"
bool wifi_connected();
NetQuality wifi_quality(int* rssiOut);
const char* wifi_ssid();
#endif
