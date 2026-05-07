#pragma once
#include <stdint.h>

// AC phase-cut dimmer via zero-crossing interrupt + hardware timer.
// Duty cycle 0 = heater fully off, 100 = heater fully on.
void hal_dimmer_init(uint8_t pin_zc, uint8_t pin_pwm);
void hal_dimmer_set(uint8_t duty_pct);
uint8_t hal_dimmer_get();
