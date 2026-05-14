#pragma once

// Wi-Fi / MQTT credentials — DO NOT commit to public repo
#define WIFI_SSID          "Jangan Lupa Sholat"
#define WIFI_PASSWORD      "murahmeriahlho"
#define MQTT_BROKER        "mqtt.zeenarif.site"
#define MQTT_PORT          1883
#define MQTT_CLIENT_ID     "inkubator_esp32"

// MQTT Topics
#define TOPIC_TELEMETRI    "inkubator/telemetri"
#define TOPIC_STATUS       "inkubator/status"
#define TOPIC_MODE_SET     "inkubator/mode/set"
#define TOPIC_PID_PARAMS   "inkubator/pid/params"
#define TOPIC_OTA_TRIGGER  "inkubator/ota/trigger"

// I2C
#define PIN_SDA            21
#define PIN_SCL            22
#define SHT31_ADDR         0x44   // Internal (ADDR pin → GND)
#define SHT31_EXT_ADDR     0x45   // External (ADDR pin → VCC)
#define LCD_ADDR           0x27

// AC Dimmer (zero-crossing + TRIAC)
#define PIN_ZERO_CROSS     27
#define PIN_DIMMER_PWM     14

// Actuators
#define PIN_FAN_RELAY      26
#define PIN_DOOR_SENSOR    13
#define PIN_BUZZER          4   // Active buzzer (3.3V direct, GPIO4 → Buzzer+ → Buzzer− → GND)

// Rotary Encoder
#define PIN_ENC_CLK        25
#define PIN_ENC_DT         33
#define PIN_ENC_SW         32

// Control setpoint
#define SETPOINT_TEMP      38.0f
#define SAMPLE_INTERVAL_MS 5000UL

// Mode 0 – Data Logger (PRBS)
#define PRBS_PWM_LOW       25
#define PRBS_PWM_HIGH      75

// Mode 1 – On-Off hysteresis band
#define ONOFF_HYSTERESIS   0.3f

// Mode 2 – PID defaults
#define PID_KP_DEFAULT     2.0f
#define PID_KI_DEFAULT     0.1f
#define PID_KD_DEFAULT     0.5f

// Mode 3 – 1D-CNN TinyML
#define CNN_WINDOW_SIZE    60
#define CNN_FEATURES       3
#define CNN_KP             8.0f

// Mode 4 – Calibration
#define CALIB_WINDOW       10
#define CALIB_STABLE_THRES 0.15f
#define CALIB_SAMPLE_MS    2000UL

// LCD
#define LCD_REFRESH_MS     500UL

// NTP
#define NTP_SERVER         "pool.ntp.org"
#define NTP_UTC_OFFSET     25200   // WIB = UTC+7