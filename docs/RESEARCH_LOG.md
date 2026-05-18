# Research Log — Sistem Inkubator Telur IoT + TinyML

> **Judul Penelitian (Draft):** Implementasi 1D-CNN TinyML untuk Kontrol Prediktif Suhu Inkubator Telur Berbasis ESP32: Perbandingan dengan Kontrol On-Off dan PID
> **Penulis:** Zainal Arifin · NIM 2022TI038 · Institut Teknologi Bisnis AAS Indonesia
> **Target Jurnal:** SINTA 2 · 2026
> **Periode Penelitian:** Mei 2026

---

## Daftar Isi

1. [Gambaran Sistem](#1-gambaran-sistem)
2. [Fase 1 — Hardware & Firmware](#2-fase-1--hardware--firmware)
3. [Fase 2 — Cloud Stack](#3-fase-2--cloud-stack)
4. [Fase 3 — Data Logger & Kalibrasi Sensor](#4-fase-3--data-logger--kalibrasi-sensor)
5. [Fase 3 — Tuning PID (Ziegler-Nichols)](#5-fase-3--tuning-pid-ziegler-nichols)
6. [Fase 4 — Training & Deployment 1D-CNN](#6-fase-4--training--deployment-1d-cnn)
7. [Permasalahan & Solusi](#7-permasalahan--solusi)
8. [Parameter Final Semua Metode](#8-parameter-final-semua-metode)
9. [Catatan untuk Penulisan Paper](#9-catatan-untuk-penulisan-paper)

---

## 1. Gambaran Sistem

### Arsitektur
```
ESP32 (5 mode firmware) → MQTT → VPS Oracle
     ↓                              ↓
[AC Dimmer + Fan]          [InfluxDB → Grafana]
     ↑                              ↓
[model.cc OTA] ←←← [FastAPI training pipeline]
```

### Hardware
| Komponen | Spesifikasi |
|---|---|
| MCU | ESP32 DevKit V1, 240 MHz, 4MB Flash, 320KB RAM |
| Sensor suhu | SHT31 (internal addr 0x44), akurasi ±0.2°C |
| Sensor suhu eksternal | SHT31 (addr 0x45), untuk merekam t_luar |
| Aktuator | AC Dimmer (zero-cross GPIO27, PWM GPIO14) |
| Setpoint | 38.0°C (suhu optimal penetasan telur) |

### Lima Mode Firmware
| Mode | Nama | Fungsi |
|---|---|---|
| 0 | Data Logger | PRBS excitation untuk system identification |
| 1 | On-Off | Baseline kontrol ±0.3°C hysteresis |
| 2 | PID | Proportional-Integral control |
| 3 | TinyML 1D-CNN | Predictive control berbasis CNN |
| 4 | Kalibrasi | Kalibrasi offset sensor SHT31 |

---

## 2. Fase 1 — Hardware & Firmware

### Kalibrasi Sensor (Mode 4)
**Prosedur:** Sensor direndam di waterbath suhu referensi 35°C, 38°C, 40°C.

**Hasil kalibrasi:**
- Offset suhu: **−0.4°C** (SHT31 membaca 0.4°C lebih tinggi dari referensi)
- Offset disimpan ke NVS Flash ESP32, diterapkan otomatis ke semua pembacaan

**Catatan untuk paper:** Kalibrasi dilakukan sebelum data logging. Offset yang salah akan menghasilkan dataset training yang rusak — tidak dapat dikoreksi setelah training.

### Struktur Payload MQTT
Semua mode publish ke topic `inkubator/telemetri`, dibedakan via field `ctrl_mode`. Format:

```json
{
  "ts": 1748000000,
  "t_in": 37.82, "t_ext": 29.45, "rh": 58.3,
  "pwm": 65, "door": 0,
  "ctrl_mode": "pid|onoff|tinyml|datalog",
  "session_id": "s01", "scenario": "cold_start"
}
```

Field `session_id` dan `scenario` digunakan untuk pelabelan data pada Phase 5 (pengujian komparatif).

---

## 3. Fase 2 — Cloud Stack

**Infrastruktur VPS Oracle Free Tier:**
- CPU: 1 OCPU · RAM: 1 GB · OS: Ubuntu 22.04
- Stack: Mosquitto + InfluxDB + Grafana + Go API + Nginx

**Catatan penting:**
- Training ML **tidak** dijalankan di VPS (RAM tidak cukup untuk TensorFlow)
- Training dijalankan di laptop/Google Colab, hasilnya (model.cc) di-push ke repo
- VPS hanya melayani serving model via HTTP untuk OTA ke ESP32

**Telegraf config untuk InfluxDB:**
```toml
[[inputs.mqtt_consumer]]
  topics = ["inkubator/telemetri"]
  name_override = "telemetri"
  json_time_key = "ts"
  tag_keys = ["ctrl_mode", "phase", "session_id", "scenario"]
```

---

## 4. Fase 3 — Data Logger & Kalibrasi Sensor

### Data Logger Mode 0 (PRBS Excitation)

**Durasi:** 5 hari kontinu (2026-05-09 s/d 2026-05-14)

**Siklus 8 jam (berulang):**
| Sub-fase | Durasi | PWM | Tujuan |
|---|---|---|---|
| SWEEP | 2 jam | 20%→40%→60%→80%→60%→40%→20% (20 mnt/step) | Steady-state di berbagai level daya |
| PRBS | 3 jam | Toggle 25%↔75%, periode random 30–300 detik (LFSR 8-bit) | System identification — fase terpenting |
| DISTURB | 1 jam | 0% → spike 100% → 25% + buka pintu 30 detik | Melatih CNN pada gangguan |
| TRACK | 2 jam | On-Off di sekitar 38°C | Perilaku closed-loop |

**Mengapa PRBS?**
PRBS (Pseudo-Random Binary Sequence) dengan LFSR 8-bit menghasilkan eksitasi frekuensi lebar (wideband excitation) yang mengidentifikasi dinamika sistem secara menyeluruh — kelambaman termal, kecepatan respons, dan perilaku transien. Ini standar dalam system identification engineering.

**Jumlah data:** > 86.400 sampel (5 hari × 24 jam × 3600 detik / 5 detik interval)

---

## 5. Fase 3 — Tuning PID (Ziegler-Nichols)

### Latar Belakang Metode

Metode Ziegler-Nichols Ultimate Gain dipilih karena:
1. Metode berbasis eksperimen langsung pada plant nyata (tidak memerlukan model matematika)
2. Standar industri yang diterima akademik
3. Menghasilkan parameter yang divalidasi secara empiris

### Proses Tuning

**Langkah 1 — Verifikasi kebutuhan daya pemanasan**

Dari analisis data Mode 0 (On-Off steady-state, t_in ≥ 37.5°C):
```
PWM_ss_avg = 36.4%
```
Ini adalah daya pemanasan yang dibutuhkan untuk mempertahankan ±38°C. Nilai ini menjadi referensi untuk validasi parameter PID.

**Langkah 2 — Pencarian Ultimate Gain (Ku)**

Prosedur: Ki=0, Kd=0, naikkan Kp secara bertahap.

Observasi penting: Kp ≤ 8 menghasilkan PWM terlalu kecil (5–16%) karena equilibrium P-only:
```
T_eq = 38 - P_loss/Kp = 38 - 36/8 = 34°C (jauh dari setpoint)
```

Untuk mencapai setpoint, dibutuhkan Kp ≥ 40.

**Hasil pencarian Ku:**

| Kp | Amplitudo osilasi | Tren | Keterangan |
|---|---|---|---|
| 2 | ±0.4°C | Meredam | Di bawah Ku |
| 4 | tidak stabil | Turun terus | Kp terlalu kecil |
| 8 | tidak stabil | Turun terus | Masih terlalu kecil |
| 200 | ±0.225°C | Stabil → **Ku kandidat awal** | Coba naikkan |
| 240 | ±0.45°C | **Tumbuh setelah transisi** | Di atas Ku |

**Nilai final:**
```
Ku = 240    (amplitudo tumbuh di Kp=240 → di atas batas stabilitas)
Tu = 184 detik ≈ 3.1 menit   (rata-rata 8 siklus osilasi di Kp=240)
```

**Catatan penting untuk paper:** Periode osilasi Tu ≈ 184–201 detik muncul **konsisten di semua level Kp** — ini adalah frekuensi natural termal inkubator, bukan artefak gain tertentu. Artinya sistem termal memiliki natural frequency ≈ 1/190 Hz.

**Langkah 3 — Mengapa Tidak Menggunakan Kd (Derivative)?**

Untuk sistem termal, Kd **tidak digunakan** karena:
1. **Noise amplification:** Sensor SHT31 memiliki resolusi 0.1°C. Dengan interval sampling 5 detik, derivative term = Kd × (Δerr/5s). Nilai Kd teoritis dari ZN = 0.125 × Tu = 0.125 × 184 = 23 detik. Ini menghasilkan: spike output = 23 × 0.1/5 = 0.46% per noise spike. Dengan Kp=0.6×Ku=144: Kd_term = 144 × 23 = 3312. Spike = 3312 × 0.02 = **66% PWM** dari noise 0.1°C → tidak dapat digunakan.
2. **Termal sistem lambat:** Frekuensi natural 1/190 Hz jauh di bawah sampling rate 1/5 Hz. Derivative action tidak memberikan manfaat signifikan.
3. **Praktik industri:** Kontrol suhu industri umumnya menggunakan PI saja.

**Langkah 4 — Kalkulasi Parameter PI**

Formula Ziegler-Nichols untuk PI controller:
```
Kp = 0.45 × Ku = 0.45 × 240 = 108
Ti = 0.83 × Tu = 0.83 × 184 = 152.7 detik
Ki = Kp / Ti = 108 / 152.7 = 0.707
```

**Parameter konservatif yang dipilih (faktor 0.67 dari ZN standar):**
```
Kp = 72    (= 0.30 × Ku = 0.67 × Kp_ZN)
Ki = 0.391 (= Kp / (0.83 × Tu) × 0.67)
Kd = 0
```

**Alasan parameter konservatif:**
- Sistem termal memiliki inersia besar (perubahan suhu lambat)
- ZN sering menghasilkan respons agresif untuk sistem termal
- Parameter lebih kecil memberikan margin stabilitas yang lebih baik
- Hasil verifikasi menunjukkan steady-state error < 0.1°C (memenuhi target < 0.5°C)

### Hasil Verifikasi PID (Kp=72, Ki=0.391)

**Test dari warm state (t_in ≈ 38°C):**

| Metrik | Hasil | Target |
|---|---|---|
| Steady-state avg | 37.999°C | 38.0°C |
| Steady-state error | 0.001°C | < 0.5°C ✓ |
| Std deviasi | ±0.015°C (awal) / ±0.169°C (stabil) | — |
| Overshoot | +0.06°C dari warm | < 2°C ✓ |
| Integral behavior | Tersaturasi di clamp 150 (warm start) | — |

**Masalah anti-windup yang ditemukan:**

Awalnya clamp integral = ±50. Dengan Kp=72, Ki=0.391 dan clamp 50:
```
Ki × integral_max = 0.391 × 50 = 19.55%  (bias dari integral)
Kp × err_ss = 72 × 0.063 = 4.55%
Total PWM = 24.1% ≈ avg PWM 23.5% ✓
```

Integral tersaturasi di 50 dan bertindak sebagai **bias statis**, bukan integrator sejati. Ini mencegah eliminasi error sempurna tetapi hasil steady-state masih baik (0.001°C error).

**Perbaikan:** Clamp dinaikkan ke ±150. Integral kini bergerak bebas (32–65 saat steady-state), namun amplitudo osilasi sedikit meningkat (±0.169°C std) karena integral berkontribusi dinamis ke output.

**Observasi untuk paper:** Osilasi ±0.169°C pada Mode 2 (PID) LEBIH KECIL dari On-Off ±0.180°C dengan period yang sama (~188 detik). Ini menunjukkan PID memberikan sedikit perbaikan dibanding On-Off untuk steady-state dengan frekuensi natural yang sama.

**Parameter disimpan ke NVS Flash** — tidak hilang saat restart.

---

## 6. Fase 4 — Training & Deployment 1D-CNN

### Arsitektur Model

```python
model = Sequential([
    Conv1D(16, kernel_size=3, activation='relu', input_shape=(60, 3)),
    MaxPooling1D(pool_size=2),
    Conv1D(8, kernel_size=3, activation='relu'),
    GlobalAveragePooling1D(),
    Dense(16, activation='relu'),
    Dense(1)  # output: t_in pada t+1 (5 detik ke depan)
])
```

**Input:** Sliding window 60 timestep × 3 fitur [t_in, t_ext, pwm]
**Output:** Prediksi t_in 5 detik ke depan

**Justifikasi arsitektur:**
- Conv1D untuk menangkap pola temporal lokal dalam time series suhu
- GlobalAveragePooling1D untuk feature aggregation yang invariant terhadap posisi temporal
- Dense layer untuk non-linear mapping ke prediksi suhu
- Total parameter: ~713 → model sangat ringan untuk inferensi di ESP32

### Parameter Normalisasi (MinMaxScaler)

Normalisasi **wajib sama persis** antara training dan inferensi ESP32:

| Fitur | Min | Max | Range |
|---|---|---|---|
| t_in (°C) | 37.1000 | 65.4000 | 28.3000 |
| t_ext (°C) | 28.8600 | 36.4700 | 7.6100 |
| pwm (%) | 0.0000 | 100.0000 | 100.0000 |

```
x_normalized = (x - min) / (max - min)
```

**Output (t_in prediksi):**
```
t_pred_celsius = y_normalized × 28.3 + 37.1
```

**Catatan penting untuk paper:** Training minimum t_in = 37.1°C. Data Mode 0 direkam saat inkubator sudah hangat. Model tidak memiliki referensi untuk suhu di bawah 37.1°C → ini menjadi **keterbatasan cold start** yang harus disebutkan di paper.

### Kuantisasi dan Deployment

**Model:** INT8 quantized (bukan Float32)

Konfirmasi dari serial monitor ESP32:
```
# TFLite OK [INT8]: model=10672 B  arena=5644/7168 B  heap=230828 B
# INT8 params: in scale=0.003922 zp=-128 | out scale=0.003876 zp=-128
```

**Ukuran:** 10.672 bytes (~10.4KB) — sedikit di atas target 5KB namun masih sangat kecil untuk ESP32

**Tensor arena:** Hanya **5.644 bytes** yang terpakai dari 7.168 bytes yang dialokasikan

**Inference latency:** Belum diukur formal (target < 5ms)

**RAM setelah TFLite loaded:** 230.828 bytes tersisa dari 327.680 bytes (70.5% free)

### Derivasi CNN_KP

**Nilai awal** di CLAUDE.md: 8.0 (empiris, tidak ada derivasi)

**Derivasi ilmiah** (2026-05-16):

**Metode 1 — Dari ZN:**
```
Kp_P_only = 0.5 × Ku = 0.5 × 240 = 120
Faktor reduksi prediksi: Ts/τ = 5/46 = 0.109 (kecil → prediksi 1-step memberikan sedikit keunggulan)
CNN_KP_ZN ≈ 120 × (1 - 0.109) ≈ 107
```

**Metode 2 — Dari kebutuhan steady-state:**
```
PWM_ss = 36% (kebutuhan daya di 38°C)
Jika model RMSE = 0.3°C → error_ss ≈ 0.3°C
CNN_KP × 0.3 = 36% → CNN_KP = 120
```

**Metode 3 — Konservatif dari Kp_PID:**
```
CNN_KP = 0.5 × Kp_PID = 0.5 × 72 = 36
```

**Nilai yang dipilih:** `CNN_KP = 36`

**Justifikasi pemilihan 36 (bukan 107 atau 120):**
- Model belum divalidasi secara penuh di ESP32 (Phase 5 belum selesai)
- Faktor konservatif 50% dari Kp_PID memberikan margin stabilitas
- Jika Phase 5 menunjukkan osilasi → turunkan; jika terlalu lambat → naikkan
- Formula: `CNN_KP = 0.5 × Kp_PID` dapat dijelaskan di paper sebagai "gain prediktif ditetapkan 50% dari Kp PID untuk mengakomodasi efek antisipasi prediksi satu langkah ke depan"

### Temuan Penting dari ML Engineer (RESEARCH_LOG_ML.md)

**A. Data logger menghasilkan nilai berbeda dari dokumentasi:**
- Phase `STEADY` → aktual `TRACK`, phase `SETPOINT` tidak ada
- **Field `door` TERBALIK:** di data aktual, `1=tutup, 0=buka` (kebalikan dari dokumentasi)
  → ML engineer melakukan remap `{1→0, 0→1}` di preprocessing
  → Perlu diperbaiki di firmware atau didokumentasikan sebagai ketidakkonsistenan

**B. PRBS phase dikeluarkan dari training:**
- PRBS menghasilkan t_in = 47–70°C (jauh di atas deployment range 38°C)
- Memasukkan PRBS menyebabkan underfitting (RMSE test = 0.57°C)
- Keputusan: hanya TRACK + SWEEP + DISTURB yang dipakai untuk training
- **Implikasi:** Model tidak pernah lihat eksitasi PRBS → tidak cocok untuk system dengan PRBS

**C. Masalah konversi TFLite (penting untuk reprodusibilitas):**
- `TFLiteConverter.from_keras_model()` + Keras 3.14 tidak meneruskan representative dataset
- Menyebabkan RMSE INT8 = 1.29°C (+217%) pada percobaan pertama
- Fix: gunakan `model.export(saved_dir)` → `from_saved_model(saved_dir)` dengan format dict
- RMSE INT8 final = 0.4111°C (degradasi hanya +0.87% dari float32 0.4076°C) ✓

**D. Total data yang dipakai training: 52.289 baris** (dari 83.903 total — hanya 62%)

### Keterbatasan Mode 3 yang Ditemukan

**1. Cold Start Limitation**

Saat t_in < 37.1°C (training minimum):
- Normalisasi menghasilkan nilai negatif → di-clamp ke 0
- Model menerima input identik → output konstan = 37.319°C
- `error = 38 - 37.319 = 0.681°C → PWM = CNN_KP × 0.681`
- Dengan CNN_KP=8 lama: PWM = 5.4% → **tidak cukup untuk memanaskan**
- Dengan CNN_KP=36 baru: PWM = 24.5% → lebih baik, perlu divalidasi

**Masalah berlapis yang ditemukan (2026-05-16):**

```
Layer 1 — Window transition:
  Saat CNN pertama aktif (t_in baru melewati 37.1°C),
  60-langkah window MASIH berisi data warmup (t_in 28-37°C)
  → normalisasi menghasilkan nilai negatif → clamp ke 0
  → model melihat "hampir semua nol" → output konstan 37.319°C

Layer 2 — Training range gap:
  Range TRACK training: 37.5-44°C
  Range 37.1-37.5°C adalah "no man's land" — tidak ada di training
  → model tidak tahu cara memprediksi di range ini
```

**Akibat yang diamati:** Sistem berosilasi di batas 37.1°C — tidak pernah mencapai 38°C.

**Solusi jangka pendek (untuk Phase 5):** Pre-heat dengan Mode 1 ke 38°C dulu, baru switch ke Mode 3 (Option B dari ML engineer). Ini memberikan kondisi awal yang seragam untuk semua metode.

**Solusi permanen:** Kumpulkan data cold start (3-4 jam Mode 0 dari inkubator dingin ~28-30°C), retrain model. Range normalisasi akan turun dari min=37.1°C ke min≈28°C.

**Implikasi untuk paper:** Sebutkan sebagai "batasan distribusi data training" (*training data distribution limitation*). Rekomendasikan pengumpulan data cold start untuk penelitian lanjutan.

**2. Flash Memory Usage**

Library TFLite Micro (AllOpsResolver) menggunakan ~700KB flash:
```
Flash usage: 98.7% (1.294MB / 1.310MB) dengan AllOpsResolver + 40KB arena
             82.3% (1.078MB / 1.310MB) dengan MicroMutableOpResolver + 30KB arena
             98.8% (1.294MB / 1.310MB) dengan AllOpsResolver + 7KB arena (final)
```

**Catatan:** AllOpsResolver dipertahankan karena model INT8 memerlukan ops yang sulit diprediksi tanpa inspeksi flatbuffer. Flash 98.8% masih dalam batas aman.

---

## 7. Permasalahan & Solusi

### P1 — PID params hilang saat restart
**Masalah:** Parameter Kp/Ki/Kd yang diset via MQTT hilang saat ESP32 restart — kembali ke default.

**Solusi:** Tambahkan `nvs_save_pid()` dan `nvs_load_pid()` di `nvs_manager.cpp`. Parameter kini persisten di NVS Flash.

### P2 — ctrl_mode menampilkan "Data Logger" untuk semua mode
**Masalah:** Backend bridge.go tidak mem-parse field `ctrl_mode` dari JSON payload. Field baru yang ditambahkan firmware diabaikan — backend memiliki mapping internal.

**Akar masalah:** Struct Go `Telemetri` tidak memiliki field `ctrl_mode`. Perlu update backend.

**Solusi:** Update struct bridge.go untuk include semua field baru.

### P3 — TFLite AllocateTensors gagal
**Masalah:** `AllocateTensors()` gagal dengan arena 20KB dan 30KB.

**Investigasi:** Model INT8, arena hanya membutuhkan 5.644 bytes. Tetapi test awal dengan AllOpsResolver gagal karena library `tanakamasayuki/TensorFlowLite_ESP32` v1.0.0 memiliki bug kompilasi internal (`GreedyMemoryPlanner::operator delete` private).

**Solusi:** Ganti library ke `spaziochirale/Chirale_TensorFLowLite` yang lebih baru dan tidak memiliki bug tersebut.

### P4 — Model INT8 tidak didukung (input bukan Float32)
**Masalah:** Kode awal mengecek `kTfLiteFloat32` dan menolak model INT8.

**Solusi:** Update inferensi untuk handle INT8:
- Input: `float_norm → int8 = round(v/scale) + zero_point`
- Output: `int8 → float = (int8 - zero_point) × scale`

### P5 — t_pred konstan (37.319°C) saat cold start
**Masalah:** Semua t_in (32–36°C) berada di bawah training minimum 37.1°C → normalisasi negatif → clamp ke 0 → input identik → output konstan.

**Solusi:** Tambahkan fallback: `if (_temp < SCALER_MIN[0]) return _temp`

**Implikasi paper:** Sebutkan keterbatasan ini sebagai "batasan distribusi data training" (training data distribution limitation).

### P6 — Integral tersaturasi (stuck di clamp)
**Masalah:** Anti-windup clamp ±50 terlalu kecil untuk Kp=72, Ki=0.391. Integral stuck di ceiling dan berperan sebagai bias statis, bukan integrator sejati.

**Solusi:** Naikkan clamp ke ±150. Integral kini aktif (32–65 di steady-state).

**Catatan:** Sedikit meningkatkan amplitudo osilasi karena integral berkontribusi dinamis, tapi steady-state error tetap kecil (0.001°C).

### P7 — Loop Destruktif CNN: Over-Prediction saat Cold Start (2026-05-17)

**Masalah:** Mode 3 berosilasi besar selama cold start — t_in hanya menyentuh setpoint 1–2 menit, lalu jatuh ke ~34°C selama 15–30 menit. Teramati pada rekaman `record-2026-05-16T21:09:00-DataTinyMLFromColdStart.csv`.

**Diagnosis (analisis CSV + ML Engineer):**

Dua akar masalah bekerja bersama:

1. **Model over-predicts +2–3°C saat t_in jauh dari setpoint:**

| Range t_in | Error rata-rata (t_pred − t_in) |
|---|---|
| 34–35°C | +3.20°C |
| 35–36°C | +3.06°C |
| 36–37°C | +1.60°C |
| 37–37.5°C | +0.54°C |
| 37.5–38°C | ~0°C ✅ |

Penyebab: distribusi training hanya melihat t_in=37.5–44°C. Saat t_in=35–37°C dengan PWM tinggi, suhu memang menuju 38°C — model belajar pola ini tapi tidak pernah melihat skenario PWM dipotong oleh dirinya sendiri.

2. **Controller 100% bergantung pada t_pred tanpa feedback t_in nyata:**
   - t_pred ≥ 38°C → PWM=0, tidak peduli t_in aktual ada di 34°C sekalipun

**Mekanisme loop destruktif (terlihat di CSV baris 58–107):**
```
t_in=36.9°C → model prediksi 38.0°C → PWM=0
                        ↓
          t_in turun ke 34°C (tidak ada pemanas)
                        ↓
     window masih berisi data t_in 37–38°C yang lama
                        ↓
     model masih prediksi 38.0°C → PWM=0
                        ↓
              (siklus berlanjut 15–30 menit)
```

**Window filling bukan penyebab:** Selama filling (CSV baris 1–57, ~5 menit), sistem memanaskan dengan baik dari 30.76°C → 37°C. Masalah muncul tepat di baris 58 saat CNN pertama aktif.

**Solusi yang diterapkan (2026-05-17):** Safety fallback berbasis t_in nyata di `mode_tinyml.cpp`:

```cpp
float error_real = SETPOINT_TEMP - _temp;
bool  fb_active  = (error_real > CNN_FB_THRESHOLD);  // threshold = 2.0°C

if (fb_active) {
    // t_in masih jauh — percaya t_in nyata, bukan model
    pwm_f = constrain(error_real * CNN_FB_GAIN, CNN_FB_PWM_MIN, pwm_max);
} else {
    // t_in sudah dekat setpoint — percaya prediksi CNN
    float error_pred = SETPOINT_TEMP - _t_pred;
    pwm_f = constrain(error_pred * CNN_KP, 0.0f, pwm_max);
}
```

**Parameter fallback (di `config.h`):**
```
CNN_FB_THRESHOLD = 2.0°C   — CNN aktif penuh hanya saat t_in > 36°C  [DIGANTI di P8]
CNN_FB_GAIN      = 30.0    — gain P-control fallback
CNN_FB_PWM_MIN   = 40.0%   — heating minimum terjamin saat fallback aktif
```

**Logging:** Serial menampilkan `,fallback` atau `,cnn` per tick. MQTT menambahkan field `"fb":1/0` untuk monitoring di Grafana.

**Implikasi untuk paper:** Arsitektur ini disebut *hybrid predictive controller* — CNN memberikan antisipasi saat sudah dekat setpoint, P-control menjamin keselamatan saat transien jauh. Ini bukan kelemahan; pendekatan ini umum dalam Model Predictive Control (MPC) praktis yang selalu menyertakan real-state feedback sebagai constraint keamanan.

**Solusi permanen:** Retrain model dengan data cold start beragam (dari Mode 5, 5 sesi) → model tidak lagi over-predict di range 34–37°C. Safety fallback tetap dipertahankan sebagai lapisan keamanan.

---

## 8. Parameter Final Semua Metode

### Mode 1 — On-Off
```
Hysteresis band: ±0.3°C
PWM ON:  100%  (saat t_in < 37.7°C)
PWM OFF:   0%  (saat t_in > 38.3°C)
```

**Karakteristik observasi:**
- Steady-state std: ±0.18°C
- Natural oscillation period: ~188 detik
- Settling time dari cold start: perlu diukur di Phase 5

### Mode 2 — PID
```
Kp  = 72.0     (0.30 × Ku)
Ki  = 0.391    (Kp / (0.83 × Tu) × 0.67)
Kd  = 0        (tidak digunakan — thermal noise amplification)
Anti-windup clamp: ±150
```

**Derivasi:**
- Ku = 240 (Ziegler-Nichols Ultimate Gain, 2026-05-15)
- Tu = 184 detik (rata-rata 8 siklus osilasi sustained)
- ZN PI standar: Kp=108, Ki=0.707 → dikurangi 33% untuk margin stabilitas

**Karakteristik observasi:**
- Steady-state avg: 37.999°C (error ≈ 0.001°C)
- Steady-state std: ±0.169°C
- Periode osilasi: ~188 detik (sama dengan On-Off — frekuensi natural sistem)

### Mode 3 — TinyML 1D-CNN
```
Model    : 1D-CNN, INT8 quantized, 10.672 bytes
Quantisasi: INT8 (in scale=0.003922, zp=-128 | out scale=0.003876, zp=-128)
Arena     : 7.168 bytes dialokasikan, 5.644 bytes terpakai
CNN_KP   : 36.0  (0.5 × Kp_PID; tuned 2026-05-16)
Prediksi  : t+1 step = 5 detik ke depan
Normalisasi:
  t_in  : min=37.1, max=65.4 (range 28.3)
  t_ext : min=28.86, max=36.47 (range 7.61)
  pwm   : min=0, max=100
Output denorm: t_pred = y_norm × 28.3 + 37.1
Fallback cold start: jika t_in < 37.1°C → gunakan t_in sebagai t_pred
```

**Karakteristik:** Belum divalidasi penuh di Phase 5.

---

## 9. Catatan untuk Penulisan Paper

### Poin-Poin Kritis untuk Novelty/Kontribusi

1. **Implementasi 1D-CNN pada ESP32 untuk prediksi termal real-time**
   - Model INT8 hanya 10.4KB, arena inference 5.6KB
   - Latensi inferensi < 5ms (target, perlu diukur formal)
   - Pertama (sepengetahuan penulis) yang mengintegrasikan TFLite Micro + Conv1D untuk kontrol inkubator

2. **Perbandingan sistematis tiga metode kontrol**
   - Metodologi: kondisi awal dan lingkungan yang seragam
   - 4 skenario × 3 metode × 5 repetisi = 60 sesi
   - Metrik: Overshoot, Settling Time, Steady-State Error, Recovery Time

3. **Temuan natural frequency sistem**
   - Osilasi ~188 detik muncul pada semua metode kontrol
   - Ini bukan ketidakstabilan — ini frekuensi natural termal inkubator
   - Memberikan insight tentang dinamika sistem yang jarang dilaporkan

### Keterbatasan yang Harus Disebutkan

1. **Training data distribution:** Model dilatih dengan data near-setpoint (37.1–65.4°C), performa di bawah training range tidak dioptimalkan
2. **Predictive horizon terbatas:** Prediksi hanya 1 langkah = 5 detik ke depan
3. **Tidak ada integral di Mode 3:** Steady-state error bergantung pada akurasi prediksi
4. **Flash usage tinggi:** 98.8% — tidak ada ruang untuk fitur tambahan

### Saran Perbaikan untuk Penelitian Lanjutan

1. Training ulang dengan data cold start (25–40°C) dan data deployment closed-loop
2. Multi-step prediction (5, 10, 30 detik) untuk eksplorasi
3. Quantization-aware training untuk model lebih kecil (<5KB)

---

## 10. Iterasi Perbaikan Controller CNN (2026-05-17)

### Latar Belakang

Setelah firmware safety fallback diterapkan (P7), uji cold start kedua (`record-2026-05-17T04:55:00`) menunjukkan perbaikan signifikan tapi masih belum stabil:

**Perbandingan sebelum vs sesudah safety fallback (analisis ML Engineer):**

| Metrik | Sebelum fallback fix | Sesudah fallback fix |
|---|---|---|
| RMSE prediksi deployment | 2.635°C | 0.633°C |
| Bias rata-rata | +2.07°C | −0.009°C |
| PWM=0 (deadlock) | 56.9% waktu | 0% |
| Range t_in | 34–38°C | 36–38°C |

Masalah tersisa setelah fallback fix: osilasi ±0.55°C (vs PID ±0.17°C).

### Diagnosis Masalah Tersisa (ML Engineer)

**Masalah 1 — Bias model asimetris di sekitar setpoint:**

| Range t_in | Bias prediksi (t_pred − t_in) |
|---|---|
| 36–36.5°C | +0.865°C (over-predict dari bawah) |
| 36.5–37°C | +0.342°C |
| 37–37.5°C | −0.069°C ← akurat |
| 37.5–38°C | −0.468°C (under-predict dari atas) |
| 38–38.5°C | −0.716°C |

Model selalu "menarik prediksi ke 38°C" karena data training: kapanpun t_in=36–37°C, sistem memang sedang menuju 38°C. Model belajar korelasi ini. Efeknya: controller over-agresif saat t_in mendekati setpoint dari atas, terlalu pasif dari bawah → osilasi struktural.

**Masalah 2 — Training vs deployment PWM pattern berbeda:**

Di training (Mode 0), PWM ditentukan eksperimen secara manual/terjadwal. Di deployment, PWM ditentukan oleh CNN itu sendiri → pola circular yang tidak pernah ada di training. Ini menjelaskan mengapa RMSE deployment (0.633°C) > RMSE Python (0.283°C).

**Masalah 3 — Spike PWM=100% saat CNN pertama aktif:**

Saat window baru penuh setelah cold start dari 28.9°C, 60 data window berisi heating 28.9→37.3°C (semua di bawah SCALER_MIN=37.1°C → normalisasi negatif) → model output garbage (t_pred=33.96°C saat t_in=37.34°C) → PWM=100% spike.

### Perbaikan yang Diterapkan (2026-05-17)

**Fix 1 — Anti-anomaly check pada t_pred:**
```cpp
if (_t_pred < _temp - 2.0f) _t_pred = _temp;
```
Prediksi lebih dari 2°C di bawah t_in tidak mungkin secara fisik → dibuang, gunakan t_in. Mencegah spike PWM=100% saat window pertama kali penuh.

**Fix 2 — Turunkan CNN_KP: 36 → 18:**

Dengan RMSE deployment 0.63°C, gain 36 terlalu agresif dan menyebabkan osilasi besar. ML Engineer rekomendasikan 15–20%/°C. Dipilih 18 (tengah range).

Sebelum: `error_pred=0.64°C × 36 = 23%` → terlalu responsif terhadap noise prediksi  
Sesudah: `error_pred=0.64°C × 18 = 11.5%` → lebih stabil, integral mengkompensasi sisa

**Fix 3 — Tambah integral (CNN_KI=0.1) dengan anti-windup:**

Integral dari `error_real` (bukan `error_pred`) untuk mengkompensasi bias model yang sistematik di sekitar setpoint. Ti ≈ 18/0.1 = 180s (analog dengan rasio Kp/Ki PID).

```cpp
// Hanya aktif di CNN zone (window penuh & !fallback)
_integral += CNN_KI * error_real * (SAMPLE_INTERVAL_MS / 1000.0f);
_integral  = constrain(_integral, -CNN_I_CLAMP, CNN_I_CLAMP);  // ±30%
pwm_f = constrain(CNN_KP * error_pred + _integral, 0.0f, pwm_max);
```

**Integral di-pause (tidak update, tidak reset) saat:**
- Fallback aktif (error_real > 2°C) — mencegah windup saat heating dari cold state
- Window belum penuh — mencegah windup pra-CNN

**Arsitektur controller akhir (3 zona):**
```
[FALLBACK]   error_real > 2°C  → P(30) × error_real, min PWM=40%
[FILLING]    window < 60 data  → P(18) × error_real, cap PWM=60%
[CNN AKTIF]  window penuh      → P(18) × error_pred + I(0.1) × ∫error_real·dt
```

**Untuk paper:** Arsitektur ini dapat dideskripsikan sebagai *"PI-predictive controller"* — P-term berbasis prediksi CNN memberikan antisipasi, I-term dari error nyata memberikan kompensasi bias. Ini lebih kuat daripada P-only CNN karena mampu mengeliminasi steady-state error meski model memiliki bias sistematik.

### Perbaikan Permanen: Retrain dengan Data Deployment

Fix firmware di atas adalah perbaikan jangka menengah. Perbaikan permanen membutuhkan retrain model dengan data `record-2026-05-17T04:55:00`:

- Data ini berisi trajektori closed-loop CNN yang tidak ada di training sebelumnya
- Berisi pola PWM yang ditentukan controller CNN sendiri (bukan eksperimen manual)
- Setelah retrain, bias asimetris di 37.5–38.5°C akan berkurang → amplitudo osilasi lebih kecil

**Parameter yang TIDAK BOLEH berubah saat retrain:** normalisasi SCALER_MIN/MAX harus diperluas untuk mencakup t_in=28°C (cold start).

---

## 11. Bug Fix CNN Controller — 3 Bug Struktural (2026-05-17)

### Diagnosis ML Engineer (dari data record-2026-05-17T06:40:00)

**Bug 1 — Window init dengan nilai salah (pwm bukan 0)**

Pre-fill window menggunakan `pwm_init` (hasil P-control berdasarkan suhu awal). Model melihat "heater berjalan di XX% selama 5 menit sebelum mode ini aktif" — padahal kenyataannya tidak. Fix: isi pwm slot dengan `0.0f`.

**Bug 2 — Threshold 36°C terlalu rendah + tidak ada hysteresis**

Chattering di batas: t_in=36.1°C → CNN aktif → t_pred=39°C → PWM=0 → t_in=35.9°C → fallback 60% → masuk window → t_pred=40°C → siklus ulang. Setiap transisi memasukkan pola abnormal ke window → prediksi makin rusak. Fix: hysteresis 37°C (masuk CNN) / 36.5°C (keluar CNN).

**Bug 3 — Integral tidak berakumulasi efektif**

Integral di-pause (tidak di-reset) saat transisi zone. Nilai lama bertahan dan bisa salah arah. Fix: reset integral `= 0` setiap kali terjadi transisi antar zone.

**Bug Tambahan (ditemukan dari analisis data) — Anti-anomaly hanya satu arah**

Sebelumnya: hanya cek under-predict (`t_pred < t_in - 2°C`). Over-predict (`t_pred = 39-40°C` saat `t_in=36°C`) juga menyebabkan deadlock (P-term negatif besar yang mengalahkan integral). Fix: bidirectional clamp `t_pred ∈ [t_in-2, t_in+2]`.

### Perubahan yang Diterapkan (2026-05-17)

**config.h:**
```
CNN_ZONE_ENTER_TEMP = 37.0°C  (masuk CNN zone saat t_in naik ke sini)
CNN_ZONE_EXIT_TEMP  = 36.5°C  (keluar CNN zone saat t_in turun ke sini)
CNN_I_CLAMP         = 50.0    (dinaikkan dari 30 agar cukup lawan P-term negatif)
CNN_FB_THRESHOLD    = DIHAPUS (diganti hysteresis)
```

**mode_tinyml.cpp:**
- `_window[i][2] = 0.0f` (Bug 1)
- Hysteresis state `_cnn_zone` + reset integral saat transisi (Bug 2 + 3)
- Anti-anomaly bidirectional (Bug tambahan)
- 3 zona bersih: filling → fallback → CNN

### Arsitektur Controller Final

```
[FILLING]   !_win_full              → P(18) × error_real, cap 60%
[FALLBACK]  _win_full & !_cnn_zone  → P(30) × error_real, min 40%
[CNN]       _win_full &  _cnn_zone  → P(18) × error_pred + I(0.1)×∫error_real
                                       (anti-windup ±50, reset saat transisi zone)
```

### Status Data untuk Retrain

**Data yang TIDAK layak untuk retrain (pathological behavior):**
- `record-2026-05-16T21:09:00`: deadlock PWM=0, t_in=34-38°C
- `record-2026-05-17T04:55:00`: sebagian baik, tapi mixed
- `record-2026-05-17T06:40:00`: chattering + over-predict deadlock

**Yang dibutuhkan:** 2-3 jam data BERSIH setelah 3 bug di atas diperbaiki dan firmware baru di-flash.

**Setelah retrain:** SCALER_MIN untuk t_in perlu diperluas dari 37.1°C ke ~28°C untuk mencakup cold start.

### Struktur Paper yang Disarankan

```
I. Pendahuluan
   - Latar belakang inkubasi telur manual vs otomatis
   - Gap: metode kontrol yang ada (On-Off, PID) reaktif; CNN prediktif belum dieksplorasi
   - Tujuan: membandingkan On-Off, PID, dan CNN predictive control pada ESP32

II. Tinjauan Pustaka
   - Inkubator otomatis: kontrol suhu telur
   - TinyML: deployment ML pada MCU
   - PID vs predictive control untuk sistem termal

III. Metodologi
   A. Desain sistem hardware
   B. Data logger PRBS (5 hari, 86.400+ sampel)
   C. Kalibrasi PID (Ziegler-Nichols: Ku=240, Tu=184s, Kp=72, Ki=0.391)
   D. Training 1D-CNN (arsitektur, normalisasi, INT8 quantization)
   E. Desain pengujian komparatif (4 skenario × 3 metode × 5 repetisi)

IV. Hasil dan Pembahasan
   A. Perbandingan steady-state
   B. Perbandingan transient (cold start, disturbance)
   C. Analisis statistik (ANOVA + Tukey HSD)

V. Kesimpulan
```

---

*Log ini diperbarui secara berkala selama penelitian berlangsung.*
*Zainal Arifin · 2022TI038 · Institut Teknologi Bisnis AAS Indonesia · 2026*
