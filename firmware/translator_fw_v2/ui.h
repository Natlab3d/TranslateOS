// TranslateOS - UI module public surface.
// Everything (ui_init, ui_post_*, app_on_*, LANG_NAMES/LANG_CODES, g_settings) is declared in app_api.h.
// ui.cpp implements: ui_init, ui_post_*, LANG_NAMES, LANG_CODES. All LVGL calls live in the LVGL task.
#pragma once
#include "app_api.h"
