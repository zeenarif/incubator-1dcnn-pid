#pragma once
#include <stdint.h>
#include <stdbool.h>

// Internal sensor (addr 0x44) — calibration offset applied
bool hal_sht31_init(uint8_t addr = 0x44);
bool hal_sht31_read(float *temp, float *rh);
void hal_sht31_set_offset(float temp_offset, float rh_offset);
float hal_sht31_get_temp_offset();
float hal_sht31_get_rh_offset();

// External sensor (addr 0x45) — raw reading, no offset
bool hal_sht31_init_ext(uint8_t addr = 0x45);
bool hal_sht31_ext_available();
bool hal_sht31_read_ext(float *temp, float *rh);
