idf_component_register(
  SRCS "src/custom-bsp.cpp"
  INCLUDE_DIRS "include"
  REQUIRES
    "driver"
    "esp_lcd"
    "esp_timer"
    "lvgl"
    "esp_lcd_ili9341"
    "heap"
    "freertos"
)
