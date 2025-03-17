#pragma once

#include "stdint.h"

#ifdef __cplusplus
extern "C" {
#endif

void mktp2016_init(uint8_t sda, uint8_t scl);

void mktp2016_read_value(uint32_t *proximity);

#ifdef __cplusplus
}
#endif
