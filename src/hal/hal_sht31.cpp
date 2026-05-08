#include "hal_sht31.h"
#include <Arduino.h>
#include <Adafruit_SHT31.h>

static Adafruit_SHT31 _sht31;
static float _temp_offset = 0.0f;
static float _rh_offset   = 0.0f;

bool hal_sht31_init(uint8_t addr) {
    if (!_sht31.begin(addr)) return false;
    _sht31.heater(false);
    return true;
}

bool hal_sht31_read(float *temp, float *rh) {
    float t = _sht31.readTemperature();
    float h = _sht31.readHumidity();
    if (isnan(t) || isnan(h)) return false;
    *temp = t + _temp_offset;
    *rh   = h + _rh_offset;
    return true;
}

void hal_sht31_set_offset(float temp_offset, float rh_offset) {
    _temp_offset = temp_offset;
    _rh_offset   = rh_offset;
}

float hal_sht31_get_temp_offset() { return _temp_offset; }
float hal_sht31_get_rh_offset()   { return _rh_offset; }

// ─── External sensor ─────────────────────────────────────────────────────────
static Adafruit_SHT31 _sht31_ext;
static bool _ext_available = false;

bool hal_sht31_init_ext(uint8_t addr) {
    if (!_sht31_ext.begin(addr)) return false;
    _sht31_ext.heater(false);
    _ext_available = true;
    return true;
}

bool hal_sht31_ext_available() { return _ext_available; }

bool hal_sht31_read_ext(float *temp, float *rh) {
    if (!_ext_available) return false;
    float t = _sht31_ext.readTemperature();
    float h = _sht31_ext.readHumidity();
    if (isnan(t) || isnan(h)) return false;
    *temp = t;
    *rh   = h;
    return true;
}
