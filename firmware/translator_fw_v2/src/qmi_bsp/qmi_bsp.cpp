#include "qmi_bsp.h"
#include "../../i2c_bsp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// QMI8658 register map
#define QMI_REG_WHO_AM_I    0x00
#define QMI_REG_CTRL1       0x02   // Serial interface, sensor enable
#define QMI_REG_CTRL2       0x03   // Accel config (range + ODR)
#define QMI_REG_CTRL3       0x04   // Gyro config
#define QMI_REG_CTRL7       0x08   // Enable accel/gyro
#define QMI_REG_STATUS0     0x2E   // Data ready flags (aDA = bit0)
#define QMI_REG_AX_L        0x35   // Accel X low byte (6 bytes: AX_L, AX_H, AY_L, AY_H, AZ_L, AZ_H)

// CTRL7: enable accel bit0=1
#define QMI_CTRL7_ACC_EN    0x01

// Accel sensitivity at 4G range: 8192 LSB/g → 1 LSB = 0.122 mg
#define QMI_ACC_SENSITIVITY_4G  (1.0f / 8192.0f * 1000.0f)  // mg per LSB

static bool g_qmi_ok = false;

static bool qmi_write(uint8_t reg, uint8_t val) {
    uint8_t buf = val;
    return i2c_write_buff(imu_dev_handle, reg, &buf, 1) == 0;
}

static bool qmi_read(uint8_t reg, uint8_t* buf, uint8_t len) {
    return i2c_read_buff(imu_dev_handle, reg, buf, len) == 0;
}

void qmi_init(void) {
    uint8_t who = 0;
    if (!qmi_read(QMI_REG_WHO_AM_I, &who, 1)) {
        Serial.println("[QMI] I2C read failed");
        return;
    }
    Serial.printf("[QMI] WHO_AM_I = 0x%02X (expect 0x05)\n", who);

    // CTRL1: auto increment on, little endian
    qmi_write(QMI_REG_CTRL1, 0x40);

    // CTRL2: accel 4G range (bits[5:4]=01) + 1000Hz ODR (bits[3:0]=3) = 0x13
    qmi_write(QMI_REG_CTRL2, 0x13);

    // CTRL7: enable accelerometer only
    qmi_write(QMI_REG_CTRL7, QMI_CTRL7_ACC_EN);

    g_qmi_ok = true;
    Serial.println("[QMI] Init OK");
}

// ─── Flip detection ───────────────────────────────────────────────────────────
volatile bool g_flipChanged    = false;
volatile bool g_isFlipped      = false;

static bool     g_flipState     = false;  // false = normal (ay ~+980), true = flipped (ay ~-980)
static uint32_t g_flipStableMs  = 0;
static bool     g_flipCandidate = false;
static bool     g_flipInitDone  = false;

static void check_flip(float ay) {
    bool candidate = (ay > 500.0f);    // flipped: tilted toward other person
    bool normal    = (ay < -500.0f);   // normal: gravity as expected
    uint32_t now   = millis();

    // On first reading, init state silently so we don't trigger a false event
    if (!g_flipInitDone) {
        if (candidate || normal) {
            g_flipState     = candidate;
            g_flipCandidate = candidate;
            g_flipStableMs  = now;
            g_flipInitDone  = true;
        }
        return;
    }

    if (candidate != g_flipCandidate) {
        if (candidate || normal) {
            g_flipCandidate = candidate;
            g_flipStableMs  = now;
        }
    } else if ((now - g_flipStableMs) > 500 && g_flipCandidate != g_flipState) {
        g_flipState   = g_flipCandidate;
        g_isFlipped   = g_flipState;
        g_flipChanged = true;
    }
}

// ─── Task ─────────────────────────────────────────────────────────────────────
void i2c_qmi_loop_task(void* arg) {
    for (;;) {
        if (!g_qmi_ok) { vTaskDelay(pdMS_TO_TICKS(1000)); continue; }

        uint8_t status = 0;
        qmi_read(QMI_REG_STATUS0, &status, 1);

        if (status & 0x01) {  // accel data ready
            uint8_t raw[6] = {0};
            if (qmi_read(QMI_REG_AX_L, raw, 6)) {
                int16_t rx = (int16_t)((raw[1] << 8) | raw[0]);
                int16_t ry = (int16_t)((raw[3] << 8) | raw[2]);
                int16_t rz = (int16_t)((raw[5] << 8) | raw[4]);
                float ax = rx * QMI_ACC_SENSITIVITY_4G;
                float ay = ry * QMI_ACC_SENSITIVITY_4G;
                float az = rz * QMI_ACC_SENSITIVITY_4G;

                check_flip(ay);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

