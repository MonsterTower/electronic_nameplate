idf_component_register(
    SRCS "main.c" "battery_monitor.c" "epaper_bus.c" "epaper_driver.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_gpio esp_adc esp_driver_spi
)
