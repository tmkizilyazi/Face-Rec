#pragma once

#include <esp_log.h>
#include "who_human_face_recognition.hpp"

// Yüz bilgilerini tutacak yapı
struct face_info_t {
    bool is_registered;
    int id;
    float confidence;
};

// Global değişkenler
extern bool face_registered;
extern char face_info_text[100];

class FaceRecognitionHandler {
public:
    static void onFaceDetected(const int& result) {
        // Yüz tespit edildiğinde yapılacak işlemler
        ESP_LOGI("FACE_RECOGNITION", "Yüz tespit edildi!");
        
        // Yüz bilgilerini kaydet
        if (result > 0) {
            face_registered = true;
            snprintf(face_info_text, sizeof(face_info_text), "ID: %d, Güven: %.2f%%", 
                    result, 100.0f);
        }
    }

    static void onFaceRegistered(const int& result) {
        // Yüz kaydedildiğinde yapılacak işlemler
        ESP_LOGI("FACE_RECOGNITION", "Yüz başarıyla kaydedildi!");
        face_registered = true;
        snprintf(face_info_text, sizeof(face_info_text), "ID: %d, Güven: %.2f%%", 
                result, 100.0f);
    }
}; 