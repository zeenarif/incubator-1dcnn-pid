#include "hal_lcd.h"
#include <LiquidCrystal_I2C.h>
#include <Arduino.h>
#include <stdio.h>

static LiquidCrystal_I2C *_lcd = nullptr;

static byte _deg[8] = {0b00110,0b01001,0b01001,0b00110,0b00000,0b00000,0b00000,0b00000};
static byte _chk[8] = {0b00000,0b00001,0b00011,0b10110,0b11100,0b01000,0b00000,0b00000};
static byte _wai[8] = {0b11111,0b01110,0b00100,0b00100,0b01010,0b10001,0b11111,0b00000};

void hal_lcd_init(uint8_t addr) {
    _lcd = new LiquidCrystal_I2C(addr, 16, 2);
    _lcd->init();
    _lcd->backlight();
    _lcd->createChar(0, _deg);
    _lcd->createChar(1, _chk);
    _lcd->createChar(2, _wai);
}

void hal_lcd_row(uint8_t row, const char *text) {
    if (!_lcd) return;
    char buf[17];
    snprintf(buf, sizeof(buf), "%-16s", text);
    _lcd->setCursor(0, row);
    _lcd->print(buf);
}

void hal_lcd_clear() {
    if (_lcd) _lcd->clear();
}

void hal_lcd_mode_banner(uint8_t mode_num, const char *mode_name) {
    char row0[17], row1[17];
    snprintf(row0, sizeof(row0), "Mode %d: %-9s", mode_num, mode_name);
    snprintf(row1, sizeof(row1), "Initializing... ");
    hal_lcd_row(0, row0);
    hal_lcd_row(1, row1);
}

void hal_lcd_show_sensor(float temp, float rh, uint8_t pwm_pct) {
    if (!_lcd) return;
    char row0[17], row1[17];
    snprintf(row0, sizeof(row0), "T:%.2fC RH:%.1f%%", temp, rh);
    snprintf(row1, sizeof(row1), "PWM:%3d%%        ", pwm_pct);
    hal_lcd_row(0, row0);
    hal_lcd_row(1, row1);
}

void hal_lcd_show_pid(float temp, float sp, float kp, uint8_t pwm_pct) {
    if (!_lcd) return;
    char row0[17], row1[17];
    snprintf(row0, sizeof(row0), "T:%.2f SP:%.1f", temp, sp);
    snprintf(row1, sizeof(row1), "PWM:%3d%% Kp:%.1f", pwm_pct, kp);
    hal_lcd_row(0, row0);
    hal_lcd_row(1, row1);
}

void hal_lcd_show_tinyml(float temp_now, float temp_pred, uint8_t pwm_pct) {
    if (!_lcd) return;
    char row0[17], row1[17];
    snprintf(row0, sizeof(row0), "T:%.2f->%.2f", temp_now, temp_pred);
    snprintf(row1, sizeof(row1), "PWM:%3d%% CNN   ", pwm_pct);
    hal_lcd_row(0, row0);
    hal_lcd_row(1, row1);
}

void hal_lcd_show_calib(float temp_raw, bool stable, float std_dev) {
    if (!_lcd) return;
    char row0[17], row1[17];
    snprintf(row0, sizeof(row0), "T:%.3fC       ", temp_raw);
    _lcd->setCursor(0, 0);
    _lcd->print(row0);
    _lcd->setCursor(12, 0);
    if (stable) _lcd->write(byte(1)); else _lcd->write(byte(2));
    snprintf(row1, sizeof(row1), "s:%.3f %-6s", std_dev, stable ? "STABLE" : "WAIT");
    hal_lcd_row(1, row1);
}
