#pragma once
#include <stdint.h>

// Non-blocking active buzzer driver.
// Call hal_buzzer_tick() every loop iteration.
//
// hal_buzzer_beep(on_ms, off_ms, repeat):
//   repeat = -1 : infinite until hal_buzzer_stop()
//   repeat = N  : N on+off cycles, then idle
//   off_ms = 0  : single tone for on_ms (no gap between repeats)

void hal_buzzer_init(uint8_t pin);
void hal_buzzer_beep(uint32_t on_ms, uint32_t off_ms, int16_t repeat);
void hal_buzzer_stop();
void hal_buzzer_tick();
