#include "nvs_manager.h"
#include "config.h"
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

void nvs_save_pid(float kp, float ki, float kd) {
    prefs.putFloat("pid_kp", kp);
    prefs.putFloat("pid_ki", ki);
    prefs.putFloat("pid_kd", kd);
}

void nvs_load_pid(float *kp, float *ki, float *kd) {
    *kp = prefs.getFloat("pid_kp", PID_KP_DEFAULT);
    *ki = prefs.getFloat("pid_ki", PID_KI_DEFAULT);
    *kd = prefs.getFloat("pid_kd", PID_KD_DEFAULT);
}
