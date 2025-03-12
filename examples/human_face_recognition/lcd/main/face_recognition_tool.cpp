#include "face_recognition_tool.hpp"
#include <string>
#include <vector>
#include <utility>

// HTTP sunucusu tarafından kullanılan fonksiyon 
// extern "C" bildirimini kaldırıyorum
std::vector<std::pair<int, std::string>> get_enrolled_list() {
    std::vector<std::pair<int, std::string>> result;
    
    // TODO: Gerçek yüz listesini döndür
    // Şimdilik test için birkaç örnek
    result.push_back(std::make_pair(1, "Test User 1"));
    result.push_back(std::make_pair(2, "Test User 2"));
    
    return result;
} 