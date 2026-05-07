#include "ntp_sync.h"
#include "../config/config.h"
#include <Arduino.h>
#include <WiFi.h>
#include "time.h"

static bool _synced = false;

void ntp_init() {
    configTime(NTP_UTC_OFFSET, 0, NTP_SERVER);
    struct tm ti;
    if (getLocalTime(&ti, 5000)) {
        _synced = true;
        Serial.println(F("# NTP sync OK"));
    } else {
        Serial.println(F("# NTP sync FAILED"));
    }
}

bool ntp_synced() { return _synced; }

uint32_t ntp_timestamp() {
    time_t now;
    time(&now);
    return (uint32_t)now;
}
