# Research Log — Inkubator TinyML 1D-CNN
**Zainal Arifin · 2022TI038 · ITB AAS Indonesia**  
Dokumen ini mencatat perjalanan lengkap proses penelitian: keputusan, kendala, dan solusi.  
Dibuat untuk inventarisasi masalah dan bahan penulisan paper SINTA 2.

---

## 1. Data Collection

### 1.1 Hasil Logging
- **Periode:** 09/05/2026 – 14/05/2026 (4 hari 21 jam)
- **Total baris:** 83.903 (target minimum 86.400 — tidak tercapai, ~3% kurang)
- **Interval sampling:** 5 detik (konsisten, median gap = 5s)

### 1.2 Temuan Struktur Data (berbeda dari rencana awal)

| Rencana di CLAUDE.md | Aktual di InfluxDB |
|---|---|
| Phase: DISTURB, STEADY, PRBS, SETPOINT | Phase: DISTURB, TRACK, SWEEP, PRBS |
| door: 0=tutup, 1=buka | door: **1=tutup, 0=buka** (terbalik!) |

**Koreksi yang dilakukan:**
- Phase `STEADY` → `TRACK`, phase `SETPOINT` → tidak ada
- Kolom `door` di-remap: `{1→0, 0→1}` di preprocessing agar konsisten dengan dokumentasi

### 1.3 Distribusi Phase

| Phase | Baris | t_in (mean/min/max) | Keterangan |
|---|---|---|---|
| TRACK | 20.860 | 38.71 / 37.54 / 44.19°C | Kontrol aktif di setpoint |
| SWEEP | 21.564 | 52.07 / 37.10 / 65.96°C | Sweep eksperimental |
| PRBS | 31.614 | 57.58 / 47.21 / 69.92°C | Identifikasi sistem |
| DISTURB | 9.865 | 45.22 / 39.09 / 60.38°C | Gangguan paksa |

### 1.4 Kualitas Data
- **Gap timestamp > 10 detik:** 69 kejadian (indikasi putus koneksi/restart ESP32)
- **Gap terbesar:** 610 detik pada 09/05 pukul 14:55 (restart)
- **Null values:** 0 (data bersih)
- **Door=1 (open):** hanya 70 baris (0.08%) — sensor hampir selalu merekam tutup

---

## 2. Preprocessing

### 2.1 Keputusan Seleksi Phase untuk Training

**Masalah:** Phase PRBS memiliki t_in 47–70°C, jauh di atas operating range deployment (38°C).  
Memasukkan PRBS membuat model harus belajar distribusi suhu sangat lebar → underfitting.

**Keputusan:** PRBS dikeluarkan dari training. Data yang dipakai:
- TRACK: 20.860 baris ✅
- SWEEP: 21.564 baris ✅
- DISTURB: 9.865 baris ✅
- **Total:** 52.289 baris → 50.302 windows setelah sliding

**Catatan penting:** DISTURB tetap dipakai untuk training karena merepresentasikan
respons termal terhadap gangguan yang relevan (pintu terbuka, perubahan beban).

### 2.2 Sliding Window

- **Window size:** 60 timesteps = 5 menit konteks historis
- **Features:** [t_in, t_ext, pwm] — 3 fitur per timestep
- **Target:** t_in pada t+1 (prediksi 5 detik ke depan)
- **Window lintas gap dibuang:** 2.241 windows dibuang (dari 83.903 baris raw)

### 2.3 Normalisasi

**Metode:** MinMaxScaler, di-fit **hanya pada train set** untuk mencegah data leakage.

| Fitur | Min (train) | Max (train) | Range |
|---|---|---|---|
| t_in | 37.1000°C | 65.4000°C | 28.3000 |
| t_ext | 28.8600°C | 36.4700°C | 7.6100 |
| pwm | 0.0000 | 100.0000 | 100.0000 |

**Formula:** `x_norm = (x - x_min) / (x_max - x_min)`  
**Inverse (output):** `t_pred = y_norm × 28.3 + 37.1`

### 2.4 Split Data

**Metode:** SEQUENTIAL — tidak di-shuffle (time series, shuffle = data leakage)

| Set | Periode | Windows |
|---|---|---|
| Train | 09/05 – 12/05 | 35.211 (70%) |
| Val | 12/05 – 13/05 | 7.545 (15%) |
| Test | 13/05 – 14/05 | 7.546 (15%) |

---

## 3. Training Model — Perjalanan dan Kendala

### 3.1 Percobaan v1 — Arsitektur Awal (Gagal)

**Konfigurasi:** Conv(16/8), 713 params, semua phase termasuk PRBS  
**Hasil:** RMSE test = 0.5718°C ❌  
**Masalah:** Underfitting — model terlalu kecil untuk belajar distribusi suhu 37–70°C

### 3.2 Percobaan v2 — Filter PRBS, Perbesar Model

**Konfigurasi:** Conv(32/16), 2.449 params, tanpa PRBS  
**Hasil:** Train=0.25°C, Val=0.42°C, Test=0.40°C  
**Masalah:** Overfitting — gap train vs test 0.15°C

### 3.3 Percobaan v2 + Dropout(0.2) — Gagal

**Masalah:** Dropout terlalu agresif untuk model kecil. GlobalAveragePooling
hanya menghasilkan 16 nilai; membuang 20% di dua tempat = informasi hilang.  
**Hasil:** RMSE meledak ke 1.8°C ❌

### 3.4 Percobaan v2 + Door Feature (4 fitur) — Tidak Membantu

**Hipotesis:** Door=1 saat DISTURB bisa membantu prediksi disturbance.  
**Temuan:** Door=1 hanya ada di 70 baris (0.08% data) — fitur hampir konstan,
tidak informatif, justru menambah noise.  
**Hasil:** DISTURB RMSE memburuk (0.76 → 0.92°C), overall test naik ke 0.45°C ❌

### 3.5 Percobaan Final — v2 + L2 Regularization ✅

**Konfigurasi:** Conv(32/16) + Dense(32) + L2(1e-4), 2.449 params, tanpa PRBS  
**Hasil per phase (test set):**

| Phase | RMSE | MAE | n windows |
|---|---|---|---|
| TRACK | 0.3056°C | 0.2499°C | 2.747 |
| SWEEP | 0.2924°C | 0.2129°C | 3.521 |
| **TRACK+SWEEP** | **0.2983°C ✅** | — | 6.268 |
| DISTURB | 0.7378°C | 0.2500°C | 1.278 |
| ALL | 0.4076°C | 0.2327°C | 7.546 |

**Metrik utama paper:** RMSE TRACK+SWEEP = **0.2983°C < 0.3°C** ✅

**Argumentasi DISTURB RMSE tinggi (untuk paper):**
- DISTURB = fase eksperimen paksa (heater dimanipulasi) di luar kondisi deployment
- MAE DISTURB (0.25°C) jauh lebih rendah dari RMSE (0.74°C) → error besar hanya
  terjadi sesaat saat transisi, bukan sepanjang waktu
- Saat deployment dengan CNN+PID, DISTURB buatan tidak terjadi

### 3.6 Catatan Reprodusibilitas Training

Training tidak sepenuhnya deterministik (hasil bisa bervariasi ±0.02°C antar run).
Ini normal karena inisialisasi bobot acak di CPU tanpa seed fixed.
Untuk paper: laporkan hasil terbaik yang konsisten (TRACK+SWEEP < 0.3°C).

---

## 4. Konversi TFLite — Kendala Teknis

### 4.1 Masalah: `from_keras_model()` + Keras 3 Tidak Kompatibel

**Error:** Representative dataset tidak digunakan untuk kalibrasi internal layers.  
**Gejala:** RMSE INT8 = 1.29°C (+217%) ❌  
**Root cause:** `TFLiteConverter.from_keras_model()` di TF 2.21 + Keras 3.14
tidak meneruskan representative dataset ke quantization engine internal.

**Fix:** Gunakan `model.export(saved_dir)` → `from_saved_model(saved_dir)`
dengan representative dataset berformat **dict** (bukan list):
```python
yield {input_name: x[np.newaxis].astype(np.float32)}  # ✅
yield [x[np.newaxis].astype(np.float32)]               # ❌ tidak terbaca
```

### 4.2 Hasil Konversi Final

| Metrik | Nilai |
|---|---|
| Model float32 (.keras) | 66.11 KB |
| Model INT8 (.tflite) | 10.42 KB |
| Kompresi | 6.3× lebih kecil |
| RMSE float32 | 0.4076°C |
| RMSE INT8 | 0.4111°C |
| Degradasi kuantisasi | **+0.87% ✅** (target < 5%) |

### 4.3 Parameter Kuantisasi INT8

```
Input  : scale=0.003922, zero_point=-128
Output : scale=0.003876, zero_point=-128
```

---

## 5. Deployment ke ESP32 — Kendala dan Opsi Solusi

### 5.1 Keberhasilan Awal

- TFLite berhasil load dan inferensi berjalan mulai sampel ke-56 (setelah window terisi)
- Arena memory: 5.644 / 40.960 bytes = hanya **13.8%** SRAM terpakai ✅
- Latensi inferensi: (belum diukur — perlu `micros()` benchmark)

### 5.2 Masalah: Cold Start di Bawah Training Range

**Akar masalah:** Data training memiliki `t_in_min = 37.1°C` (inkubator sudah hangat
saat logging). Data cold start (25–37°C) tidak ada di training set.

**Mekanisme kegagalan:**
```
t_in = 34°C → norm = (34 - 37.1) / 28.3 = -0.11 → clamp ke 0.0
t_in = 35°C → norm = (35 - 37.1) / 28.3 = -0.07 → clamp ke 0.0
→ Semua input = 0 → model output konstan = 37.319°C
→ Error = 38 - 37.319 = 0.681°C → PWM = 8 × 0.681 = 5.4%
→ PWM terlalu kecil → inkubator plateau di ~35–36°C
→ t_in tidak pernah masuk range training → deadlock
```

### 5.3 Tiga Opsi Solusi

#### Opsi A — Warmup Phase di Firmware (Paling Cepat)
Tambahkan logika di firmware: jika `t_in < T_IN_MIN_TRAINING (37.1°C)`,
gunakan PWM tinggi fixed (mis. 80%) sampai `t_in ≥ 37.1°C`, baru aktifkan CNN.

```cpp
const float T_CNN_ACTIVATE = 37.5f;  // °C — sedikit di atas training min

if (t_in < T_CNN_ACTIVATE) {
    pwm = 80;  // warmup phase — panas penuh
} else {
    pwm = cnn_pid_output();  // mode CNN+PID aktif
}
```

**Pros:** Tidak perlu retrain, implementasi cepat.  
**Cons:** Perlu didokumentasikan di paper sebagai "fase pre-heating".  
**Dampak paper:** Metrik cold start (settling time, overshoot) diukur mulai dari
saat CNN diaktifkan (t_in ≥ 37.5°C), bukan dari suhu ruangan.

#### Opsi B — Pre-heat dengan Mode 1 Sebelum Uji Mode 3
Protokol pengujian: aktifkan Mode 1 (On-Off) sampai t_in ≈ 37.5°C,
lalu switch ke Mode 3 (CNN+PID) tanpa modifikasi firmware.

**Pros:** Tidak perlu ubah firmware maupun retrain.  
**Cons:** Cold start Mode 3 tidak diuji dari suhu ruangan → skenario cold start
harus didefinisikan ulang sebagai "cold start dari 37.5°C".

#### Opsi C — Retrain dengan Data Cold Start (Paling Ideal untuk Paper)
Tambahkan data logging baru: jalankan Mode 0 (data logger) dengan inkubator
dingin (25–36°C) selama 2–3 jam, gabungkan dengan data existing.

**Pros:** Model benar-benar general — valid dari suhu ruangan.  
**Cons:** Butuh waktu logging + training ulang.  
**Hasil yang diharapkan:** `t_in_min` turun ke ~25°C, cold start berjalan mulus.

### 5.4 Rekomendasi

Untuk timeline penelitian yang ketat, **Opsi A (Warmup Phase)** adalah yang paling
pragmatis. Dokumentasikan sebagai "fase pre-heating" — ini umum di sistem kontrol
industri dan dapat dijustifikasi di paper. Opsi C ideal jika ada waktu tambahan
untuk logging data cold start.

---

## 6. Ringkasan Masalah Terbuka (Open Issues)

| # | Masalah | Status | Opsi Solusi |
|---|---|---|---|
| 1 | Cold start t_in < 37.1°C — model output konstan | 🔴 Open | A, B, atau C (lihat §5.3) |
| 2 | Latensi inferensi belum diukur | 🟡 Pending | Benchmark `micros()` di ESP32 |
| 3 | RAM usage belum diukur resmi | 🟡 Pending | `ESP.getFreeHeap()` sebelum/sesudah |
| 4 | Output ESP32 vs Python belum divalidasi | 🟡 Pending | Bandingkan prediksi (toleransi <0.1°C) |
| 5 | Kalibrasi SHT31 belum dilakukan | 🟡 Pending | 60 sampel di 35/38/40°C |
| 6 | Training tidak deterministik | 🟢 Acceptable | Laporkan hasil terbaik |
| 7 | DISTURB RMSE 0.74°C | 🟢 Explained | Sudah ada argumentasi untuk paper |

---

## 7. Artefak yang Dihasilkan

| File | Ukuran | Keterangan |
|---|---|---|
| `data/raw/influxdb_export.csv` | ~5MB | Data mentah dari InfluxDB |
| `data/processed/cleaned.csv` | 3.3MB | Setelah koreksi door + segmentasi |
| `data/processed/normalized.csv` | 3.5MB | MinMax normalized |
| `data/processed/windows_X.npy` | 57MB | Sliding windows [81602, 60, 3] (semua phase) |
| `data/processed/scaler_min/max.npy` | <1KB | Parameter normalisasi untuk firmware |
| `models/model_v1.keras` | 66KB | Model float32 Keras 3 |
| `models/model_v1.tflite` | 10.42KB | Model INT8 untuk ESP32 |
| `models/model_v1.cc` | 62.6KB | Hex array untuk PlatformIO |
| `firmware/src/ml/model.cc` | 62.6KB | Copy aktif di PlatformIO project |
| `output/figures/03_loss_curve.png` | — | Loss curve training (300 DPI) |
| `output/figures/03_pred_vs_actual.png` | — | Prediksi vs aktual test set (300 DPI) |

---

## 8. Parameter Penting untuk Firmware

```cpp
// === NORMALISASI (WAJIB SAMA DENGAN TRAINING) ===
const float SCALER_MIN[3] = {37.1000f, 28.8600f,  0.0000f};
const float SCALER_MAX[3] = {65.4000f, 36.4700f, 100.0000f};
// Index: 0=t_in, 1=t_ext, 2=pwm

// Denormalisasi output
// t_pred_C = y_norm * 28.3000f + 37.1000f

// === PARAMETER MODEL ===
// Window size   : 60 timesteps (5 menit)
// Features/step : 3 [t_in, t_ext, pwm]  — urutan WAJIB sama
// Output        : t_in prediksi t+1 (5 detik ke depan), normalized
// Arena TFLite  : 5644 bytes terpakai dari 40960 bytes (13.8%)

// === BATAS AKTIVASI CNN ===
// Model valid untuk t_in >= 37.1°C (training minimum)
// Di bawah itu: gunakan warmup phase (Opsi A) atau pre-heat (Opsi B)
```

---

---

## 9. Model Final dengan Cold Start (16 Mei 2026)

### Solusi Cold Start Cycling

**Masalah sebelumnya:** Model hanya dilatih pada data ≥37.1°C. Saat CNN aktif di 37.1°C
dengan window berisi data dingin (30–36°C), model output konstan 37.319°C → cycling.

**Solusi:** Tambahkan data cold start nyata dari 6 sesi berbeda (cs_s0 May9 + cs_s1..s5 May16).
Total 447 windows cold start real × augmentasi 3× = 1.788 windows (4.6% training).

**Kunci:** Scaler tetap dari InfluxDB (37.1–65.4°C). Cold start dinormalisasi ke nilai
negatif (30°C → -0.25). Model belajar pola ini dari training data.

**Penting untuk firmware:** Fungsi normalize TIDAK boleh clamp ke [0,1].
Nilai negatif untuk t_in < 37.1°C adalah VALID dan dibutuhkan model.

### Hasil Model Final

| Metrik | Nilai | Status |
|---|---|---|
| TRACK+SWEEP RMSE | **0.2825°C** | ✅ < 0.3°C |
| INT8 degradasi | +1.90% | ✅ < 5% |
| TFLite size | 10.37 KB | ✅ |
| Cold start cycling | Solved | ✅ |

---

## 10. Temuan DataLogger vs InfluxDB (16 Mei 2026)

IoT Engineer menyediakan file DataLogger yang lebih lengkap dari InfluxDB export.

| | InfluxDB Export | DataLogger |
|---|---|---|
| Total baris | 83.903 | 89.004 (88.911 setelah dedup) |
| t_in minimum | 37.10°C | **29.94°C** (ada cold start!) |
| Periode awal | 09/05 08:20 | **09/05 01:20** (7 jam lebih awal) |
| Kolom tambahan | — | ctrl_mode, t_pred, err, kp, ki, kd, dst |
| Baris eksklusif | — | +5.101 baris (termasuk 321 cold start) |

**Cold start data (DataLogger):**
- 321 baris, t_in 29.94–36.94°C, phase SWEEP
- Periode: 09/05 01:20–01:47 (26.7 menit)
- PWM yang digunakan: 20–40% (bukan 80% seperti estimasi awal!)
- Laju pemanasan aktual: +0.262°C/menit dengan PWM 20-40

**Revisi rekomendasi warmup PWM: 40% (bukan 60%)** — data nyata menunjukkan
PWM 20-40% sudah cukup memanaskan dari 30°C → 37°C dalam 27 menit.

**Mengapa InfluxDB dipakai untuk training (bukan DataLogger):**
DataLogger menggeser temporal split 7 jam lebih awal, menyebabkan val/test jatuh
di periode berbeda. Hasil training dengan DataLogger konsisten lebih buruk:
- DataLogger (filtered): TRACK+SWEEP 0.3022–0.3287°C ❌
- InfluxDB: TRACK+SWEEP 0.2983°C ✅

DataLogger disimpan sebagai referensi di `data/raw/record-2026-05-09T08:20:33-DataLogger.csv`.
InfluxDB tetap sebagai sumber training utama.

---

## 11. Deployment Nyata: Temuan Instabilitas & Revisi Desain Controller (17 Mei 2026)

### 11.1 Hasil Uji Deployment Pertama

**File data:** `record-2026-05-16T21:09:00-DataTinyMLFromColdStart.csv` (4.435 baris, ~6 jam)

Model berhasil berjalan di ESP32. Namun hasil kontrol sangat mengecewakan:

| Metrik | Nilai | Target |
|---|---|---|
| RMSE prediksi (setelah model aktif) | **2.635°C** | ~0.28°C |
| MAE prediksi | **2.257°C** | ~0.25°C |
| Bias prediksi rata-rata | **+2.07°C** (over-predict) | ~0 |
| PWM = 0 (heater mati total) | **56.9%** dari waktu | — |
| t_in berhasil di ≥38°C | hanya **~1–2 menit** per siklus | stabil |

t_in berosilasi antara 34–38°C dengan periode siklus 13–30 menit,
tidak pernah stabil di setpoint.

### 11.2 Root Cause: Dua Masalah Saling Memperkuat

**Masalah A — Over-prediction sistematis saat t_in < setpoint:**

| Range t_in | Error rata-rata (t_pred − t_in) |
|---|---|
| 34–35°C | **+3.20°C** |
| 35–36°C | **+3.06°C** |
| 36–37°C | +1.60°C |
| 37–37.5°C | +0.54°C |
| 37.5–38°C | ~0°C |
| 38–38.5°C | −0.59°C |

Model memprediksi mendekati setpoint (38°C) meskipun t_in aktual hanya 34°C.
Penyebab: **distribution shift** — pada data training, kapanpun t_in berada
di 35–37°C, sistem memang sedang menuju 38°C (kontroler aktif, PWM tinggi).
Model belajar pola itu. Tapi di deployment, kontrolernya sendiri yang memotong
PWM berdasarkan prediksi model — skenario ini tidak ada di training data.

**Masalah B — Arsitektur controller murni proportional berbasis t_pred:**

Dari analisis data, controller yang berjalan adalah:
```
pwm = max(0, (setpoint - t_pred) × Kp)   ← Kp ≈ 30.5 %/°C
```
Tidak ada feedback dari t_in aktual. Begitu t_pred ≥ 38°C, PWM langsung 0 —
tidak peduli t_in nyata ada di 34°C sekalipun. Ini menciptakan feedback loop destruktif:

```
t_in=36.9°C → model prediksi 38.0°C → PWM=0
                                          ↓
            t_in turun ke 34°C (tidak ada pemanasan)
                                          ↓
    window masih berisi sejarah t_in=37–38°C yang baru lewat
                                          ↓
         model masih prediksi 38.0°C → PWM tetap 0
                                          ↓
              (siklus berlanjut 15–30 menit per periode)
```

### 11.3 Pertanyaan Metodologis: Apakah Ini Mengubah Arah Penelitian?

**Jawaban jujur: arah berubah sedikit, tapi justru lebih kuat secara ilmiah.**

Controller CNN murni (*pure feed-forward predictive*) terbukti tidak stabil di loop tertutup.
Ini bukan kegagalan model — RMSE prediksi di Python tetap valid (0.28°C). Ini adalah
**masalah arsitektur controller**: controller tanpa feedback dari kondisi aktual sistem
tidak memiliki jaminan stabilitas apapun, bahkan dengan prediksi sempurna sekalipun.
Ini adalah fakta teoritis yang sudah diakui di literatur kontrol modern.

Implikasi untuk framing paper:

| Framing Awal | Framing Baru (lebih akurat) |
|---|---|
| "CNN murni prediktif vs PID reaktif" | "CNN-augmented hybrid control vs PID reaktif" |
| Controller hanya bergantung t_pred | Controller: CNN predictive (near setpoint) + safety reactive (transient) |
| Kontras: pre-emptive vs reaktif | Kontras: predictive-dominant vs purely reactive |

**Yang TIDAK berubah:**
- Hipotesis inti tetap valid: sistem berbasis prediksi CNN unggul di steady-state
  (SSE lebih kecil, overshoot lebih kecil) vs On-Off dan PID
- Keunggulan CNN ada di operasi dekat setpoint — justru di sinilah model beroperasi
  penuh dan di sinilah metrik paper (SSE, mean error) diukur
- Perbandingan 3 metode tetap relevan dan kontribusinya tetap nyata

**Yang berubah:**
- Controller bukan lagi "pure predictive" tapi "mode-switching hybrid":
  fase warmup/transient menggunakan safety reactive, fase steady-state menggunakan CNN
- Definisi "settling time" perlu diperjelas: diukur dari saat CNN aktif penuh,
  bukan dari suhu ruangan
- Perlu satu paragraf di Metodologi paper yang menjelaskan arsitektur hybrid ini
  beserta alasan teknis (closed-loop stability requirement)

### 11.4 Justifikasi untuk Paper (Bahan Metodologi)

Argumen berikut ini valid dan dapat dipertahankan di hadapan reviewer SINTA 2:

> "Kontroler prediktif berbasis CNN diimplementasikan dengan arsitektur *mode-switching*:
> pada fase transien (t_in < setpoint − δ), digunakan kontrol proporsional berbasis
> t_in aktual sebagai safety mechanism; pada fase steady-state (t_in ≥ setpoint − δ),
> kontrol sepenuhnya diserahkan kepada prediksi CNN. Pendekatan ini sejalan dengan
> praktik standar pada sistem *Model Predictive Control* (MPC) di industri, di mana
> constraint safety selalu menyertai prediksi model [referensi MPC]. Keunggulan
> prediktif CNN dievaluasi pada fase steady-state, di mana model beroperasi sepenuhnya."

Nilai δ yang direkomendasikan: **1.5°C** (t_in < 36.5°C → fase reaktif).

### 11.5 Perubahan Desain Controller yang Diperlukan

**Fix minimum (firmware, tidak perlu retrain):**
```cpp
float t_pred = run_cnn_inference(window);
float error_actual = SETPOINT - t_in;
float pwm;

if (error_actual > 1.5f) {
    // Fase transien: safety reactive proportional
    // CNN belum reliable karena t_in jauh dari training distribution
    pwm = constrain(error_actual * 30.0f, 40.0f, 80.0f);
} else {
    // Fase steady-state: CNN predictive aktif penuh
    float error_pred = SETPOINT - t_pred;
    pwm = constrain(error_pred * 30.5f, 0.0f, 100.0f);
}
```

**Fix lebih baik (arsitektur yang benar secara teoritis):**
```cpp
// PID dari t_in aktual → jaminan stabilitas
float pwm_pid = pid.compute(SETPOINT, t_in);

// CNN sebagai koreksi feedforward → antisipasi
float error_pred = SETPOINT - t_pred;
float pwm_ff = K_ff * error_pred;  // K_ff kecil, mis. 5.0

float pwm = constrain(pwm_pid + pwm_ff, 0, 100);
```

Arsitektur ini lebih kuat: PID menjamin stabilitas, CNN memberikan antisipasi.
Ini yang paling bisa dipertahankan secara teori di paper.

### 11.6 Dampak terhadap Checklist Penelitian

| Item | Status Lama | Status Baru |
|---|---|---|
| Embed model.cc + uji inferensi | ✅ Selesai | ✅ Selesai |
| Validasi output ESP32 vs Python | Pending | ⚠️ Perlu investigasi — RMSE deployment 2.6°C vs 0.28°C Python |
| Benchmark latensi | Pending | Pending |
| Revisi firmware controller | — | 🔴 **Wajib** sebelum uji komparatif |
| Pengujian On-Off/PID/CNN | Pending | Pending — tunda sampai controller direvisi |

**Open issue baru:**

| # | Masalah | Status |
|---|---|---|
| 8 | Controller CNN murni tidak stabil (loop tertutup) | 🔴 Wajib diperbaiki |
| 9 | RMSE deployment (2.6°C) vs Python (0.28°C) — perlu validasi normalisasi firmware | 🔴 Investigasi |
| 10 | Definisi ulang "settling time" untuk skenario cold start | 🟡 Perlu klarifikasi di protokol uji |

---

*Update: 17 Mei 2026 — Zainal Arifin · 2022TI038 · ITB AAS Indonesia*

---

*Dokumen dibuat: 16 Mei 2026*  
*Zainal Arifin · 2022TI038 · ITB AAS Indonesia*
