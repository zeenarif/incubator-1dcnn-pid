#include "hal_buzzer.h"
#include <Arduino.h>

enum BuzzState : uint8_t { BUZZ_IDLE, BUZZ_ON, BUZZ_OFF };

static uint8_t   _pin;
static BuzzState _state       = BUZZ_IDLE;
static uint32_t  _on_ms       = 0;
static uint32_t  _off_ms      = 0;
static int16_t   _cycles_left = 0;   // -1 = infinite
static uint32_t  _phase_start = 0;

void hal_buzzer_init(uint8_t pin) {
    _pin = pin;
    pinMode(pin, OUTPUT);
    digitalWrite(pin, LOW);
}

void hal_buzzer_beep(uint32_t on_ms, uint32_t off_ms, int16_t repeat) {
    _on_ms       = on_ms;
    _off_ms      = off_ms;
    _cycles_left = repeat;
    _state       = BUZZ_ON;
    _phase_start = millis();
    digitalWrite(_pin, HIGH);
}

void hal_buzzer_stop() {
    _state       = BUZZ_IDLE;
    _cycles_left = 0;
    digitalWrite(_pin, LOW);
}

void hal_buzzer_tick() {
    if (_state == BUZZ_IDLE) return;

    uint32_t now = millis();

    if (_state == BUZZ_ON) {
        if (now - _phase_start < _on_ms) return;
        digitalWrite(_pin, LOW);
        if (_off_ms == 0) {
            // No gap between cycles — decrement and restart immediately
            if (_cycles_left > 0 && --_cycles_left == 0) { _state = BUZZ_IDLE; return; }
            _phase_start = now;
            digitalWrite(_pin, HIGH);   // restart (only reaches here if infinite)
        } else {
            _state       = BUZZ_OFF;
            _phase_start = now;
        }
        return;
    }

    // BUZZ_OFF
    if (now - _phase_start < _off_ms) return;
    if (_cycles_left > 0 && --_cycles_left == 0) { _state = BUZZ_IDLE; return; }
    // More cycles (or infinite)
    digitalWrite(_pin, HIGH);
    _state       = BUZZ_ON;
    _phase_start = now;
}
