#include "mode_tinyml.h"
#include "../hal/hal_sht31.h"
#include "../hal/hal_lcd.h"
#include "../hal/hal_dimmer.h"
#include "../hal/hal_door.h"
#include "../config/config.h"
#include "../comms/mqtt_client.h"
#include "../comms/ntp_sync.h"
#include <Arduino.h>

// Sliding window: [CNN_WINDOW_SIZE][CNN_FEATURES]
// Features: [suhu_in, suhu_ext, pwm_aktif]
static float    _window[CNN_WINDOW_SIZE][CNN_FEATURES];
static uint8_t  _win_idx   = 0;
static bool     _win_full  = false;
static uint32_t _last_ms   = 0;
static float    _temp      = 0.0f;
static float    _temp_ext  = 0.0f;
static float    _rh        = 0.0f;
static uint8_t  _pwm       = 0;
static float    _t_pred    = 0.0f;
static char     _session_id[16] = "";
static char     _scenario[24]   = "";

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

void mode_tinyml_set_session(const char *session_id, const char *scenario) {
    strncpy(_session_id, session_id, sizeof(_session_id) - 1);
    strncpy(_scenario,   scenario,   sizeof(_scenario)   - 1);
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
    Serial.println(F("# ts,temp_in_C,temp_ext_C,rh_pct,t_pred,pwm_pct,door,ctrl_mode,session_id,scenario"));
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

    float t_ext, h_ext;
    if (hal_sht31_read_ext(&t_ext, &h_ext)) _temp_ext = t_ext;

    _window[_win_idx][0] = _temp;
    _window[_win_idx][1] = _temp_ext;
    _window[_win_idx][2] = (float)_pwm;
    _win_idx = (_win_idx + 1) % CNN_WINDOW_SIZE;
    if (_win_idx == 0) _win_full = true;

    _t_pred = _infer_stub();

    float error = SETPOINT_TEMP - _t_pred;
    float out   = CNN_KP * error;
    _pwm = (uint8_t)constrain((int)out, 0, 100);
    hal_dimmer_set(_pwm);

    hal_lcd_show_tinyml(_temp, _t_pred, _pwm);

    bool door = hal_door_is_open();
    uint32_t ts = ntp_synced() ? ntp_timestamp() : 0;

    Serial.print(ts); Serial.print(',');
    Serial.print(_temp, 2); Serial.print(',');
    Serial.print(_temp_ext, 2); Serial.print(',');
    Serial.print(_rh, 2); Serial.print(',');
    Serial.print(_t_pred, 3); Serial.print(',');
    Serial.print(_pwm); Serial.print(',');
    Serial.print(door ? 1 : 0); Serial.print(',');
    Serial.print("tinyml"); Serial.print(',');
    Serial.print(_session_id); Serial.print(',');
    Serial.println(_scenario);

    if (mqtt_is_connected()) {
        char payload[224];
        snprintf(payload, sizeof(payload),
            "{\"ts\":%lu,\"t_in\":%.2f,\"t_ext\":%.2f,\"rh\":%.1f"
            ",\"t_pred\":%.3f,\"pwm\":%u,\"door\":%u"
            ",\"ctrl_mode\":\"tinyml\""
            ",\"session_id\":\"%s\",\"scenario\":\"%s\"}",
            ts, _temp, _temp_ext, _rh,
            _t_pred, _pwm, (uint8_t)door,
            _session_id, _scenario);
        mqtt_publish(TOPIC_TELEMETRI, payload);
    }
}

bool mode_tinyml_model_ready() {
    return false;  // stub; set true once model.cc is integrated in Fase 4
}
