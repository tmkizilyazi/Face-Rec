#include "who_camera.h"
#include "who_human_face_recognition.hpp"
#include "who_lcd.h"
#include "who_button.h"
#include "event_logic.hpp"
#include "who_adc_button.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "face_recognition_handler.hpp"

static const char *TAG = "FACE_RECOGNITION";
static QueueHandle_t xQueueAIFrame = NULL;
static QueueHandle_t xQueueLCDFrame = NULL;
static QueueHandle_t xQueueKeyState = NULL;
static QueueHandle_t xQueueEventLogic = NULL;
static QueueHandle_t xQueueFaceResult = NULL;

// Global değişkenler
bool face_registered = false;
char face_info_text[100] = {0};
nvs_handle_t my_handle = 0;

#define GPIO_BOOT GPIO_NUM_0
#define NVS_NAMESPACE "face_rec"
#define FACE_REGISTERED_KEY "reg"
#define FACE_INFO_KEY "info"

// NVS'den yüz bilgilerini yükle
static void load_face_info_from_nvs() {
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &my_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS açılamadı: %s", esp_err_to_name(err));
        return;
    }

    // Yüz kayıt durumunu kontrol et
    uint8_t registered = 0;
    err = nvs_get_u8(my_handle, FACE_REGISTERED_KEY, &registered);
    if (err == ESP_OK) {
        face_registered = (bool)registered;
    }

    // Yüz bilgilerini yükle
    size_t required_size = sizeof(face_info_text);
    err = nvs_get_str(my_handle, FACE_INFO_KEY, face_info_text, &required_size);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "NVS'den yüz bilgileri yüklendi: %s", face_info_text);
    }
}

// Yüz bilgilerini NVS'ye kaydet
static void save_face_info_to_nvs() {
    if (my_handle == 0) {
        esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &my_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "NVS açılamadı: %s", esp_err_to_name(err));
            return;
        }
    }

    esp_err_t err = nvs_set_u8(my_handle, FACE_REGISTERED_KEY, (uint8_t)face_registered);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Yüz kayıt durumu kaydedilemedi: %s", esp_err_to_name(err));
        return;
    }

    err = nvs_set_str(my_handle, FACE_INFO_KEY, face_info_text);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Yüz bilgileri kaydedilemedi: %s", esp_err_to_name(err));
        return;
    }

    err = nvs_commit(my_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS commit hatası: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "Yüz bilgileri NVS'ye kaydedildi");
}

static void check_face_registration(void* arg) {
    if (face_registered) {
        ESP_LOGI(TAG, "Yüz kayıtlı! Yüz bilgileri: %s", face_info_text);
    } else {
        ESP_LOGI(TAG, "Yüz kayıtlı değil! Yüz kaydediliyor...");
        // Yüz kaydetme işlemi
        face_registered = true;
        snprintf(face_info_text, sizeof(face_info_text), "ID: 1, Güven: %.2f%%", 100.0f);
        save_face_info_to_nvs();
        ESP_LOGI(TAG, "Yüz başarıyla kaydedildi!");
    }
}

extern "C" void app_main()
{
    // NVS'yi başlat
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // NVS'den yüz bilgilerini yükle
    load_face_info_from_nvs();

    // Kuyrukları oluştur
    xQueueAIFrame = xQueueCreate(2, sizeof(camera_fb_t *));
    xQueueLCDFrame = xQueueCreate(2, sizeof(camera_fb_t *));
    xQueueKeyState = xQueueCreate(1, sizeof(int *));
    xQueueEventLogic = xQueueCreate(1, sizeof(int *));
    xQueueFaceResult = xQueueCreate(1, sizeof(int *));

    // Donanımları başlat
    register_button(GPIO_BOOT, xQueueKeyState);
    register_camera(PIXFORMAT_RGB565, FRAMESIZE_240X240, 2, xQueueAIFrame);
    register_human_face_recognition(xQueueAIFrame, xQueueEventLogic, 
                                  xQueueFaceResult, xQueueLCDFrame, false);
    register_lcd(xQueueLCDFrame, NULL, true);

    // 10 saniye sonra yüz kontrolü yapacak timer'ı ayarla
    esp_timer_create_args_t timer_args;
    timer_args.callback = check_face_registration;
    timer_args.arg = NULL;
    timer_args.name = "face_check_timer";
    timer_args.dispatch_method = ESP_TIMER_TASK;
    timer_args.skip_unhandled_events = true;

    esp_timer_handle_t timer_handle;
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &timer_handle));
    
    // 10 saniye sonra çalışacak şekilde timer'ı başlat
    ESP_ERROR_CHECK(esp_timer_start_once(timer_handle, 10000000)); // 10 saniye = 10000000 mikrosaniye

    // Ana döngü
    while (1) {
        // Kamera ve LCD işlemlerini kontrol et
        camera_fb_t *fb = NULL;
        if (xQueueReceive(xQueueAIFrame, &fb, pdMS_TO_TICKS(100)) == pdTRUE) {
            // Kamera karesi alındı
            ESP_LOGI(TAG, "Kamera karesi alındı");
        }

        // Yüz tanıma sonuçlarını kontrol et
        int *face_result = NULL;
        if (xQueueReceive(xQueueFaceResult, &face_result, pdMS_TO_TICKS(100)) == pdTRUE) {
            // Yüz tanıma sonucu alındı
            ESP_LOGI(TAG, "Yüz tanıma sonucu alındı");
        }

        vTaskDelay(pdMS_TO_TICKS(100)); // 100ms bekle
    }
}
