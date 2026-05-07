#pragma once
#include <stdint.h>
#include <stdbool.h>

void ntp_init();
bool ntp_synced();
uint32_t ntp_timestamp();
