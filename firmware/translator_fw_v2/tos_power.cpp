#include "tos_power.h"
#include <Arduino.h>
#include "user_config.h"
#include "src/tca9554/esp_io_expander_tca9554.h"
#include "src/button_bsp/button_bsp.h"
#include "src/adc_bsp/adc_bsp.h"
#include "src/lcd_bl_bsp/lcd_bl_pwm_bsp.h"
#include "i2c_bsp.h"

esp_io_expander_handle_t g_io_exp = NULL;
static bool s_vbatPowerFlag = false;      // vendor 07_BATT_PWR_Test logic

void power_init() {
  i2c_master_bus_handle_t bus = NULL;
  if (i2c_master_get_bus_handle(0, &bus) != ESP_OK || !bus) { Serial.println("[PWR] no I2C bus"); return; }
  if (esp_io_expander_new_i2c_tca9554(bus, ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000, &g_io_exp) != ESP_OK) {
    Serial.println("[PWR] TCA9554 init failed"); return;
  }
  // Vendor V1: pin 6 = power latch (HIGH keeps the board on), pin 7 = speaker amplifier enable.
  esp_io_expander_set_dir(g_io_exp, IO_EXPANDER_PIN_NUM_6, IO_EXPANDER_OUTPUT);
  esp_io_expander_set_level(g_io_exp, IO_EXPANDER_PIN_NUM_6, 1);
  esp_io_expander_set_dir(g_io_exp, IO_EXPANDER_PIN_NUM_7, IO_EXPANDER_OUTPUT);
  esp_io_expander_set_level(g_io_exp, IO_EXPANDER_PIN_NUM_7, 1);
  adc_bsp_init();
  Serial.println("[PWR] latch on, PA on");
}

static void pwr_task(void*) {
  for (;;) {
    EventBits_t ev = xEventGroupWaitBits(pwr_groups, set_bit_all, pdTRUE, pdFALSE, pdMS_TO_TICKS(2000));
    if (get_bit_button(ev, 0)) {
      Serial.println("[PWR] click");
      app_on_power_click();
    } else if (get_bit_button(ev, 1)) {
      // Vendor logic: only switch off after a release has been seen (flag), never on the press that
      // powered the board on. On USB power the latch cannot cut power, so this only reboots there.
      if (s_vbatPowerFlag) {
        s_vbatPowerFlag = false;
        Serial.println("[PWR] long press -> power off");
        app_before_power_off();
        if (g_io_exp) esp_io_expander_set_level(g_io_exp, IO_EXPANDER_PIN_NUM_6, 0);
      } else {
        Serial.println("[PWR] long press ignored (power-on press)");
      }
    } else if (get_bit_button(ev, 2)) {
      if (!s_vbatPowerFlag) s_vbatPowerFlag = true;
    }
  }
}

void power_start_tasks() {
  button_Init();
  // Vendor power_Test(): GPIO16 high at boot -> powered from battery via the PWR button.
  // Read after button_Init() so the pin is configured as an input.
  s_vbatPowerFlag = gpio_get_level(GPIO_NUM_16);
  Serial.printf("[PWR] buttons ready, vbatFlag=%d\n", s_vbatPowerFlag);
  xTaskCreatePinnedToCoreWithCaps(pwr_task, "pwr_task", 4 * 1024, NULL, 2, NULL, 1, MALLOC_CAP_SPIRAM);
}

int power_battery_pct() {
  float v = 0; int raw = 0;
  adc_get_value(&v, &raw);                 // vendor: volts after the x3 divider
  // single-cell Li-ion, rough linear map 3.30 V -> 0 %, 4.15 V -> 100 %
  int pct = (int)((v - 3.30f) / (4.15f - 3.30f) * 100.0f);
  return pct < 0 ? 0 : (pct > 100 ? 100 : pct);
}

void power_set_backlight_level(int level) {
  int pct = 40 + (level - 1) * (60 / 9);
  uint16_t duty = 255 - (pct * 255) / 100;
  setUpduty(duty);
}
void power_set_backlight_dim() { setUpduty(LCD_PWM_MODE_175); }
