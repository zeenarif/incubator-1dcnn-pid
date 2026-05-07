#pragma once

void mode_pid_init();
void mode_pid_tick();
void mode_pid_set_params(float kp, float ki, float kd);
