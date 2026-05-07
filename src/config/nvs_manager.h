#pragma once
#include <stdint.h>

void nvs_init();
void nvs_save_mode(uint8_t mode);
uint8_t nvs_load_mode();
void nvs_save_temp_offset(float offset);
float nvs_load_temp_offset();
void nvs_save_rh_offset(float offset);
float nvs_load_rh_offset();
