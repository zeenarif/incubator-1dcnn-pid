# Panduan Langkah Selanjutnya — Setelah Data Logger 5 Hari

> Status per 2026-05-14: Data Mode 0 (5 hari) sudah terkumpul di InfluxDB.  
> Preprocessing + training 1D-CNN sedang berjalan.  
> Zainal Arifin · 2022TI038 · Institut Teknologi Bisnis AAS Indonesia

---

## Gambaran Besar

```
[SEKARANG]
  Training 1D-CNN berjalan di PC/server
       │
       ├─── PARALEL: Mode 1 On-Off → karakterisasi baseline
       │
       └─── PARALEL: Mode 2 PID → step response → tuning Kp/Ki/Kd
       
[SETELAH TRAINING SELESAI]
  Konversi model → model.cc → embed ke firmware
       │
       └─── Mode 3 TinyML → uji inferensi → benchmark
       
[FASE 5 — NANTI]
  Pengujian komparatif resmi: On-Off vs PID vs TinyML
  (60 sesi, kondisi seragam)
```

**Penting:** Data On-Off dan PID yang dikumpulkan sekarang adalah untuk **tuning dan karakterisasi awal**, bukan data komparatif resmi. Sesi resmi Phase 5 dilakukan setelah Mode 3 siap, agar kondisi ketiga metode bisa dibandingkan secara seragam.

---

## Bagian A — Sambil Menunggu Training Selesai

### A.1 Menjalankan Mode 1 (On-Off Control)

**Tujuan:** Melihat karakteristik baseline — seberapa besar overshoot, seberapa lama settling time, seberapa stabil di steady-state. Ini juga verifikasi bahwa Mode 1 berjalan benar sebelum Phase 5.

#### Cara Mengaktifkan Mode 1

1. Putar rotary encoder sampai LCD menampilkan:
   ```
   Mode? [ON-OFF ]
   Hold SW to OK
   ```
2. Tekan dan tahan tombol SW lebih dari 1 detik, lalu lepas
3. Serial monitor mencetak: `# Mode switched to 1`
4. LCD berganti ke tampilan mode aktif

#### Yang Perlu Diamati

Biarkan berjalan minimal **2–4 jam** dari kondisi suhu ruangan (cold start). Catat:

| Yang dicatat | Cara mengukur |
|---|---|
| Overshoot (°C) | Suhu tertinggi yang dicapai sebelum stabil, dikurangi 38°C |
| Settling time (menit) | Waktu dari nyala sampai suhu masuk dan tetap dalam ±0.5°C dari 38°C |
| Steady-state error (°C) | Selisih rata-rata suhu saat stabil dengan 38°C |
| Frekuensi switching (kali/jam) | Berapa kali relay/dimmer on-off per jam saat steady |

#### Disturbance Test (opsional, tapi berguna)

Setelah suhu stabil di 38°C, buka pintu inkubator selama **30 detik**, lalu tutup. Catat:
- Berapa °C suhu turun?
- Berapa menit untuk kembali ke 38°C?

Data ini menjadi referensi perbandingan saat Mode 3 diuji dengan skenario yang sama.

---

### A.2 Menjalankan Mode 2 (PID) — Step Response untuk Tuning

**Tujuan:** Mendapatkan parameter Kp/Ki/Kd yang tepat menggunakan metode Ziegler-Nichols Ultimate Gain.

#### Persiapan

Pastikan `mode_pid.cpp` sudah dikompilasi dengan parameter awal:
```cpp
float Kp = 2.0f;   // mulai rendah
float Ki = 0.0f;   // nonaktifkan dulu
float Kd = 0.0f;   // nonaktifkan dulu
```

#### Cara Mengaktifkan Mode 2

1. Putar rotary encoder sampai LCD menampilkan:
   ```
   Mode? [PID    ]
   Hold SW to OK
   ```
2. Tekan dan tahan tombol SW lebih dari 1 detik, lalu lepas
3. Serial monitor mencetak: `# Mode switched to 2`

#### Prosedur Ziegler-Nichols Ultimate Gain

Lakukan ini **hanya dengan Kp, Ki=0, Kd=0**:

1. Set Kp awal = 2.0, biarkan berjalan dari cold start sampai stabil atau osilasi
2. Jika tidak osilasi, naikkan Kp (2 → 4 → 6 → 8 → ...)
3. Temukan **Ku** (Ultimate Gain) = nilai Kp terkecil yang menyebabkan osilasi berkelanjutan (tidak membesar, tidak mengecil)
4. Ukur **Tu** (Ultimate Period) = periode satu siklus osilasi dalam detik

#### Hitung Parameter PID dari Ku dan Tu

| Tipe Kontrol | Kp | Ki | Kd |
|---|---|---|---|
| P saja | 0.50 × Ku | — | — |
| PI | 0.45 × Ku | 1.2 / Tu | — |
| PID klasik | 0.60 × Ku | 2.0 / Tu | 0.125 × Tu |

Gunakan nilai **PID klasik** sebagai titik awal, lalu fine-tune secara empiris.

#### Verifikasi Setelah Tuning

Setelah parameter ditetapkan, biarkan Mode 2 berjalan minimal **2 jam** dan pastikan:
- Tidak ada osilasi berkelanjutan
- Settling time < 30 menit dari cold start
- Steady-state error < ±0.5°C
- Overshoot < 2°C

---

## Bagian B — Setelah Training Selesai

### B.1 Evaluasi Hasil Training

Sebelum lanjut, pastikan model memenuhi target:

| Metrik | Target | Hasil Anda |
|---|---|---|
| RMSE test set | < 0.3°C | ... |
| MAE test set | < 0.2°C | ... |
| Degradasi RMSE setelah INT8 | < 5% | ... |
| Ukuran model INT8 | < 5 KB | ... |

Jika RMSE > 0.3°C, jangan langsung deploy — cek dulu:
- Apakah ada data yang corrupt (ts = 0, t_in > 50°C atau < 20°C)?
- Apakah normalisasi sudah benar (min-max dari training set, bukan seluruh dataset)?
- Apakah sliding window [60 × 3] sudah benar urutannya?

---

### B.2 Konversi Model ke model.cc

Jalankan pipeline konversi di PC/server:

```bash
# 1. Simpan model Keras
model.save("incubator_1dcnn.h5")

# 2. Konversi ke TFLite Float32
converter = tf.lite.TFLiteConverter.from_keras_model(model)
tflite_model = converter.convert()
open("model_f32.tflite", "wb").write(tflite_model)

# 3. Kuantisasi INT8 (butuh representative dataset)
def representative_dataset():
    for sample in X_train[:500]:
        yield [sample.reshape(1, 60, 3).astype(np.float32)]

converter.optimizations = [tf.lite.Optimize.DEFAULT]
converter.representative_dataset = representative_dataset
converter.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
converter.inference_input_type = tf.int8
converter.inference_output_type = tf.int8
tflite_int8 = converter.convert()
open("model_int8.tflite", "wb").write(tflite_int8)

# 4. Konversi ke model.cc (xxd)
# Jalankan di terminal:
# xxd -i model_int8.tflite > model.cc
# Lalu rename array di dalam file ke: g_model dan g_model_len
```

Setelah konversi, **verifikasi ukuran**:
```bash
ls -lh model_int8.tflite   # harus < 5 KB
wc -c model_int8.tflite    # dalam bytes
```

---

### B.3 Embed model.cc ke Firmware

1. Salin `model.cc` ke `src/ml/model.cc` (ganti file lama)
2. Pastikan nama array di `model.cc` sesuai dengan yang di-refer `mode_tinyml.cpp`:
   ```cpp
   // model.cc harus punya:
   const unsigned char g_model[] = { ... };
   const unsigned int g_model_len = ...;
   ```
3. Build dan upload:
   ```bash
   pio run --target upload
   ```

---

### B.4 Uji Inferensi Mode 3

#### Cara Mengaktifkan Mode 3

1. Putar rotary encoder sampai LCD menampilkan:
   ```
   Mode? [TINYML ]
   Hold SW to OK
   ```
2. Tekan dan tahan tombol SW lebih dari 1 detik, lalu lepas

#### Yang Perlu Dicek di Serial Monitor

```
# Mode switched to 3
# TFLite init OK, model loaded
# Sliding window: filling... (60 samples needed)
# Inference OK: T_pred=37.84, T_act=37.82, err=0.02
```

Jika muncul `TFLite init FAIL` — cek:
- Ukuran `TENSOR_ARENA_SIZE` di `mode_tinyml.cpp` (naikkan jika perlu, target < 30 KB)
- Array `g_model` dan `g_model_len` sudah terekspos dengan benar

#### Benchmark Latensi

Jalankan 100 siklus inferensi dan ukur:
```cpp
uint32_t t0 = micros();
for (int i = 0; i < 100; i++) model_infer(window);
uint32_t elapsed = micros() - t0;
Serial.printf("# Avg inference: %lu us\n", elapsed / 100);
```
Target: < 5.000 µs per inferensi.

#### Verifikasi Akurasi vs Python

Ambil 10 sampel dari test set, jalankan inferensi di Python dan di ESP32, bandingkan output:
```
Sample | Python (°C) | ESP32 (°C) | Selisih
   1   |   37.84     |   37.85    |  0.01°C  ✓
   2   |   38.12     |   38.11    |  0.01°C  ✓
```
Toleransi: selisih < 0.1°C (efek kuantisasi INT8).

---

### B.5 Ukur RAM

Tambahkan ke `mode_tinyml.cpp` saat inisialisasi:
```cpp
uint32_t heap_before = ESP.getFreeHeap();
// ... init TFLite model ...
uint32_t heap_after = ESP.getFreeHeap();
Serial.printf("# RAM model: %lu bytes\n", heap_before - heap_after);
```
Target: < 30.000 bytes.

---

## Ringkasan Checklist

### Sambil Menunggu Training

- [ ] Mode 1 (On-Off): berjalan minimal 2 jam dari cold start
- [ ] Mode 1: catat overshoot, settling time, steady-state error
- [ ] Mode 1: uji disturbance (buka pintu 30 detik)
- [ ] Mode 2 (PID): cari Ku dan Tu dengan Ultimate Gain method
- [ ] Mode 2: hitung Kp/Ki/Kd, update ke firmware, verifikasi stabil

### Setelah Training

- [ ] RMSE test set < 0.3°C ✓
- [ ] Kuantisasi INT8, verifikasi RMSE naik < 5%
- [ ] Konversi ke `model.cc` (xxd), ukuran < 5 KB
- [ ] Embed ke `src/ml/model.cc`, build berhasil
- [ ] Mode 3: TFLite init OK, tidak crash
- [ ] Benchmark latensi: < 5.000 µs per inferensi
- [ ] RAM model: < 30.000 bytes
- [ ] Akurasi vs Python: selisih < 0.1°C untuk 10 sampel

---

## Catatan untuk Phase 5 (Pengujian Komparatif)

Setelah Mode 3 stabil, baru lakukan pengujian komparatif resmi:
- **60 sesi**: 4 skenario × 3 metode × 5 repetisi
- **Skenario:** cold start, disturbance (pintu 30s), env change ±3°C, steady state 2 jam
- **Semua sesi dalam kondisi lingkungan sedekat mungkin** (suhu ruangan, suhu awal inkubator)
- Data On-Off/PID yang dikumpulkan sekarang **tidak dipakai sebagai data resmi Phase 5**

---

*Update dokumen ini setiap ada keputusan atau hasil baru.*  
*Zainal Arifin · 2022TI038 · Institut Teknologi Bisnis AAS Indonesia · 2026*
