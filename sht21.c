/*
 * Driver for the SHT2x temperature and humidity sensor.
 *
 * Licensed under the Apache License, Version 2.0, January 2004 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *      http://www.apache.org/licenses/
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE CONTRIBUTORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS WITH THE SOFTWARE.
 *
 */

#include <stdint.h>
#include <sys/types.h>
#include <stdlib.h>
#include <unistd.h>
#include <esp/uart.h>
#include <stdio.h>
#include "FreeRTOS.h"
#include "task.h"
#include "i2c/i2c.h"
#include "bmp180/bmp180.h"

#include <espressif/esp_misc.h>
#include "espressif/esp8266/gpio_register.h"

#include "buffer.h"
#include "leds.h"



/* Sensor commands */
typedef enum
{
    TRIG_T_MEASUREMENT_HM    = 0xE3, /* Trigger temp measurement - hold master. */
    TRIG_RH_MEASUREMENT_HM   = 0xE5, /* Trigger humidity measurement - hold master. */
    TRIG_T_MEASUREMENT_POLL  = 0xF3, /* Trigger temp measurement - no hold master. */
    TRIG_RH_MEASUREMENT_POLL = 0xF5, /* Trigger humidity measurement - no hold master. */
    USER_REG_W               = 0xE6, /* Writing user register. */
    USER_REG_R               = 0xE7, /* Reading user register. */
    SOFT_RESET               = 0xFE  /* Soft reset. */
} sht2x_command;

typedef enum
{
    SHT2x_RES_12_14BIT       = 0x00, // RH=12bit, T=14bit
    SHT2x_RES_8_12BIT        = 0x01, // RH= 8bit, T=12bit
    SHT2x_RES_10_13BIT       = 0x80, // RH=10bit, T=13bit
    SHT2x_RES_11_11BIT       = 0x81, // RH=11bit, T=11bit
    SHT2x_RES_MASK           = 0x81  // Mask for res. bits (7,0) in user reg.
} sht2x_resolution;

typedef enum
{
    SHT2x_EOB_ON             = 0x40, // end of battery
    SHT2x_EOB_MASK           = 0x40, // Mask for EOB bit(6) in user reg.
} sht2x_eob;

typedef enum
{
    I2C_ADR_W                = 128,   /* Sensor I2C address + write bit. */
    I2C_ADR_R                = 129    /* Sensor I2C address + read bit. */
} sht2x_addr;


#define SCL_PIN GPIO_ID_PIN((0))      /* Nodemcu pin D3 */
#define SDA_PIN GPIO_ID_PIN((2))      /* Nodemcu pin D4 */

static bool sht2x_check_crc(uint8_t data[], uint8_t num_bytes, uint8_t checksum)
{
    uint8_t crc = 0;
    uint8_t i;

    for (i = 0; i < num_bytes; ++i) {
        crc ^= (data[i]);
        for (uint8_t bit = 8; bit > 0; --bit) {
            if (crc & 0x80)
                crc = (crc << 1) ^ 0x131;
            else
                crc = (crc << 1);
        }
    }
    return crc == checksum;
}

static bool sht2x_read_user_register(uint8_t *value)
{
    i2c_start();

    if (!i2c_write(I2C_ADR_W) || !i2c_write(USER_REG_R)) {
        i2c_stop();
        return false;
    }

    i2c_start();

    if (!i2c_write(I2C_ADR_R)) {
        i2c_stop();
        return false;
    }

    *value = i2c_read(0);
    uint8_t crc = i2c_read(1);
    i2c_stop();
    return sht2x_check_crc(value, 1, crc);
}

static bool sht2x_write_user_register(uint8_t value)
{
    i2c_start();

    bool result = i2c_write(I2C_ADR_W) &&
        i2c_write(USER_REG_W) &&
        i2c_write(value);

    i2c_stop();
    return result;
}

static bool sht2x_soft_reset()
{
    i2c_start();

    bool result = i2c_write(I2C_ADR_W) && i2c_write(SOFT_RESET);
    i2c_stop();
    /* Wait until sensor restarted. */
    sdk_os_delay_us(15000);

    return result;
}

static bool sht2x_get_serial_number(uint8_t *serial_number)
{
    i2c_start();
    if (!i2c_write(I2C_ADR_W) ||
        !i2c_write(0xFA) ||
        !i2c_write(0x0F))
        goto fail;
    i2c_start();
    if (!i2c_write(I2C_ADR_R))
        goto fail;
    serial_number[5] = i2c_read(0);
    if (!sht2x_check_crc(&serial_number[5], 1, i2c_read(0)))
        goto fail;
    serial_number[4] = i2c_read(0);
    if (!sht2x_check_crc(&serial_number[4], 1, i2c_read(0)))
        goto fail;
    serial_number[3] = i2c_read(0);
    if (!sht2x_check_crc(&serial_number[3], 1, i2c_read(0)))
        goto fail;
    serial_number[2] = i2c_read(0);
    if (!sht2x_check_crc(&serial_number[2], 1, i2c_read(1)))
        goto fail;
    i2c_stop();

    i2c_start();
    if (!i2c_write(I2C_ADR_W) ||
        !i2c_write(0xFC) ||
        !i2c_write(0xC9))
        goto fail;
    i2c_start();
    if (!i2c_write(I2C_ADR_R))
        goto fail;
    uint8_t data[2];
    data[0] = i2c_read(0);
    data[1] = i2c_read(0);
    if (!sht2x_check_crc(data, 2, i2c_read(0)))
        goto fail;
    serial_number[1] = data[0];
    serial_number[0] = data[1];
    data[0] = i2c_read(0);
    data[1] = i2c_read(0);
    if (!sht2x_check_crc(data, 2, i2c_read(1)))
        goto fail;
    serial_number[7] = data[0];
    serial_number[6] = data[1];
    i2c_stop();

    return true;

 fail:
    i2c_stop();
    return false;
}

/*
 * Measure the temperature if temp_rh is 0 and the relative humidity
 * if temp_rh is 1. Return 0 on success and 1 if there is an error.
 */
static uint8_t sht2x_measure_poll(int temp_rh, uint8_t data[], uint8_t *crc)
{
    i2c_start();
    if (!i2c_write(I2C_ADR_W) ||
        !i2c_write(temp_rh ? TRIG_RH_MEASUREMENT_POLL : TRIG_T_MEASUREMENT_POLL)) {
        i2c_stop();
        return 1;
    }

    int i = 0;
    int res;
    do {
        i2c_start();
        sdk_os_delay_us(10000);
        res = i2c_write(I2C_ADR_R);
        if (i++ >= 20) {
            i2c_stop();
            return 1;
        }
    } while (res == 0);

    data[0] = i2c_read(0);
    data[1] = i2c_read(0);
    *crc = i2c_read(1);
    i2c_stop();
    return sht2x_check_crc(data, 2, *crc);
}

/* LEB-128 signed encoding. */
static uint8_t encode_leb128_signed(int32_t n, uint8_t *buf)
{
    uint8_t len = 0;
    while (1) {
        if (-0x40 <= n && n <= 0x3f) {
            buf[len++] = n & 0x7f;
            return len;
        }
        buf[len++] = (n & 0x7f) | 0x80;
        n >>= 7;
    }
}



static void sht2x_read_task(void *pvParameters)
{
    /* Delta encoding state. */
    uint32_t last_index = 0;
    uint16_t last_temp = 0;
    uint16_t last_rh = 0;

    /* Reset the sensor and try reading the serial number to try
     * detecting the sensor. */
    uint8_t sht2x_serial_number[8];
    bool sht2x_available = sht2x_soft_reset() &&
        sht2x_get_serial_number(sht2x_serial_number);

    /* Set resolution to 12 bit temperature and 14 bit relative humidity. */
    uint8_t user_reg;
    if (sht2x_read_user_register(&user_reg)) {
        user_reg = (user_reg & ~SHT2x_RES_MASK) | SHT2x_RES_12_14BIT;
        if (!sht2x_write_user_register(user_reg))
            sht2x_available = false;
    } else {
        sht2x_available = false;
    }

    bmp180_constants_t bmp180_constants;
    bool bmp180_available = bmp180_is_available() &&
        bmp180_fillInternalConstants(&bmp180_constants);
    uint32_t last_bmp180_index = 0;
    uint32_t last_bmp180_temp = 0;
    uint32_t last_bmp180_pressure = 0;

    if (!sht2x_available && !bmp180_available)
        vTaskDelete(NULL);

    for (;;) {
        vTaskDelay(10000 / portTICK_RATE_MS);

        if (sht2x_available) {
            uint8_t data[4];

            uint8_t temp_crc;
            if (!sht2x_measure_poll(0, data, &temp_crc)) {
                blink_red();
                continue;
            }

            uint16_t temp = ((uint16_t) data[0]) << 8 | data[1];
            temp >>= 2; /* Strip the two low status bits */

            uint8_t rh_crc;
            if (!sht2x_measure_poll(1, &data[2], &rh_crc)) {
                blink_red();
                continue;
            }

            uint16_t rh = ((uint16_t) data[2]) << 8 | data[3];
            rh >>= 2; /* Strip the two low status bits */

            while (1) {
                uint8_t outbuf[8];
                /* Delta encoding */
                int32_t temp_delta = (int32_t)temp - (int32_t)last_temp;
                uint8_t len = encode_leb128_signed(temp_delta, outbuf);
                int32_t rh_delta = (int32_t)rh - (int32_t)last_rh;
                len += encode_leb128_signed(rh_delta, &outbuf[len]);
                /* Include the xor of both crcs */
                outbuf[len++] = temp_crc ^ rh_crc;
                int32_t code = DBUF_EVENT_SHT2X_TEMP_HUM;
                uint32_t new_index = dbuf_append(last_index, code, outbuf, len, 1, 0);
                if (new_index == last_index)
                    break;

                /* Moved on to a new buffer. Reset the delta encoding
                 * state and retry. */
                last_index = new_index;
                last_temp = 0;
                last_rh = 0;
            };

            blink_green();

            /* Commit the values logged. Note this is the only task
             * accessing this state so these updates are synchronized
             * with the last event of this class append. */
            last_temp = temp;
            last_rh = rh;
        }

        /* Hack in the BMP180 support here as the I2C driver is not
         * multi-task safe. */

        if (bmp180_available) {
            int32_t temperature;
            uint32_t pressure;
            if (bmp180_measure(&bmp180_constants, &temperature, &pressure, 3)) {
                while (1) {
                    uint8_t outbuf[12];
                    /* Delta encoding */
                    int32_t temp_delta = (int32_t)temperature - (int32_t)last_bmp180_temp;
                    uint8_t len = encode_leb128_signed(temp_delta, outbuf);
                    int32_t pressure_delta = (int32_t)pressure - (int32_t)last_bmp180_pressure;
                    len += encode_leb128_signed(pressure_delta, &outbuf[len]);
                    int32_t code = DBUF_EVENT_BMP180_TEMP_PRESSURE;
                    uint32_t new_index = dbuf_append(last_bmp180_index, code, outbuf, len, 1, 0);
                    if (new_index == last_bmp180_index)
                        break;

                    /* Moved on to a new buffer. Reset the delta encoding
                     * state and retry. */
                    last_bmp180_index = new_index;
                    last_bmp180_temp = 0;
                    last_bmp180_pressure = 0;
                };

                blink_green();

                /* Commit the values logged. Note this is the only task
                 * accessing this state so these updates are synchronized
                 * with the last event of this class append. */
                last_bmp180_temp = temperature;
                last_bmp180_pressure = pressure;
            }
        }
    }
}




void init_sht2x()
{
    i2c_init(SCL_PIN, SDA_PIN);

    xTaskCreate(&sht2x_read_task, (signed char *)"sht2x_read_task", 256, NULL, 2, NULL);
}