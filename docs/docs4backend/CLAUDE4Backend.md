# CLAUDE.md — Backend Inkubator IoT (Go)

This file provides guidance to Claude Code when working with this repository.

> **Project:** Sistem Inkubator Telur IoT + TinyML — Cloud Backend  
> **Author:** Zainal Arifin · NIM 2022TI038 · Institut Teknologi Bisnis AAS Indonesia  
> **Target publikasi:** Jurnal SINTA 2 · 2026  
> **Firmware repo:** `../firmware/` (PlatformIO + C++) — lihat `firmware/CLAUDE.md`  
> **VPS:** Oracle Cloud Infrastructure Free Tier — 1 OCPU, 1 GB RAM, Ubuntu 22.04

---

## Prinsip Arsitektur

**Training ML tidak berjalan di VPS.** Resource 1 CPU + 1 GB RAM tidak cukup untuk
TensorFlow. Training dijalankan di laptop atau Google Colab, hasilnya (`model.cc`)
di-push ke repo, lalu VPS melakukan `git pull` dan serve file tersebut ke ESP32 via HTTP.

**VPS hanya menjalankan:**
- Go binary (API + MQTT bridge) — ~15 MB RAM
- Mosquitto — ~5 MB RAM
- InfluxDB — ~150–300 MB RAM
- Grafana — ~150–200 MB RAM
- Nginx — ~5 MB RAM

**Total estimasi RAM:** ~350–550 MB → aman di 1 GB.

### Arsitektur Sistem Lengkap

```
Laptop / Google Colab (training)
  ├── query data: GET /api/data/export  (dari VPS)
  ├── train_1dcnn.py  →  model.cc
  └── git push  model.cc  →  repo (branch: main, path: ml/output/)
                                  │
                             git pull (manual / webhook)
                                  │
                                  ▼
┌─────────────────────────────────────────────────────────┐
│  VPS Oracle OCI — docker compose                        │
│                                                         │
│  [Nginx :443]  ──► /          → Grafana  :3000          │
│                └─► /api/      → Go API   :8080          │
│                                                         │
│  [Go + Gin :8080]                                       │
│    GET  /api/status                                     │
│    GET  /api/data/export          → query InfluxDB      │
│    GET  /api/models/:ver/model.cc → serve file OTA      │
│    POST /api/ota/deploy           → publish MQTT        │
│    POST /api/mqtt/command         → publish MQTT        │
│                                                         │
│    [MQTT Bridge goroutine]                              │
│      subscribe inkubator/#  →  parse  →  write InfluxDB │
│                                                         │
│  [Mosquitto :1883]                                      │
│  [InfluxDB  :8086]  (internal only)                     │
│  [Grafana   :3000]  (via Nginx)                         │
└─────────────────────────────────────────────────────────┘
          ▲  MQTT publish/subscribe
          │
        ESP32  (firmware — lihat firmware/CLAUDE.md)
          │  inkubator/telemetri  (JSON, setiap 5 detik)
          │  inkubator/status
          ◄  inkubator/mode/set
          ◄  inkubator/pid/params
          ◄  inkubator/ota/trigger  (URL download model.cc)
          │
          ▼  HTTP GET /api/models/latest/model.cc
        ESP32 download model.cc → simpan SPIFFS → restart → Mode 3 aktif
```

---

## Struktur Direktori

```
inkubator-backend/
├── CLAUDE.md                          # ← file ini
├── .env                               # kredensial — JANGAN commit
├── .env.example                       # template tanpa nilai nyata
├── .gitignore
├── go.mod
├── go.sum
│
├── cmd/
│   └── server/
│       └── main.go                    # entry point: init config, DI, start server
│
├── internal/
│   ├── config/
│   │   └── config.go                  # load .env → struct Config
│   │
│   ├── api/
│   │   ├── router.go                  # setup Gin router + middleware
│   │   ├── response.go                # Response struct + helper Success/Error
│   │   ├── handler_status.go          # GET /api/status
│   │   ├── handler_data.go            # GET /api/data/export
│   │   ├── handler_model.go           # GET /api/models/:ver/model.cc
│   │   ├── handler_ota.go             # POST /api/ota/deploy
│   │   └── handler_command.go         # POST /api/mqtt/command
│   │
│   ├── mqtt/
│   │   ├── client.go                  # paho MQTT client wrapper (connect, pub, sub)
│   │   └── bridge.go                  # subscribe inkubator/# → parse → InfluxDB
│   │
│   └── influx/
│       ├── client.go                  # InfluxDB client wrapper
│       ├── writer.go                  # WriteAPI: tulis telemetri + status
│       └── reader.go                  # QueryAPI: status count, export CSV
│
├── ml/
│   └── output/                        # hasil training dari Colab/laptop
│       ├── .gitkeep
│       └── v{N}/                      # v1/, v2/, v3/, ...
│           ├── model.h5               # gitignored (besar)
│           ├── model.tflite           # gitignored (besar)
│           ├── model_int8.tflite      # gitignored (besar)
│           ├── model.cc               # ✅ di-commit — ini yang di-serve ke ESP32
│           └── metrics.json           # RMSE, MAE, ukuran model
│
├── deployments/
│   ├── docker-compose.yml
│   │
│   ├── mosquitto/
│   │   └── config/
│   │       └── mosquitto.conf
│   │
│   ├── grafana/
│   │   └── provisioning/
│   │       ├── datasources/
│   │       │   └── influxdb.yml
│   │       └── dashboards/
│   │           ├── dashboard.yml
│   │           └── inkubator.json
│   │
│   └── nginx/
│       └── nginx.conf
│
└── scripts/
    └── deploy_model.sh                # git pull + restart Go container
```

---

## Setup & Menjalankan di GoLand

### Prasyarat

```bash
# Go 1.22+
go version

# Docker + Compose v2
docker compose version

# Copy dan isi .env
cp .env.example .env
# Edit .env dengan nilai nyata
```

### GoLand Run Configuration

Buat **Run Configuration** baru di GoLand:
- **Type:** Go Build
- **Run kind:** File
- **File:** `cmd/server/main.go`
- **Working directory:** root project (`inkubator-backend/`)
- **Environment:** centang "Load from .env file" → pilih `.env`

Atau jalankan dari terminal GoLand:

```bash
# Jalankan semua service infrastructure dulu
docker compose -f deployments/docker-compose.yml up -d mosquitto influxdb grafana nginx

# Jalankan Go API (dengan hot-reload via air, opsional)
go run cmd/server/main.go

# Atau dengan air untuk hot-reload
go install github.com/air-verse/air@latest
air
```

### Build & Deploy ke VPS

```bash
# Build binary Linux (dari macOS/Windows)
GOOS=linux GOARCH=amd64 go build -o bin/server ./cmd/server

# Upload ke VPS
scp bin/server user@vps-ip:/opt/inkubator-backend/

# Di VPS: jalankan via docker compose
docker compose -f deployments/docker-compose.yml up -d
```

### Perintah Development Sehari-hari

```bash
# Download dependencies
go mod tidy

# Run test
go test ./...

# Run test verbose
go test -v ./internal/...

# Cek semua container berjalan
docker compose -f deployments/docker-compose.yml ps

# Lihat log Go API saja
docker compose -f deployments/docker-compose.yml logs -f api

# Lihat log semua
docker compose -f deployments/docker-compose.yml logs -f

# Restart hanya Go API (misal setelah git pull model.cc)
docker compose -f deployments/docker-compose.yml restart api

# Stop semua
docker compose -f deployments/docker-compose.yml down
```

### Test Endpoint Manual

```bash
# Status sistem
curl http://localhost:8080/api/status | jq

# Export data CSV (untuk Colab)
curl "http://localhost:8080/api/data/export?from=2026-05-07T00:00:00Z&to=2026-05-12T00:00:00Z" \
     -H "X-API-Key: api_key_dari_env" \
     -o data_training.csv

# Deploy model ke ESP32
curl -X POST http://localhost:8080/api/ota/deploy \
     -H "Content-Type: application/json" \
     -H "X-API-Key: api_key_dari_env" \
     -d '{"version": "v1"}'

# Kirim perintah ke ESP32
curl -X POST http://localhost:8080/api/mqtt/command \
     -H "Content-Type: application/json" \
     -H "X-API-Key: api_key_dari_env" \
     -d '{"topic": "inkubator/mode/set", "payload": "2"}'

# Simulasi telemetri dari ESP32 (untuk test bridge)
mosquitto_pub -h localhost -p 1883 \
  -t "inkubator/telemetri" \
  -m '{"ts":1748000000,"t_in":37.82,"t_ext":28.5,"rh":58.3,"pwm":65,"phase":"prbs","door":0}'
```

---

## Environment Variables (`.env`)

```bash
# Server
SERVER_PORT=8080
API_KEY=ganti_dengan_random_string_panjang   # untuk proteksi endpoint sensitif

# MQTT (nama service = hostname di Docker network)
MQTT_BROKER=mosquitto
MQTT_PORT=1883
MQTT_CLIENT_ID=inkubator-backend

# InfluxDB
INFLUXDB_URL=http://influxdb:8086
INFLUXDB_TOKEN=ganti_dengan_token_panjang_random
INFLUXDB_ORG=inkubator
INFLUXDB_BUCKET=inkubator

# Grafana (hanya untuk docker-compose, tidak dibaca Go)
GF_SECURITY_ADMIN_USER=admin
GF_SECURITY_ADMIN_PASSWORD=ganti_password_kuat

# Model
MODEL_BASE_PATH=./ml/output             # path relatif dari root project
MODEL_DOWNLOAD_BASE_URL=https://zeenarif.site/api/models

# Domain (untuk Nginx)
DOMAIN=zeenarif.site
```

---

## MQTT Topics

Konsisten dengan `firmware/src/config/config.h`:

| Topic | Arah | Siapa publish | Siapa subscribe | Format payload |
|-------|------|--------------|-----------------|----------------|
| `inkubator/telemetri` | ESP32 → VPS | ESP32 | bridge.go | JSON, setiap 5 detik |
| `inkubator/status` | ESP32 → VPS | ESP32 | bridge.go | JSON, setiap 30 detik |
| `inkubator/mode/set` | VPS → ESP32 | handler_command.go | ESP32 | `"0"`–`"4"` |
| `inkubator/pid/params` | VPS → ESP32 | handler_command.go | ESP32 | `"kp,ki,kd"` — lihat di bawah |
| `inkubator/ota/trigger` | VPS → ESP32 | handler_ota.go | ESP32 | URL string |

> **`inkubator/pid/params` — format wajib:** `"2.5,0.1,0.3"` (tiga float dipisah koma, **tanpa key name**).
> Firmware mem-parse dengan `sscanf(buf, "%f,%f,%f", &kp, &ki, &kd)`.
> Format `"kp:2.5,ki:0.1,kd:0.3"` **TIDAK akan ter-parse** dan perintah akan diabaikan.

### Format JSON `inkubator/telemetri`

Semua mode kirim ke topic yang sama. Bedakan via field `ctrl_mode`.

**Mode 0 — Data Logger:**
```json
{
  "ts": 1748000000, "t_in": 37.82, "t_ext": 29.45, "rh": 58.3,
  "pwm": 65, "phase": "PRBS", "door": 0, "ctrl_mode": "datalog"
}
```

**Mode 1 — On-Off:**
```json
{
  "ts": 1748000000, "t_in": 37.82, "t_ext": 29.45, "rh": 58.3,
  "pwm": 100, "door": 0, "ctrl_mode": "onoff",
  "session_id": "s01", "scenario": "cold_start"
}
```

**Mode 2 — PID:**
```json
{
  "ts": 1748000000, "t_in": 37.82, "t_ext": 29.45, "rh": 58.3,
  "pwm": 65, "err": 0.180, "integral": 12.345,
  "kp": 2.000, "ki": 0.100, "kd": 0.500,
  "door": 0, "ctrl_mode": "pid",
  "session_id": "s21", "scenario": "disturbance"
}
```

**Mode 3 — TinyML:**
```json
{
  "ts": 1748000000, "t_in": 37.82, "t_ext": 29.45, "rh": 58.3,
  "t_pred": 37.850, "pwm": 12, "door": 0, "ctrl_mode": "tinyml",
  "session_id": "s41", "scenario": "steady"
}
```

### Format JSON `inkubator/status`

Dikirim setiap 30 detik, tidak bergantung mode aktif:
```json
{
  "ts": 1748000000, "uptime": 86400,
  "mode": 1, "mode_name": "onoff",
  "free_heap": 234560, "wifi_rssi": -65
}
```

**Catatan `bridge.go`:** `ts` sudah berupa Unix timestamp nyata (NTP aktif di firmware).
Tetap sertakan guard: jika `ts < 1_000_000_000`, fallback ke `time.Now()` untuk robustness.

---

## InfluxDB Schema

```
bucket    : inkubator
org       : inkubator

measurement: telemetri
  tags:
    ctrl_mode   string   "datalog" | "onoff" | "pid" | "tinyml"
    phase       string   "SWEEP" | "PRBS" | "DISTURB" | "TRACK"  (hanya Mode 0)
    session_id  string   "s01"–"s60" (hanya Mode 1–3, kosong saat tuning)
    scenario    string   "cold_start" | "disturbance" | "env_change" | "steady"
  fields:
    t_in        float64  suhu dalam inkubator (°C), offset kalibrasi sudah diterapkan
    t_ext       float64  suhu luar inkubator (°C), 0.0 jika sensor tidak terpasang
    rh          float64  kelembapan relatif (%)
    pwm         int64    PWM dimmer 0–100
    door        int64    0 = tutup, 1 = terbuka
    t_pred      float64  prediksi suhu t+1 (°C), hanya Mode 3
    err         float64  error PID = setpoint − t_in, hanya Mode 2
    integral    float64  integral PID (di-clamp ±50), hanya Mode 2
    kp          float64  Kp aktif, hanya Mode 2
    ki          float64  Ki aktif, hanya Mode 2
    kd          float64  Kd aktif, hanya Mode 2
  _time:        dari field ts (Unix timestamp — NTP aktif), fallback time.Now()

measurement: status
  tags:
    mode_name   string   "datalog" | "onoff" | "pid" | "tinyml" | "calib"
  fields:
    mode        int64    nomor mode aktif 0–4
    uptime      int64    detik sejak boot
    free_heap   int64    free heap bytes ESP32 (alert jika < 50000)
    wifi_rssi   int64    dBm, signal strength (alert jika < -85)
  _time:        dari field ts, fallback time.Now()
```

> **Tag vs field:** `ctrl_mode`, `phase`, `session_id`, `scenario`, `mode_name` wajib sebagai **tag**
> (bukan field) agar Grafana bisa filter tanpa full table scan.

---

## Standard API Response

**Semua endpoint JSON** menggunakan envelope yang sama tanpa kecuali.
Hanya `GET /api/models/:version/model.cc` yang tidak menggunakan envelope ini
karena responsenya adalah file binary untuk ESP32.

### Struktur Envelope

```go
// internal/api/response.go
// Gunakan struct ini untuk SEMUA response JSON — sukses maupun error.

type Response struct {
    Status   string      `json:"status"`             // "success" | "error"
    Message  string      `json:"message"`            // kalimat singkat, human-readable
    Data     interface{} `json:"data,omitempty"`     // payload utama, nil jika error
    Metadata interface{} `json:"metadata,omitempty"` // pagination, timing, context — opsional
}
```

### HTTP Status Code

| Kondisi | HTTP | `status` |
|---------|------|----------|
| Sukses | 200 OK | `"success"` |
| Sukses dibuat | 201 Created | `"success"` |
| Input tidak valid | 400 Bad Request | `"error"` |
| API key salah / tidak ada | 401 Unauthorized | `"error"` |
| Topic MQTT tidak diizinkan | 403 Forbidden | `"error"` |
| Resource tidak ditemukan | 404 Not Found | `"error"` |
| Validasi bisnis gagal (misal RMSE > 0.3) | 422 Unprocessable Entity | `"error"` |
| Error internal server | 500 Internal Server Error | `"error"` |

### Helper Functions (wajib digunakan di semua handler)

```go
// internal/api/response.go

func Success(c *gin.Context, message string, data interface{}) {
    c.JSON(http.StatusOK, Response{
        Status:  "success",
        Message: message,
        Data:    data,
    })
}

func SuccessWithMeta(c *gin.Context, message string, data interface{}, meta interface{}) {
    c.JSON(http.StatusOK, Response{
        Status:   "success",
        Message:  message,
        Data:     data,
        Metadata: meta,
    })
}

func Error(c *gin.Context, httpCode int, message string) {
    c.JSON(httpCode, Response{
        Status:  "error",
        Message: message,
    })
}
```

---

## API Endpoints

### `GET /api/status`

Tidak butuh API key. Health check seluruh sistem.

**Response 200:**
```json
{
  "status": "success",
  "message": "Sistem berjalan normal",
  "data": {
    "mqtt_connected":    true,
    "influxdb_connected": true,
    "sample_count":      87420,
    "ready_for_training": true,
    "latest_model_version": "v2",
    "latest_model_rmse": 0.21
  },
  "metadata": {
    "uptime_seconds": 86400,
    "server_time":    "2026-05-14T10:00:00Z"
  }
}
```

`ready_for_training: true` jika `sample_count >= 20000`.

---

### `GET /api/data/export`

**Butuh `X-API-Key` header.**
Export data InfluxDB sebagai CSV untuk didownload ke Colab/laptop.
Response ini **tidak** menggunakan JSON envelope — langsung stream CSV.

Query params:
- `from` — RFC3339, contoh `2026-05-07T00:00:00Z`
- `to`   — RFC3339, contoh `2026-05-12T23:59:59Z`

**Response 200** — `Content-Type: text/csv`:
```
timestamp_unix,t_in,t_ext,rh,pwm,door,ctrl_mode,phase,session_id,scenario,t_pred,err,integral,kp,ki,kd
1748000000,37.82,28.50,58.30,65,0,datalog,PRBS,,,,,,,
1748000005,37.85,28.45,58.25,65,0,pid,,s01,cold_start,,0.150,8.230,2.000,0.100,0.500
```

Kolom mode-specific (`phase`, `t_pred`, `err`, dll) berisi nilai kosong jika mode tidak memproduksi field tersebut.

**Response 400** — parameter tidak lengkap atau format salah:
```json
{
  "status": "error",
  "message": "Parameter 'from' dan 'to' wajib diisi dengan format RFC3339"
}
```

**Response 401** — API key tidak valid:
```json
{
  "status": "error",
  "message": "API key tidak valid atau tidak ditemukan"
}
```

---

### `GET /api/models/:version/model.cc`

Tidak butuh API key — ESP32 tidak bisa mengirim header.
Serve file binary `model.cc` dari disk.
`:version` bisa berupa `"v1"`, `"v2"`, atau `"latest"` (resolve ke versi tertinggi).

**Response 200** — `Content-Type: text/plain`, file attachment.

**Response 404** — versi tidak ditemukan:
```json
{
  "status": "error",
  "message": "model.cc tidak ditemukan untuk versi v99"
}
```

---

### `POST /api/ota/deploy`

**Butuh `X-API-Key` header.**
Validasi model lalu kirim trigger OTA ke ESP32 via MQTT.

**Request body:**
```json
{ "version": "v1" }
```

**Validasi (urutan):**
1. `model.cc` ada di disk → 404 jika tidak
2. `metrics.json` ada dan valid → 422 jika tidak
3. `rmse_test` < 0.3°C → 422 jika tidak

**Response 200:**
```json
{
  "status": "success",
  "message": "OTA trigger berhasil dikirim ke ESP32",
  "data": {
    "version":      "v1",
    "download_url": "https://zeenarif.site/api/models/v1/model.cc",
    "mqtt_topic":   "inkubator/ota/trigger"
  },
  "metadata": {
    "rmse_test":     0.21,
    "mae_test":      0.14,
    "model_size_kb": 1.2,
    "trained_at":    "2026-05-14T10:00:00Z"
  }
}
```

**Response 404** — model.cc tidak ada:
```json
{
  "status": "error",
  "message": "model.cc tidak ditemukan untuk versi v1. Pastikan sudah git push dari Colab."
}
```

**Response 422** — RMSE tidak memenuhi threshold:
```json
{
  "status": "error",
  "message": "Deploy ditolak: RMSE model (0.42°C) melebihi batas maksimum 0.3°C"
}
```

**Response 422** — `metrics.json` tidak ada:
```json
{
  "status": "error",
  "message": "metrics.json tidak ditemukan untuk versi v1. File ini wajib di-push bersama model.cc."
}
```

---

### `POST /api/mqtt/command`

**Butuh `X-API-Key` header.**
Publish perintah ke ESP32 via MQTT. Hanya topic yang ada di whitelist yang diizinkan.

**Request body:**
```json
{
  "topic":   "inkubator/mode/set",
  "payload": "2"
}
```

**Whitelist topic:**
- `inkubator/mode/set` — payload: string `"0"`–`"4"`
- `inkubator/pid/params` — payload: string `"kp,ki,kd"` contoh: `"2.5,0.1,0.3"` (**tanpa key name**)

**Response 200:**
```json
{
  "status": "success",
  "message": "Perintah berhasil dikirim ke ESP32",
  "data": {
    "topic":   "inkubator/mode/set",
    "payload": "2"
  }
}
```

**Response 400** — payload mode tidak valid:
```json
{
  "status": "error",
  "message": "Payload untuk topic mode/set harus berupa angka '0' hingga '4'"
}
```

**Response 403** — topic tidak ada di whitelist:
```json
{
  "status": "error",
  "message": "Topic 'inkubator/ota/trigger' tidak diizinkan melalui endpoint ini"
}
```

---

## Konvensi Kode Go

- **Go version:** 1.22+
- **Router:** `github.com/gin-gonic/gin`
- **MQTT:** `github.com/eclipse/paho.mqtt.golang`
- **InfluxDB:** `github.com/influxdata/influxdb-client-go/v2`
- **Env:** `github.com/joho/godotenv`
- **Error handling:** selalu wrap dengan konteks — `fmt.Errorf("mqtt connect: %w", err)`
- **Logging:** `log/slog` (stdlib Go 1.21+), structured JSON di production
- **Tidak pakai GORM** — InfluxDB bukan SQL, gunakan InfluxDB Go client langsung
- **Dependency injection:** manual via struct, tidak pakai framework DI
- **Goroutine:** MQTT bridge berjalan sebagai goroutine terpisah, gunakan `context` untuk shutdown graceful
- **Config:** semua dari environment variable via `internal/config/config.go`, tidak ada hardcode

### Response — wajib pakai helper dari `internal/api/response.go`

**Jangan pernah panggil `c.JSON()` langsung di handler.** Selalu gunakan helper
`Success`, `SuccessWithMeta`, atau `Error` agar envelope konsisten di seluruh codebase.

```go
// ✅ Benar
func (h *StatusHandler) Handle(c *gin.Context) {
    // ...
    Success(c, "Sistem berjalan normal", gin.H{
        "mqtt_connected": h.mqtt.IsConnected(),
        "sample_count":   count,
    })
}

// ❌ Salah — bypass envelope
func (h *StatusHandler) Handle(c *gin.Context) {
    c.JSON(200, gin.H{"ok": true})
}
```

**Satu-satunya pengecualian:** `GET /api/models/:version/model.cc` boleh
menggunakan `c.File()` karena response-nya adalah file binary untuk ESP32,
bukan JSON.

### Pola struct handler

```go
// Semua handler menerima dependency via struct, bukan global variable
type OTAHandler struct {
    mqtt  *mqtt.Client
    model *model.Manager
    cfg   *config.Config
}

func (h *OTAHandler) Deploy(c *gin.Context) {
    var req deployRequest
    if err := c.ShouldBindJSON(&req); err != nil {
        Error(c, http.StatusBadRequest, "Request body tidak valid")
        return
    }
    // ...
    Success(c, "OTA trigger berhasil dikirim ke ESP32", gin.H{...})
}
```

### Graceful shutdown

```go
// cmd/server/main.go harus handle SIGINT/SIGTERM
// urutan: stop MQTT bridge goroutine → disconnect MQTT → shutdown HTTP server
```

---

## Model Versioning

Setelah training di Colab/laptop:

```bash
# Struktur yang di-push ke repo
ml/output/v1/
├── model.cc        # ✅ commit ini
└── metrics.json    # ✅ commit ini

# metrics.json format:
{
  "version":        "v1",
  "trained_at":     "2026-05-14T10:00:00Z",
  "sample_count":   87420,
  "rmse_test":      0.21,
  "mae_test":       0.14,
  "rmse_int8":      0.22,
  "model_size_kb":  1.2,
  "window_size":    60,
  "features":       ["t", "rh", "pwm"]
}
```

Setelah `git push`, di VPS:

```bash
# Manual
cd /opt/inkubator-backend && git pull
docker compose -f deployments/docker-compose.yml restart api

# Atau via script
bash scripts/deploy_model.sh v1
```

---

## Aturan Penting

1. **`POST /api/ota/deploy` HARUS validasi `metrics.json`** — tolak jika RMSE > 0.3°C
2. **`GET /api/data/export` HARUS ada API key** — data sensor tidak boleh publik
3. **`GET /api/models/:ver/model.cc` tidak perlu auth** — ESP32 tidak bisa kirim header
4. **MQTT bridge harus reconnect otomatis** — jika Mosquitto restart, bridge tidak boleh mati
5. **Handle `ts` firmware yang belum NTP** — jika `ts < 1_000_000_000`, pakai `time.Now()`
6. **Tidak ada training di VPS** — jika ada kode yang import TensorFlow/Python di Go, itu salah
7. **Model di-serve dari disk** — bukan dari database, bukan dari memory
8. **Whitelist topic MQTT command** — jangan publish ke sembarang topic dari API

---

## Progress Checklist

> **Legend:** ✅ Selesai · ⚠️ Parsial · [ ] Belum

### Fase 2 — Cloud Stack

- ✅ `go.mod` + `go.sum` (dependencies)
- ✅ `.env.example` + `.gitignore`
- ✅ `internal/config/config.go`
- ✅ `internal/influx/client.go` + `writer.go` + `reader.go`
- ✅ `internal/mqtt/client.go` + `bridge.go`
- ✅ `internal/api/router.go`
- ✅ `internal/api/response.go`
- ✅ `internal/api/model_manager.go` ← helper resolve versi + baca metrics.json (tambahan, tidak di struktur awal)
- ✅ `internal/api/handler_status.go`
- ✅ `internal/api/handler_data.go`
- ✅ `internal/api/handler_model.go`
- ✅ `internal/api/handler_ota.go`
- ✅ `internal/api/handler_command.go`
- ✅ `cmd/server/main.go`
- ✅ `deployments/docker-compose.yml`
- ✅ `deployments/Dockerfile` ← multi-stage build (tambahan)
- ✅ `.github/workflows/docker-publish.yml` ← build & push ke GHCR on push ke main
- ✅ `deployments/mosquitto/config/mosquitto.conf`
- ✅ `deployments/grafana/provisioning/datasources/influxdb.yml`
- ✅ `deployments/grafana/provisioning/dashboards/dashboard.yml`
- ✅ `deployments/grafana/provisioning/dashboards/inkubator.json`
- ✅ `deployments/nginx/nginx.conf`
- ✅ `scripts/deploy_model.sh`
- [ ] Uji lokal: semua container healthy
- [ ] Uji MQTT bridge: publish manual → data masuk InfluxDB
- [ ] Uji export CSV: data bisa didownload
- [ ] Deploy VPS + TLS Let's Encrypt
- [ ] End-to-end: ESP32 → MQTT → InfluxDB → Grafana tampil

### Fase 4 — OTA Pipeline

- [ ] `ml/output/v1/metrics.json` + `model.cc` (setelah training di Colab)
- [ ] Uji `GET /api/models/v1/model.cc` — file terdownload
- [ ] Uji `POST /api/ota/deploy` — MQTT trigger terkirim
- [ ] Uji OTA end-to-end: ESP32 download → restart → Mode 3 aktif

---

## Catatan Integrasi dengan Firmware

- **Offset kalibrasi** sudah diterapkan di firmware. Data yang masuk InfluxDB sudah bersih — tidak perlu koreksi di bridge.
- **Timestamp firmware:** `ts` adalah Unix timestamp nyata — NTP sudah aktif di firmware. Bridge tetap harus punya guard fallback: jika `ts < 1_000_000_000`, pakai `time.Now()`.
- **Format MQTT command PID:** `"2.5,0.1,0.3"` — tiga float dipisah koma **tanpa key name**. Firmware parse dengan `sscanf(buf, "%f,%f,%f")`. Format `"kp:2.5"` **tidak akan ter-parse**.
- **OTA trigger payload:** URL string mentah, bukan JSON. `"https://zeenarif.site/api/models/v1/model.cc"` — sesuai `ota_manager.cpp`.

---

*Dokumen ini adalah panduan hidup — update setiap kali ada keputusan baru.*  
*Zainal Arifin · 2022TI038 · Institut Teknologi Bisnis AAS Indonesia · 2026*
