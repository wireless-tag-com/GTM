/*
  * ESPRESSIF MIT License
  *
  * Copyright (c) 2017 <ESPRESSIF SYSTEMS (SHANGHAI) PTE LTD>
  *
  * Permission is hereby granted for use on ESPRESSIF SYSTEMS products only, in which case,
  * it is free of charge, to any person obtaining a copy of this software and associated
  * documentation files (the "Software"), to deal in the Software without restriction, including
  * without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense,
  * and/or sell copies of the Software, and to permit persons to whom the Software is furnished
  * to do so, subject to the following conditions:
  *
  * The above copyright notice and this permission notice shall be included in all copies or
  * substantial portions of the Software.
  *
  * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
  * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
  * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
  * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
  * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
  *
  */

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "driver/i2c.h"
#include "i2c_device.h"
#include "i2c_bus.h"
#include "audio_mutex.h"
#include "audio_mem.h"

#define ESP_INTR_FLG_DEFAULT  (0)
#define ESP_I2C_MASTER_BUF_LEN  (0)
#define I2C_ACK_CHECK_EN 1

#define I2C_BUS_CHECK(a, str, ret)  if(!(a)) {                               \
    ESP_LOGE(TAG, "%s:%d (%s):%s", __FILE__, __LINE__, __FUNCTION__, str);   \
    return (ret);                                                            \
}

typedef struct {
    i2c_config_t     i2c_conf;   /*!<I2C bus parameters*/
    i2c_port_t       i2c_port;   /*!<I2C port number */
    int              ref_count;  /*!<Reference Count for multiple client */
    xSemaphoreHandle bus_lock;   /*!<Lock for bus */
} i2c_bus_t;

static const char *TAG = "I2C_BUS";

i2c_bus_handle_t i2c_bus_create(i2c_port_t port, i2c_config_t *conf)
{
    return i2c_malloc_device(I2C_NUM_0, conf->sda_io_num, conf->scl_io_num, 100000, 0x38);
}

esp_err_t i2c_bus_write_bytes(i2c_bus_handle_t bus, int addr, uint8_t *reg, int regLen, uint8_t *data, int datalen)
{
    if (regLen != 1) {
        ESP_LOGE(TAG, "Not support regLen > 1");
        assert(0);
    }

    i2c_device_change_addr(bus, addr >> 1);
    return i2c_write_bytes(bus, reg[0], data, datalen);
}

esp_err_t i2c_bus_write_data(i2c_bus_handle_t bus, int addr, uint8_t *data, int datalen)
{
    ESP_LOGE(TAG, "Not support");
    assert(0);
    return ESP_FAIL;
}

esp_err_t i2c_bus_read_bytes(i2c_bus_handle_t bus, int addr, uint8_t *reg, int reglen, uint8_t *outdata, int datalen)
{
    if (reglen != 1) {
        ESP_LOGE(TAG, "Not support regLen > 1");
        assert(0);
    }

    i2c_device_change_addr(bus, addr >> 1);
    return i2c_read_bytes(bus, reg[0], outdata, datalen);
}

esp_err_t i2c_bus_delete(i2c_bus_handle_t bus)
{
    return ESP_OK;
}

esp_err_t i2c_bus_cmd_begin(i2c_bus_handle_t bus, i2c_cmd_handle_t cmd, portBASE_TYPE ticks_to_wait)
{
    ESP_LOGE(TAG, "Not support");
    assert(0);
    return ESP_FAIL;
}

esp_err_t i2c_bus_probe_addr(i2c_bus_handle_t bus, uint8_t addr)
{
    i2c_device_change_addr(bus, addr >> 1);
    return i2c_device_valid(bus);
}
