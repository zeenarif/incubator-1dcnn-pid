#include "mode_onoff.h"
#include "../hal/hal_sht31.h"
#include "../hal/hal_lcd.h"
#include "../hal/hal_dimmer.h"
#include "../config/config.h"
#include <Arduino.h>

static uint32_t _last_ms  = 0;
static float    _temp     = 0.0f;
static float    _rh       = 0.0f;
static uint8_t  _pwm      = 0;

void mode_onoff_init() {
    hal_dimmer_set(0);
    _pwm = 0;
    _last_ms = millis();
    hal_lcd_mode_banner(1, "ON-OFF");

    Serial.println(F("# Mode 1: On-Off Control"));
    Serial.println(F("# ms,temp_C,rh_pct,pwm_pct,state"));
}

void mode_onoff_tick() {
    uint32_t now = millis();
    if (now - _last_ms < SAMPLE_INTERVAL_MS) return;
    _last_ms = now;

    float t, h;
    if (!hal_sht31_read(&t, &h)) {
        Serial.println(F("# WARN: read fail"));
        return;
    }
    _temp = t;
    _rh   = h;

    // On-Off with hysteresis
    if (_temp < SETPOINT_TEMP - ONOFF_HYSTERESIS) {
        _pwm = 100;
    } else if (_temp > SETPOINT_TEMP + ONOFF_HYSTERESIS) {
        _pwm = 0;
    }
    hal_dimmer_set(_pwm);

    hal_lcd_show_sensor(_temp, _rh, _pwm);

    Serial.print(now);
    Serial.print(',');
    Serial.print(_temp, 2);
    Serial.print(',');
    Serial.print(_rh, 2);
    Serial.print(',');
    Serial.print(_pwm);
    Serial.print(',');
    Serial.println(_pwm > 0 ? "HEAT" : "IDLE");
}
