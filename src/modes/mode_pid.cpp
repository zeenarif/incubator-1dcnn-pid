#include "mode_pid.h"
#include "../hal/hal_sht31.h"
#include "../hal/hal_lcd.h"
#include "../hal/hal_dimmer.h"
#include "../hal/hal_door.h"
#include "../config/config.h"
#include "../config/nvs_manager.h"
#include "../comms/mqtt_client.h"
#include "../comms/ntp_sync.h"
#include <Arduino.h>

static float    _kp        = PID_KP_DEFAULT;
static float    _ki        = PID_KI_DEFAULT;
static float    _kd        = PID_KD_DEFAULT;
static float    _integral  = 0.0f;
static float    _prev_err  = 0.0f;
static uint32_t _last_ms   = 0;
static float    _temp      = 0.0f;
static float    _temp_ext  = 0.0f;
static float    _rh        = 0.0f;
static uint8_t  _pwm       = 0;
static char     _session_id[16] = "";
static char     _scenario[24]   = "";

void mode_pid_set_session(const char *session_id, const char *scenario) {
    strncpy(_session_id, session_id, sizeof(_session_id) - 1);
    strncpy(_scenario,   scenario,   sizeof(_scenario)   - 1);
}

void mode_pid_init() {
    nvs_load_pid(&_kp, &_ki, &_kd);
    hal_dimmer_set(0);
    _integral  = 0.0f;
    _prev_err  = 0.0f;
    _pwm       = 0;
    _last_ms   = millis();
    hal_lcd_mode_banner(2, "PID");

    Serial.println(F("# Mode 2: PID Control"));
    Serial.printf("# Kp=%.3f Ki=%.3f Kd=%.3f (loaded from NVS)\n", _kp, _ki, _kd);
    Serial.println(F("# ts,temp_in_C,temp_ext_C,rh_pct,pwm_pct,err,integral,door,ctrl_mode,session_id,scenario"));
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

    float t_ext, h_ext;
    if (hal_sht31_read_ext(&t_ext, &h_ext)) _temp_ext = t_ext;

    float err = SETPOINT_TEMP - _temp;
    _integral += err * dt;
    _integral  = constrain(_integral, -150.0f, 150.0f);  // anti-windup
    float derivative = (err - _prev_err) / dt;
    _prev_err = err;

    float output = _kp * err + _ki * _integral + _kd * derivative;
    _pwm = (uint8_t)constrain((int)output, 0, 100);
    hal_dimmer_set(_pwm);

    hal_lcd_show_pid(_temp, SETPOINT_TEMP, _kp, _pwm);

    bool door = hal_door_is_open();
    uint32_t ts = ntp_synced() ? ntp_timestamp() : 0;

    Serial.print(ts); Serial.print(',');
    Serial.print(_temp, 2); Serial.print(',');
    Serial.print(_temp_ext, 2); Serial.print(',');
    Serial.print(_rh, 2); Serial.print(',');
    Serial.print(_pwm); Serial.print(',');
    Serial.print(err, 3); Serial.print(',');
    Serial.print(_integral, 3); Serial.print(',');
    Serial.print(door ? 1 : 0); Serial.print(',');
    Serial.print("pid"); Serial.print(',');
    Serial.print(_session_id); Serial.print(',');
    Serial.println(_scenario);

    if (mqtt_is_connected()) {
        char payload[256];
        snprintf(payload, sizeof(payload),
            "{\"ts\":%lu,\"t_in\":%.2f,\"t_ext\":%.2f,\"rh\":%.1f"
            ",\"pwm\":%u,\"err\":%.3f,\"integral\":%.3f"
            ",\"kp\":%.3f,\"ki\":%.3f,\"kd\":%.3f"
            ",\"door\":%u,\"ctrl_mode\":\"pid\""
            ",\"session_id\":\"%s\",\"scenario\":\"%s\"}",
            ts, _temp, _temp_ext, _rh,
            _pwm, err, _integral,
            _kp, _ki, _kd,
            (uint8_t)door,
            _session_id, _scenario);
        mqtt_publish(TOPIC_TELEMETRI, payload);
    }
}

void mode_pid_set_params(float kp, float ki, float kd) {
    _kp       = kp;
    _ki       = ki;
    _kd       = kd;
    _integral = 0.0f;
    nvs_save_pid(_kp, _ki, _kd);
    Serial.printf("# PID params updated & saved: Kp=%.3f Ki=%.3f Kd=%.3f\n", _kp, _ki, _kd);
}
