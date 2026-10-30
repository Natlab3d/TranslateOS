/* TranslateOS: LVGL assert handler (see lv_conf.h LV_ASSERT_HANDLER).
 * The vendor default was `while(1);`, which hangs the LVGL task until the task watchdog fires and
 * leaves no clue. This prints where it failed and aborts, so the panic handler saves a coredump with
 * the LVGL backtrace and the device reboots. */
#pragma once
#include <stdlib.h>
#include "esp_rom_sys.h"

static inline void lvgl_assert_hook(const char * file, int line)
{
    esp_rom_printf("\n[LVGL] ASSERT FAILED %s:%d -> abort (coredump)\n", file, line);
    abort();
}
