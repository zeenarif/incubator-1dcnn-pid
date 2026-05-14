#include "mode_onoff.h"
#include "../hal/hal_sht31.h"
#include "../hal/hal_lcd.h"
#include "../hal/hal_dimmer.h"
#include "../hal/hal_door.h"
#include "../config/config.h"
#include "../comms/mqtt_client.h"
#include "../comms/ntp_sync.h"
#include <Arduino.h>

static uint32_t _last_ms   = 0;
static float    _temp      = 0.0f;
static float    _temp_ext  = 0.0f;
static float    _rh        = 0.0f;
static uint8_t  _pwm       = 0;
static char     _session_id[16] = "";
static char     _scenario[24]   = "";

void mode_onoff_set_session(const char *session_id, const char *scenario) {
    strncpy(_session_id, session_id, sizeof(_session_id) - 1);
    strncpy(_scenario,   scenario,   sizeof(_scenario)   - 1);
}

void mode_onoff_init() {
    hal_dimmer_set(0);
    _pwm      = 0;
    _last_ms  = millis();
    hal_lcd_mode_banner(1, "ON-OFF");

    Serial.println(F("# Mode 1: On-Off Control"));
    Serial.println(F("# ts,temp_in_C,temp_ext_C,rh_pct,pwm_pct,door,ctrl_mode,session_id,scenario"));
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

    float t_ext, h_ext;
    if (hal_sht31_read_ext(&t_ext, &h_ext)) _temp_ext = t_ext;

    if (_temp < SETPOINT_TEMP - ONOFF_HYSTERESIS) {
        _pwm = 100;
    } else if (_temp > SETPOINT_TEMP + ONOFF_HYSTERESIS) {
        _pwm = 0;
    }
    hal_dimmer_set(_pwm);

    hal_lcd_show_sensor(_temp, _rh, _pwm);

    bool door = hal_door_is_open();
    uint32_t ts = ntp_synced() ? ntp_timestamp() : 0;

    Serial.print(ts); Serial.print(',');
    Serial.print(_temp, 2); Serial.print(',');
    Serial.print(_temp_ext, 2); Serial.print(',');
    Serial.print(_rh, 2); Serial.print(',');
    Serial.print(_pwm); Serial.print(',');
    Serial.print(door ? 1 : 0); Serial.print(',');
    Serial.print("onoff"); Serial.print(',');
    Serial.print(_session_id); Serial.print(',');
    Serial.println(_scenario);

    if (mqtt_is_connected()) {
        char payload[192];
        snprintf(payload, sizeof(payload),
            "{\"ts\":%lu,\"t_in\":%.2f,\"t_ext\":%.2f,\"rh\":%.1f"
            ",\"pwm\":%u,\"door\":%u,\"ctrl_mode\":\"onoff\""
            ",\"session_id\":\"%s\",\"scenario\":\"%s\"}",
            ts, _temp, _temp_ext, _rh,
            _pwm, (uint8_t)door,
            _session_id, _scenario);
        mqtt_publish(TOPIC_TELEMETRI, payload);
    }
}
