#pragma once
#include <stdint.h>

void hal_lcd_init(uint8_t addr);

// Generic single-string update for a row (max 16 chars)
void hal_lcd_row(uint8_t row, const char *text);

// Mode-specific display helpers
void hal_lcd_mode_banner(uint8_t mode_num, const char *mode_name);
void hal_lcd_show_sensor(float temp, float rh, uint8_t pwm_pct);
void hal_lcd_show_pid(float temp, float sp, float kp, uint8_t pwm_pct);
void hal_lcd_show_tinyml(float temp_now, float temp_pred, uint8_t pwm_pct);
void hal_lcd_show_calib(float temp_raw, bool stable, float std_dev);
void hal_lcd_clear();
