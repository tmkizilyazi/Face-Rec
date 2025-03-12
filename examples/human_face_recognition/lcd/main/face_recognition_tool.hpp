#pragma once

#include <string>
#include <vector>
#include <utility> // std::pair için

// Asıl face_info_t yapısı için model_zoo/face_recognition_tool.hpp dosyasını dahil ediyoruz
#include "model_zoo/face_recognition_tool.hpp"

// C++ fonksiyonu olarak bildirim
std::vector<std::pair<int, std::string>> get_enrolled_list(); 