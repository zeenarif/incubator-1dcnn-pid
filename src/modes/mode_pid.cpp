#include "mode_pid.h"
#include "../hal/hal_sht31.h"
#include "../hal/hal_lcd.h"
#include "../hal/hal_dimmer.h"
#include "../config/config.h"
#include <Arduino.h>

static float    _kp       = PID_KP_DEFAULT;
static float    _ki       = PID_KI_DEFAULT;
static float    _kd       = PID_KD_DEFAULT;
static float    _integral = 0.0f;
static float    _prev_err = 0.0f;
static uint32_t _last_ms  = 0;
static float    _temp     = 0.0f;
static float    _rh       = 0.0f;
static uint8_t  _pwm      = 0;

void mode_pid_init() {
    hal_dimmer_set(0);
    _integral  = 0.0f;
    _prev_err  = 0.0f;
    _pwm       = 0;
    _last_ms   = millis();
    hal_lcd_mode_banner(2, "PID");

    Serial.println(F("# Mode 2: PID Control"));
    Serial.print(F("# Kp="));
    Serial.print(_kp); Serial.print(F(" Ki="));
    Serial.print(_ki); Serial.print(F(" Kd="));
    Serial.println(_kd);
    Serial.println(F("# ms,temp_C,rh_pct,pwm_pct,error,integral"));
}

void mode_pid_tick() {
    uint32_t now = millis();
    if (now - _last_ms < SAMPLE_INTERVAL_MS) return;
    float dt = (now - _last_ms) / 1000.0f;
    _last_ms = now;

    float t, h;
    if (!hal_sht31_read(&t, &h)) {
        Serial.println(F("# WARN: read fail"));
        return;
    }
    _temp = t;
    _rh   = h;

    float err = SETPOINT_TEMP - _temp;
    _integral += err * dt;
    _integral = constrain(_integral, -50.0f, 50.0f);  // anti-windup
    float derivative = (err - _prev_err) / dt;
    _prev_err = err;

    float output = _kp * err + _ki * _integral + _kd * derivative;
    _pwm = (uint8_t)constrain((int)output, 0, 100);
    hal_dimmer_set(_pwm);

    hal_lcd_show_pid(_temp, SETPOINT_TEMP, _kp, _pwm);

    Serial.print(now);
    Serial.print(',');
    Serial.print(_temp, 2);
    Serial.print(',');
    Serial.print(_rh, 2);
    Serial.print(',');
    Serial.print(_pwm);
    Serial.print(',');
    Serial.print(err, 3);
    Serial.print(',');
    Serial.println(_integral, 3);
}

void mode_pid_set_params(float kp, float ki, float kd) {
    _kp = kp;
    _ki = ki;
    _kd = kd;
    _integral = 0.0f;
    Serial.print(F("# PID params updated: Kp="));
    Serial.print(_kp); Serial.print(F(" Ki="));
    Serial.print(_ki); Serial.print(F(" Kd="));
    Serial.println(_kd);
}
