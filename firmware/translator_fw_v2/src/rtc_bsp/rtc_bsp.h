// TranslateOS: PCF85063 RTC on the vendor I2C bus (rtc_dev_handle from i2c_bsp.c).
// The vendor 02_I2C_PCF85063 example uses SensorLib + Wire, which opens a second I2C driver on the same
// pins and blocks forever when the bus is already owned; this version reuses the existing device handle.
#ifndef RTC_BSP_H
#define RTC_BSP_H

#include <Arduino.h>

typedef struct
{
  uint16_t year;
  uint8_t month;
  uint8_t day;
  uint8_t hour;
  uint8_t minute;
  uint8_t second;
  uint8_t week;
}RtcDateTime_t;

bool rtc_init(void);                 // false if the chip does not answer; never blocks
void i2c_rtc_setTime(uint16_t year,uint8_t month,uint8_t day,uint8_t hour,uint8_t minute,uint8_t second);
RtcDateTime_t i2c_rtc_get(void);     // year 0 if the chip is missing or its time is not valid

#endif
