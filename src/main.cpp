// ============================================================
//  Firmware Kalibrasi Sensor SHT31 — Metode Water Bath
//  Zainal Arifin · 2022TI038 · ITB AAS Indonesia
//
//  Hardware:
//    - ESP32 DevKit V1
//    - Sensor SHT31  → I2C (SDA=21, SCL=22), addr 0x44
//    - LCD 16x2 I2C  → I2C (SDA=21, SCL=22), addr 0x27
//
//  Cara pakai:
//    1. Upload firmware ke ESP32
//    2. Masukkan SHT31 ke water bath pada titik suhu target
//    3. Tunggu LCD menunjukkan status "STABIL"
//    4. Catat pembacaan SHT31 vs termometer referensi Anda
//    5. Hitung offset = T_referensi - T_SHT31
//    6. Ulangi untuk 3 titik: 35°C, 38°C, 40°C
//
//  Output Serial Monitor (115200 baud):
//    Format CSV siap copy-paste ke spreadsheet:
//    timestamp_ms, suhu_C, rh_pct, stabilitas, std_dev
// ============================================================

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_SHT31.h>
#include <LiquidCrystal_I2C.h>

// ----------------------------------------------------------
// KONFIGURASI — sesuaikan jika perlu
// ----------------------------------------------------------
#define PIN_SDA           21
#define PIN_SCL           22

#define SHT31_ADDR        0x44   // 0x44 jika pin ADDR → GND
// 0x45 jika pin ADDR → VCC
#define LCD_ADDR          0x27   // coba 0x3F jika LCD tidak muncul

#define SAMPLE_INTERVAL_MS  2000  // baca sensor tiap 2 detik
#define WINDOW_SIZE         10    // jendela 10 sampel untuk hitung stabilitas
// = 20 detik untuk deteksi stabil
#define STABLE_THRESHOLD    0.15f // std dev < 0.15°C = dianggap stabil
#define LCD_REFRESH_MS      500   // refresh LCD tiap 500ms

// ----------------------------------------------------------
// Karakter custom LCD (simbol derajat dan ikon stabil/tunggu)
// ----------------------------------------------------------
// Karakter 0: simbol derajat °
byte charDegree[8] = {
        0b00110,
        0b01001,
        0b01001,
        0b00110,
        0b00000,
        0b00000,
        0b00000,
        0b00000
};
// Karakter 1: centang ✓ (stabil)
byte charCheck[8] = {
        0b00000,
        0b00001,
        0b00011,
        0b10110,
        0b11100,
        0b01000,
        0b00000,
        0b00000
};
// Karakter 2: jam pasir (menunggu)
byte charWait[8] = {
        0b11111,
        0b01110,
        0b00100,
        0b00100,
        0b01010,
        0b10001,
        0b11111,
        0b00000
};

// ----------------------------------------------------------
// Objek global
// ----------------------------------------------------------
Adafruit_SHT31    sht31;
LiquidCrystal_I2C lcd(LCD_ADDR, 16, 2);

// Buffer ring untuk menghitung stabilitas
float   tempBuffer[WINDOW_SIZE];
uint8_t bufIndex    = 0;
bool    bufFull     = false;
bool    isStable    = false;
float   currentTemp = 0.0f;
float   currentRH   = 0.0f;
float   currentStd  = 0.0f;

uint32_t lastSampleMs = 0;
uint32_t lastLcdMs    = 0;
uint32_t sampleCount  = 0;

// State mesin kalibrasi
enum CalibState {
    STATE_WARMING,   // belum cukup sampel
    STATE_UNSTABLE,  // cukup sampel tapi belum stabil
    STATE_STABLE     // stabil — siap catat
};
CalibState calibState = STATE_WARMING;

// ----------------------------------------------------------
// Fungsi utilitas
// ----------------------------------------------------------

// Hitung mean dari buffer
float calcMean() {
    uint8_t n = bufFull ? WINDOW_SIZE : bufIndex;
    if (n == 0) return 0.0f;
    float sum = 0.0f;
    for (uint8_t i = 0; i < n; i++) sum += tempBuffer[i];
    return sum / n;
}

// Hitung standar deviasi dari buffer
float calcStdDev(float mean) {
    uint8_t n = bufFull ? WINDOW_SIZE : bufIndex;
    if (n < 2) return 99.0f;  // tidak cukup data
    float sumSq = 0.0f;
    for (uint8_t i = 0; i < n; i++) {
        float diff = tempBuffer[i] - mean;
        sumSq += diff * diff;
    }
    return sqrtf(sumSq / (n - 1));
}

// Tambah sampel ke buffer ring
void pushSample(float temp) {
    tempBuffer[bufIndex] = temp;
    bufIndex = (bufIndex + 1) % WINDOW_SIZE;
    if (bufIndex == 0) bufFull = true;
}

// Update state stabilitas
void updateStability() {
    uint8_t n = bufFull ? WINDOW_SIZE : bufIndex;
    if (n < WINDOW_SIZE) {
        calibState = STATE_WARMING;
        return;
    }
    float mean = calcMean();
    currentStd = calcStdDev(mean);
    if (currentStd < STABLE_THRESHOLD) {
        calibState = STATE_STABLE;
    } else {
        calibState = STATE_UNSTABLE;
    }
}

// ----------------------------------------------------------
// Tampilan LCD
//
// Baris 0: suhu + status ikon
// Baris 1: RH% + std dev
//
// Contoh saat stabil:
//   [T: 37.82*C  ✓   ]
//   [RH:58.3% s:0.08 ]
//
// Contoh saat warming:
//   [T: 37.82*C  ⏳  ]
//   [RH:58.3% WARMING ]
// ----------------------------------------------------------
void updateLCD() {
    lcd.setCursor(0, 0);

    // Baris 0: suhu
    char line0[17];
    char tempStr[7];
    dtostrf(currentTemp, 5, 2, tempStr);   // "37.82" atau " 9.50"
    snprintf(line0, sizeof(line0), "T:%sC ", tempStr);
    lcd.print(line0);

    // Ikon status (di kolom 12–15)
    lcd.setCursor(11, 0);
    switch (calibState) {
        case STATE_WARMING:
            lcd.write(byte(2));   // jam pasir
            lcd.print(" WRM");
            break;
        case STATE_UNSTABLE:
            lcd.write(byte(2));
            lcd.print(" MOV");
            break;
        case STATE_STABLE:
            lcd.write(byte(1));   // centang
            lcd.print(" OK!");
            break;
    }

    // Baris 1: RH + std dev
    lcd.setCursor(0, 1);
    char line1[17];
    char rhStr[5];
    char sdStr[5];
    dtostrf(currentRH,   4, 1, rhStr);
    dtostrf(currentStd,  4, 2, sdStr);
    snprintf(line1, sizeof(line1), "RH:%s%% s:%s", rhStr, sdStr);
    lcd.print(line1);
}

// ----------------------------------------------------------
// Output Serial — format CSV
// Header dicetak sekali di setup()
// ----------------------------------------------------------
void printCsvHeader() {
    Serial.println(F("# ============================================"));
    Serial.println(F("# Kalibrasi SHT31 — Water Bath Method"));
    Serial.println(F("# Salin data di bawah ke spreadsheet"));
    Serial.println(F("# Kolom: ms_since_boot, suhu_C, rh_pct, state, std_dev_C"));
    Serial.println(F("# state: 0=warming 1=unstable 2=stable"));
    Serial.println(F("# ============================================"));
    Serial.println(F("ms_since_boot,suhu_C,rh_pct,state,std_dev_C"));
}

void printCsvRow() {
    // Format: 1234567,37.82,58.30,2,0.08
    Serial.print(millis());
    Serial.print(',');
    Serial.print(currentTemp, 2);
    Serial.print(',');
    Serial.print(currentRH, 2);
    Serial.print(',');
    Serial.print((uint8_t)calibState);
    Serial.print(',');
    Serial.println(currentStd, 3);
}

// ----------------------------------------------------------
// Pesan human-readable ke Serial (prefix #)
// Tidak masuk ke CSV karena diawali '#'
// ----------------------------------------------------------
void printStatusMsg() {
    Serial.print(F("# ["));
    switch (calibState) {
        case STATE_WARMING:   Serial.print(F("WARMING  ")); break;
        case STATE_UNSTABLE:  Serial.print(F("BERGERAK ")); break;
        case STATE_STABLE:    Serial.print(F("*** STABIL ***")); break;
    }
    Serial.print(F("] T="));
    Serial.print(currentTemp, 3);
    Serial.print(F("°C  RH="));
    Serial.print(currentRH, 1);
    Serial.print(F("%  StdDev="));
    Serial.print(currentStd, 3);
    Serial.print(F("°C  Sampel#"));
    Serial.println(sampleCount);

    // Cetak pesan panduan saat pertama kali stabil
    if (calibState == STATE_STABLE && sampleCount % 5 == 0) {
        Serial.println(F("#"));
        Serial.println(F("# >>> STABIL — Catat pembacaan berikut:"));
        Serial.print(F("#     T_SHT31   = "));
        Serial.print(currentTemp, 3);
        Serial.println(F(" °C"));
        Serial.println(F("#     T_referensi = [baca termometer Anda]"));
        Serial.println(F("#     offset = T_referensi - T_SHT31"));
        Serial.println(F("#"));
    }
}

// ----------------------------------------------------------
// I2C Scanner — bantu cari alamat LCD jika tidak diketahui
// ----------------------------------------------------------
void scanI2C() {
    Serial.println(F("# Scanning I2C bus..."));
    uint8_t found = 0;
    for (uint8_t addr = 1; addr < 127; addr++) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0) {
            Serial.print(F("#   Device ditemukan di 0x"));
            Serial.println(addr, HEX);
            found++;
        }
    }
    if (found == 0) Serial.println(F("#   Tidak ada device I2C ditemukan!"));
    Serial.println(F("# Scan selesai."));
}

// ----------------------------------------------------------
// SETUP
// ----------------------------------------------------------
void setup() {
    Serial.begin(115200);
    delay(500);

    Serial.println(F("\n# ========================================"));
    Serial.println(F("# Firmware Kalibrasi SHT31 — Water Bath"));
    Serial.println(F("# Zainal Arifin · 2022TI038"));
    Serial.println(F("# ========================================"));

    // Inisialisasi I2C dengan pin custom ESP32
    Wire.begin(PIN_SDA, PIN_SCL);
    Wire.setClock(100000);  // 100kHz — lebih stabil untuk kabel panjang

    // I2C scan untuk debug
    scanI2C();

    // Inisialisasi LCD
    lcd.init();
    lcd.backlight();
    lcd.createChar(0, charDegree);
    lcd.createChar(1, charCheck);
    lcd.createChar(2, charWait);

    lcd.setCursor(0, 0);
    lcd.print(F("SHT31 Calibration"));
    lcd.setCursor(0, 1);
    lcd.print(F("Initializing...   "));
    delay(1000);

    // Inisialisasi SHT31
    if (!sht31.begin(SHT31_ADDR)) {
        Serial.println(F("# ERROR: SHT31 tidak ditemukan!"));
        Serial.println(F("# Cek: kabel SDA/SCL, alamat I2C, daya 3.3V"));

        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print(F("ERROR: SHT31"));
        lcd.setCursor(0, 1);
        lcd.print(F("Tidak ditemukan!"));
    }

    // Aktifkan heater SHT31 = OFF (heater internal bisa bias pembacaan)
    sht31.heater(false);

    Serial.println(F("# SHT31 OK."));
    Serial.println(F("# LCD OK."));
    Serial.println(F("#"));
    Serial.println(F("# PETUNJUK PENGGUNAAN:"));
    Serial.println(F("# 1. Celupkan SHT31 ke water bath (dalam plastik kedap)"));
    Serial.println(F("# 2. Panaskan air ke suhu target (35, 38, atau 40 C)"));
    Serial.println(F("# 3. Tunggu LCD menampilkan [STABIL / ✓ OK!]"));
    Serial.println(F("# 4. Catat T_SHT31 dan T_referensi dari termometer Anda"));
    Serial.println(F("# 5. Offset = T_referensi - T_SHT31"));
    Serial.println(F("#"));
    printCsvHeader();

    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print(F("Mulai pengukuran"));
    lcd.setCursor(0, 1);
    lcd.print(F("Warming up...   "));
    delay(500);

    // Inisialisasi buffer dengan 0
    memset(tempBuffer, 0, sizeof(tempBuffer));
    lastSampleMs = millis();
    lastLcdMs    = millis();
}

// ----------------------------------------------------------
// LOOP UTAMA
// ----------------------------------------------------------
void loop() {
    uint32_t now = millis();

    // ---- Baca sensor setiap SAMPLE_INTERVAL_MS ----
    if (now - lastSampleMs >= SAMPLE_INTERVAL_MS) {
        lastSampleMs = now;

        float t = sht31.readTemperature();
        float h = sht31.readHumidity();

        // Validasi data (NaN = gagal baca)
        if (isnan(t) || isnan(h)) {
            Serial.println(F("# WARN: Gagal baca SHT31, skip sampel ini"));

            lcd.setCursor(0, 0);
            lcd.print(F("WARN: Read fail "));
            return;
        }

        currentTemp = t;
        currentRH   = h;
        sampleCount++;

        // Masukkan ke buffer stabilitas
        pushSample(t);
        updateStability();

        // Output Serial
        printCsvRow();
        printStatusMsg();
    }

    // ---- Refresh LCD setiap LCD_REFRESH_MS ----
    if (now - lastLcdMs >= LCD_REFRESH_MS) {
        lastLcdMs = now;
        if (sampleCount > 0) {
            updateLCD();
        }
    }
}