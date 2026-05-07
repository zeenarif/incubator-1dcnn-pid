#pragma once
#include <stdbool.h>

void hal_door_init(int pin);
bool hal_door_is_open();
