#include "hal_dimmer.h"
#include <Arduino.h>
#include "esp_idf_version.h"

// ── Version-aware ESP32 timer API ─────────────────────────────────────────────
// ESP32 Arduino 3.x (IDF 5.x) changed the timer API.
#if ESP_IDF_VERSION_MAJOR >= 5
#  define TIMER_NEW_API
#endif

static uint8_t           _pin_zc  = 0;
static uint8_t           _pin_pwm = 0;
static volatile uint8_t  _duty    = 0;
static hw_timer_t       *_timer   = nullptr;

static void IRAM_ATTR _on_timer() {
    gpio_set_level((gpio_num_t)_pin_pwm, 1);
    ets_delay_us(100);
    gpio_set_level((gpio_num_t)_pin_pwm, 0);
}

static void IRAM_ATTR _on_zero_cross() {
    if (_duty == 0) {
        gpio_set_level((gpio_num_t)_pin_pwm, 0);
        return;
    }
    if (_duty >= 100) {
        gpio_set_level((gpio_num_t)_pin_pwm, 1);
        ets_delay_us(100);
        gpio_set_level((gpio_num_t)_pin_pwm, 0);
        return;
    }
    // Firing delay: (100 - duty) * 100µs  (0–10 ms for 50 Hz)
    uint32_t delay_us = (uint32_t)(100 - _duty) * 100UL;

#ifdef TIMER_NEW_API
    timerStop(_timer);
    timerRestart(_timer);
    timerAlarm(_timer, delay_us, false, 0);
    timerStart(_timer);
#else
    timerRestart(_timer);
    timerAlarmWrite(_timer, delay_us, false);
    timerAlarmEnable(_timer);
#endif
}

void hal_dimmer_init(uint8_t pin_zc, uint8_t pin_pwm) {
    _pin_zc  = pin_zc;
    _pin_pwm = pin_pwm;

    gpio_reset_pin((gpio_num_t)pin_pwm);
    gpio_set_direction((gpio_num_t)pin_pwm, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)pin_pwm, 0);

#ifdef TIMER_NEW_API
    _timer = timerBegin(1000000);          // 1 MHz → 1 µs resolution
    timerAttachInterrupt(_timer, &_on_timer);
    timerAlarm(_timer, 9000, false, 0);    // initial dummy alarm
#else
    _timer = timerBegin(0, 80, true);      // timer 0, 80 MHz/80 = 1 MHz
    timerAttachInterrupt(_timer, &_on_timer, true);
    timerAlarmWrite(_timer, 9000, false);
#endif

    pinMode(pin_zc, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(pin_zc), _on_zero_cross, RISING);
}

void hal_dimmer_set(uint8_t duty_pct) {
    _duty = (duty_pct > 100) ? 100 : duty_pct;
    if (_duty == 0) gpio_set_level((gpio_num_t)_pin_pwm, 0);
}

uint8_t hal_dimmer_get() { return _duty; }
