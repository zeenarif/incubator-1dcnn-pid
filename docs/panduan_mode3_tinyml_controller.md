# Panduan Mode 3 — Alur Kerja Controller TinyML 1D-CNN

> **Firmware:** ESP32 Inkubator Telur  
> **File kode:** `src/modes/mode_tinyml.cpp`, `src/config/config.h`  
> **Terakhir diperbarui:** 2026-05-17

---

## Gambaran Besar

Mode 3 menggunakan model 1D-CNN untuk **memprediksi suhu 5 detik ke depan**, lalu
menggunakan selisih antara prediksi dan setpoint sebagai dasar pengendalian heater (AC
Dimmer). Setiap 5 detik sistem membaca sensor, memperbarui window, menjalankan inferensi,
dan menghitung PWM.

```
Sensor SHT31 → Sliding Window [60 × 3] → Normalisasi → TFLite INT8 → Denormalisasi
                                                                             ↓
                                                                        t_pred (°C)
                                                                             ↓
                                                              Anti-anomaly + Hysteresis
                                                                             ↓
                                                                    PWM Computation
                                                                             ↓
                                                                     AC Dimmer (0–100%)
```

---

## 1. Input Model — Sliding Window

### Struktur

Model menerima **60 timestep × 3 fitur** = 180 nilai sebagai input:

```
Window [60 × 3]:
  ┌──────────┬────────┬────────┬─────┐
  │  t-59    │  t_in  │  t_ext │ pwm │  ← data terlama
  │  t-58    │  t_in  │  t_ext │ pwm │
  │   ...    │  ...   │  ...   │ ... │
  │  t-1     │  t_in  │  t_ext │ pwm │
  │  t=0     │  t_in  │  t_ext │ pwm │  ← data terbaru (sekarang)
  └──────────┴────────┴────────┴─────┘
```

### Tiga Fitur

| Indeks | Fitur | Satuan | Keterangan |
|--------|-------|--------|------------|
| 0 | `t_in` | °C | Suhu dalam inkubator (SHT31 internal, sudah dikalibrasi) |
| 1 | `t_ext` | °C | Suhu lingkungan luar (SHT31 eksternal) |
| 2 | `pwm` | % | PWM heater yang **sudah diterapkan** pada tick tersebut |

> **Penting:** `pwm` dalam window adalah PWM **tick sebelumnya** — bukan tick saat ini.
> Ini mencerminkan "seberapa banyak panas yang sudah diinjeksikan" yang hasilnya baru
> terlihat pada t_in berikutnya.

### Cara Window Diisi

Window adalah **circular buffer** dengan panjang 60. Setiap 5 detik, satu slot ditimpa:

```cpp
_window[_win_idx][0] = _temp;       // t_in sekarang
_window[_win_idx][1] = _temp_ext;   // t_ext sekarang
_window[_win_idx][2] = (float)_pwm; // pwm tick sebelumnya

_win_idx = (_win_idx + 1) % CNN_WINDOW_SIZE;
if (_win_idx == 0) _win_full = true; // full setelah 60 tick = 5 menit
```

### Inisialisasi Window — Pre-fill dan Langsung Aktif

Saat Mode 3 pertama kali diaktifkan, seluruh window diisi dengan **duplikat pembacaan
pertama**, lalu model langsung berjalan dari tick 1:

```cpp
// pwm_prefill proporsional terhadap jarak dari setpoint, cap 20%
float pwm_prefill = constrain(CNN_KP * (SETPOINT - t_init), 0.0f, 20.0f);

for (int i = 0; i < 60; i++) {
    _window[i][0] = t_init;       // suhu saat ini (diduplikat ke semua slot)
    _window[i][1] = t_ext_init;   // suhu luar saat ini
    _window[i][2] = pwm_prefill;  // heater virtual: 0% (di setpoint) → 20% (cold)
}
_win_full = true;  // model langsung aktif dari tick 1
```

Model "membaca" konteks virtual: *"inkubator berada di t_init selama 5 menit terakhir
dengan heater berjalan di pwm_prefill%."* Nilai pwm_prefill adaptif:

| t_init | pwm_prefill | Efek pada prediksi |
|--------|------------|-------------------|
| 32°C | 20% | Model tahu ada heating → prediksi wajar |
| 37.5°C | 9% | Model tahu heater kecil → prediksi stabil |
| 38°C | 0% | Model tahu heater off → prediksi wajar untuk di setpoint | Setiap tick, satu slot pre-fill digantikan data nyata, sehingga
akurasi model meningkat secara bertahap.

**Keuntungan vs pendekatan lama (tunggu 60 tick):**
- Saat CNN zone aktif (t_in mencapai 37°C, ≈3 menit), window sudah berisi 30-35 data
  nyata — jauh lebih akurat dari 0 data nyata pada pendekatan lama
- Tidak ada jeda 5 menit sebelum model mulai berprediksi
- t_pred terlihat di CSV dari tick pertama (untuk diagnostik)

---

## 2. Normalisasi — Sebelum Masuk Model

Nilai raw sensor harus dinormalisasi sebelum dikirim ke model. Normalisasi menggunakan
**MinMaxScaler yang sama persis dengan saat training**:

```
x_normalized = (x - min) / (max - min)
```

### Parameter Normalisasi

| Fitur | Min | Max | Range |
|-------|-----|-----|-------|
| t_in (°C) | 37.1000 | 65.4000 | 28.3000 |
| t_ext (°C) | 28.8600 | 36.4700 | 7.6100 |
| pwm (%) | 0.0000 | 100.0000 | 100.0000 |

```cpp
static const float SCALER_MIN[3] = {37.1000f, 28.8600f,  0.0000f};
static const float SCALER_MAX[3] = {65.4000f, 36.4700f, 100.0000f};

float v_norm = (v - SCALER_MIN[f]) / (SCALER_MAX[f] - SCALER_MIN[f]);
```

> **Catatan penting:** Hasil normalisasi **bisa negatif** untuk t_in < 37.1°C
> (di bawah training minimum). Nilai negatif ini **TIDAK di-clamp ke 0** — model dilatih
> untuk menerimanya sebagai representasi cold start.

**Contoh:**
- t_in = 36°C → (36 - 37.1) / 28.3 = **−0.039** (negatif, valid)
- t_in = 38°C → (38 - 37.1) / 28.3 = **+0.032**
- pwm = 40%  → (40 - 0) / 100 = **0.40**

---

## 3. Inferensi — Menjalankan Model

### Model: INT8 Quantized

Model dikompilasi dalam format **INT8 quantized** untuk efisiensi memori ESP32:
- Ukuran: ~10.6 KB
- Arena RAM: 5.644 bytes
- Latensi: < 5 ms

Input float ternormalisasi harus di-**quantize** ke int8 sebelum masuk model:

```cpp
// Quantize: float_norm → int8
int q = (int)roundf(v_norm / in_scale) + in_zero_point;
tensor_input[idx] = (int8_t)constrain(q, -128, 127);

// Parameter model: in_scale = 0.003922, in_zero_point = -128
```

### Cara Input Tensor Diisi

Data dalam window dimasukkan **secara kronologis** (oldest → newest):

```cpp
for (int t = 0; t < 60; t++) {
    int src = (_win_idx + t) % 60;   // mulai dari slot tertua
    for (int f = 0; f < 3; f++) {
        float v_norm = normalize(_window[src][f], f);
        tensor[t * 3 + f] = quantize(v_norm);
    }
}
```

Tata letak tensor input (flat array 180 elemen):

```
[t_in_t-59, t_ext_t-59, pwm_t-59,   ← timestep tertua
 t_in_t-58, t_ext_t-58, pwm_t-58,
 ...
 t_in_t-1,  t_ext_t-1,  pwm_t-1,
 t_in_t-0,  t_ext_t-0,  pwm_t-0]    ← timestep terbaru
```

### Output Model

Model menghasilkan **satu nilai** (skalar): prediksi t_in pada t+1 (5 detik ke depan),
dalam skala normalisasi.

```cpp
// Dequantize: int8 → float_norm
float y_norm = (output_int8 - out_zero_point) * out_scale;
// Parameter model: out_scale = 0.003876, out_zero_point = -128
```

---

## 4. Denormalisasi — Kembali ke Derajat Celsius

```cpp
t_pred = y_norm * T_IN_RANGE + T_IN_MIN
       = y_norm * 28.3 + 37.1
```

**Contoh:**
- y_norm = 0.032 → t_pred = 0.032 × 28.3 + 37.1 = **38.0°C**
- y_norm = −0.039 → t_pred = −0.039 × 28.3 + 37.1 = **36.0°C**

---

## 5. Anti-Anomaly Check

Sebelum prediksi digunakan, dilakukan **pengecekan fisika bidireksional** untuk membuang
prediksi yang tidak masuk akal:

```cpp
// Under-predict: lebih dari 2°C di bawah suhu aktual (tidak mungkin dalam 5 detik)
if (t_pred < t_in - 2.0f)  t_pred = t_in;

// Over-predict: lebih dari 1.5°C di atas suhu aktual
if (t_pred > t_in + 1.5f)  t_pred = t_in + 1.5f;
```

**Mengapa ini diperlukan:**
- **Under-predict:** Saat window baru penuh setelah cold start, model melihat pola
  heating yang tidak ada dalam training → prediksi garbage (33-35°C saat t_in=37°C) →
  menyebabkan PWM spike ke 100%
- **Over-predict:** Model memprediksi suhu terlalu tinggi (39-40°C saat t_in=37°C) →
  P-term sangat negatif → PWM = 0 → deadlock

---

## 6. Hysteresis CNN Zone

CNN **tidak langsung aktif** saat window penuh. Ada sistem hysteresis berbasis t_in:

```
CNN masuk  : t_in naik ke ≥ 37.0°C  (model akurat di range ini)
CNN keluar : t_in turun ke < 36.5°C  (kembali ke fallback)
Band 0.5°C mencegah chattering di batas
```

```cpp
if (!_cnn_zone && t_in >= 37.0f) _cnn_zone = true;   // masuk CNN
if ( _cnn_zone && t_in < 36.5f)  _cnn_zone = false;  // keluar CNN
```

**Alasan:** Model dilatih dengan data di range 37.1–65.4°C. Di bawah 37°C, model
over-predicts secara sistematik (+0.5 s.d. +2°C) → lebih baik pakai P-control langsung.

---

## 7. Komputasi PWM — Dua Zona

Berdasarkan kondisi window dan hysteresis, sistem memilih satu dari tiga strategi:

### Zona 1: FALLBACK (t_in < 36.5°C — di luar CNN zone)

```
Kondisi: _win_full && !_cnn_zone
Formula: pwm = CNN_FB_GAIN × error_real  (minimum 40%, max 100%)
         error_real = 38.0 - t_in

Contoh: t_in = 36°C → error = 2°C → pwm = 30 × 2 = 60%
        t_in = 35°C → error = 3°C → pwm = 30 × 3 = 90%
        t_in = 37°C → error = 1°C → pwm = 30 × 1 = 40% (dipaksa minimum)
```

CNN_FB_GAIN = 30, minimum PWM = 40% (menjamin heating tidak berhenti sepenuhnya).

### Zona 2: CNN AKTIF (t_in ≥ 37°C)

```
Kondisi: _win_full && _cnn_zone
Formula: pwm = CNN_KP × error_pred + integral
         error_pred = 38.0 - t_pred    (dari model)
         integral  += CNN_KI × error_real × 5   (tiap 5 detik)
         integral   = clamp(integral, -50, +50)
```

**Dua komponen:**

| Komponen | Formula | Fungsi |
|----------|---------|--------|
| **P-predictive** | `CNN_KP × (38 - t_pred)` | Antisipasi berbasis prediksi CNN |
| **I-compensator** | `∫ CNN_KI × (38 - t_in) dt` | Kompensasi bias model sistematik |

**Integral detail:**
- Berbasis `error_real` (t_in aktual, bukan prediksi) → tidak terpengaruh bias model
- Persisten lintas siklus — tidak di-reset saat transisi zone
- Anti-windup ±50% — mencegah akumulasi berlebihan
- Di-reset ke 0 hanya saat `mode_tinyml_init()` dipanggil (mode switch)

**Contoh skenario:**

```
t_in = 37.5°C, t_pred = 38.9°C (over-predict)
error_pred = 38.0 - 38.9 = -0.9°C
P = -0.9 × 18 = -16.2%

error_real = 38.0 - 37.5 = 0.5°C
integral += 0.1 × 0.5 × 5 = +0.25  (per tick)

Setelah 65 tick dengan error_real ≈ 0.5°C:
  integral ≈ 65 × 0.25 = 16.25%

pwm = constrain(-16.2 + 16.25, 0, 100) = 0.05% ≈ 0%
```

```
t_in = 37.0°C, t_pred = 38.5°C (capped oleh anti-anomaly)
error_pred = 38.0 - 38.5 = -0.5°C
P = -0.5 × 18 = -9%

integral sudah = 20% (terakumulasi)

pwm = constrain(-9 + 20, 0, 100) = 11%
```

---

## 8. Diagram Alur Lengkap (Per Tick, setiap 5 detik)

```
┌─────────────────────────────────────────────────────────┐
│                    SETIAP 5 DETIK                       │
└─────────────────────────────────────────────────────────┘
         │
         ▼
  Baca sensor SHT31
  t_in, t_ext, rh
         │
         ▼
  Simpan ke window[_win_idx] = {t_in, t_ext, pwm_sebelumnya}
  _win_idx++; if wrap → _win_full = true
         │
         ▼
  ┌──────────────────────┐
  │   Jalankan inferensi │
  │   (jika !_win_full:  │
  │    kembalikan t_in)  │
  └──────────────────────┘
         │
         ▼  t_pred (°C)
  ┌──────────────────────────────┐
  │   Anti-anomaly check         │
  │   t_pred = clamp(t_pred,     │
  │     t_in - 2.0,  t_in + 1.5)│
  └──────────────────────────────┘
         │
         ▼
  ┌──────────────────────────────┐
  │   Update hysteresis          │
  │   t_in ≥ 37.0 → CNN masuk   │
  │   t_in < 36.5 → CNN keluar  │
  └──────────────────────────────┘
         │
         ▼
  ┌──────────────────────────────────────────────────────┐
  │              PILIH ZONA PWM                          │
  ├──────────────┬─────────────────┬────────────────────┤
  │  !_win_full  │ !_cnn_zone      │  _cnn_zone         │
  │  FILLING     │  FALLBACK       │  CNN AKTIF         │
  │              │                 │                    │
  │  P = 18 ×   │  P = 30 ×       │  P = 18 × error_   │
  │  error_real  │  error_real     │      pred          │
  │  cap 60%    │  min 40%        │  I += 0.1×err_r×5  │
  │             │                 │  I = clamp(I,±50)  │
  │             │                 │  PWM = P + I       │
  └─────────────┴─────────────────┴────────────────────┘
         │
         ▼
  _pwm = (uint8_t)constrain(pwm_f, 0, pwm_max)
  hal_dimmer_set(_pwm)
         │
         ▼
  Log ke Serial + MQTT
  (pwm yang ter-log = total P+I = nilai aktual ke heater)
```

---

## 9. Parameter Lengkap Saat Ini

```cpp
// Model
CNN_WINDOW_SIZE    = 60       // timestep per window (5 menit)
CNN_FEATURES       = 3        // t_in, t_ext, pwm

// Normalisasi (WAJIB sama dengan training)
SCALER_MIN         = {37.1, 28.86, 0.0}
SCALER_MAX         = {65.4, 36.47, 100.0}

// Controller
SETPOINT_TEMP      = 38.0°C
CNN_KP             = 18.0     // gain P-predictive
CNN_KI             =  0.1     // gain integral (Ti = 180 detik)
CNN_I_CLAMP        = 50.0     // anti-windup (±50%)
CNN_ZONE_ENTER     = 37.0°C  // hysteresis masuk CNN
CNN_ZONE_EXIT      = 36.5°C  // hysteresis keluar CNN

// Fallback
CNN_FB_GAIN        = 30.0
CNN_FB_PWM_MIN     = 40.0%

// Anti-anomaly
LOWER_BOUND        = t_in - 2.0°C
UPPER_BOUND        = t_in + 1.5°C
```

---

## 10. Mengapa Tidak Pure CNN?

Controller ini adalah **PI-predictive hybrid** — bukan pure CNN. Alasannya:

| Masalah | Akibat jika pure CNN | Solusi |
|---------|---------------------|--------|
| Model over-predicts saat t_in < 37°C | PWM = 0, deadlock | Fallback P-control |
| Model bias sistematik di setpoint | Steady-state error | Integral dari error_real |
| Window berisi data cold start → garbage prediction | PWM spike 100% | Anti-anomaly check |
| Chattering di batas threshold | Pola window rusak | Hysteresis 0.5°C |

Secara akademis, arsitektur ini dapat dideskripsikan sebagai:
> *"Model Predictive Control (MPC) dengan feedback aktual sebagai kompensator bias,
> dilengkapi safety fallback berbasis P-control untuk kondisi di luar distribusi training."*

---

*Zainal Arifin · 2022TI038 · Institut Teknologi Bisnis AAS Indonesia · 2026*
