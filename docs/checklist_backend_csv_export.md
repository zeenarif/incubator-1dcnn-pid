# Checklist Backend — CSV Export untuk Data TinyML

> **Konteks:** Data Mode 3 (TinyML) diunduh via `GET /api/data/export` dan digunakan
> oleh ML Engineer untuk analisis dan retraining model. Beberapa kolom kritis perlu
> dipastikan ter-capture dan ter-export dengan benar.
>
> **Perintah export saat ini:**
> ```bash
> curl -X GET "https://zeenarif.site/api/data/export?from=...&to=..." \
>      -H "Content-Type: application/json" -o "record.csv"
> ```

---

## 1. Kolom CSV — Mapping dari MQTT ke InfluxDB ke CSV

CSV yang diharapkan ML Engineer memiliki **16 kolom** dalam urutan ini:

```
timestamp_unix, t_in, t_ext, rh, pwm, door, ctrl_mode, phase,
session_id, scenario, t_pred, err, integral, kp, ki, kd
```

### Mapping MQTT JSON → InfluxDB field → CSV kolom

| CSV Kolom | MQTT Field | Tipe | Catatan |
|-----------|-----------|------|---------|
| `timestamp_unix` | `ts` | integer | Unix timestamp (bukan RFC3339) |
| `t_in` | `t_in` | float | Suhu dalam, 2 desimal |
| `t_ext` | `t_ext` | float | Suhu luar, 2 desimal |
| `rh` | `rh` | float | Kelembapan, 1 desimal |
| `pwm` | `pwm` | integer | Total output heater (P+I), 0–100 |
| `door` | `door` | integer | 1=tutup, 0=buka |
| `ctrl_mode` | `ctrl_mode` | string | "tinyml", "pid", "onoff", dll |
| `phase` | `phase` | string | Biasanya kosong di Mode 3 |
| `session_id` | `session_id` | string | Label sesi |
| `scenario` | `scenario` | string | Label skenario |
| `t_pred` | `t_pred` | float | Prediksi CNN (atau = t_in saat filling) |
| `err` | — | float | **Tidak ada di Mode 3** → export `0.0` |
| `integral` | `integral` | float | ⚠️ BARU — nilai CNN integral internal |
| `kp` | — | float | **Tidak ada di Mode 3** → export `0.0` |
| `ki` | — | float | **Tidak ada di Mode 3** → export `0.0` |
| `kd` | — | float | **Tidak ada di Mode 3** → export `0.0` |

---

## 2. Checklist yang Perlu Diperiksa di Backend Go

### ✅ / ❌ Item 1 — Field `integral` dari MQTT ditangkap Telegraf?

**Yang terjadi:** Firmware sebelumnya tidak mengirim field `integral` di MQTT payload.
Setelah update firmware terbaru (2026-05-17), payload Mode 3 sekarang menyertakan:

```json
{
  "ts": 1778993189,
  "t_in": 37.91,
  "t_ext": 32.47,
  "rh": 49.60,
  "t_pred": 39.410,
  "pwm": 24,
  "door": 1,
  "fb": 0,
  "integral": 49.40,   ← BARU
  "ctrl_mode": "tinyml",
  "session_id": "",
  "scenario": ""
}
```

**Yang perlu dicek:**
- Apakah Telegraf config `[[inputs.mqtt_consumer]]` menggunakan mode `json_v2` atau `grok`?
  - Jika **schemaless** (default JSON): field baru otomatis masuk InfluxDB ✓
  - Jika **whitelist fields**: perlu tambah `integral` ke daftar field yang di-parse
- Cek di InfluxDB: apakah measurement `telemetri` sudah memiliki field `integral`?

```bash
# Verifikasi di InfluxDB (via Flux):
from(bucket: "inkubator")
  |> range(start: -1h)
  |> filter(fn: (r) => r._measurement == "telemetri" and r._field == "integral")
  |> limit(n: 5)
```

---

### ✅ / ❌ Item 2 — Query SQL/Flux di backend — apakah SELECT semua field atau spesifik?

**Yang perlu dicek di kode Go backend** (file handler export CSV):

```go
// ❌ Jika ada query seperti ini (SELECT spesifik tanpa integral):
query := `SELECT ts, t_in, t_ext, rh, pwm, door, ctrl_mode, t_pred FROM telemetri`

// ✅ Harus mencakup integral:
query := `SELECT ts, t_in, t_ext, rh, pwm, door, ctrl_mode, phase,
           session_id, scenario, t_pred, integral FROM telemetri`
```

Jika menggunakan Flux InfluxDB:
```flux
// ❌ Jika hanya filter field tertentu:
|> filter(fn: (r) => r._field == "t_in" or r._field == "pwm")

// ✅ Harus termasuk integral (atau ambil semua field):
|> pivot(rowKey:["_time"], columnKey: ["_field"], valueColumn: "_value")
// lalu mapping ke CSV dengan semua kolom yang diperlukan
```

---

### ✅ / ❌ Item 3 — Format timestamp di CSV

**Yang diperlukan:** Kolom pertama CSV harus berisi **Unix timestamp integer** (bukan
string ISO 8601), agar kompatibel dengan script Python ML Engineer:

```python
# ML Engineer membaca CSV seperti ini:
df = pd.read_csv("record.csv")
df["timestamp_unix"].astype(int)  # harus integer
```

**Yang perlu dicek:**
```go
// ❌ Jika backend export timestamp sebagai string RFC3339:
"2026-05-17T11:45:00+07:00"

// ✅ Harus integer Unix timestamp:
1778993189
```

Jika InfluxDB menyimpan timestamp sebagai nanoseconds, konversi di backend:
```go
unixSec := influxTimestamp.UnixNano() / 1e9
```

---

### ✅ / ❌ Item 4 — Kolom kosong diisi `0.0`, bukan `NULL` atau string kosong

Kolom `err`, `kp`, `ki`, `kd` tidak ada di Mode 3 payload MQTT. Saat export CSV,
kolom ini harus diisi `0.000` (bukan `null`, `""`, atau dihilangkan).

**Yang perlu dicek:**
```go
// ❌ Jika backend output:
"37.91,,,,32.47,..."  // kolom kosong

// ✅ Harus:
"37.91,0.000,0.000,0.000,32.47,..."
```

---

### ✅ / ❌ Item 5 — Kolom `fb` dari MQTT tidak masuk CSV

Field `fb` (fallback indicator, 0 atau 1) ada di MQTT payload Mode 3 tapi **tidak
ada di format CSV 16-kolom** yang digunakan ML Engineer. Pastikan field ini tidak
mengganggu urutan kolom.

```go
// Kolom yang TIDAK boleh masuk ke CSV 16-kolom standar:
// "fb", "door" sudah ada di posisi 6

// Perhatikan: "door" di CSV = kolom 6, jangan confused dengan "fb"
```

---

### ✅ / ❌ Item 6 — Query rentang waktu: apakah timezone +07:00 dihandle benar?

Perintah export saat ini menggunakan:
```
?from=2026-05-17T11:45:00%2B07:00&to=2026-05-17T14:25:00%2B07:00
```

`%2B` adalah URL-encoded `+`. **Yang perlu dicek:**

```go
// Backend Go harus parse parameter ini dengan benar:
fromStr := r.URL.Query().Get("from")
// fromStr = "2026-05-17T11:45:00+07:00"

fromTime, err := time.Parse(time.RFC3339, fromStr)
// Ini harus menghasilkan waktu yang benar (UTC+7 = WIB)
```

Verifikasi: data yang diunduh memang dimulai tepat dari `ts` yang sesuai dengan
`2026-05-17T11:45:00+07:00` (dalam Unix timestamp: 1778993100).

---

### ✅ / ❌ Item 7 — Header CSV harus persis 16 kolom dengan nama yang tepat

```go
// ✅ Header yang benar:
header := "timestamp_unix,t_in,t_ext,rh,pwm,door,ctrl_mode,phase,session_id,scenario,t_pred,err,integral,kp,ki,kd\n"
```

ML Engineer menggunakan nama kolom ini langsung di script Python:
```python
df["integral"]     # harus ada
df["t_pred"]       # harus ada
df["pwm"]          # harus ada (total P+I, bukan P saja)
```

---

## 3. Urutan Perbaikan yang Disarankan

```
1. [ ] Flash firmware terbaru ke ESP32
       → MQTT sekarang mengirim field "integral" yang benar

2. [ ] Cek Telegraf config — pastikan field "integral" ter-capture
       → Cek file telegraf.conf atau docker-compose environment

3. [ ] Cek query backend Go di handler export
       → Pastikan SELECT mencakup "integral"
       → Pastikan timestamp di-export sebagai Unix integer

4. [ ] Cek header CSV yang di-generate
       → Harus tepat 16 kolom, nama sesuai

5. [ ] Download test CSV dan verifikasi:
       → kolom "integral" menampilkan nilai ~49-65 (bukan 0.000)
       → kolom "pwm" = total output heater
       → timestamp_unix adalah integer
```

---

## 4. Contoh Baris CSV yang Benar (Mode 3 Steady State)

```
timestamp_unix,t_in,t_ext,rh,pwm,door,ctrl_mode,phase,session_id,scenario,t_pred,err,integral,kp,ki,kd
1778993189,37.91,32.47,49.60,24,1,tinyml,,,,39.410,0.000,49.40,0.000,0.000,0.000
1778993194,37.89,32.47,49.60,24,1,tinyml,,,,39.390,0.000,49.43,0.000,0.000,0.000
```

Perhatikan: `integral` ≈ 49.40 (mendekati I_CLAMP lama 50), bukan 0.000.

---

*Zainal Arifin · 2022TI038 · Institut Teknologi Bisnis AAS Indonesia · 2026*
