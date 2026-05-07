#include "nvs_manager.h"
#include <Preferences.h>

static Preferences prefs;

void nvs_init() {
    prefs.begin("inkubator", false);
}

void nvs_save_mode(uint8_t mode) {
    prefs.putUChar("mode", mode);
}

uint8_t nvs_load_mode() {
    return prefs.getUChar("mode", 4);
}

void nvs_save_temp_offset(float offset) {
    prefs.putFloat("temp_off", offset);
}

float nvs_load_temp_offset() {
    return prefs.getFloat("temp_off", 0.0f);
}

void nvs_save_rh_offset(float offset) {
    prefs.putFloat("rh_off", offset);
}

float nvs_load_rh_offset() {
    return prefs.getFloat("rh_off", 0.0f);
}
