#include "mqtt_client.h"
#include "../config/config.h"
#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>

static WiFiClient   _wifi_client;
static PubSubClient _mqtt_client(_wifi_client);
static mqtt_callback_t _user_cb = nullptr;

static void _on_message(char *topic, uint8_t *payload, unsigned int len) {
    if (_user_cb) _user_cb(topic, payload, (uint32_t)len);
}

void mqtt_init(mqtt_callback_t cb) {
    _user_cb = cb;
    _mqtt_client.setServer(MQTT_BROKER, MQTT_PORT);
    _mqtt_client.setCallback(_on_message);
}

static bool _wifi_connect() {
    if (WiFi.status() == WL_CONNECTED) return true;
    Serial.print(F("# WiFi connecting..."));
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    uint8_t tries = 0;
    while (WiFi.status() != WL_CONNECTED && tries < 20) {
        delay(500);
        Serial.print('.');
        tries++;
    }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED) {
        Serial.print(F("# WiFi OK: "));
        Serial.println(WiFi.localIP());
        return true;
    }
    Serial.println(F("# WiFi FAILED — running offline"));
    return false;
}

bool mqtt_connect() {
    if (!_wifi_connect()) return false;
    if (_mqtt_client.connected()) return true;
    Serial.print(F("# MQTT connecting..."));
    if (_mqtt_client.connect(MQTT_CLIENT_ID)) {
        Serial.println(F("OK"));
        _mqtt_client.subscribe(TOPIC_MODE_SET);
        _mqtt_client.subscribe(TOPIC_PID_PARAMS);
        _mqtt_client.subscribe(TOPIC_OTA_TRIGGER);
        return true;
    }
    Serial.print(F("# MQTT FAILED state="));
    Serial.println(_mqtt_client.state());
    return false;
}

bool mqtt_is_connected() {
    return _mqtt_client.connected();
}

void mqtt_loop() {
    if (!_mqtt_client.connected()) {
        static uint32_t last_retry = 0;
        if (millis() - last_retry > 30000UL) {
            last_retry = millis();
            mqtt_connect();
        }
    }
    _mqtt_client.loop();
}

bool mqtt_publish(const char *topic, const char *payload) {
    if (!_mqtt_client.connected()) return false;
    return _mqtt_client.publish(topic, payload);
}

void mqtt_subscribe(const char *topic) {
    if (_mqtt_client.connected()) _mqtt_client.subscribe(topic);
}
