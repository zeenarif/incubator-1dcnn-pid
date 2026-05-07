#include "hal_door.h"
#include <Arduino.h>

static int _pin = -1;

void hal_door_init(int pin) {
    _pin = pin;
    pinMode(pin, INPUT_PULLUP);
}

// LOW = door open (pulled low by magnetic switch)
bool hal_door_is_open() {
    if (_pin < 0) return false;
    return digitalRead(_pin) == LOW;
}
