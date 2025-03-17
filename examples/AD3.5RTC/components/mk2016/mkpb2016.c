#include "mkpb2016.h"
#include "i2c_device.h"

static uint32_t mkpb2016_proximity = 0;

static I2CDevice_t mktp2016_device;

void mktp2016_init(uint8_t sda, uint8_t scl) {
    mktp2016_device = i2c_malloc_device(I2C_NUM_0, sda, scl, 400000, 0x39);
}

void mktp2016_read_value(uint32_t *proximity) {
    uint8_t data[6] = {0x00};
    uint8_t prox_low = 0; 
    uint8_t prox_high = 0;
    i2c_read_bytes(mktp2016_device, 0x18, data, 1);
    prox_low = data[0];

    i2c_read_bytes(mktp2016_device, 0x19, data, 1);
    prox_high = data[0];

    *proximity = (prox_high << 8) | prox_low;
    mkpb2016_proximity = *proximity;
}
