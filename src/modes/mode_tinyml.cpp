#include "mode_tinyml.h"
#include "../hal/hal_sht31.h"
#include "../hal/hal_lcd.h"
#include "../hal/hal_dimmer.h"
#include "../hal/hal_door.h"
#include "../config/config.h"
#include "../comms/mqtt_client.h"
#include "../comms/ntp_sync.h"

#include "tensorflow/lite/micro/all_ops_resolver.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/schema/schema_generated.h"

#include <Arduino.h>

// Model data — compiled from src/ml/model.cc
extern const unsigned int g_model_data_len;
extern const uint8_t      g_model_data[];

// ── Normalization constants — must match training exactly ──────────────────
static const float SCALER_MIN[3] = {37.1000f, 28.8600f,   0.0000f};
static const float SCALER_MAX[3] = {65.4000f, 36.4700f, 100.0000f};
static const float T_IN_RANGE    = 28.3000f;  // 65.4 - 37.1
static const float T_IN_MIN      = 37.1000f;

static inline float _norm(float v, int f) {
    return (v - SCALER_MIN[f]) / (SCALER_MAX[f] - SCALER_MIN[f]);
}
static inline float _denorm(float y) {
    return y * T_IN_RANGE + T_IN_MIN;
}

// ── TFLite Micro runtime ───────────────────────────────────────────────────
static constexpr int ARENA_SIZE = 7 * 1024;  // actual usage: ~5644 bytes
alignas(16) static uint8_t          _arena[ARENA_SIZE];
static tflite::AllOpsResolver _resolver;
static const tflite::Model*          _tfl_model     = nullptr;
static tflite::MicroInterpreter*     _interpreter   = nullptr;
static TfLiteTensor*                 _input_tensor  = nullptr;
static TfLiteTensor*                 _output_tensor = nullptr;
static bool                          _model_ok      = false;
static bool                          _tfl_init_done = false;

// ── Sliding window ─────────────────────────────────────────────────────────
static float    _window[CNN_WINDOW_SIZE][CNN_FEATURES];
static uint8_t  _win_idx  = 0;
static bool     _win_full = false;
static uint32_t _last_ms  = 0;
static float    _temp     = 0.0f;
static float    _temp_ext = 0.0f;
static float    _rh       = 0.0f;
static uint8_t  _pwm      = 0;
static float    _t_pred   = SETPOINT_TEMP;
static float    _integral = 0.0f;
static bool     _cnn_zone = false;  // hysteresis state: true = CNN aktif
static char     _session_id[16] = "";
static char     _scenario[24]   = "";

// ── TFLite init — called once, survives mode switches ─────────────────────
static void _tflite_init() {
    if (_tfl_init_done) return;
    _tfl_init_done = true;

    _tfl_model = tflite::GetModel(g_model_data);
    if (_tfl_model->version() != TFLITE_SCHEMA_VERSION) {
        Serial.printf("# TFLite: schema v%lu != supported v%d\n",
                      _tfl_model->version(), TFLITE_SCHEMA_VERSION);
        return;
    }

    static tflite::MicroInterpreter static_interp(
        _tfl_model, _resolver, _arena, ARENA_SIZE);
    _interpreter = &static_interp;

    if (_interpreter->AllocateTensors() != kTfLiteOk) {
        Serial.println(F("# TFLite: AllocateTensors FAILED — arena terlalu kecil?"));
        return;
    }

    _input_tensor  = _interpreter->input(0);
    _output_tensor = _interpreter->output(0);

    // Validasi tipe tensor: support Float32 dan INT8
    if (_input_tensor->type != kTfLiteFloat32 && _input_tensor->type != kTfLiteInt8) {
        Serial.printf("# TFLite: tipe tensor tidak didukung: %d\n", _input_tensor->type);
        return;
    }

    // Validasi ukuran input: 60 × 3 elemen (float=720B, int8=180B)
    uint32_t expected_elements = CNN_WINDOW_SIZE * CNN_FEATURES;
    uint32_t actual_elements   = _input_tensor->bytes /
        (_input_tensor->type == kTfLiteFloat32 ? sizeof(float) : sizeof(int8_t));
    if (actual_elements != expected_elements) {
        Serial.printf("# TFLite: input elements mismatch: %lu (expected %lu)\n",
                      actual_elements, expected_elements);
        return;
    }

    _model_ok = true;

    const char* ttype = (_input_tensor->type == kTfLiteFloat32) ? "Float32" : "INT8";
    Serial.printf("# TFLite OK [%s]: model=%u B  arena=%lu/%d B  heap=%lu B\n",
                  ttype, g_model_data_len,
                  _interpreter->arena_used_bytes(), ARENA_SIZE,
                  (uint32_t)ESP.getFreeHeap());
    if (_input_tensor->type == kTfLiteInt8) {
        Serial.printf("# INT8 params: in scale=%.6f zp=%d | out scale=%.6f zp=%d\n",
                      _input_tensor->params.scale,  _input_tensor->params.zero_point,
                      _output_tensor->params.scale, _output_tensor->params.zero_point);
    }
}

// ── Inference ──────────────────────────────────────────────────────────────
// Catatan ML Engineer: model baru dilatih dengan nilai negatif untuk cold start.
// JANGAN clamp normalisasi ke [0,1] — nilai negatif valid dan diperlukan.
// Window selalu "penuh" sejak tick pertama (pre-fill di init).
static float _infer() {
    if (!_model_ok) return _temp;  // model gagal dimuat

    bool is_int8 = (_input_tensor->type == kTfLiteInt8);
    float in_scale = _input_tensor->params.scale;
    int   in_zp    = _input_tensor->params.zero_point;

    // Isi input tensor secara kronologis: oldest → newest
    // TIDAK di-clamp ke [0,1] — model sudah dilatih dengan nilai negatif (cold start)
    for (int t = 0; t < CNN_WINDOW_SIZE; t++) {
        int src = (_win_idx + t) % CNN_WINDOW_SIZE;
        for (int f = 0; f < CNN_FEATURES; f++) {
            float v = _norm(_window[src][f], f);  // bisa negatif untuk cold start
            int idx = t * CNN_FEATURES + f;
            if (is_int8) {
                // Quantize: float → int8
                int q = (int)roundf(v / in_scale) + in_zp;
                _input_tensor->data.int8[idx] = (int8_t)constrain(q, -128, 127);
            } else {
                _input_tensor->data.f[idx] = v;
            }
        }
    }

    if (_interpreter->Invoke() != kTfLiteOk) {
        Serial.println(F("# TFLite: Invoke FAILED"));
        return _temp;
    }

    float y_norm;
    if (is_int8) {
        // Dequantize: int8 → float
        float out_scale = _output_tensor->params.scale;
        int   out_zp    = _output_tensor->params.zero_point;
        y_norm = (_output_tensor->data.int8[0] - out_zp) * out_scale;
    } else {
        y_norm = _output_tensor->data.f[0];
    }

    return _denorm(y_norm);
}

// ── Public API ─────────────────────────────────────────────────────────────
void mode_tinyml_set_session(const char *session_id, const char *scenario) {
    strncpy(_session_id, session_id, sizeof(_session_id) - 1);
    strncpy(_scenario,   scenario,   sizeof(_scenario)   - 1);
}

void mode_tinyml_init() {
    hal_dimmer_set(0);

    // Baca suhu awal sebelum mengisi window
    float t_init = SETPOINT_TEMP, h_init = 0.0f, t_ext_init = 0.0f, h_ext_init = 0.0f;
    hal_sht31_read(&t_init, &h_init);
    hal_sht31_read_ext(&t_ext_init, &h_ext_init);

    // Pre-fill seluruh window dengan pembacaan pertama yang diduplikat.
    // Ini memungkinkan model langsung berjalan dari tick pertama tanpa harus
    // menunggu 60 sampel (5 menit). Model "membaca" sejarah virtual:
    // "inkubator berada di t_init selama 5 menit terakhir dengan heater mati."
    // Slot akan digantikan satu per satu oleh data nyata setiap tick.
    float pwm_init    = constrain(CNN_KP * (SETPOINT_TEMP - t_init), 0.0f, 60.0f);
    // pwm_prefill: konteks historis window — proporsional terhadap jarak dari setpoint,
    // cap 20% agar tidak terlalu agresif. Lebih realistis dari 0 (model tidak salah baca
    // "heater mati total") tapi lebih konservatif dari pwm_init untuk hindari over-predict.
    // Contoh: t_init=32°C → 20%, t_init=37.5°C → 9%, t_init=38°C → 0%
    float pwm_prefill = constrain(CNN_KP * (SETPOINT_TEMP - t_init), 0.0f, 20.0f);
    for (int i = 0; i < CNN_WINDOW_SIZE; i++) {
        _window[i][0] = t_init;
        _window[i][1] = t_ext_init;
        _window[i][2] = pwm_prefill;
    }

    _win_idx   = 0;
    _win_full  = true;    // ← inference langsung aktif dari tick pertama
    _cnn_zone  = false;   // hysteresis: CNN zone aktif hanya saat t_in >= 37°C
    _pwm       = (uint8_t)pwm_init;
    _t_pred    = SETPOINT_TEMP;
    _integral  = 0.0f;
    _last_ms  = millis();
    hal_lcd_mode_banner(3, "1D-CNN");

    _tflite_init();  // idempotent — aman dipanggil berulang

    if (_model_ok) {
        Serial.printf("# Mode 3: TinyML 1D-CNN aktif — inference langsung (pre-fill t_init=%.2f)\n", t_init);
    } else {
        Serial.println(F("# Mode 3: TinyML 1D-CNN STUB — model gagal dimuat"));
    }
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

    // Update sliding window
    _window[_win_idx][0] = _temp;
    _window[_win_idx][1] = _temp_ext;
    _window[_win_idx][2] = (float)_pwm;
    _win_idx = (_win_idx + 1) % CNN_WINDOW_SIZE;
    if (_win_idx == 0) _win_full = true;

    // Jalankan inferensi + ukur latensi
    uint32_t t0 = micros();
    _t_pred = _infer();
    uint32_t inf_us = micros() - t0;

    // Anti-anomaly bidirectional: prediksi di luar batas fisik tidak masuk akal
    // Under-predict (< t_in-2°C) → PWM=100% spike
    // Over-predict  (> t_in+1.5°C) → P-term negatif besar → PWM deadlock
    // 1.5°C upper cap (ML Engineer, 2026-05-17): memaksa P-term lebih kecil
    // saat model over-predict, agar integral bisa mengimbangi lebih cepat
    if (_t_pred < _temp - 2.0f)  _t_pred = _temp;
    if (_t_pred > _temp + 1.5f)  _t_pred = _temp + 1.5f;

    // ── Hysteresis CNN zone ────────────────────────────────────────────────
    // CNN aktif hanya saat t_in >= 37°C. Kembali ke fallback saat t_in < 36.5°C.
    // Band 0.5°C mencegah chattering. Integral TIDAK di-reset saat transisi —
    // dibiarkan akumulasi lintas siklus agar bisa melawan over-prediction model.
    // Anti-windup ±50 mencegah runaway. Reset hanya di mode_tinyml_init().
    if (!_cnn_zone && _temp >= CNN_ZONE_ENTER_TEMP) _cnn_zone = true;
    if ( _cnn_zone && _temp <  CNN_ZONE_EXIT_TEMP)  _cnn_zone = false;

    // ── PWM computation: 2 zona (FILLING dihapus — _win_full selalu true) ───
    // [FALLBACK]  !_cnn_zone  → P dari t_in nyata, min 40% (t_in < 36.5°C)
    // [CNN]        _cnn_zone  → P-predictive + I dari error_real (t_in >= 37°C)
    bool  in_cnn     = _cnn_zone;
    float error_real = SETPOINT_TEMP - _temp;
    float pwm_f;

    if (!in_cnn) {
        // Cap 60% mencegah overshoot saat cold start (t_in jauh dari setpoint).
        // Di dekat setpoint (error ≤ 2°C → pwm ≤ 60%) cap ini tidak berpengaruh.
        pwm_f = constrain(error_real * CNN_FB_GAIN, CNN_FB_PWM_MIN, 60.0f);
    } else {
        float error_pred = SETPOINT_TEMP - _t_pred;
        _integral += CNN_KI * error_real * (SAMPLE_INTERVAL_MS / 1000.0f);
        _integral  = constrain(_integral, -CNN_I_CLAMP, CNN_I_CLAMP);
        pwm_f = constrain(CNN_KP * error_pred + _integral, 0.0f, 100.0f);
    }
    _pwm = (uint8_t)pwm_f;
    hal_dimmer_set(_pwm);

    hal_lcd_show_tinyml(_temp, _t_pred, _pwm);

    bool     door = hal_door_is_open();
    uint32_t ts   = ntp_synced() ? ntp_timestamp() : 0;

    // Serial output
    Serial.print(ts);            Serial.print(',');
    Serial.print(_temp, 2);      Serial.print(',');
    Serial.print(_temp_ext, 2);  Serial.print(',');
    Serial.print(_rh, 2);        Serial.print(',');
    Serial.print(_t_pred, 3);    Serial.print(',');
    Serial.print(_pwm);          Serial.print(',');
    Serial.print(door ? 1 : 0);  Serial.print(',');
    Serial.print(F("tinyml"));   Serial.print(',');
    Serial.print(_session_id);   Serial.print(',');
    Serial.print(_scenario);
    if (in_cnn) {
        Serial.printf(",cnn,I=%.1f,inf_us=%lu", _integral, inf_us);
    } else {
        Serial.printf(",fallback,I=%.1f,inf_us=%lu", _integral, inf_us);
    }
    Serial.println();

    if (mqtt_is_connected()) {
        char payload[256];
        // "integral" diisi dengan nilai CNN _integral (bukan PID integral)
        // agar terlihat di CSV/InfluxDB untuk verifikasi oleh ML Engineer
        snprintf(payload, sizeof(payload),
            "{\"ts\":%lu,\"t_in\":%.2f,\"t_ext\":%.2f,\"rh\":%.1f"
            ",\"t_pred\":%.3f,\"pwm\":%u,\"door\":%u,\"fb\":%u"
            ",\"integral\":%.2f"
            ",\"ctrl_mode\":\"tinyml\""
            ",\"session_id\":\"%s\",\"scenario\":\"%s\"}",
            ts, _temp, _temp_ext, _rh,
            _t_pred, _pwm, (uint8_t)door, (uint8_t)(!in_cnn),
            _integral,
            _session_id, _scenario);
        mqtt_publish(TOPIC_TELEMETRI, payload);
    }
}

bool mode_tinyml_model_ready() {
    return _model_ok;
}
