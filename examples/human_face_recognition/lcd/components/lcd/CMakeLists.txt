cmake_minimum_required(VERSION 3.5)

set(src_dirs        .)
set(include_dirs    .)

# Register the component with ESP-IDF build system
idf_component_register(SRC_DIRS ${src_dirs}
                      INCLUDE_DIRS ${include_dirs}
                      REQUIRES esp_lcd driver esp_timer modules esp-dl)