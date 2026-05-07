#include "ota_manager.h"
#include <Arduino.h>
#include <SPIFFS.h>
#include <HTTPClient.h>

#define MODEL_PATH  "/model.cc"

static bool _model_available = false;

void ota_init() {
    if (!SPIFFS.begin(true)) {
        Serial.println(F("# SPIFFS mount failed"));
        return;
    }
    _model_available = SPIFFS.exists(MODEL_PATH);
    if (_model_available)
        Serial.println(F("# OTA: model.cc found in SPIFFS"));
    else
        Serial.println(F("# OTA: no model.cc yet"));
}

bool ota_download_model(const char *url) {
    HTTPClient http;
    http.begin(url);
    int code = http.GET();
    if (code != 200) {
        Serial.print(F("# OTA HTTP error: "));
        Serial.println(code);
        http.end();
        return false;
    }

    File f = SPIFFS.open(MODEL_PATH, FILE_WRITE);
    if (!f) {
        Serial.println(F("# OTA: cannot open SPIFFS for write"));
        http.end();
        return false;
    }

    WiFiClient *stream = http.getStreamPtr();
    uint8_t buf[256];
    int total = 0;
    while (http.connected()) {
        int avail = stream->available();
        if (avail > 0) {
            int n = stream->readBytes(buf, min(avail, (int)sizeof(buf)));
            f.write(buf, n);
            total += n;
        }
    }
    f.close();
    http.end();

    _model_available = true;
    Serial.print(F("# OTA: model downloaded, bytes="));
    Serial.println(total);
    return true;
}

bool ota_model_available() { return _model_available; }
