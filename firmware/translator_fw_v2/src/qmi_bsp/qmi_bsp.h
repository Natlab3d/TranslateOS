#pragma once

#include <Arduino.h>

void qmi_init(void);
void i2c_qmi_loop_task(void* arg);

extern volatile bool g_flipChanged;
extern volatile bool g_isFlipped;

