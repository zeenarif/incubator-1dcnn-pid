# Referensi Payload MQTT — ESP32 Inkubator

> Dokumen ini mendeskripsikan semua pesan MQTT yang dikirim/diterima ESP32.  
> Gunakan sebagai acuan konfigurasi Telegraf, InfluxDB, dan Grafana.  
> Zainal Arifin · 2022TI038 · Institut Teknologi Bisnis AAS Indonesia

---

## Ringkasan Topics

| Topic | Arah | Publisher | Interval | Deskripsi |
|---|---|---|---|---|
| `inkubator/telemetri` | ESP32 → Backend | Mode 0, 1, 2, 3 | 5 detik | Data sensor + aktuator |
| `inkubator/status` | ESP32 → Backend | `main.cpp` | 30 detik | Heartbeat: mode, uptime, heap, RSSI |
| `inkubator/mode/set` | Backend → ESP32 | Grafana / API | — | Ganti mode aktif (kirim angka `0`–`4`) |
| `inkubator/pid/params` | Backend → ESP32 | Grafana / API | — | Update Kp/Ki/Kd (format: `"2.5,0.1,0.3"`) |
| `inkubator/ota/trigger` | Backend → ESP32 | FastAPI | — | URL model.cc baru untuk diunduh |

> **Mode 4 (Kalibrasi)** tidak publish MQTT — output hanya ke Serial CSV.

---

## Topic: `inkubator/telemetri`

Semua mode mengirim ke topic yang sama. Bedakan mode lewat field **`ctrl_mode`**.  
Interval: **setiap 5 detik** (`SAMPLE_INTERVAL_MS = 5000`).

---

### Mode 0 — Data Logger (PRBS)

```json
{
  "ts":        1748000000,
  "t_in":      37.82,
  "t_ext":     29.45,
  "rh":        58.3,
  "pwm":       65,
  "phase":     "PRBS",
  "door":      0,
  "ctrl_mode": "datalog"
}
```

| Field | Tipe | Satuan | Keterangan |
|---|---|---|---|
| `ts` | uint32 | detik Unix | `0` jika NTP belum sync |
| `t_in` | float | °C | Suhu dalam inkubator, sudah terkoreksi offset kalibrasi |
| `t_ext` | float | °C | Suhu luar inkubator. `0.00` jika sensor eksternal tidak terpasang |
| `rh` | float | % | Kelembapan relatif |
| `pwm` | uint8 | % (0–100) | PWM AC Dimmer aktif |
| `phase` | string | — | Sub-fase: `SWEEP` / `PRBS` / `DISTURB` / `TRACK` |
| `door` | uint8 | 0/1 | `0` = tutup, `1` = terbuka |
| `ctrl_mode` | string | — | Selalu `"datalog"` |

---

### Mode 1 — On-Off Control

```json
{
  "ts":         1748000000,
  "t_in":       37.82,
  "t_ext":      29.45,
  "rh":         58.3,
  "pwm":        100,
  "door":       0,
  "ctrl_mode":  "onoff",
  "session_id": "s01",
  "scenario":   "cold_start"
}
```

| Field | Tipe | Satuan | Keterangan |
|---|---|---|---|
| `ts` | uint32 | detik Unix | |
| `t_in` | float | °C | |
| `t_ext` | float | °C | |
| `rh` | float | % | |
| `pwm` | uint8 | % | `100` = pemanas ON, `0` = OFF |
| `door` | uint8 | 0/1 | |
| `ctrl_mode` | string | — | Selalu `"onoff"` |
| `session_id` | string | — | ID sesi uji. Kosong `""` saat tuning awal |
| `scenario` | string | — | `cold_start` / `disturbance` / `env_change` / `steady`. Kosong `""` saat tuning |

---

### Mode 2 — PID Control

```json
{
  "ts":         1748000000,
  "t_in":       37.82,
  "t_ext":      29.45,
  "rh":         58.3,
  "pwm":        65,
  "err":        0.180,
  "integral":   12.345,
  "kp":         2.000,
  "ki":         0.100,
  "kd":         0.500,
  "door":       0,
  "ctrl_mode":  "pid",
  "session_id": "s21",
  "scenario":   "disturbance"
}
```

| Field | Tipe | Satuan | Keterangan |
|---|---|---|---|
| `ts` | uint32 | detik Unix | |
| `t_in` | float | °C | |
| `t_ext` | float | °C | |
| `rh` | float | % | |
| `pwm` | uint8 | % | Output PID setelah `constrain(0, 100)` |
| `err` | float | °C | `SETPOINT - t_in`. Positif = terlalu dingin |
| `integral` | float | °C·s | Integral term, di-clamp ±50 (anti-windup) |
| `kp` | float | — | Kp aktif saat ini |
| `ki` | float | — | Ki aktif saat ini |
| `kd` | float | — | Kd aktif saat ini |
| `door` | uint8 | 0/1 | |
| `ctrl_mode` | string | — | Selalu `"pid"` |
| `session_id` | string | — | |
| `scenario` | string | — | |

---

### Mode 3 — TinyML 1D-CNN

```json
{
  "ts":         1748000000,
  "t_in":       37.82,
  "t_ext":      29.45,
  "rh":         58.3,
  "t_pred":     37.850,
  "pwm":        12,
  "door":       0,
  "ctrl_mode":  "tinyml",
  "session_id": "s41",
  "scenario":   "steady"
}
```

| Field | Tipe | Satuan | Keterangan |
|---|---|---|---|
| `ts` | uint32 | detik Unix | |
| `t_in` | float | °C | |
| `t_ext` | float | °C | |
| `rh` | float | % | |
| `t_pred` | float | °C | Prediksi suhu t+1 (5 detik ke depan) dari model CNN |
| `pwm` | uint8 | % | `constrain(Kp × (setpoint − t_pred), 0, 100)` |
| `door` | uint8 | 0/1 | |
| `ctrl_mode` | string | — | Selalu `"tinyml"` |
| `session_id` | string | — | |
| `scenario` | string | — | |

---

### Mode 4 — Kalibrasi

**Tidak publish MQTT.** Output hanya ke Serial dengan format CSV:
```
ms,temp_C,rh_pct,stable,std_dev
12500,37.824,58.30,0,99.000
```

---

## Topic: `inkubator/status`

Dikirim setiap **30 detik** oleh `main.cpp`, tanpa bergantung mode aktif.  
Berguna sebagai **heartbeat** — jika tidak muncul >1 menit, ESP32 kemungkinan offline atau MQTT putus.

```json
{
  "ts":        1748000000,
  "uptime":    86400,
  "mode":      1,
  "mode_name": "onoff",
  "free_heap": 234560,
  "wifi_rssi": -65
}
```

| Field | Tipe | Satuan | Keterangan |
|---|---|---|---|
| `ts` | uint32 | detik Unix | `0` jika NTP belum sync |
| `uptime` | uint32 | detik | Waktu sejak boot (`millis() / 1000`) |
| `mode` | uint8 | — | Nomor mode aktif: `0`–`4` |
| `mode_name` | string | — | `"datalog"` / `"onoff"` / `"pid"` / `"tinyml"` / `"calib"` |
| `free_heap` | uint32 | bytes | RAM heap tersisa (`ESP.getFreeHeap()`). Pantau untuk deteksi memory leak |
| `wifi_rssi` | int8 | dBm | Kekuatan sinyal WiFi. Baik: > −70 dBm, Buruk: < −85 dBm |

---

## Topic: `inkubator/mode/set` (Subscribe)

Kirim satu karakter angka untuk ganti mode aktif:

```
Payload: "0"   → Mode 0 Data Logger
Payload: "1"   → Mode 1 On-Off
Payload: "2"   → Mode 2 PID
Payload: "3"   → Mode 3 TinyML
Payload: "4"   → Mode 4 Kalibrasi
```

---

## Topic: `inkubator/pid/params` (Subscribe)

Format payload: `"kp,ki,kd"` (tiga float dipisah koma, tanpa spasi):

```
Payload: "3.6,0.24,1.125"
```

ESP32 otomatis reset integral saat parameter diupdate.

---

## Topic: `inkubator/ota/trigger` (Subscribe)

Payload: URL langsung ke file `model.cc` di FastAPI:

```
Payload: "http://vps.example.com/models/latest/model.cc"
```

ESP32 akan download → simpan ke SPIFFS → restart otomatis.

---

## Konfigurasi Telegraf (MQTT Consumer → InfluxDB)

Gunakan **dua input terpisah** agar measurement di InfluxDB bisa dibedakan dengan mudah:

```toml
# Telemetri sensor (Mode 0-3) → measurement "telemetri"
[[inputs.mqtt_consumer]]
  servers      = ["tcp://localhost:1883"]
  topics       = ["inkubator/telemetri"]
  data_format  = "json"
  name_override = "telemetri"
  json_time_key    = "ts"
  json_time_format = "unix"
  tag_keys     = ["ctrl_mode", "phase", "session_id", "scenario"]

# Status heartbeat → measurement "status"
[[inputs.mqtt_consumer]]
  servers      = ["tcp://localhost:1883"]
  topics       = ["inkubator/status"]
  data_format  = "json"
  name_override = "status"
  json_time_key    = "ts"
  json_time_format = "unix"
  tag_keys     = ["mode_name"]

[[outputs.influxdb_v2]]
  urls   = ["http://localhost:8086"]
  token  = "YOUR_TOKEN"
  org    = "YOUR_ORG"
  bucket = "inkubator"
```

Field `ctrl_mode`, `phase`, `session_id`, `scenario`, `mode_name` dijadikan **tag** (bukan field) agar bisa difilter di Grafana tanpa full table scan.

---

## Panel Grafana yang Disarankan

### Dashboard: Monitoring Real-Time

Semua query dari measurement `telemetri`:

| Panel | Filter field | Tipe panel |
|---|---|---|
| Suhu Dalam | `t_in` | Time series |
| Suhu Luar | `t_ext` | Time series (overlay) |
| PWM Dimmer | `pwm` | Time series |
| Kelembapan | `rh` | Time series |
| Status Pintu | `door` | State timeline |

Panel **Status ESP32** dari measurement `status`:

| Panel | Filter field | Tipe panel | Keterangan |
|---|---|---|---|
| Mode Aktif | `mode` | Stat | Tampilkan `mode_name` sebagai label |
| Free Heap | `free_heap` | Gauge | Alert jika < 50.000 bytes |
| WiFi RSSI | `wifi_rssi` | Gauge | Alert jika < −85 dBm |
| Uptime | `uptime` | Stat | Deteksi reboot tak terduga |
| Last Seen | *(last timestamp)* | Stat | Alert jika > 2 menit tidak ada data |

### Dashboard: Analisis PID

| Panel | Query tambahan | Keterangan |
|---|---|---|
| Error (°C) | `filter(fn: (r) => r["_field"] == "err")` | Terlihat osilasi untuk Ziegler-Nichols |
| Integral | `filter(fn: (r) => r["_field"] == "integral")` | Deteksi windup |

### Dashboard: Perbandingan Phase 5

Filter per `ctrl_mode` dan `session_id`:
```flux
from(bucket: "inkubator")
  |> range(start: -7d)
  |> filter(fn: (r) => r["_measurement"] == "telemetri")
  |> filter(fn: (r) => r["_field"] == "t_in")
  |> filter(fn: (r) => r["ctrl_mode"] == "pid" and r["scenario"] == "cold_start")
```

---

## Konsistensi Antar Mode

| Field | Mode 0 | Mode 1 | Mode 2 | Mode 3 |
|---|---|---|---|---|
| `ts` | ✅ | ✅ | ✅ | ✅ |
| `t_in` | ✅ | ✅ | ✅ | ✅ |
| `t_ext` | ✅ | ✅ | ✅ | ✅ |
| `rh` | ✅ | ✅ | ✅ | ✅ |
| `pwm` | ✅ | ✅ | ✅ | ✅ |
| `door` | ✅ | ✅ | ✅ | ✅ |
| `ctrl_mode` | ✅ `"datalog"` | ✅ `"onoff"` | ✅ `"pid"` | ✅ `"tinyml"` |
| `phase` | ✅ | — | — | — |
| `session_id` | — | ✅ | ✅ | ✅ |
| `scenario` | — | ✅ | ✅ | ✅ |
| `err` | — | — | ✅ | — |
| `integral` | — | — | ✅ | — |
| `kp/ki/kd` | — | — | ✅ | — |
| `t_pred` | — | — | — | ✅ |

---

*Update dokumen ini setiap ada perubahan payload di firmware.*  
*Zainal Arifin · 2022TI038 · Institut Teknologi Bisnis AAS Indonesia · 2026*
