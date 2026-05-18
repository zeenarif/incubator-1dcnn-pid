#include "mode_coldstart.h"
#include "../hal/hal_sht31.h"
#include "../hal/hal_lcd.h"
#include "../hal/hal_dimmer.h"
#include "../hal/hal_door.h"
#include "../hal/hal_buzzer.h"
#include "../config/config.h"
#include "../comms/mqtt_client.h"
#include "../comms/ntp_sync.h"
#include <Arduino.h>

// ── Konfigurasi sesi cold start ───────────────────────────────────────────
// Urutan: mulai dari suhu TERTINGGI ke terendah
// t_target : T_in harus ≤ nilai ini sebelum sesi dimulai
// pwm      : PWM tetap selama sesi logging (sesuai rekomendasi ML Engineer)
// t_stop   : sesi selesai saat T_in ≥ nilai ini (inkubator sudah panas)

struct ColdSession {
    float       t_target;   // °C — tunggu hingga T_in ≤ ini
    uint8_t     pwm;        // % — PWM tetap selama logging
    float       t_stop;     // °C — berhenti logging saat T_in ≥ ini
    const char *id;         // label untuk MQTT session_id
};

static const ColdSession SESSIONS[] = {
    {36.0f, 40, 38.5f, "cs_s1"},   // Sesi 1: ~35°C, PWM=40%  (~8  mnt)
    {33.0f, 40, 38.5f, "cs_s2"},   // Sesi 2: ~32°C, PWM=40%  (~15 mnt)
    {31.0f, 60, 38.5f, "cs_s3"},   // Sesi 3: ~30°C, PWM=60%  (~10 mnt)
    {29.0f, 40, 38.5f, "cs_s4"},   // Sesi 4: ~28°C, PWM=40%  (~20 mnt)
    {26.0f, 40, 38.5f, "cs_s5"},   // Sesi 5: ~25°C, PWM=40%  (~25 mnt)
};
static constexpr uint8_t NUM_SESSIONS = sizeof(SESSIONS) / sizeof(SESSIONS[0]);

// ── State machine ─────────────────────────────────────────────────────────
enum ColdState {
    CL_WAITING,   // Menunggu T_in turun ke target sesi
    CL_LOGGING,   // Logging aktif, heater fixed PWM
    CL_COOLING,   // Sesi selesai, mendinginkan ke target sesi berikutnya
    CL_READY,     // Sudah di suhu target, tunggu pintu ditutup
    CL_DONE       // Semua sesi selesai
};

static ColdState _state    = CL_WAITING;
static uint8_t   _sess     = 0;
static uint32_t  _last_ms  = 0;
static uint32_t  _log_cnt  = 0;
static float     _temp     = 0.0f;
static float     _temp_ext = 0.0f;
static float     _rh       = 0.0f;
static bool      _prev_door_open = false;

// ── Helpers ───────────────────────────────────────────────────────────────
static void _lcd_update(bool door_open) {
    char row0[17], row1[17];
    const char *st =
        _state == CL_WAITING ? "TUNGGU" :
        _state == CL_LOGGING ? (door_open ? "PAUSE " : "LOG   ") :
        _state == CL_COOLING ? "DINGIN" :
        _state == CL_READY   ? "TUTUP!" : "SELESAI";

    snprintf(row0, sizeof(row0), "CS S%u/5 %s", _sess + 1, st);

    if (_state == CL_COOLING && _sess + 1 < NUM_SESSIONS) {
        snprintf(row1, sizeof(row1), "Tuju%.0f T=%.1f",
                 SESSIONS[_sess + 1].t_target, _temp);
    } else if (_state == CL_WAITING) {
        snprintf(row1, sizeof(row1), "T=%.1f>%.0f",
                 _temp, SESSIONS[_sess].t_target);
    } else {
        uint8_t p = (_state == CL_LOGGING && !door_open) ? SESSIONS[_sess].pwm : 0;
        snprintf(row1, sizeof(row1), "T=%.1f P=%u%% #%lu",
                 _temp, p, _log_cnt);
    }
    hal_lcd_row(0, row0);
    hal_lcd_row(1, row1);
}

static void _publish(bool door_open) {
    if (!mqtt_is_connected()) return;
    uint32_t ts  = ntp_synced() ? ntp_timestamp() : 0;
    uint8_t  pwm = (_state == CL_LOGGING && !door_open) ? SESSIONS[_sess].pwm : 0;
    char payload[200];
    snprintf(payload, sizeof(payload),
        "{\"ts\":%lu,\"t_in\":%.2f,\"t_ext\":%.2f,\"rh\":%.1f"
        ",\"pwm\":%u,\"door\":%u,\"ctrl_mode\":\"coldstart\""
        ",\"session_id\":\"%s\",\"scenario\":\"cold_start\"}",
        ts, _temp, _temp_ext, _rh, pwm, (uint8_t)door_open,
        _sess < NUM_SESSIONS ? SESSIONS[_sess].id : "done");
    mqtt_publish(TOPIC_TELEMETRI, payload);
}

static void _serial_csv(bool door_open) {
    uint8_t pwm = (_state == CL_LOGGING && !door_open) ? SESSIONS[_sess].pwm : 0;
    Serial.print(ntp_synced() ? ntp_timestamp() : 0); Serial.print(',');
    Serial.print(_temp, 2);      Serial.print(',');
    Serial.print(_temp_ext, 2);  Serial.print(',');
    Serial.print(_rh, 2);        Serial.print(',');
    Serial.print(pwm);           Serial.print(',');
    Serial.print(door_open?1:0); Serial.print(",coldstart,");
    Serial.println(_sess < NUM_SESSIONS ? SESSIONS[_sess].id : "done");
}

// ── Public API ────────────────────────────────────────────────────────────
void mode_coldstart_init() {
    hal_dimmer_set(0);
    hal_buzzer_stop();
    _state         = CL_WAITING;
    _sess          = 0;
    _log_cnt       = 0;
    _last_ms       = millis();
    _prev_door_open = hal_door_is_open();

    hal_lcd_mode_banner(5, "COLDLOG");
    Serial.println(F("# Mode 5: Cold Start Data Logger"));
    Serial.printf("# %u sesi, mulai dari sesi 1: T_in≤%.0f°C, PWM=%u%%\n",
                  NUM_SESSIONS, SESSIONS[0].t_target, SESSIONS[0].pwm);
    Serial.println(F("# ts,t_in,t_ext,rh,pwm,door,ctrl_mode,session_id"));
    Serial.println(F("# Tunggu inkubator mendingin ke suhu target sesi 1..."));
}

void mode_coldstart_tick() {
    uint32_t now = millis();
    if (now - _last_ms < SAMPLE_INTERVAL_MS) return;
    _last_ms = now;

    float t, h;
    if (!hal_sht31_read(&t, &h)) {
        Serial.println(F("# WARN: sensor read fail"));
        return;
    }
    _temp = t;
    _rh   = h;

    float t_ext, h_ext;
    if (hal_sht31_read_ext(&t_ext, &h_ext)) _temp_ext = t_ext;

    bool door_open = !hal_door_is_open();  // sensor terbalik: hal returns true=tutup

    // ── Safety: pintu terbuka → heater OFF selalu ─────────────────────────
    if (door_open) hal_dimmer_set(0);

    switch (_state) {

    // ── Menunggu T_in turun ke target sesi ───────────────────────────────
    case CL_WAITING:
        hal_dimmer_set(0);
        if (_temp <= SESSIONS[_sess].t_target) {
            _state   = CL_LOGGING;
            _log_cnt = 0;
            hal_buzzer_beep(100, 100, 3);   // 3 beep pendek = sesi dimulai
            Serial.printf("# [SESI %u MULAI] T_in=%.2f PWM=%u%% target_stop=%.1f\n",
                          _sess + 1, _temp, SESSIONS[_sess].pwm, SESSIONS[_sess].t_stop);
        }
        break;

    // ── Logging dengan fixed PWM ──────────────────────────────────────────
    case CL_LOGGING:
        // Pintu tertutup → heater aktif | Pintu terbuka → sudah di-off oleh safety
        if (!door_open) hal_dimmer_set(SESSIONS[_sess].pwm);

        _log_cnt++;
        _publish(door_open);
        _serial_csv(door_open);

        // Deteksi pintu baru terbuka → log pesan
        if (door_open && !_prev_door_open) {
            Serial.println(F("# Pintu terbuka — heater OFF, logging terus..."));
        }
        if (!door_open && _prev_door_open) {
            Serial.println(F("# Pintu tertutup — heater ON kembali"));
        }

        // Cek kondisi selesai sesi
        if (_temp >= SESSIONS[_sess].t_stop) {
            hal_dimmer_set(0);
            Serial.printf("# [SESI %u SELESAI] %lu sampel, T_in=%.2f\n",
                          _sess + 1, _log_cnt, _temp);

            if (_sess + 1 >= NUM_SESSIONS) {
                // Semua sesi selesai
                _state = CL_DONE;
                hal_buzzer_beep(200, 100, 10);  // Pola selesai
                Serial.println(F("# ======================================"));
                Serial.println(F("# SEMUA SESI COLD START SELESAI!"));
                Serial.println(F("# Kirim data ke ML Engineer untuk retrain."));
                Serial.println(F("# ======================================"));
            } else {
                // Lanjut ke sesi berikutnya — mulai cooling
                _state = CL_COOLING;
                hal_buzzer_beep(200, 300, -1);  // Buzz 200ms/300ms terus selama cooling
                Serial.printf("# Cooling ke %.0f°C untuk sesi %u...\n",
                              SESSIONS[_sess + 1].t_target, _sess + 2);
                Serial.printf("# Buka pintu / masukkan es untuk mempercepat.\n");
            }
        }
        break;

    // ── Mendinginkan ke target sesi berikutnya ────────────────────────────
    case CL_COOLING:
        hal_dimmer_set(0);
        if (_temp <= SESSIONS[_sess + 1].t_target) {
            hal_buzzer_stop();
            _state = CL_READY;
            hal_buzzer_beep(1500, 500, -1);  // Beep panjang terus = siap, tutup pintu
            Serial.printf("# [READY] T_in=%.2f ≤ %.1f → TUTUP PINTU untuk mulai sesi %u!\n",
                          _temp, SESSIONS[_sess + 1].t_target, _sess + 2);
        }
        break;

    // ── Menunggu pintu ditutup untuk mulai sesi berikutnya ───────────────
    case CL_READY:
        hal_dimmer_set(0);
        // Pintu baru ditutup (transisi open→closed)
        if (!door_open && _prev_door_open) {
            hal_buzzer_stop();
            _sess++;
            _log_cnt = 0;
            _state   = CL_LOGGING;
            hal_buzzer_beep(100, 100, 3);   // 3 beep pendek = sesi dimulai
            Serial.printf("# [SESI %u MULAI] T_in=%.2f PWM=%u%% target_stop=%.1f\n",
                          _sess + 1, _temp, SESSIONS[_sess].pwm, SESSIONS[_sess].t_stop);
        }
        break;

    case CL_DONE:
        hal_dimmer_set(0);
        break;
    }

    _lcd_update(door_open);
    _prev_door_open = door_open;
}
