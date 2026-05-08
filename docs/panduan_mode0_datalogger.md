# Panduan Mode 0 — Data Logger (PRBS Excitation)

> Baca dokumen ini sebelum memulai sesi data logging.  
> Zainal Arifin · 2022TI038 · Institut Teknologi Bisnis AAS Indonesia

---

## Prasyarat Sebelum Memulai

1. **Kalibrasi sensor sudah selesai** — offset tersimpan di NVS Flash
   - SHT31 internal terbaca 0.4°C lebih tinggi dari referensi → offset = -0.4°C (sudah tersimpan)
2. **MQTT broker aktif** — cek koneksi dari serial monitor, harus muncul `MQTT OK`
3. **NTP sync** — timestamp di CSV harus angka Unix (bukan 0), tandanya NTP berhasil
4. **Sensor eksternal SHT31** (addr 0x45) — pasang jika ingin merekam suhu luar inkubator

---

## Cara Mengaktifkan Mode 0

1. Putar rotary encoder sampai LCD menampilkan:
   ```
   Mode? [DATALOG]
   Hold SW to OK
   ```
2. **Tekan dan tahan tombol SW lebih dari 1 detik**, lalu lepas
3. Serial monitor akan mencetak: `# Mode switched to 0`
4. LCD baris atas berganti ke `M0:SWEEP  #00000`
5. Data MQTT mulai dikirim setiap 5 detik

---

## Struktur Data yang Dikirim

**MQTT Topic:** `inkubator/telemetri`

**Format JSON:**
```json
{
  "ts":    1748000000,
  "t_in":  37.82,
  "t_ext": 29.45,
  "rh":    58.30,
  "pwm":   65,
  "phase": "PRBS",
  "door":  0
}
```

| Field | Tipe | Satuan | Keterangan |
|-------|------|--------|------------|
| `ts` | uint32 | detik (Unix) | 0 jika NTP belum sync |
| `t_in` | float | °C | Suhu dalam inkubator, sudah terkoreksi offset kalibrasi |
| `t_ext` | float | °C | Suhu luar inkubator, raw. **0.00 jika sensor belum terpasang** |
| `rh` | float | % | Kelembapan relatif (sensor internal) |
| `pwm` | uint8 | % (0–100) | PWM AC Dimmer aktif |
| `phase` | string | — | Sub-fase aktif saat ini |
| `door` | uint8 | 0/1 | 0 = tutup, 1 = terbuka |

**Format Serial CSV** (untuk logging lokal):
```
timestamp_unix,temp_in_C,temp_ext_C,rh_pct,pwm,phase,door
1748000000,37.82,29.45,58.30,65,PRBS,0
```
Baris diawali `#` = pesan status, bukan data.

---

## Siklus 8 Jam — Empat Sub-Phase

Satu siklus = 8 jam, berjalan otomatis dan berulang terus selama Mode 0 aktif.

```
Jam 0–2   → SWEEP
Jam 2–5   → PRBS
Jam 5–6   → DISTURB
Jam 6–8   → TRACK
Jam 8     → kembali ke SWEEP (ulang)
```

---

### SWEEP (Jam 0–2, durasi 2 jam)

PWM naik-turun di level tetap, masing-masing 20 menit:

```
20% → 40% → 60% → 80% → 60% → 40% → 20%
```

**Tujuan:** merekam respons steady-state di berbagai level daya pemanasan.  
**Intervensi manual:** tidak ada.

---

### PRBS (Jam 2–5, durasi 3 jam)

PWM toggle acak antara **25% ↔ 75%**, interval random 30 detik – 5 menit (dihasilkan LFSR 8-bit, seed `0xAC`).

**Tujuan:** eksitasi frekuensi lebar untuk system identification — fase terpenting untuk training CNN. CNN belajar kelambaman termal, kecepatan respons, dan dinamika transien.  
**Intervensi manual:** tidak ada.

---

### DISTURB (Jam 5–6, durasi 1 jam)

Kombinasi otomatis + **satu aksi manual**:

| Waktu dalam fase | PWM | Keterangan |
|-----------------|-----|------------|
| 0 – 30 detik | 25% | **BUKA PINTU** secara manual |
| 30 detik – 2,5 menit | 0% otomatis | Pintu boleh ditutup kembali |
| 2,5 – 3,5 menit | 100% otomatis | — |
| 3,5 – 60 menit | 25% otomatis | — |

**Yang perlu dilakukan:**
- Pantau LCD — saat tampil `M0:DISTURB`, segera buka pintu inkubator
- Tahan terbuka selama **±30 detik**, lalu tutup kembali
- Lakukan **setiap kali** LCD menampilkan DISTURB (1x per siklus 8 jam)

**Tujuan:** melatih CNN mengenali dan merespons gangguan kehilangan panas tiba-tiba.  
**Kalau terlewat:** data tetap terekam, hanya tidak ada variasi suhu dari pintu terbuka pada siklus tersebut — tidak merusak dataset keseluruhan.

---

### TRACK (Jam 6–8, durasi 2 jam)

On-off control otomatis di sekitar setpoint **38°C ± 0.3°C**.

**Tujuan:** merekam perilaku closed-loop saat sistem kembali ke setpoint.  
**Intervensi manual:** tidak ada.

---

## Jadwal Intervensi Manual

| Waktu | Aksi |
|-------|------|
| Jam 5 sejak Mode 0 aktif | Buka pintu 30 detik |
| Jam 13 (siklus ke-2) | Buka pintu 30 detik |
| Jam 21 (siklus ke-3) | Buka pintu 30 detik |
| dst. setiap 8 jam | Buka pintu 30 detik |

Selama 5 hari logging = **±15 kali** buka pintu.

---

## Target Dataset

| Parameter | Target |
|-----------|--------|
| Durasi logging | Minimal 5 hari (7 hari ideal) |
| Interval sampling | 5 detik |
| Total sampel minimum | 86.400 baris |
| Split training | 70% train / 15% val / 15% test — **SEQUENTIAL, jangan di-shuffle** |

---

## Monitoring Selama Logging

Cek serial monitor sesekali. Tanda-tanda data logger berjalan normal:
- Baris CSV muncul setiap 5 detik dengan angka `ts` yang valid (Unix timestamp)
- Kolom `phase` berganti sesuai jadwal siklus
- Tidak ada baris `# WARN` berulang-ulang

Tanda masalah:
- `ts = 0` terus-menerus → NTP tidak sync, cek koneksi WiFi
- Tidak ada data di broker → cek `mqtt_is_connected` via serial (`# MQTT FAILED`)
- `# WARN: int sensor read fail` berulang → cek kabel SHT31
