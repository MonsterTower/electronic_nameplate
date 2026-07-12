#pragma once

#include "esp_err.h"

/* SSD1683 驱动层：完成一次全刷测试图案后由调用者决定何时休眠。 */
esp_err_t epaper_driver_show_test_pattern(void);
esp_err_t epaper_driver_sleep(void);
