#include "esp_http_server.h"
#include "esp_log.h"
#include "http_server.h"
#include <string.h>
#include <stdlib.h>
#include "nvs_user_store.h"
#include "esp_system.h" // esp_restart()

#include <string>
#include <vector>

// get_enrolled_list fonksiyonunu kullanacağız
// extern "C" bildirimini çıkarttım, normal C++ fonksiyonu olarak deneyelim
std::vector<std::pair<int, std::string>> get_enrolled_list();
extern "C" esp_err_t get_user_code(int id, char* code_buf, size_t buf_size);

static const char *TAG = "HTTP_SERVER";

extern const unsigned char index_html_start[] asm("_binary_index_html_start");
extern const unsigned char index_html_end[]   asm("_binary_index_html_end");

// ======================================================
// URL Decode Fonksiyonu => UTF-8 dahil özel karakterleri
// %XX'den çözer + '+' => ' '
// ======================================================
static void url_decode(char* dst, const char* src, size_t dstSize)
{
    size_t ri = 0, wi = 0;
    while (src[ri] != '\0' && wi < (dstSize - 1)) {
        if (src[ri] == '%' && src[ri+1] && src[ri+2]) {
            char hex[3];
            hex[0] = src[ri+1];
            hex[1] = src[ri+2];
            hex[2] = '\0';
            int val = strtol(hex, NULL, 16); // 16 taban
            dst[wi++] = (char)val;
            ri += 3;
        }
        else if (src[ri] == '+') {
            dst[wi++] = ' ';
            ri++;
        } else {
            dst[wi++] = src[ri++];
        }
    }
    dst[wi] = '\0';
}

// -- Prototipler (app_main veya benzeri yerden):
extern "C" void request_enroll(const char* name);
extern "C" void request_save_user(void);
extern "C" void request_delete(int user_id);

// --------- Yardımcılar (MIME tipi) ----------
static void set_html_resp_utf8(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=UTF-8");
    httpd_resp_set_hdr(req,"Content-Type","text/html; charset=UTF-8");
}
static void set_json_resp_utf8(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json; charset=UTF-8");
    httpd_resp_set_hdr(req,"Content-Type","application/json; charset=UTF-8");
}

// ======================================================
//  / => index.html dön
// ======================================================
static esp_err_t root_get_handler(httpd_req_t *req)
{
    size_t sz = index_html_end - index_html_start;
    set_html_resp_utf8(req);
    httpd_resp_send(req, (const char*)index_html_start, sz);
    return ESP_OK;
}

// ======================================================
//  /users => kayıtlı liste (id, code, name) JSON
// ======================================================
static esp_err_t users_get_handler(httpd_req_t *req)
{
    set_json_resp_utf8(req);

    auto userList = get_enrolled_list();

    // JSON dizi oluştur => [{"id":...,"code":"...", "name":"..."}, ...]
    std::string json = "[";
    for(size_t i = 0; i < userList.size(); i++){
        if(i > 0) json += ",";

        int face_id          = userList[i].first;
        std::string faceName = userList[i].second;

        // Rastgele kod => NVS'ten çek
        char codeBuf[64] = {0};
        if(get_user_code(face_id, codeBuf, sizeof(codeBuf)) != ESP_OK){
            snprintf(codeBuf, sizeof(codeBuf), "%d", face_id);
        }

        char tmp[256];
        snprintf(tmp, sizeof(tmp),
                 "{\"id\":%d,\"code\":\"%s\",\"name\":\"%s\"}",
                 face_id, codeBuf, faceName.c_str());
        json += tmp;
    }
    json += "]";

    httpd_resp_sendstr(req, json.c_str());
    return ESP_OK;
}

// ======================================================
//  /add_user?name=... => Enroll prepare
// ======================================================
static esp_err_t add_user_handler(httpd_req_t* req)
{
    char query[128];
    if(httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK){
        char name_enc[64];
        if(httpd_query_key_value(query, "name", name_enc, sizeof(name_enc)) == ESP_OK){
            // --- URL decode ---
            char decoded[64];
            url_decode(decoded, name_enc, sizeof(decoded));

            // -> Kaydı başlat
            request_enroll(decoded);

            set_json_resp_utf8(req);
            httpd_resp_sendstr(req, "{\"status\":\"enroll_prepare\"}");
            return ESP_OK;
        }
    }
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing name");
    return ESP_FAIL;
}

// ======================================================
//  /save_user => kaydı tamamla
// ======================================================
static esp_err_t save_user_handler(httpd_req_t* req)
{
    request_save_user();
    set_json_resp_utf8(req);
    httpd_resp_sendstr(req, "{\"status\":\"enroll_requested\"}");
    return ESP_OK;
}

// ======================================================
//  /delete_user?id=... => Sil
// ======================================================
static esp_err_t delete_user_handler(httpd_req_t* req)
{
    char query[64];
    if(httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK){
        char id_str[8];
        if(httpd_query_key_value(query, "id", id_str, sizeof(id_str)) == ESP_OK){
            int user_id = atoi(id_str);
            request_delete(user_id);

            set_json_resp_utf8(req);
            httpd_resp_sendstr(req, "{\"status\":\"delete_requested\"}");
            return ESP_OK;
        }
    }
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing id");
    return ESP_FAIL;
}

// ======================================================
//  /set_config?ssid=...&pass=...&broker=...&user=...&mpass=...
//  => Wi-Fi / MQTT ayarlarını sakla, sonra restart
// ======================================================
static esp_err_t set_config_handler(httpd_req_t* req)
{
    char query[300];
    if(httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK){
        char ssidEnc[64]={0}, passEnc[64]={0};
        char brokerEnc[128]={0}, muserEnc[64]={0}, mpassEnc[64]={0};

        httpd_query_key_value(query,"ssid",   ssidEnc,   sizeof(ssidEnc));
        httpd_query_key_value(query,"pass",   passEnc,   sizeof(passEnc));
        httpd_query_key_value(query,"broker", brokerEnc, sizeof(brokerEnc));
        httpd_query_key_value(query,"user",   muserEnc,  sizeof(muserEnc));
        httpd_query_key_value(query,"mpass",  mpassEnc,  sizeof(mpassEnc));

        // Decode
        char ssid[64], wpass[64], broker[128], muser[64], mpass[64];
        url_decode(ssid,   ssidEnc,   sizeof(ssid));
        url_decode(wpass,  passEnc,   sizeof(wpass));
        url_decode(broker, brokerEnc, sizeof(broker));
        url_decode(muser,  muserEnc,  sizeof(muser));
        url_decode(mpass,  mpassEnc,  sizeof(mpass));

        ESP_LOGI(TAG, "set_config => ssid='%s', pass='%s'", ssid, wpass);
        ESP_LOGI(TAG, "set_config => broker='%s', user='%s', mpass='%s'", broker, muser, mpass);

        if(strlen(ssid) == 0){
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing SSID");
            return ESP_FAIL;
        }

        esp_err_t wErr = storeWifiConfig(ssid, wpass);
        esp_err_t mErr = storeMqttConfig(broker, muser, mpass);

        set_json_resp_utf8(req);
        if(wErr == ESP_OK && mErr == ESP_OK){
            httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
            esp_restart();
        } else {
            httpd_resp_sendstr(req, "{\"status\":\"fail\"}");
        }
        return ESP_OK;
    }
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing query");
    return ESP_FAIL;
}

// ======================================================
//  /get_config => o anki Wi-Fi + MQTT ayarlarını döndür
// ======================================================
static esp_err_t get_config_handler(httpd_req_t* req)
{
    char ssid[64]={0}, pass[64]={0};
    char broker[128]={0}, user[64]={0}, mp[64]={0};

    esp_err_t rw = getWifiConfig(ssid,  sizeof(ssid),
                                 pass,  sizeof(pass));
    esp_err_t rm = getMqttConfig(broker,sizeof(broker),
                                 user,  sizeof(user),
                                 mp,    sizeof(mp));

    char json[512];
    snprintf(json, sizeof(json),
             "{\"wifi_ssid\":\"%s\",\"wifi_pass\":\"%s\","
             "\"mqtt_broker\":\"%s\",\"mqtt_user\":\"%s\",\"mqtt_pass\":\"%s\"}",
             (rw==ESP_OK? ssid:""), (rw==ESP_OK? pass:""),
             (rm==ESP_OK? broker:""), (rm==ESP_OK? user:""), (rm==ESP_OK? mp:"")
    );

    set_json_resp_utf8(req);
    httpd_resp_sendstr(req, json);
    return ESP_OK;
}

// ======================================================
//  start_webserver()
// ======================================================
void start_webserver(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    ESP_LOGI(TAG, "Starting HTTP server...");
    httpd_handle_t server = nullptr;
    if(httpd_start(&server, &cfg) == ESP_OK){
        // 1) Root => index.html
        httpd_uri_t uri_root = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = root_get_handler,
            .user_ctx = nullptr
        };
        httpd_register_uri_handler(server, &uri_root);

        // 2) /users
        httpd_uri_t uri_users = {
            .uri="/users",
            .method=HTTP_GET,
            .handler=users_get_handler,
            .user_ctx=nullptr
        };
        httpd_register_uri_handler(server, &uri_users);

        // 3) /add_user
        httpd_uri_t uri_add = {
            .uri="/add_user",
            .method=HTTP_GET,
            .handler=add_user_handler,
            .user_ctx=nullptr
        };
        httpd_register_uri_handler(server, &uri_add);

        // 4) /save_user
        httpd_uri_t uri_save = {
            .uri="/save_user",
            .method=HTTP_GET,
            .handler=save_user_handler,
            .user_ctx=nullptr
        };
        httpd_register_uri_handler(server, &uri_save);

        // 5) /delete_user
        httpd_uri_t uri_del = {
            .uri="/delete_user",
            .method=HTTP_GET,
            .handler=delete_user_handler,
            .user_ctx=nullptr
        };
        httpd_register_uri_handler(server, &uri_del);

        // 6) /set_config
        httpd_uri_t uri_cfg = {
            .uri="/set_config",
            .method=HTTP_GET,
            .handler=set_config_handler,
            .user_ctx=nullptr
        };
        httpd_register_uri_handler(server, &uri_cfg);

        // 7) /get_config
        httpd_uri_t uri_getcfg = {
            .uri="/get_config",
            .method=HTTP_GET,
            .handler=get_config_handler,
            .user_ctx=nullptr
        };
        httpd_register_uri_handler(server, &uri_getcfg);

        ESP_LOGI(TAG, "HTTP server ready => / , /set_config, /get_config, /users, etc.");
    } else {
        ESP_LOGE(TAG, "Failed to start HTTP server");
    }
}
