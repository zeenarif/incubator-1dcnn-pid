#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef void (*mqtt_callback_t)(const char *topic, const uint8_t *payload, uint32_t len);

void mqtt_init(mqtt_callback_t cb);
bool mqtt_connect();
bool mqtt_is_connected();
void mqtt_loop();
bool mqtt_publish(const char *topic, const char *payload);
void mqtt_subscribe(const char *topic);
