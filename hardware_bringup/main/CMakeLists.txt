idf_component_register(
    SRCS "main.c" "battery_monitor.c" "epaper_bus.c" "epaper_driver.c"
         "app_model.c" "display_surface.c" "display_pages.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_gpio esp_adc esp_driver_spi u8g2
)
