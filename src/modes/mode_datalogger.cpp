#include "mode_datalogger.h"
#include "../hal/hal_sht31.h"
#include "../hal/hal_lcd.h"
#include "../hal/hal_dimmer.h"
#include "../hal/hal_door.h"
#include "../hal/hal_buzzer.h"
#include "../config/config.h"
#include "../comms/mqtt_client.h"
#include "../comms/ntp_sync.h"
#include <Arduino.h>
#include <math.h>

// ─── Sub-phase durations ────────────────────────────────────────────────────
#define PHASE_SWEEP_MS      (2UL * 3600000UL)   //  2 h
#define PHASE_PRBS_MS       (3UL * 3600000UL)   //  3 h
#define PHASE_DISTURB_MS    (1UL * 3600000UL)   //  1 h
#define PHASE_TRACKING_MS   (2UL * 3600000UL)   //  2 h
#define PHASE_TOTAL_MS      (8UL * 3600000UL)   //  8 h cycle

// ─── Steady-state sweep steps (7 steps × 20 min each = 2 h) ─────────────────
static const uint8_t SWEEP_LEVELS[] = {20, 40, 60, 80, 60, 40, 20};
#define SWEEP_STEP_MS  (20UL * 60000UL)

// ─── PRBS LFSR 8-bit ─────────────────────────────────────────────────────────
struct PRBSExcitation {
    uint8_t  lfsr          = 0xAC;
    uint8_t  current_pwm   = PRBS_PWM_LOW;
    uint32_t next_switch   = 0;

    uint8_t next_bit() {
        uint8_t bit = ((lfsr >> 7) ^ (lfsr >> 5) ^ (lfsr >> 4) ^ (lfsr >> 3)) & 1;
        lfsr = (lfsr << 1) | bit;
        return bit;
    }
    uint32_t random_period_ms() {
        uint16_t r = (lfsr * 17 + 43) % 271;
        return (30UL + r) * 1000UL;
    }
    uint8_t tick(uint32_t now) {
        if (now >= next_switch) {
            next_switch = now + random_period_ms();
            current_pwm = next_bit() ? PRBS_PWM_HIGH : PRBS_PWM_LOW;
        }
        return current_pwm;
    }
};

// ─── On-off setpoint tracking (for phase 4 sub-phase) ────────────────────────
static uint8_t _onoff_pwm(float temp) {
    static uint8_t s = 0;
    if (temp < SETPOINT_TEMP - ONOFF_HYSTERESIS) s = 100;
    else if (temp > SETPOINT_TEMP + ONOFF_HYSTERESIS) s = 0;
    return s;
}

// ─── State ───────────────────────────────────────────────────────────────────
static PRBSExcitation _prbs;
static uint32_t _phase_start = 0;
static uint32_t _last_ms     = 0;
static float    _temp        = 0.0f;
static float    _temp_ext    = 0.0f;
static float    _rh          = 0.0f;
static uint8_t  _pwm         = 0;
static uint32_t _sample_cnt  = 0;

enum SubPhase { PH_SWEEP=0, PH_PRBS, PH_DISTURB, PH_TRACKING };
static SubPhase _sub = PH_SWEEP;
static const char *_phase_names[] = {"SWEEP","PRBS","DISTURB","TRACK"};

// ─── Disturbance sequence ─────────────────────────────────────────────────────
// Within the 1h disturbance phase:
//  0–30s   : door open event (physical action by user); buzzer beeps 500/500
//  28–30s  : 2s warning beep (close door soon)
//  30s–2.5m: PWM = 0
//  2.5m–3.5m: PWM = 100 (spike)
//  3.5m–60m: back to PRBS_LOW
#define DISTURB_DOOR_WARN_MS  28000UL   // warn user 2 s before window closes
#define DISTURB_DOOR_END_MS   30000UL   // door must be closed by here

static uint8_t _disturb_pwm(uint32_t elapsed_ms) {
    if (elapsed_ms < 150000UL)   return 0;      // 2.5 min off
    if (elapsed_ms < 210000UL)   return 100;    // 1 min spike
    return PRBS_PWM_LOW;
}

// ─── DISTURB-phase buzzer state ───────────────────────────────────────────────
static bool _db_beeping = false;   // alternating beep active
static bool _db_warned  = false;   // 2 s warning already triggered

static void _update_disturb_buzzer(SubPhase cur_sub, uint32_t elapsed) {
    // Reset when outside DISTURB phase
    if (cur_sub != PH_DISTURB) {
        if (_db_beeping || _db_warned) {
            hal_buzzer_stop();
            _db_beeping = false;
            _db_warned  = false;
        }
        return;
    }

    uint32_t d = elapsed - PHASE_SWEEP_MS - PHASE_PRBS_MS;

    if (d < DISTURB_DOOR_WARN_MS) {
        if (!_db_beeping) {
            _db_beeping = true;
            hal_buzzer_beep(500, 500, -1);  // infinite 500/500 until transition
        }
    } else if (d < DISTURB_DOOR_END_MS) {
        if (!_db_warned) {
            _db_warned  = true;
            _db_beeping = false;
            hal_buzzer_beep(2000, 0, 1);    // single 2 s closing-warning beep
        }
    } else {
        if (_db_beeping) {
            hal_buzzer_stop();
            _db_beeping = false;
        }
        // _db_warned stays true; 2 s beep auto-ends via hal_buzzer_tick()
    }
}

static SubPhase _current_sub(uint32_t elapsed) {
    if (elapsed < PHASE_SWEEP_MS)                         return PH_SWEEP;
    if (elapsed < PHASE_SWEEP_MS + PHASE_PRBS_MS)         return PH_PRBS;
    if (elapsed < PHASE_SWEEP_MS + PHASE_PRBS_MS + PHASE_DISTURB_MS) return PH_DISTURB;
    return PH_TRACKING;
}

void mode_datalogger_init() {
    hal_dimmer_set(0);
    hal_buzzer_stop();
    _phase_start = millis();
    _last_ms     = millis();
    _sub         = PH_SWEEP;
    _pwm         = SWEEP_LEVELS[0];
    _sample_cnt  = 0;
    _db_beeping  = false;
    _db_warned   = false;
    _prbs.next_switch = millis();

    hal_lcd_mode_banner(0, "DATALOG");
    Serial.println(F("# Mode 0: Data Logger (PRBS Excitation)"));
    Serial.println(F("# timestamp_unix,temp_in_C,temp_ext_C,rh_pct,pwm,phase,door"));
}

void mode_datalogger_tick() {
    uint32_t now     = millis();
    uint32_t elapsed = (now - _phase_start) % PHASE_TOTAL_MS;

    // Buzzer state machine runs every call — not rate-limited
    _update_disturb_buzzer(_current_sub(elapsed), elapsed);

    if (now - _last_ms < SAMPLE_INTERVAL_MS) return;
    _last_ms = now;

    float t, h;
    if (!hal_sht31_read(&t, &h)) {
        Serial.println(F("# WARN: int sensor read fail"));
        return;
    }
    _temp = t;
    _rh   = h;

    float t_ext, h_ext;
    if (hal_sht31_read_ext(&t_ext, &h_ext)) _temp_ext = t_ext;

    _sub  = _current_sub(elapsed);

    switch (_sub) {
    case PH_SWEEP: {
        uint32_t step_idx = (elapsed / SWEEP_STEP_MS);
        if (step_idx >= 7) step_idx = 6;
        _pwm = SWEEP_LEVELS[step_idx];
        break;
    }
    case PH_PRBS:
        _pwm = _prbs.tick(now);
        break;
    case PH_DISTURB: {
        uint32_t d_elapsed = elapsed - PHASE_SWEEP_MS - PHASE_PRBS_MS;
        _pwm = _disturb_pwm(d_elapsed);
        break;
    }
    case PH_TRACKING:
        _pwm = _onoff_pwm(_temp);
        break;
    }

    hal_dimmer_set(_pwm);
    bool door = hal_door_is_open();

    uint32_t ts = ntp_synced() ? ntp_timestamp() : 0;

    // LCD: row0 = phase + sample count, row1 = t_in/t_ext + PWM
    {
        char row0[17], row1[17];
        snprintf(row0, sizeof(row0), "M0:%-6s #%05lu", _phase_names[_sub], _sample_cnt % 100000);
        snprintf(row1, sizeof(row1), "%.1f/%.1f P:%3d%%", _temp, _temp_ext, _pwm);
        hal_lcd_row(0, row0);
        hal_lcd_row(1, row1);
    }

    // Serial CSV: timestamp_unix,temp_in_C,temp_ext_C,rh_pct,pwm,phase,door
    Serial.print(ts);
    Serial.print(',');
    Serial.print(_temp, 2);
    Serial.print(',');
    Serial.print(_temp_ext, 2);
    Serial.print(',');
    Serial.print(_rh, 2);
    Serial.print(',');
    Serial.print(_pwm);
    Serial.print(',');
    Serial.print(_phase_names[_sub]);
    Serial.print(',');
    Serial.println(door ? 1 : 0);

    // MQTT publish JSON
    if (mqtt_is_connected()) {
        char payload[128];
        snprintf(payload, sizeof(payload),
            "{\"ts\":%lu,\"t_in\":%.2f,\"t_ext\":%.2f,\"rh\":%.1f,\"pwm\":%u,\"phase\":\"%s\",\"door\":%u}",
            ts, _temp, _temp_ext, _rh, _pwm, _phase_names[_sub], (uint8_t)door);
        mqtt_publish(TOPIC_TELEMETRI, payload);
    }

    _sample_cnt++;
}
