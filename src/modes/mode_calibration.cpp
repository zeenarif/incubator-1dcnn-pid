#include "mode_calibration.h"
#include "../hal/hal_sht31.h"
#include "../hal/hal_lcd.h"
#include "../hal/hal_dimmer.h"
#include "../config/config.h"
#include "../config/nvs_manager.h"
#include <Arduino.h>
#include <math.h>

static float  _buf[CALIB_WINDOW];
static uint8_t _idx      = 0;
static bool    _full     = false;
static float   _cur_temp = 0.0f;
static float   _cur_rh   = 0.0f;
static float   _std_dev  = 99.0f;
static bool    _stable   = false;
static uint32_t _last_ms = 0;
static uint32_t _sample_cnt = 0;

static float _calc_mean() {
    uint8_t n = _full ? CALIB_WINDOW : _idx;
    if (n == 0) return 0.0f;
    float s = 0.0f;
    for (uint8_t i = 0; i < n; i++) s += _buf[i];
    return s / n;
}

static float _calc_std(float mean) {
    uint8_t n = _full ? CALIB_WINDOW : _idx;
    if (n < 2) return 99.0f;
    float sq = 0.0f;
    for (uint8_t i = 0; i < n; i++) {
        float d = _buf[i] - mean;
        sq += d * d;
    }
    return sqrtf(sq / (n - 1));
}

void mode_calibration_init() {
    hal_dimmer_set(0);
    hal_lcd_mode_banner(4, "CALIBRATE");
    memset(_buf, 0, sizeof(_buf));
    _idx = 0; _full = false; _stable = false;
    _last_ms = millis();
    _sample_cnt = 0;

    Serial.println(F("# Mode 4: Calibration"));
    Serial.println(F("# ms,temp_C,rh_pct,stable,std_dev"));
}

void mode_calibration_tick() {
    uint32_t now = millis();
    if (now - _last_ms < CALIB_SAMPLE_MS) return;
    _last_ms = now;

    float t, h;
    if (!hal_sht31_read(&t, &h)) {
        Serial.println(F("# WARN: SHT31 read fail"));
        return;
    }
    _cur_temp = t;
    _cur_rh   = h;
    _sample_cnt++;

    _buf[_idx] = t;
    _idx = (_idx + 1) % CALIB_WINDOW;
    if (_idx == 0) _full = true;

    uint8_t n = _full ? CALIB_WINDOW : _idx;
    if (n >= CALIB_WINDOW) {
        float mean = _calc_mean();
        _std_dev = _calc_std(mean);
        _stable  = (_std_dev < CALIB_STABLE_THRES);
    } else {
        _std_dev = 99.0f;
        _stable  = false;
    }

    hal_lcd_show_calib(_cur_temp, _stable, _std_dev);

    // CSV output
    Serial.print(now);
    Serial.print(',');
    Serial.print(_cur_temp, 3);
    Serial.print(',');
    Serial.print(_cur_rh, 2);
    Serial.print(',');
    Serial.print(_stable ? 1 : 0);
    Serial.print(',');
    Serial.println(_std_dev, 3);

    if (_stable && (_sample_cnt % 5 == 0)) {
        Serial.println(F("# *** STABIL — catat T_SHT31 vs T_referensi ***"));
        Serial.print(F("#   T_SHT31 = "));
        Serial.print(_cur_temp, 3);
        Serial.println(F(" C  →  offset = T_ref - T_SHT31"));
    }
}
