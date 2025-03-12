#include "nvs_user_store.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static const char* USER_NAMESPACE   = "user_store";
static const char* CONFIG_NAMESPACE = "config_store";

extern "C" esp_err_t init_user_store(void)
{
    esp_err_t err = nvs_flash_init();
    if(err==ESP_ERR_NVS_NO_FREE_PAGES || err==ESP_ERR_NVS_NEW_VERSION_FOUND){
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    return err;
}

// Eklenen fonksiyonlar: store_user_name, get_user_name, delete_user
extern "C" esp_err_t store_user_name(int id, const char* name)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(USER_NAMESPACE, NVS_READWRITE, &handle);
    if(err != ESP_OK) return err;

    char key[32];
    snprintf(key,sizeof(key),"user_%d", id);

    err = nvs_set_str(handle, key, name);
    if(err==ESP_OK){
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

extern "C" esp_err_t get_user_name(int id, char* name, size_t size)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(USER_NAMESPACE, NVS_READONLY, &handle);
    if(err != ESP_OK) return err;

    char key[32];
    snprintf(key,sizeof(key),"user_%d", id);
    err = nvs_get_str(handle, key, name, &size);

    nvs_close(handle);
    return err;
}

extern "C" esp_err_t delete_user(int id)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(USER_NAMESPACE, NVS_READWRITE, &handle);
    if(err != ESP_OK) return err;

    char key[32];
    snprintf(key,sizeof(key),"user_%d", id);

    err = nvs_erase_key(handle, key);
    if(err==ESP_OK){
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

// user_code
extern "C" esp_err_t set_user_code(int user_id, const char* code)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(USER_NAMESPACE, NVS_READWRITE, &handle);
    if(err!=ESP_OK) return err;

    char key[32];
    snprintf(key, sizeof(key), "code_for_%d", user_id);
    err = nvs_set_str(handle, key, code);
    if(err==ESP_OK){
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

extern "C" esp_err_t get_user_code(int user_id, char* code, size_t code_len)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(USER_NAMESPACE, NVS_READONLY, &handle);
    if(err!=ESP_OK) return err;

    char key[32];
    snprintf(key, sizeof(key), "code_for_%d", user_id);
    err = nvs_get_str(handle, key, code, &code_len);
    nvs_close(handle);
    return err;
}

extern "C" esp_err_t delete_user_code(int user_id)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(USER_NAMESPACE, NVS_READWRITE, &handle);
    if(err!=ESP_OK) return err;

    char key[32];
    snprintf(key, sizeof(key), "code_for_%d", user_id);

    err = nvs_erase_key(handle, key);
    if(err==ESP_OK){
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

// Wi-Fi / MQTT
extern "C" esp_err_t storeWifiConfig(const char* ssid, const char* pass)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if(err != ESP_OK) return err;

    if(nvs_set_str(handle, "wifi_ssid", ssid) != ESP_OK){
        nvs_close(handle);
        return ESP_FAIL;
    }
    if(nvs_set_str(handle, "wifi_pass", pass) != ESP_OK){
        nvs_close(handle);
        return ESP_FAIL;
    }
    err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

extern "C" esp_err_t getWifiConfig(char* ssid, size_t ssidLen,
                                   char* pass, size_t passLen)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CONFIG_NAMESPACE, NVS_READONLY, &handle);
    if(err!=ESP_OK){
        return err;
    }
    esp_err_t r1 = nvs_get_str(handle, "wifi_ssid", ssid, &ssidLen);
    esp_err_t r2 = nvs_get_str(handle, "wifi_pass", pass, &passLen);
    nvs_close(handle);

    if(r1==ESP_OK && r2==ESP_OK){
        return ESP_OK;
    }
    return ESP_FAIL;
}

extern "C" esp_err_t storeMqttConfig(const char* broker, const char* user, const char* pass)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if(err != ESP_OK) return err;

    if(nvs_set_str(handle, "mqtt_broker", broker) != ESP_OK){
        nvs_close(handle);
        return ESP_FAIL;
    }
    if(nvs_set_str(handle, "mqtt_user", user) != ESP_OK){
        nvs_close(handle);
        return ESP_FAIL;
    }
    if(nvs_set_str(handle, "mqtt_pass", pass) != ESP_OK){
        nvs_close(handle);
        return ESP_FAIL;
    }
    err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

extern "C" esp_err_t getMqttConfig(char* broker, size_t brokerLen,
                                   char* user,   size_t userLen,
                                   char* pass,   size_t passLen)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CONFIG_NAMESPACE, NVS_READONLY, &handle);
    if(err != ESP_OK){
        return err;
    }

    esp_err_t r1 = nvs_get_str(handle, "mqtt_broker", broker, &brokerLen);
    esp_err_t r2 = nvs_get_str(handle, "mqtt_user",   user,   &userLen);
    esp_err_t r3 = nvs_get_str(handle, "mqtt_pass",   pass,   &passLen);

    nvs_close(handle);
    if(r1==ESP_OK && r2==ESP_OK && r3==ESP_OK){
        return ESP_OK;
    }
    return ESP_FAIL;
}
