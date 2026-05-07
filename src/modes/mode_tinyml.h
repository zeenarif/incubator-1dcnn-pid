#pragma once

void mode_tinyml_init();
void mode_tinyml_tick();
// Returns true if real model is loaded, false if using stub
bool mode_tinyml_model_ready();
