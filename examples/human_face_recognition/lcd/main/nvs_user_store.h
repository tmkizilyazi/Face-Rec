#pragma once

#include "esp_err.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t init_user_store(void);

// store_user_name, get_user_name, delete_user
esp_err_t store_user_name(int id, const char* name);
esp_err_t get_user_name(int id, char* name, size_t size);
esp_err_t delete_user(int id);

// user_code
esp_err_t set_user_code(int user_id, const char* code);
esp_err_t get_user_code(int user_id, char* code, size_t code_len);
esp_err_t delete_user_code(int user_id);

// Wi-Fi
esp_err_t storeWifiConfig(const char* ssid, const char* pass);
esp_err_t getWifiConfig(char* ssid, size_t ssidLen,
                        char* pass, size_t passLen);

// MQTT
esp_err_t storeMqttConfig(const char* broker, const char* user, const char* pass);
esp_err_t getMqttConfig(char* broker, size_t brokerLen,
                        char* user,   size_t userLen,
                        char* pass,   size_t passLen);

#ifdef __cplusplus
}
#endif
