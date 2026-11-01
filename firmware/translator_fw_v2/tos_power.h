// Board power: TCA9554 expander (power latch pin 6, speaker PA pin 7), PWR/BOOT buttons (vendor
// button_bsp), battery ADC (vendor adc_bsp), backlight (vendor lcd_bl_pwm_bsp).
#ifndef TRANSLATEOS_POWER_H_
#define TRANSLATEOS_POWER_H_
#include <stdint.h>

void power_init();                 // expander first: keeps the board on when running from battery
void power_start_tasks();          // button handling task
int  power_battery_pct();
void power_set_backlight_level(int level);   // 1..10 (settings brightness)
void power_set_backlight_dim();              // screensaver
void power_set_pa(bool on);                  // speaker amplifier (TCA9554 pin 7): on only while playing
void app_on_power_click();         // implemented by the app: PWR single click
void app_before_power_off();       // implemented by the app: save state before the latch opens
#endif
