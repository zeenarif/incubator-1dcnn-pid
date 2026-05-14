# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

> **Project:** Sistem Inkubator Telur IoT + TinyML  
> **Author:** Zainal Arifin · NIM 2022TI038 · Institut Teknologi Bisnis AAS Indonesia  
> **Target publikasi:** Jurnal SINTA 2 · 2026

---

## Project Overview

Firmware ESP32 untuk sistem inkubator telur otomatis dengan **5 mode operasi**, menggabungkan:
- Sensor SHT31 (suhu/kelembapan) dengan kalibrasi offset
- AC Dimmer + Fan sebagai aktuator
- 1D-CNN TinyML untuk kontrol prediktif
- Pipeline cloud: MQTT → InfluxDB → Grafana → FastAPI training

### Arsitektur Sistem

```
[SHT31 + Door] → [ESP32 Multi-Mode Firmware] → [MQTT] → [VPS Oracle]
                         ↓                                    ↓
                  [AC Dimmer + Fan]              [InfluxDB → Grafana]
                         ↑                                    ↓
                  [model.cc OTA] ←←←← [FastAPI: Train 1D-CNN + Generate model.cc]
```

### Lima Mode Firmware (pilih via Rotary Encoder, disimpan ke NVS Flash)

| Mode | Nama | Status |
|------|------|--------|
| Mode 0 | Data Logger (PRBS Excitation) | ✅ Berjalan, MQTT publish aktif, 5 hari data terkumpul |
| Mode 1 | On-Off Control (baseline) | ✅ Kode ada (55 baris) |
| Mode 2 | PID Control (baseline) | ⚠️ Skeleton ada (82 baris), belum tuning |
| Mode 3 | 1D-CNN TinyML Predictive (target utama) | 🔲 Belum — menunggu model.cc |
| Mode 4 | Sensor Calibration Utility | ✅ Ada di main.cpp |

---

## Build & Flash Commands

Project menggunakan [PlatformIO](https://platformio.org/). Semua perintah asumsikan `pio` ada di PATH.

```bash
# Build only
pio run

# Build + upload ke ESP32
pio run --target upload

# Buka serial monitor (115200 baud)
pio device monitor

# Upload lalu langsung monitor
pio run --target upload && pio device monitor

# Clean build artifacts
pio run --target clean

# Jika auto-detect port gagal
pio run --target upload --upload-port /dev/ttyUSB0
pio device monitor --port /dev/ttyUSB0 --baud 115200
```

No test runner dikonfigurasi (`test/` masih scaffold kosong).

---

## Hardware

| Komponen | Interface | Address/Pin |
|----------|-----------|-------------|
| ESP32 DevKit V1 | — | target board |
| SHT31 (temp/humidity) | I2C SDA=21, SCL=22 | 0x44 (ADDR→GND) atau 0x45 (ADDR→VCC) |
| LCD 16×2 | I2C (same bus) | 0x27, coba 0x3F jika blank |
| AC Dimmer | Zero-cross=27, PWM=14 | — |
| Fan relay | GPIO 26 | — |
| Door sensor | GPIO 13 | — |
| Rotary Encoder | CLK=25, DT=33, SW=32 | — |

I2C berjalan di 100 kHz (`Wire.setClock(100000)`) untuk toleransi panjang kabel. I2C scanner otomatis berjalan saat boot — gunakan untuk debug address mismatch.

---

## Konfigurasi Pin

```cpp
// config.h
#define PIN_SDA         21   // SHT31 + LCD I2C Data
#define PIN_SCL         22   // SHT31 + LCD I2C Clock
#define PIN_ZERO_CROSS  27   // AC Dimmer zero-crossing interrupt
#define PIN_DIMMER_PWM  14   // AC Dimmer control output
#define PIN_ENC_CLK     25   // Rotary encoder A
#define PIN_ENC_DT      33   // Rotary encoder B
#define PIN_ENC_SW      32   // Rotary encoder button
#define PIN_FAN_RELAY   26   // Fan relay (reserved)
#define PIN_DOOR_SENSOR 13   // Door sensor (reserved)

#define SETPOINT_TEMP        38.0f
#define SAMPLE_INTERVAL_MS   5000
#define PRBS_PWM_LOW         25
#define PRBS_PWM_HIGH        75
```

---

## Struktur File Proyek

```
firmware/
├── platformio.ini
├── src/
│   ├── main.cpp                   # Boot, mode manager, status publish, MQTT handler
│   ├── hal/
│   │   ├── hal_sht31.cpp/h        # Driver SHT31 + kalibrasi offset dari NVS
│   │   ├── hal_dimmer.cpp/h       # AC Light Dimmer PWM (zero-crossing)
│   │   ├── hal_fan.cpp/h          # Fan relay GPIO 26
│   │   ├── hal_door.cpp/h         # Door sensor GPIO 13
│   │   ├── hal_lcd.cpp/h          # LCD 16x2 I2C
│   │   ├── hal_buzzer.cpp/h       # Active buzzer GPIO 4
│   ├── modes/
│   │   ├── mode_datalogger.cpp/h  # Mode 0: PRBS excitation + MQTT publish
│   │   ├── mode_onoff.cpp/h       # Mode 1: On-Off ±0.3°C + MQTT + session support
│   │   ├── mode_pid.cpp/h         # Mode 2: PID + MQTT params + MQTT publish
│   │   ├── mode_tinyml.cpp/h      # Mode 3: 1D-CNN inference + MQTT publish
│   │   └── mode_calibration.cpp/h # Mode 4: Sensor calibration (Serial only)
│   ├── ml/
│   │   └── model.cc               # GENERATED — 1D-CNN TFLite model (inject via OTA)
│   ├── comms/
│   │   ├── mqtt_client.cpp/h      # MQTT publish/subscribe
│   │   ├── ota_manager.cpp/h      # OTA update receiver (model.cc)
│   │   └── ntp_sync.cpp/h         # NTP time sync
│   └── config/
│       ├── config.h               # Pin definitions, setpoint, MQTT topics, intervals
│       └── nvs_manager.cpp/h      # Mode + offset save/load dari NVS Flash
├── lib/
│   └── tflite-micro/              # TensorFlow Lite Micro library
└── docs/
    ├── panduan_mode0_datalogger.md       # SOP menjalankan Mode 0
    ├── panduan_langkah_selanjutnya.md    # Checklist fase setelah data logger selesai
    └── referensi_payload_mqtt.md         # Format JSON semua topic MQTT + konfigurasi Telegraf/Grafana
```

---

## Code Architecture

### Arsitektur Firmware Multi-Mode (`src/main.cpp`)

Loop utama mengelola 5 mode via rotary encoder. Mode tersimpan di NVS Flash — bertahan setelah reboot.

- **Mode switch**: rotate encoder → pilih mode → long-press SW ≥2 detik → konfirmasi + buzzer beep
- **MQTT handler** (`_on_mqtt`): terima `inkubator/mode/set`, `inkubator/pid/params`, `inkubator/ota/trigger`
- **Status publish**: setiap 30 detik kirim `inkubator/status` berisi mode, uptime, free_heap, wifi_rssi
- **Semua mode publish ke** `inkubator/telemetri` dengan field `ctrl_mode` sebagai pembeda

Payload tiap mode mengandung field `session_id` dan `scenario` (settable via `mode_X_set_session()`) untuk kebutuhan pelabelan data Phase 5.

Mode 4 (Kalibrasi) menggunakan ring buffer 10 sampel, stability detection std dev < 0.15°C, output ke Serial CSV saja — tidak publish MQTT.

### Key Constants

| Constant | Default | Efek |
|----------|---------|------|
| `SAMPLE_INTERVAL_MS` | 5000 ms | Sensor read rate (Mode 0–3) |
| `STATUS_INTERVAL_MS` | 30000 ms | Heartbeat publish ke `inkubator/status` |
| `WINDOW_SIZE` | 10 samples | Stability window (Mode 4 calibration) |
| `STABLE_THRESHOLD` | 0.15°C | Std-dev cutoff untuk STABLE (Mode 4) |
| `LCD_REFRESH_MS` | 500 ms | LCD update rate |
| `SHT31_ADDR` | 0x44 | Ganti 0x45 jika ADDR pin ke VCC |
| `LCD_ADDR` | 0x27 | Ganti 0x3F untuk beberapa modul |
| `PRBS_PWM_LOW` | 25% | Level rendah PRBS excitation |
| `PRBS_PWM_HIGH` | 75% | Level tinggi PRBS excitation |
| `SETPOINT_TEMP` | 38.0°C | Target suhu inkubator |

---

## Mode 0: Data Logger (PALING KRITIS)

**Wajib berjalan minimal 5 hari sebelum training CNN.**

### Format CSV Data Logger

Setiap 5 detik, satu baris CSV dikirim via MQTT ke InfluxDB:
```
timestamp_unix, suhu_in, suhu_ext, kelembapan, pwm_aktif, mode_eksitasi, door_status
1748000000, 37.82, 29.45, 58.3, 65, "prbs", 0
```

### Skema Eksitasi PWM (4 sub-fase siklus)

| Sub-fase | Durasi | Deskripsi |
|----------|--------|-----------|
| Steady-State Sweep | 2 jam | PWM: 20%→40%→60%→80%→60%→40%→20%, masing-masing 20 menit |
| PRBS Excitation | 3 jam | Toggle antara 25%↔75%, periode random 30s–300s |
| Disturbance Injection | 1 jam | Buka pintu 30s, drop PWM 0% 2 menit, spike 100% 1 menit |
| Setpoint Tracking | 2 jam | On-Off di sekitar 38°C, semua data tetap direkam |

### Target Dataset

| Parameter | Target |
|-----------|--------|
| Durasi logging | 5–7 hari |
| Interval sampling | 5 detik |
| Total sampel minimum | 86.400 baris |
| Split training | 70% train / 15% val / 15% test — **SEQUENTIAL** (jangan di-shuffle) |

### Implementasi PRBS (LFSR 8-bit)

```cpp
class PRBSExcitation {
  uint8_t lfsr = 0xAC;  // seed non-zero
  uint8_t pwm_low = 25, pwm_high = 75;
  uint32_t next_switch_ms;

  uint8_t next_bit() {
    uint8_t bit = ((lfsr >> 7) ^ (lfsr >> 5) ^ (lfsr >> 4) ^ (lfsr >> 3)) & 1;
    lfsr = (lfsr << 1) | bit;
    return bit;
  }
  uint32_t random_period_ms() {
    uint16_t r = (lfsr * 17 + 43) % 271;
    return (30 + r) * 1000UL;  // 30000ms – 300000ms
  }
public:
  uint8_t get_pwm(uint32_t now_ms) {
    if (now_ms >= next_switch_ms) {
      next_switch_ms = now_ms + random_period_ms();
      return next_bit() ? pwm_high : pwm_low;
    }
    return current_pwm;
  }
};
```

---

## Mode 3: 1D-CNN TinyML

### Arsitektur Model

```python
# Input: sliding window [60 timesteps × 3 features]
# Features: [suhu_in_t, suhu_ext_t, pwm_aktif_t]
# Output: prediksi suhu_in pada t+1 (5 detik ke depan)

model = Sequential([
    Conv1D(16, kernel_size=3, activation='relu', input_shape=(60, 3)),
    MaxPooling1D(pool_size=2),
    Conv1D(8, kernel_size=3, activation='relu'),
    GlobalAveragePooling1D(),
    Dense(16, activation='relu'),
    Dense(1)  # T_pred
])
# Total param: ~713 | Float32: ~3KB | INT8: ~1KB | RAM inferensi: ~15-25KB
```

### Kontrol Proporsional Berbasis Prediksi

```cpp
// mode_tinyml.cpp
float T_pred = model_infer(sliding_window);  // prediksi suhu 5 detik ke depan
float error  = SETPOINT_TEMP - T_pred;       // error prediktif, bukan error saat ini
float Kp     = 8.0f;                         // tuning empiris
float pwm    = constrain(Kp * error, 0, 100);
dimmer_set_pwm(pwm);
```

### Target Metrik Model

| Metrik | Target |
|--------|--------|
| RMSE test set | < 0.3°C |
| MAE test set | < 0.2°C |
| Degradasi RMSE setelah INT8 | < 5% |
| Ukuran model INT8 | < 5 KB |
| RAM inferensi ESP32 | < 30 KB |
| Latensi inferensi | < 5 ms |

---

## MQTT Topics

```
inkubator/telemetri    → publish: JSON data sensor setiap 5 detik (Mode 0–3)
inkubator/status       → publish: mode, uptime, free_heap, wifi_rssi setiap 30 detik
inkubator/mode/set     ← subscribe: set mode aktif (payload: "0"–"4")
inkubator/pid/params   ← subscribe: update Kp/Ki/Kd (payload: "kp,ki,kd")
inkubator/ota/trigger  ← subscribe: trigger OTA download model.cc (payload: URL)
```

Lihat `docs/referensi_payload_mqtt.md` untuk format JSON lengkap setiap topic.

---

## Alur OTA Model (Kritis untuk Mode 3)

```
1. Data logger ≥20.000 sampel terkumpul
2. User trigger: POST /api/train via Grafana button
3. FastAPI: InfluxDB → preprocess → train 1D-CNN
4. Konversi: model.h5 → model.tflite → INT8 → model.cc
5. FastAPI: simpan ke /models/latest/
6. User approve: POST /api/ota/deploy
7. FastAPI: publish MQTT ke inkubator/ota/trigger + URL download
8. ESP32: download model.cc → simpan ke SPIFFS → restart
9. ESP32 boot: load model dari SPIFFS → Mode 3 aktif
```

---

## Target Latency

| Komponen | Target |
|----------|--------|
| L_sensor (I2C SHT31) | < 10 ms |
| L_inferensi (model.cc) | < 5 ms |
| L_aktuasi (set PWM dimmer) | < 2 ms |
| L_total | < 100 ms |
| L_monitoring (ESP32 → Grafana) | < 5 detik |

Ukur dengan `micros()`, 100 siklus.

---

## Config Files

### `src/config/config.h`

Berisi WiFi SSID/password dan MQTT broker address. File ini **sudah di-`#include` oleh `main.cpp`**. Mengandung kredensial nyata — **pastikan sudah ada di `.gitignore` sebelum push ke remote publik.**

---

## Aturan Penting (Jangan Dilanggar)

1. **Mode 0 minimal 5 hari** sebelum training — jangan training dengan data < 20.000 sampel
2. **Kalibrasi sensor SEBELUM data logging** — offset salah = dataset training rusak
3. **Split data SEQUENTIAL, bukan random** — data time series tidak boleh di-shuffle
4. **Simpan semua checkpoint model** — setiap model.cc yang di-deploy harus diberi versi
5. **Uji firmware offline dulu** — pastikan semua mode stabil tanpa WiFi sebelum integrasi cloud
6. **Latensi monitoring tidak kritis** — kontrol aktuator berjalan lokal di ESP32, tidak bergantung internet

---

## Progress Checklist

> **Legend:** ✅ Kode ada dan substansial · ⚠️ Parsial/perlu dilengkapi · [ ] Belum dimulai

### Fase 1 — HAL + Firmware Skeleton

- [x] `hal_sht31.cpp`: baca suhu/RH, terapkan offset dari NVS ✅ (30 baris)
- [x] `hal_dimmer.cpp`: PWM via zero-crossing (GPIO 27/14) ✅ (75 baris, ESP-IDF version-aware)
- [x] `hal_fan.cpp`: relay on/off GPIO 26 ✅ (20 baris)
- [x] `hal_door.cpp`: baca status pintu GPIO 13 ✅ (15 baris)
- [x] `hal_lcd.cpp`: display mode + suhu + PWM ✅ (78 baris)
- [x] `nvs_manager.cpp`: simpan/load mode aktif dan sensor offset ✅ (32 baris)
- [x] `main.cpp`: rotary encoder → mode switching + status publish setiap 30 detik ✅ (276 baris)
- [x] **Mode 0**: PRBS excitation + sub-fase siklus ✅, MQTT publish JSON + `ctrl_mode:"datalog"` ✅
- [x] **Mode 1**: On-Off ±0.3°C + MQTT publish + `session_id`/`scenario` support ✅ (83 baris)
- [x] **Mode 2**: PID + MQTT publish (err, integral, kp/ki/kd) + `session_id`/`scenario` ✅ (109 baris)
- [x] **Mode 4**: tampilkan suhu raw, simpan offset ke NVS ✅ (98 baris)
- [x] MQTT publish ke broker — berhasil, data mengalir ke InfluxDB ✅
- [x] `inkubator/status` heartbeat publish aktif (mode, uptime, free_heap, wifi_rssi) ✅
- [ ] Uji offline (tanpa WiFi) — semua mode stabil
- [ ] LCD tampilkan mode, suhu, PWM dengan benar

### Fase 2 — Cloud Stack (VPS Oracle)

- [x] Setup VPS: buka port 1883, 8883, 443, 80 ✅
- [x] `docker-compose.yml`: Mosquitto + InfluxDB + Grafana + FastAPI + Nginx ✅
- [x] Mosquitto: uji koneksi dari ESP32 ✅
- [x] InfluxDB: bucket "inkubator", measurement "telemetri" ✅
- [x] MQTT → InfluxDB bridge ✅
- [x] End-to-end: ESP32 → MQTT → InfluxDB → Grafana tampil ✅
- [ ] Grafana: dashboard suhu, kelembapan, PWM, mode (status?)
- [ ] FastAPI: `/api/status`, `/api/train`, `/api/ota/deploy`
- [ ] Nginx: TLS dengan Let's Encrypt

### Fase 3 — PID + Kalibrasi

- [x] **Mode 2**: PID lengkap ✅ — MQTT publish aktif, update Kp/Ki/Kd via `inkubator/pid/params` ✅
- [x] **data logging Mode 0**: ✅ 5 hari data terkumpul (2026-05-14)
- [ ] Tuning PID Ziegler-Nichols dari data real
- [ ] **Mode 4** lengkap: kalibrasi 35°C, 38°C, 40°C
- [ ] Rekam tabel kalibrasi (referensi vs SHT31 sebelum/sesudah)

### Fase 4 — 1D-CNN TinyML

- [x] Export InfluxDB → CSV (≥20.000 baris) ✅
- [x] Preprocessing: cleaning, normalisasi, sliding window [60,3] ✅ (sedang berjalan 2026-05-14)
- [x] Split sequential 70/15/15 ✅
- [ ] Train 1D-CNN Keras, RMSE < 0.3°C (sedang berjalan)
- [ ] Kuantisasi INT8, verifikasi RMSE naik < 5%
- [ ] Konversi ke `model.cc`
- [ ] **Mode 3**: embed model.cc, uji inferensi pertama di ESP32
- [ ] Bandingkan output ESP32 vs Python (toleransi < 0.1°C)
- [ ] Ukur RAM: `ESP.getFreeHeap()` sebelum/sesudah inisialisasi model
- [ ] Benchmark latensi: 100 siklus `micros()`
- [ ] Uji alur OTA end-to-end

### Fase 5 — Pengujian Komparatif & Paper

- [ ] 4 skenario × 3 metode × 5 repetisi = 60 sesi uji
- [ ] Skenario: cold start, disturbance (pintu 30s), env change ±3°C, steady state 2 jam
- [ ] Metrik: Overshoot, Settling Time, Steady-State Error, Recovery Time
- [ ] ANOVA one-way + post-hoc Tukey HSD
- [ ] Grafik 300 DPI
- [ ] Draft paper (template jurnal SINTA 2)
- [ ] Similarity check < 20%
- [ ] Submit jurnal

---

## Timeline

| Minggu | Fase | Target |
|--------|------|--------|
| W1 | F1 awal | HAL semua komponen + Mode 0, 1, 4 |
| W2 | F2 | Docker Compose VPS, MQTT-InfluxDB-Grafana hidup |
| W3–W4 | F3 | Mode 2 PID, kalibrasi, mulai data logging 5 hari |
| W5–W6 | F4 awal | Preprocessing, training 1D-CNN, konversi ke ESP32 |
| W7 | F4 lanjutan | OTA pipeline, benchmark, Mode 3 stabil |
| W8 | F5 awal | Pengujian komparatif 60 sesi |
| W9 | F5 | Analisis statistik, grafik |
| W10 | F5 | Penulisan paper, submit |

---

*Dokumen ini adalah panduan hidup — update setiap kali ada keputusan baru.*  
*Zainal Arifin · 2022TI038 · Institut Teknologi Bisnis AAS Indonesia · 2026*
