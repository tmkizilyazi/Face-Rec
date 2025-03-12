#pragma once
#include <stdint.h>

// Logo boyutları
static int logo_en_240x240_lcd_width = 240;
static int logo_en_240x240_lcd_height = 240;

// DISPLAY_CAMERA_DIRECTLY tanımlıysa boş bir logo (siyah) kullanılır
#ifdef DISPLAY_CAMERA_DIRECTLY
// Boş logo (siyah ekran) - kamera görüntüsü için
static const uint16_t logo_en_240x240_lcd[57600] = {0}; // Tüm değerler 0 (siyah)
#else
// Normal durumda logo verisi buraya gelecek
// Orijinal logo çok büyük olduğu için sadece başlangıç kısmını gösteriyoruz
static const uint16_t logo_en_240x240_lcd[57600] = {
    65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 
    // ... (geri kalanı genellikle orijinal koddan alınır)
    65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535
};
#endif 