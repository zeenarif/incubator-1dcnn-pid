#include "mode_tinyml.h"
#include "../hal/hal_sht31.h"
#include "../hal/hal_lcd.h"
#include "../hal/hal_dimmer.h"
#include "../config/config.h"
#include <Arduino.h>

// Sliding window: [CNN_WINDOW_SIZE][CNN_FEATURES]
// Features: [suhu_in, suhu_ext(=0), pwm_aktif]
static float    _window[CNN_WINDOW_SIZE][CNN_FEATURES];
static uint8_t  _win_idx   = 0;
static bool     _win_full  = false;
static uint32_t _last_ms   = 0;
static float    _temp      = 0.0f;
static float    _rh        = 0.0f;
static uint8_t  _pwm       = 0;
static float    _t_pred    = 0.0f;

// Stub inference: identity (predict = current temp) until model.cc is integrated
static float _infer_stub() {
    if (!_win_full) return _temp;
    // Simple weighted average of last few samples as naive predictor
    float sum = 0.0f;
    int start = (_win_idx == 0) ? CNN_WINDOW_SIZE - 1 : _win_idx - 1;
    for (int i = 0; i < 5; i++) {
        int idx = (start - i + CNN_WINDOW_SIZE) % CNN_WINDOW_SIZE;
        sum += _window[idx][0];
    }
    return sum / 5.0f;
}

void mode_tinyml_init() {
    hal_dimmer_set(0);
    memset(_window, 0, sizeof(_window));
    _win_idx  = 0;
    _win_full = false;
    _pwm      = 0;
    _last_ms  = millis();
    hal_lcd_mode_banner(3, "1D-CNN");

    Serial.println(F("# Mode 3: TinyML 1D-CNN (STUB — model.cc not loaded)"));
    Serial.println(F("# ms,temp_C,rh_pct,t_pred,pwm_pct"));
}

void mode_tinyml_tick() {
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

    // Push to sliding window
    _window[_win_idx][0] = _temp;
    _window[_win_idx][1] = 0.0f;   // suhu_ext placeholder
    _window[_win_idx][2] = (float)_pwm;
    _win_idx = (_win_idx + 1) % CNN_WINDOW_SIZE;
    if (_win_idx == 0) _win_full = true;

    _t_pred = _infer_stub();

    float error = SETPOINT_TEMP - _t_pred;
    float out   = CNN_KP * error;
    _pwm = (uint8_t)constrain((int)out, 0, 100);
    hal_dimmer_set(_pwm);

    hal_lcd_show_tinyml(_temp, _t_pred, _pwm);

    Serial.print(now);
    Serial.print(',');
    Serial.print(_temp, 2);
    Serial.print(',');
    Serial.print(_rh, 2);
    Serial.print(',');
    Serial.print(_t_pred, 3);
    Serial.print(',');
    Serial.println(_pwm);
}

bool mode_tinyml_model_ready() {
    return false;  // stub; set true once model.cc is integrated in Fase 4
}
