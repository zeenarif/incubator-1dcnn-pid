// ============================================================
//  Firmware Inkubator Telur IoT + TinyML — ESP32
//  Zainal Arifin · 2022TI038 · Institut Teknologi Bisnis AAS Indonesia
//
//  Mode 0: Data Logger (PRBS Excitation)
//  Mode 1: On-Off Control
//  Mode 2: PID Control
//  Mode 3: 1D-CNN TinyML (stub — aktif setelah model.cc di-OTA)
//  Mode 4: Sensor Calibration Utility
//
//  Mode selection: Rotary encoder rotate → cycle modes
//                  Long press SW (≥2s) → beep + confirm + save to NVS
// ============================================================

#include <Arduino.h>
#include <Wire.h>

#include "config/config.h"
#include "config/nvs_manager.h"

#include "hal/hal_sht31.h"
#include "hal/hal_dimmer.h"
#include "hal/hal_fan.h"
#include "hal/hal_door.h"
#include "hal/hal_lcd.h"
#include "hal/hal_buzzer.h"

#include "modes/mode_datalogger.h"
#include "modes/mode_onoff.h"
#include "modes/mode_pid.h"
#include "modes/mode_tinyml.h"
#include "modes/mode_calibration.h"

#include "comms/mqtt_client.h"
#include "comms/ntp_sync.h"
#include "comms/ota_manager.h"

// ─── Rotary encoder state ────────────────────────────────────────────────────
static volatile int      _enc_delta        = 0;
static volatile uint32_t _sw_down_ms       = 0;
static volatile bool     _sw_long_fired    = false;  // cleared on each new press

static void IRAM_ATTR _enc_isr() {
    int dt = digitalRead(PIN_ENC_DT);
    _enc_delta += (dt == HIGH) ? 1 : -1;
}

// ISR only captures press timestamp; 2 s threshold is checked in loop()
static void IRAM_ATTR _sw_isr() {
    if (digitalRead(PIN_ENC_SW) == LOW) {
        _sw_down_ms    = millis();
        _sw_long_fired = false;
    }
}

// ─── Mode management ─────────────────────────────────────────────────────────
static uint8_t _active_mode    = 4;
static uint8_t _pending_mode   = 4;
static bool    _selecting      = false;
static uint32_t _select_timeout = 0;

static const char *MODE_NAMES[] = {"DATALOG","ON-OFF","PID","CNN","CALIB"};

static void _mode_init(uint8_t m) {
    switch (m) {
    case 0: mode_datalogger_init();  break;
    case 1: mode_onoff_init();       break;
    case 2: mode_pid_init();         break;
    case 3: mode_tinyml_init();      break;
    case 4: mode_calibration_init(); break;
    }
}

static void _mode_tick(uint8_t m) {
    switch (m) {
    case 0: mode_datalogger_tick();  break;
    case 1: mode_onoff_tick();       break;
    case 2: mode_pid_tick();         break;
    case 3: mode_tinyml_tick();      break;
    case 4: mode_calibration_tick(); break;
    }
}

// ─── MQTT message handler ─────────────────────────────────────────────────────
static void _on_mqtt(const char *topic, const uint8_t *payload, uint32_t len) {
    char buf[64];
    uint32_t n = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
    memcpy(buf, payload, n);
    buf[n] = '\0';

    if (strcmp(topic, TOPIC_MODE_SET) == 0) {
        uint8_t m = (uint8_t)atoi(buf);
        if (m <= 4) {
            hal_dimmer_set(0);
            _active_mode = m;
            nvs_save_mode(m);
            _mode_init(m);
        }
    } else if (strcmp(topic, TOPIC_PID_PARAMS) == 0) {
        float kp, ki, kd;
        if (sscanf(buf, "%f,%f,%f", &kp, &ki, &kd) == 3)
            mode_pid_set_params(kp, ki, kd);
    } else if (strcmp(topic, TOPIC_OTA_TRIGGER) == 0) {
        Serial.print(F("# OTA trigger: "));
        Serial.println(buf);
        if (ota_download_model(buf)) ESP.restart();
    }
}

// ─── I2C Scanner ─────────────────────────────────────────────────────────────
static void _scan_i2c() {
    Serial.println(F("# Scanning I2C..."));
    for (uint8_t a = 1; a < 127; a++) {
        Wire.beginTransmission(a);
        if (Wire.endTransmission() == 0) {
            Serial.print(F("#  0x"));
            Serial.println(a, HEX);
        }
    }
}

// ─── LCD mode-select UI ───────────────────────────────────────────────────────
static void _lcd_show_select(uint8_t candidate) {
    char row0[17], row1[17];
    snprintf(row0, sizeof(row0), "Mode? [%-8s]", MODE_NAMES[candidate]);
    snprintf(row1, sizeof(row1), "Hold SW to OK   ");
    hal_lcd_row(0, row0);
    hal_lcd_row(1, row1);
}

// ─── setup ───────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println(F("\n# Inkubator Firmware — Boot"));

    // NVS
    nvs_init();
    _active_mode  = nvs_load_mode();
    _pending_mode = _active_mode;

    // I2C
    Wire.begin(PIN_SDA, PIN_SCL);
    Wire.setClock(100000);
    _scan_i2c();

    // LCD
    hal_lcd_init(LCD_ADDR);

    // SHT31
    float temp_off = nvs_load_temp_offset();
    float rh_off   = nvs_load_rh_offset();
    if (!hal_sht31_init(SHT31_ADDR)) {
        Serial.println(F("# ERROR: SHT31 not found!"));
        hal_lcd_row(0, "ERROR: SHT31");
        hal_lcd_row(1, "Check wiring");
        while (true) delay(1000);
    }
    hal_sht31_set_offset(temp_off, rh_off);
    Serial.print(F("# SHT31 int OK, offset="));
    Serial.print(temp_off, 3);
    Serial.println(F("C"));

    if (!hal_sht31_init_ext(SHT31_EXT_ADDR)) {
        Serial.println(F("# WARN: SHT31 ext not found — t_ext will report 0"));
    } else {
        Serial.println(F("# SHT31 ext OK"));
    }

    // Dimmer + Fan + Door
    hal_dimmer_init(PIN_ZERO_CROSS, PIN_DIMMER_PWM);
    hal_fan_init(PIN_FAN_RELAY);
    hal_door_init(PIN_DOOR_SENSOR);

    // Rotary encoder
    pinMode(PIN_ENC_CLK, INPUT_PULLUP);
    pinMode(PIN_ENC_DT,  INPUT_PULLUP);
    pinMode(PIN_ENC_SW,  INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(PIN_ENC_CLK), _enc_isr, FALLING);
    attachInterrupt(digitalPinToInterrupt(PIN_ENC_SW),  _sw_isr,  FALLING);

    // Buzzer
    hal_buzzer_init(PIN_BUZZER);

    // Cloud (non-blocking) — mqtt_connect() di-skip saat setup, retry via mqtt_loop()
    mqtt_init(_on_mqtt);
    ntp_init();
    ota_init();

    // Start active mode
    Serial.print(F("# Starting Mode "));
    Serial.println(_active_mode);
    _mode_init(_active_mode);
}

// ─── loop ────────────────────────────────────────────────────────────────────
void loop() {
    // ── Rotary encoder: detect rotation → enter selection ──────────────────
    int delta = 0;
    noInterrupts();
    delta = _enc_delta;
    _enc_delta = 0;
    interrupts();

    if (delta != 0) {
        if (!_selecting) {
            _selecting = true;
            _pending_mode = _active_mode;
        }
        _select_timeout = millis() + 5000;
        _pending_mode = (uint8_t)((_pending_mode + 5 + (delta > 0 ? 1 : -1)) % 5);
        _lcd_show_select(_pending_mode);
    }

    // ── Auto-cancel selection after 5 s of no rotation ─────────────────────
    if (_selecting && millis() > _select_timeout) {
        _selecting = false;
        _pending_mode = _active_mode;
    }

    // ── Long-press SW (2 s): confirm mode switch + beep feedback ────────────
    if (_selecting) {
        noInterrupts();
        bool    fired = _sw_long_fired;
        uint32_t down = _sw_down_ms;
        interrupts();

        if (!fired && digitalRead(PIN_ENC_SW) == LOW && millis() - down >= 2000) {
            noInterrupts();
            _sw_long_fired = true;
            interrupts();

            _selecting = false;
            hal_buzzer_beep(500, 0, 1);   // single 500 ms "confirmed" beep
            if (_pending_mode != _active_mode) {
                hal_dimmer_set(0);
                _active_mode = _pending_mode;
                nvs_save_mode(_active_mode);
                Serial.print(F("# Mode switched to "));
                Serial.println(_active_mode);
                _mode_init(_active_mode);
            }
        }
    }

    // ── Run active mode ──────────────────────────────────────────────────────
    if (!_selecting) {
        _mode_tick(_active_mode);
    }

    // ── Buzzer pattern engine ────────────────────────────────────────────────
    hal_buzzer_tick();

    // ── MQTT keep-alive ──────────────────────────────────────────────────────
    mqtt_loop();
}
