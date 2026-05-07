#include "hal_fan.h"
#include <Arduino.h>

static int  _pin    = -1;
static bool _state  = false;

void hal_fan_init(int pin) {
    _pin = pin;
    pinMode(pin, OUTPUT);
    digitalWrite(pin, LOW);
    _state = false;
}

void hal_fan_set(bool on) {
    if (_pin < 0) return;
    _state = on;
    digitalWrite(_pin, on ? HIGH : LOW);
}

bool hal_fan_get() { return _state; }
