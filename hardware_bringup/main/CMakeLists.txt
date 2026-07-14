idf_component_register(
    SRCS "main.c" "battery_monitor.c" "audio_test.c" "ai_service.c" "epaper_bus.c" "epaper_driver.c" "app_cache.c"
         "power_manager.c"
         "app_model.c" "display_surface.c" "display_pages.c"
         "network_service.c" "weather_service.c" "schedule_service.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_gpio esp_driver_i2s esp_adc esp_driver_spi u8g2
             esp_wifi esp_netif esp_event lwip nvs_flash
             esp_http_client esp-tls mbedtls wpa_supplicant cjson esp_pm esp_timer
)
