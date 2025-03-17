#include <string>
#include <vector>
#include <map>
#include <utility>
#include <time.h>
#include <sys/time.h>
#include <cstdio>
#include <cstring>
#include <cstdlib> // rand()
#include <errno.h>
#include <inttypes.h>  // PRId32 makroları için

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"

#include "esp_system.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"

#include "lwip/inet.h"        // htonl
#include "lwip/apps/sntp.h"   // sntp_*
#include "mqtt_client.h"
#include "mbedtls/base64.h"

#include "who_camera.h"
#include "who_lcd.h"
#include "who_human_face_recognition.hpp"
#include "event_logic.hpp"
#include "face_recognition_tool.hpp"
#include "http_server.h"

// NVS fonksiyonları
#include "nvs_user_store.h"

#include "esp_heap_caps.h"
#include "esp_mac.h"  // MAC adresi için

static const char* TAG = "APP_MAIN";

// ----------- RTOS Queues -----------
static QueueHandle_t xQueueAIFrame    = nullptr;
static QueueHandle_t xQueueLCDFrame   = nullptr;
static QueueHandle_t xQueueEventLogic = nullptr;
static QueueHandle_t xQueueResult     = nullptr;

// ---------- Connectivity flags -----
static bool s_wifi_connected     = false;
static bool s_internet_available = false;
static bool s_mqtt_connected     = false;

// MQTT client handle
static esp_mqtt_client_handle_t s_mqtt_client = nullptr;

// IP/MAC
static char    s_device_ip[16]      = {0};
static uint8_t s_device_mac[6]      = {0};
static char    s_device_mac_str[18] = {0};

// Face rec global
bool enroll_mode       = false;
bool enroll_requested  = false;
bool do_enroll_now     = false;
char pending_enroll_name[64] = {0};

// 10s reconnect timer
static TimerHandle_t s_reconnectTimer = nullptr;
static void reconnect_timer_cb(TimerHandle_t xTimer);

// ====================================================
// NVS'ten okunan veya varsayılan Wi-Fi/MQTT
// ====================================================
static char s_wifi_ssid[64]  = {0};
static char s_wifi_pass[64]  = {0};

static char s_mqtt_broker_uri[128] = {0};
static char s_mqtt_username[64]    = {0};
static char s_mqtt_password[64]    = {0};

// SABİT topic eşleştirmeleri
static char s_offline_face_topic[64]         = "face_logs";
static char s_offline_master_data_topic[64]  = "master_data";
static char s_offline_master_del_topic[64]   = "master_delete";

static char s_online_face_topic[64]          = "bakelor/pr14/prod/live_face";
static char s_online_master_data_topic[64]   = "bakelor/pr14/prod/master_data";
static char s_online_master_del_topic[64]    = "bakelor/pr14/prod/master_delete";

// Yeni topic formatı için global değişken ekleyelim
static char s_camera_topic[64] = {0};

//----------------------------------------------------------------
// Forward Declarations
//----------------------------------------------------------------
static void mqtt_event_handler(void* arg, esp_event_base_t base,
                               int32_t event_id, void* event_data);
static esp_err_t sd_init(void);
static void mqtt_init(void);
static void wifi_init(void);
static void initialize_sntp(void);
static void obtain_time(void);
static void wifi_event_handler(void* arg, esp_event_base_t base,
                               int32_t event_id, void* data);
static bool drain_offline_from_logs();

//================================================================
// Single logs.txt => Offline lines => [OFFLINE]|<topic>|<payload>
//================================================================
static void store_offline_in_logs(const char* topic, const char* message)
{
    FILE* f = fopen("/sdcard/logs.txt", "a");
    if (!f) {
        ESP_LOGW(TAG, "store_offline_in_logs => cannot open logs.txt for append!");
        return;
    }
    fprintf(f, "[OFFLINE]|%s|%s\n", topic, message);
    fclose(f);
    ESP_LOGI(TAG, "Offline appended => topic=%s, msg=%s", topic, message);
}

static bool drain_offline_from_logs()
{
    FILE* f = fopen("/sdcard/logs.txt", "r");
    if (!f) {
        ESP_LOGI(TAG, "No logs.txt => no drain needed");
        return true;
    }

    std::vector<std::string> lines;
    char buf[1024];
    while (fgets(buf, sizeof(buf), f)) {
        size_t len = strlen(buf);
        while (len > 0 && (buf[len - 1] == '\r' || buf[len - 1] == '\n')) {
            buf[len - 1] = '\0';
            --len;
        }
        lines.push_back(std::string(buf));
    }
    fclose(f);

    if (lines.empty()) {
        remove("/sdcard/logs.txt");
        return true;
    }

    std::vector<std::string> keepLines;
    for (auto &l : lines) {
        if (l.rfind("[OFFLINE]|", 0) == 0) {
            auto delim1 = l.find('|');
            auto delim2 = (delim1 == std::string::npos) ? std::string::npos : l.find('|', delim1 + 1);
            if (delim2 == std::string::npos) {
                keepLines.push_back(l);
                continue;
            }
            std::string storedTopic = l.substr(delim1 + 1, delim2 - (delim1 + 1));
            std::string payload = l.substr(delim2 + 1);

            std::string realTopic;
            if (storedTopic == s_offline_master_data_topic) {
                realTopic = s_online_master_data_topic;
            } else if (storedTopic == s_offline_master_del_topic) {
                realTopic = s_online_master_del_topic;
            } else if (storedTopic == s_offline_face_topic) {
                realTopic = s_online_face_topic;
            } else {
                realTopic = storedTopic;
            }

            int msg_id = esp_mqtt_client_publish(
                s_mqtt_client,
                realTopic.c_str(),
                payload.c_str(),
                0, // length
                1, // qos
                0  // retain
            );
            if (msg_id < 0) {
                ESP_LOGW(TAG, "drain_offline => fail => %s => %s", realTopic.c_str(), payload.c_str());
                keepLines.push_back(l);
            } else {
                ESP_LOGI(TAG, "drain_offline => published => %s => %s", realTopic.c_str(), payload.c_str());
            }
        } else {
            keepLines.push_back(l);
        }
    }

    if (keepLines.empty()) {
        remove("/sdcard/logs.txt");
        ESP_LOGI(TAG, "All offline lines published => logs.txt removed");
        return true;
    }

    FILE* wf = fopen("/sdcard/logs.txt", "w");
    if (!wf) {
        ESP_LOGW(TAG, "cannot rewrite logs.txt => keep old lines!");
        return false;
    }
    for (auto &kl : keepLines) {
        fprintf(wf, "%s\n", kl.c_str());
    }
    fclose(wf);
    ESP_LOGI(TAG, "logs.txt => re-written => keep=%u lines", (unsigned)keepLines.size());
    return true;
}

//================================================================
// SNTP
//================================================================
static void initialize_sntp(void)
{
    ESP_LOGI(TAG, "Init SNTP => pool.ntp.org");
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_init();

    setenv("TZ", "UTC-3", 1);
    tzset();
}

static void obtain_time(void)
{
    initialize_sntp();

    time_t now = 0;
    struct tm timeinfo;
    memset(&timeinfo, 0, sizeof(timeinfo));

    int retry = 0, max_retry = 15;
    while (timeinfo.tm_year < (2023 - 1900) && ++retry < max_retry) {
        ESP_LOGI(TAG, "Waiting SNTP... (%d/%d)", retry, max_retry);
        vTaskDelay(pdMS_TO_TICKS(2000));
        time(&now);
        localtime_r(&now, &timeinfo);
    }
    if (timeinfo.tm_year < (2023 - 1900)) {
        ESP_LOGW(TAG, "Failed to sync time via SNTP!");
    } else {
        ESP_LOGI(TAG, "Time synced => %s", asctime(&timeinfo));
    }
}

//================================================================
// on_user_deleted => "master_delete"
//================================================================
extern "C" void on_user_deleted(int user_id)
{
    ESP_LOGI(TAG, "on_user_deleted => user_id=%d", user_id);

    {
        FILE* lf = fopen("/sdcard/logs.txt", "a");
        if (lf) {
            fprintf(lf, "[LOG] on_user_deleted => %d\n", user_id);
            fclose(lf);
        }
    }

    char stored_code[32] = {0};
    if (get_user_code(user_id, stored_code, sizeof(stored_code)) != ESP_OK) {
        snprintf(stored_code, sizeof(stored_code), "%d", user_id);
    }

    char json_msg[128];
    snprintf(json_msg, sizeof(json_msg),
             "{\"Code\":\"%s\",\"MacAddress\":\"%s\"}",
             stored_code, s_device_mac_str);

    if (s_wifi_connected && s_internet_available && s_mqtt_connected) {
        esp_mqtt_client_publish(s_mqtt_client,
                                s_online_master_del_topic,
                                json_msg, 0, 1, 0);
    } else {
        store_offline_in_logs(s_offline_master_del_topic, json_msg);
    }

    delete_user_code(user_id);
}

//================================================================
// on_user_added => parted => "master_data"
//================================================================
extern "C" void on_user_added(int user_id,
                              const float* emb_data,
                              size_t emb_count,
                              const char* user_name)
{
    ESP_LOGI(TAG, "on_user_added => id=%d, name=%s", user_id, (user_name ? user_name : ""));

    uint32_t random_prefix = (esp_random() % 900000) + 100000;

    char full_id[16];
    snprintf(full_id, sizeof(full_id), "%lu-%d", (unsigned long)random_prefix, user_id);

    // Rastgele üretilen ID'yi NVS vb. kaydetmek için
    set_user_code(user_id, full_id);

    {
        FILE* lf = fopen("/sdcard/logs.txt", "a");
        if (lf) {
            fprintf(lf, "[LOG] on_user_added => %s name=%s\n", full_id, (user_name ? user_name : ""));
            fclose(lf);
        }
    }

    // Embedding datasının base64'e çevrilmesi
    size_t emb_size = emb_count * sizeof(float);
    std::string b64_str;
    if (emb_data && emb_count > 0) {
        size_t out_len_est = 4 * ((emb_size + 2) / 3);
        std::vector<unsigned char> b64_buf(out_len_est + 1, 0);
        size_t out_len = 0;
        int r = mbedtls_base64_encode(
                    b64_buf.data(), b64_buf.size(), &out_len,
                    (const unsigned char*)emb_data, emb_size
                );
        if (r != 0) {
            ESP_LOGE(TAG, "Base64 fail => %d", r);
            return;
        }
        b64_str.assign((char*)b64_buf.data(), out_len);
    }

    // MQTT veya offline kayda gönderilmek üzere JSON hazırlama
    auto publish_or_store_json = [&](const char* code, const char* name_str,
                                     const char* b64_part, int part_idx) {
        char json_msg[2048];
        if (part_idx == 0) {
            snprintf(json_msg, sizeof(json_msg),
                     "{\"Code\":\"%s\",\"Name\":\"%s\",\"MacAddress\":\"%s\",\"ADD_USER\":\"%s\"}",
                     code, name_str, s_device_mac_str, b64_part);
        } else {
            snprintf(json_msg, sizeof(json_msg),
                     "{\"Code\":\"%s\",\"Name\":\"%s\",\"MacAddress\":\"%s\",\"ADD_USER_PART%d\":\"%s\"}",
                     code, name_str, s_device_mac_str, part_idx, b64_part);
        }

        if (s_wifi_connected && s_internet_available && s_mqtt_connected) {
            esp_mqtt_client_publish(s_mqtt_client,
                                    s_online_master_data_topic,
                                    json_msg, 0, 1, 0);
        } else {
            store_offline_in_logs(s_offline_master_data_topic, json_msg);
        }
    };

    std::string encName = (user_name ? user_name : "");

    // Embedding çok büyükse parça parça gönder
    const size_t CHUNK_SIZE = 600;
    if (b64_str.size() <= CHUNK_SIZE) {
        publish_or_store_json(full_id, encName.c_str(), b64_str.c_str(), 0);
    } else {
        size_t total_len = b64_str.size();
        size_t offset = 0;
        int part_num = 1;
        while (offset < total_len) {
            size_t slice_len = CHUNK_SIZE;
            if (offset + slice_len > total_len) {
                slice_len = total_len - offset;
            }
            std::string part_data = b64_str.substr(offset, slice_len);
            publish_or_store_json(full_id, encName.c_str(), part_data.c_str(), part_num);

            offset += slice_len;
            part_num++;
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
}

//================================================================
// send_mqtt_message => offline=face_logs => online=live_face
//================================================================
extern "C" void send_mqtt_message(const char* event, int id)
{
    char stored_code[64] = {0};
    if (get_user_code(id, stored_code, sizeof(stored_code)) != ESP_OK) {
        snprintf(stored_code, sizeof(stored_code), "%d", id);
    }

    time_t now = 0;
    time(&now);
    struct tm ti;
    memset(&ti, 0, sizeof(ti));
    localtime_r(&now, &ti);

    char date_str[16];
    strftime(date_str, sizeof(date_str), "%Y-%m-%d", &ti);

    char time_str[16];
    strftime(time_str, sizeof(time_str), "%H:%M:%S", &ti);

    char json_msg[512];
    snprintf(json_msg, sizeof(json_msg),
             "{\"cardId\":\"%s\",\"gateDirection\":\"in\",\"macAddress\":\"%s\",\"createAt\":\"%s\",\"createdTime\":\"%s\"}",
             stored_code, s_device_mac_str, date_str, time_str);

    ESP_LOGI(TAG, "[COMMON-LOG]: %s", json_msg);

    {
        FILE* f = fopen("/sdcard/logs.txt", "a");
        if (f) {
            fprintf(f, "%s\n", json_msg);
            fclose(f);
        }
    }

    if (s_wifi_connected && s_internet_available && s_mqtt_connected) {
        esp_mqtt_client_publish(
            s_mqtt_client,
            s_online_face_topic,
            json_msg, 0, 1, 0
        );
    } else {
        store_offline_in_logs(s_offline_face_topic, json_msg);
    }
}

//================================================================
// wifi_event_handler
//================================================================
static void wifi_event_handler(void* arg, esp_event_base_t base,
                               int32_t event_id, void* data)
{
    if (base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            s_wifi_connected     = false;
            s_internet_available = false;
            s_mqtt_connected     = false;
            ESP_LOGW(TAG, "WIFI disco => reconnect in 10s");
            if (s_reconnectTimer) {
                xTimerStart(s_reconnectTimer, 0);
            }
            break;
        default:
            break;
        }
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_wifi_connected     = true;
        s_internet_available = true;

        ip_event_got_ip_t* e = (ip_event_got_ip_t*)data;
        esp_ip4addr_ntoa(&e->ip_info.ip, s_device_ip, sizeof(s_device_ip));

        esp_wifi_get_mac(WIFI_IF_STA, s_device_mac);
        snprintf(s_device_mac_str, sizeof(s_device_mac_str),
                 "%02X:%02X:%02X:%02X:%02X:%02X",
                 s_device_mac[0], s_device_mac[1],
                 s_device_mac[2], s_device_mac[3],
                 s_device_mac[4], s_device_mac[5]);

        ESP_LOGI(TAG, "STA connected => IP=%s => sync time...", s_device_ip);
        if (s_mqtt_client) {
            esp_mqtt_client_start(s_mqtt_client);
        }
        obtain_time();
    }
}

//================================================================
// reconnect_timer_cb
//================================================================
static void reconnect_timer_cb(TimerHandle_t xTimer)
{
    ESP_LOGI(TAG, "[TIMER] => 10s => esp_wifi_connect()");
    esp_wifi_connect();
}

//================================================================
// mqtt_event_handler
//================================================================
static void mqtt_event_handler(void* arg, esp_event_base_t base,
                               int32_t event_id, void* event_data)
{
    esp_mqtt_event_handle_t evt = (esp_mqtt_event_handle_t)event_data;
    int msg_id;
    
    switch (event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT connected");
        s_mqtt_connected = true;
        
        // Camera topic'ine subscribe ol
        ESP_LOGI(TAG, "Subscribing to camera topic: %s", s_camera_topic);
        
        msg_id = esp_mqtt_client_subscribe(s_mqtt_client, s_camera_topic, 1);
        if (msg_id < 0) {
            ESP_LOGE(TAG, "Subscribe failed! Error code: %d", msg_id);
        } else {
            ESP_LOGI(TAG, "Subscribe successful, msg_id=%d", msg_id);
        }
        
        drain_offline_from_logs();
        break;
        
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT disconnected");
        s_mqtt_connected = false;
        break;
    
    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", evt->msg_id);
        break;
        
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "Publish ack => msg_id=%d", evt->msg_id);
        break;
        
    case MQTT_EVENT_DATA:
    {
        // Gelen veriyi null-terminate et
        char* topic = (char*)malloc(evt->topic_len + 1);
        char* data = (char*)malloc(evt->data_len + 1);
        if (!topic || !data) {
            ESP_LOGE(TAG, "Memory allocation failed");
            if (topic) free(topic);
            if (data) free(data);
            break;
        }
        
        memcpy(topic, evt->topic, evt->topic_len);
        memcpy(data, evt->data, evt->data_len);
        topic[evt->topic_len] = 0;
        data[evt->data_len] = 0;
        
        ESP_LOGI(TAG, "*** MQTT Data Received ***");
        ESP_LOGI(TAG, "  Topic: %s", topic);
        ESP_LOGI(TAG, "  Data: %s", data);
        
        // Camera topic kontrolü
        if (strcmp(topic, s_camera_topic) == 0) {
            ESP_LOGI(TAG, "Camera topic matched!");
            
            // "status" içeren yanıtları işleme - bunlar cihazın kendi gönderdiği yanıtlardır
            if (strstr(data, "\"status\":") != NULL) {
                ESP_LOGI(TAG, "Skipping our own status message");
                free(topic);
                free(data);
                break;
            }
            
            // Sadece name alanı içeren mesajları işle
            std::string json(data);
            
            size_t name_pos = json.find("\"name\":");
            if (name_pos != std::string::npos) {
                name_pos = json.find("\"", name_pos + 7);
                if (name_pos != std::string::npos) {
                    size_t name_end = json.find("\"", name_pos + 1);
                    if (name_end != std::string::npos) {
                        std::string name = json.substr(name_pos + 1, name_end - name_pos - 1);
                        ESP_LOGI(TAG, "MQTT enrollment request for: %s", name.c_str());
                        
                        // HTTP server'daki gibi kayıt işlemini başlat
                        request_enroll(name.c_str());
                        
                        // Yanıt gönder
                        char response[128];
                        snprintf(response, sizeof(response),
                                "{\"status\":\"enroll_prepare\",\"name\":\"%s\",\"mac\":\"%s\"}",
                                name.c_str(), s_device_mac_str);
                        
                        ESP_LOGI(TAG, "Publishing response: %s", response);
                        esp_mqtt_client_publish(s_mqtt_client,
                                              s_camera_topic,
                                              response, 0, 1, 0);
                        
                        // HTTP server'daki gibi kaydı tamamla
                        request_save_user();
                    }
                }
            } else {
                ESP_LOGW(TAG, "No name field found in JSON");
            }
        } else {
            ESP_LOGW(TAG, "Topic did not match camera topic");
        }
        
        free(topic);
        free(data);
        break;
    }
    
    default:
        ESP_LOGI(TAG, "Other MQTT event: %d", (int)event_id);
        break;
    }
}

//================================================================
// sd_init
//================================================================
static esp_err_t sd_init(void)
{
    ESP_LOGI(TAG, "Initializing SD card...");
    esp_vfs_fat_sdmmc_mount_config_t cfg = {
        .format_if_mount_failed     = true,
        .max_files                  = 5,
        .allocation_unit_size       = 16 * 1024,
        .disk_status_check_enable   = false,
        // .use_one_fat                = false
    };
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags        = SDMMC_HOST_FLAG_1BIT;
    host.max_freq_khz = SDMMC_FREQ_PROBING;

    sdmmc_slot_config_t slot_cfg = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_cfg.width = 1;
    slot_cfg.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    slot_cfg.clk = GPIO_NUM_39;
    slot_cfg.cmd = GPIO_NUM_38;
    slot_cfg.d0  = GPIO_NUM_40;

    vTaskDelay(pdMS_TO_TICKS(100));
    esp_err_t ret = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot_cfg, &cfg, nullptr);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SD => %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "SD mounted => /sdcard");
    return ESP_OK;
}

//================================================================
// mqtt_init
//================================================================
static void mqtt_init(void)
{
    ESP_LOGI(TAG, "Starting MQTT...");

    esp_mqtt_client_config_t cfg = {};
    cfg.broker.address.uri = s_mqtt_broker_uri;
    cfg.credentials.username = s_mqtt_username;
    cfg.credentials.authentication.password = s_mqtt_password;

    s_mqtt_client = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(
        s_mqtt_client,
        (esp_mqtt_event_id_t)ESP_EVENT_ANY_ID,
        mqtt_event_handler,
        nullptr
    );
}

//================================================================
// wifi_init => AP+STA => pass length>0 => WPA2, else OPEN
//================================================================
static esp_netif_t* s_ap_netif = nullptr;

static void wifi_init(void)
{
    // MAC adresini al
    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    snprintf(s_device_mac_str, sizeof(s_device_mac_str),
             "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "Device MAC: %s", s_device_mac_str);
    
    // Camera topic'i oluştur
    snprintf(s_camera_topic, sizeof(s_camera_topic),
             "bakelor/pr14/prod/%s/camera", s_device_mac_str);
    ESP_LOGI(TAG, "Camera topic: %s", s_camera_topic);

    // 1) NVS'ten Wi-Fi/MQTT ayarlarını çek, yoksa varsayılanları kullan
    if (getWifiConfig(s_wifi_ssid, sizeof(s_wifi_ssid),
                      s_wifi_pass, sizeof(s_wifi_pass)) != ESP_OK)
    {
        strcpy(s_wifi_ssid,  "MY_SSID");  // Varsayılan SSID
        strcpy(s_wifi_pass,  "");
    }

    if (getMqttConfig(s_mqtt_broker_uri, sizeof(s_mqtt_broker_uri),
                      s_mqtt_username,   sizeof(s_mqtt_username),
                      s_mqtt_password,   sizeof(s_mqtt_password)) != ESP_OK)
    {
        strcpy(s_mqtt_broker_uri, "mqtt://192.168.1.14:1883");
        strcpy(s_mqtt_username,   "");
        strcpy(s_mqtt_password,   "");
    }

    // 2) NVS ve netif init
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // 3) AP + STA netif oluştur
    s_ap_netif = esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();

    // 4) Wi-Fi driver init
    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));

    // 5) Event handler kayıt
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID,
        &wifi_event_handler, nullptr, nullptr));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP,
        &wifi_event_handler, nullptr, nullptr));

    // 6) STA config (router'a bağlanırken kullanılacak)
    wifi_config_t sta_cfg = {};
    strncpy((char*)sta_cfg.sta.ssid,     s_wifi_ssid, sizeof(sta_cfg.sta.ssid) - 1);
    strncpy((char*)sta_cfg.sta.password, s_wifi_pass, sizeof(sta_cfg.sta.password) - 1);

    // Eğer şifre boş ise OPEN; aksi takdirde WPA2:
    size_t pass_len = strlen((char*)sta_cfg.sta.password);
    if (pass_len > 0) {
        sta_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        sta_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
        ESP_LOGW(TAG, "[WIFI] => pass_len=0 => using OPEN");
    }

    // İsteğe bağlı: STA'yı aynı kanala sabitlemek isterseniz:
    sta_cfg.sta.channel = 6;  // Router da kanal 6'da olmalı!

    // 7) AP config
    wifi_config_t ap_cfg = {};
    strcpy((char*)ap_cfg.ap.ssid,     "ESP32-AP");
    strcpy((char*)ap_cfg.ap.password, "12345678");
    ap_cfg.ap.ssid_len       = strlen("ESP32-AP");
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.authmode       = WIFI_AUTH_WPA2_PSK;
    // AP'yi de kanal 6'da sabitleyelim
    ap_cfg.ap.channel        = 6;

    // 8) AP+STA mode ve ayarları ata
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP,  &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    // 9) AP IP'sini sabitle (192.168.4.1 /24)
    {
        esp_netif_ip_info_t ip_info;
        memset(&ip_info, 0, sizeof(ip_info));
        ip_info.ip.addr      = ESP_IP4TOADDR(192,168,4,1);
        ip_info.netmask.addr = ESP_IP4TOADDR(255,255,255,0);
        ip_info.gw.addr      = ip_info.ip.addr; // gateway = 192.168.4.1

        // DHCP sunucusunu durdur, sonra yeni IP ver
        ESP_ERROR_CHECK(esp_netif_dhcps_stop(s_ap_netif));
        ESP_ERROR_CHECK(esp_netif_set_ip_info(s_ap_netif, &ip_info));
        ESP_ERROR_CHECK(esp_netif_dhcps_start(s_ap_netif));

        ESP_LOGI(TAG, "[AP IP] => 192.168.4.1/24 (Static)");
    }

    // 10) Opsiyonel: reconnect timer
    s_reconnectTimer = xTimerCreate("reconnectTimer",
                                    pdMS_TO_TICKS(10000),
                                    pdFALSE, // tek seferlik
                                    nullptr,
                                    reconnect_timer_cb);

    // 11) Dahili web server vs. başlat
    start_webserver();

    ESP_LOGI(TAG,"AP+STA Mode => see logs");
    // Powersave kapat (opsiyonel)
    esp_wifi_set_ps(WIFI_PS_NONE);
}

//================================================================
// request_enroll, request_save_user, request_delete
//================================================================
extern "C" void request_enroll(const char* name)
{
    ESP_LOGI(TAG, "request_enroll => %s", name);
    strncpy(pending_enroll_name, name, sizeof(pending_enroll_name) - 1);
    enroll_mode       = true;
    enroll_requested  = true;
}

extern "C" void request_save_user(void)
{
    ESP_LOGI(TAG, "request_save_user => do_enroll_now");
    do_enroll_now = true;
}

extern "C" void request_delete(int user_id)
{
    ESP_LOGI(TAG, "request_delete => user_id=%d", user_id);
    set_delete_id(user_id);
    recognizer_state_t st = DELETE;
    xQueueSend(xQueueEventLogic, &st, portMAX_DELAY);
}

//================================================================
// handle_face_info
//================================================================
static void handle_face_info(face_info_t* info)
{
    ESP_LOGI(TAG, "face_info => ID=%d sim=%.3f", info->id, info->similarity);
    
    // Eğer kayıt modu aktifse ve yüz algılandıysa otomatik kaydet
    if (enroll_mode && enroll_requested && info->id < 0) {
        ESP_LOGI(TAG, "Auto-enrolling detected face for user: %s", pending_enroll_name);
        
        // Yüzü kaydet
        do_enroll_now = true;
        request_save_user();
        
        // Kayıt durumunu MQTT ile bildir
        if (s_mqtt_connected) {
            char response[128];
            snprintf(response, sizeof(response),
                    "{\"status\":\"enrolled\",\"name\":\"%s\",\"mac\":\"%s\"}",
                    pending_enroll_name, s_device_mac_str);
            
            esp_mqtt_client_publish(s_mqtt_client,
                                  s_camera_topic,
                                  response, 0, 1, 0);
        }
        
        // Kayıt modunu kapat
        enroll_mode = false;
        enroll_requested = false;
        
        ESP_LOGI(TAG, "Enrollment completed for user: %s", pending_enroll_name);
    }
    // Yüz tanıma modunda ve yüz tanındıysa
    else if (!enroll_mode && info->id >= 0 && info->similarity > 0.5) {
        ESP_LOGI(TAG, "Face recognized: ID=%d sim=%.3f", info->id, info->similarity);
        
        // MQTT ile bildir
        if (s_mqtt_connected) {
            char response[128];
            snprintf(response, sizeof(response),
                    "{\"status\":\"recognized\",\"id\":%d,\"similarity\":%.3f,\"mac\":\"%s\"}",
                    info->id, info->similarity, s_device_mac_str);
            
            esp_mqtt_client_publish(s_mqtt_client,
                                  s_camera_topic,
                                  response, 0, 1, 0);
        }
        
        // Tanıma durumunu güncelle ve MQTT mesajı gönder
        recognizer_state_t state = RECOGNIZE;
        xQueueSend(xQueueEventLogic, &state, portMAX_DELAY);
        send_mqtt_message("face_recognized", info->id);
    }
}

//================================================================
// app_main
//================================================================
extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "Application start...");

    // SD kartı initialize etmeye çalış
    if (sd_init() != ESP_OK) {
        ESP_LOGW(TAG, "No SD => logs in RAM only => offline won't work!");
    }

    // RTOS Queues oluştur
    xQueueAIFrame    = xQueueCreate(2, sizeof(camera_fb_t*));
    xQueueLCDFrame   = xQueueCreate(2, sizeof(camera_fb_t*));
    xQueueEventLogic = xQueueCreate(10, sizeof(recognizer_state_t));
    xQueueResult     = xQueueCreate(10, sizeof(face_info_t));

    // Kamera, yüz tanıma ve LCD ile ilgili kayıtlar
    register_camera(PIXFORMAT_RGB565, FRAMESIZE_240X240, 1, xQueueAIFrame);
    register_human_face_recognition(xQueueAIFrame, xQueueEventLogic, xQueueResult, xQueueLCDFrame, true);
    register_lcd(xQueueLCDFrame, nullptr, true);

    // NVS'ten kullanıcı verilerinin yüklenmesi
    init_user_store();

    // Wi-Fi ve MQTT başlat
    wifi_init();
    mqtt_init();

    ESP_LOGI(TAG, "Application ready.");

    recognizer_state_t event_state;
    face_info_t face_info_data;

    // Sonsuz döngü: Kuyrukları dinle, gelen verileri işle
    while (true) {
        if (xQueueReceive(xQueueResult, &face_info_data, pdMS_TO_TICKS(10))) {
            handle_face_info(&face_info_data);
        }
        if (xQueueReceive(xQueueEventLogic, &event_state, pdMS_TO_TICKS(10))) {
            // event_logic veya yüz tanıma hâl değiştirdiğinde yapılacaklar
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
