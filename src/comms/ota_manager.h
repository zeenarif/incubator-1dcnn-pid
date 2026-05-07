#pragma once
#include <stdbool.h>

void ota_init();
bool ota_download_model(const char *url);
bool ota_model_available();
