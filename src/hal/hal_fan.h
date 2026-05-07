#pragma once
#include <stdbool.h>

void hal_fan_init(int pin);
void hal_fan_set(bool on);
bool hal_fan_get();
