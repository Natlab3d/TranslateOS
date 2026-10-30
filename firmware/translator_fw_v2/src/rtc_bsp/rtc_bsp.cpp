#include <stdio.h>
#include "rtc_bsp.h"
#include "../../i2c_bsp.h"

// PCF85063 registers: 0x04 sec (bit7 = oscillator-stopped flag), 0x05 min, 0x06 hour, 0x07 day,
// 0x08 weekday, 0x09 month, 0x0A year (00-99). All BCD.
#define RTC_REG_SEC 0x04

static bool s_ok = false;

static inline uint8_t bcd2bin(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
static inline uint8_t bin2bcd(uint8_t v) { return ((v / 10) << 4) | (v % 10); }

bool rtc_init(void)
{
  uint8_t b[7];
  s_ok = (rtc_dev_handle != NULL) && (i2c_read_buff(rtc_dev_handle, RTC_REG_SEC, b, 7) == 0);
  Serial.printf("[RTC] PCF85063 %s\n", s_ok ? "found" : "NOT answering (continuing without RTC)");
  return s_ok;
}

void i2c_rtc_setTime(uint16_t year,uint8_t month,uint8_t day,uint8_t hour,uint8_t minute,uint8_t second)
{
  if (!s_ok) return;
  // weekday 0-6 via Sakamoto's method
  static const int t[] = {0,3,2,5,0,3,5,1,4,6,2,4};
  int y = year - (month < 3);
  uint8_t wd = (y + y/4 - y/100 + y/400 + t[month-1] + day) % 7;
  uint8_t b[7] = { bin2bcd(second), bin2bcd(minute), bin2bcd(hour), bin2bcd(day), wd, bin2bcd(month), bin2bcd((uint8_t)(year % 100)) };
  i2c_write_buff(rtc_dev_handle, RTC_REG_SEC, b, 7);       // clears the oscillator-stopped flag too
}

RtcDateTime_t i2c_rtc_get(void)
{
  RtcDateTime_t r = {};
  uint8_t b[7];
  if (!s_ok || i2c_read_buff(rtc_dev_handle, RTC_REG_SEC, b, 7) != 0) return r;
  if (b[0] & 0x80) return r;                                // oscillator stopped: time not valid
  r.second = bcd2bin(b[0] & 0x7F); r.minute = bcd2bin(b[1] & 0x7F); r.hour = bcd2bin(b[2] & 0x3F);
  r.day = bcd2bin(b[3] & 0x3F);    r.week = b[4] & 0x07;
  r.month = bcd2bin(b[5] & 0x1F);  r.year = 2000 + bcd2bin(b[6]);
  return r;
}
