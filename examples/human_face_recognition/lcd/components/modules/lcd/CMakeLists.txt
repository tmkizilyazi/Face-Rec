set(srcs "who_lcd.c" "logo_en_240x240_lcd.c")
set(include_dirs ".")

set(requires spi_flash driver esp_lcd esp_timer)

# LCD bileşenini kaydet
idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS ${include_dirs}
                       REQUIRES ${requires}
                       PRIV_REQUIRES esp_timer) 