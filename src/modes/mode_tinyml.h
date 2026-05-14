#pragma once

void mode_tinyml_init();
void mode_tinyml_tick();
bool mode_tinyml_model_ready();
void mode_tinyml_set_session(const char *session_id, const char *scenario);
