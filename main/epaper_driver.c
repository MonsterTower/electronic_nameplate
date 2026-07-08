#include "epaper_driver.h"

#include <stdbool.h>
#include <stdio.h>

#include "epaper_bus.h"

#define EPD_CMD_DRIVER_OUTPUT_CONTROL 0x01
#define EPD_CMD_DEEP_SLEEP_MODE 0x10
#define EPD_CMD_DATA_ENTRY_MODE 0x11
#define EPD_CMD_SW_RESET 0x12
#define EPD_CMD_WRITE_RAM_BW 0x24
#define EPD_CMD_WRITE_RAM_COLOR 0x26
#define EPD_CMD_DISPLAY_UPDATE_CONTROL_1 0x21
#define EPD_CMD_DISPLAY_UPDATE_CONTROL_2 0x22
#define EPD_CMD_MASTER_ACTIVATION 0x20
#define EPD_CMD_BORDER_WAVEFORM_CONTROL 0x3C
#define EPD_CMD_SET_RAM_X_ADDRESS 0x44
#define EPD_CMD_SET_RAM_Y_ADDRESS 0x45
#define EPD_CMD_SET_RAM_X_COUNTER 0x4E
#define EPD_CMD_SET_RAM_Y_COUNTER 0x4F
#define EPD_CMD_TEMPERATURE_SENSOR_CONTROL 0x18

#define EPD_BUSY_LEVEL 1
#define EPD_BUSY_TIMEOUT_MS 20000
#define EPD_ENTRY_MODE_XY_INCREMENT 0x03
#define EPD_POWER_ON_SEQUENCE 0xC0
#define EPD_POWER_OFF_SEQUENCE 0x83
#define EPD_FULL_UPDATE_SEQUENCE 0xF4
#define EPD_POWER_OFF_AFTER_REFRESH 0

static bool s_power_is_on;

static void epaper_driver_set_memory_area(void)
{
    /* GDEM029T94 物理显存是 128x296，X 以字节为单位，Y 以像素行为单位。 */
    epaper_driver_write_command(EPD_CMD_SET_RAM_X_ADDRESS);
    epaper_driver_write_data(0x00);
    epaper_driver_write_data((EPAPER_MEMORY_BYTES_PER_ROW - 1) & 0xFF);

    epaper_driver_write_command(EPD_CMD_SET_RAM_Y_ADDRESS);
    epaper_driver_write_data(0x00);
    epaper_driver_write_data(0x00);
    epaper_driver_write_data((EPAPER_MEMORY_HEIGHT - 1) & 0xFF);
    epaper_driver_write_data(((EPAPER_MEMORY_HEIGHT - 1) >> 8) & 0xFF);
}

static void epaper_driver_set_memory_pointer(void)
{
    /* 每次写显存前都把地址指针归零，避免上一次刷新后指针停在末尾。 */
    epaper_driver_write_command(EPD_CMD_SET_RAM_X_COUNTER);
    epaper_driver_write_data(0x00);

    epaper_driver_write_command(EPD_CMD_SET_RAM_Y_COUNTER);
    epaper_driver_write_data(0x00);
    epaper_driver_write_data(0x00);
    epaper_driver_wait_busy();
}

static void epaper_driver_init_display(void)
{
    /*
     * 这里按 GxEPD 的 GxGDEM029T94::_InitDisplay() 对齐。
     * 页面层使用横屏，但控制器参数必须仍按 128x296 物理方向设置。
     */
    epaper_driver_write_command(EPD_CMD_SW_RESET);
    epaper_bus_delay_ms(10);
    epaper_driver_wait_busy();

    epaper_driver_write_command(EPD_CMD_DRIVER_OUTPUT_CONTROL);
    epaper_driver_write_data((EPAPER_MEMORY_HEIGHT - 1) & 0xFF);
    epaper_driver_write_data(((EPAPER_MEMORY_HEIGHT - 1) >> 8) & 0xFF);
    epaper_driver_write_data(0x00);

    epaper_driver_write_command(EPD_CMD_BORDER_WAVEFORM_CONTROL);
    epaper_driver_write_data(0x05);

    epaper_driver_write_command(EPD_CMD_DISPLAY_UPDATE_CONTROL_1);
    epaper_driver_write_data(0x00);
    epaper_driver_write_data(0x80);

    epaper_driver_write_command(EPD_CMD_TEMPERATURE_SENSOR_CONTROL);
    epaper_driver_write_data(0x80);

    epaper_driver_write_command(EPD_CMD_DATA_ENTRY_MODE);
    epaper_driver_write_data(EPD_ENTRY_MODE_XY_INCREMENT);

    epaper_driver_set_memory_area();
    epaper_driver_set_memory_pointer();
}

static void epaper_driver_power_on(void)
{
    if (s_power_is_on) {
        return;
    }

    /* GxEPD 在写显存前先执行 PowerOn，避免控制器还没进入可写刷新状态。 */
    epaper_driver_write_command(EPD_CMD_DISPLAY_UPDATE_CONTROL_2);
    epaper_driver_write_data(EPD_POWER_ON_SEQUENCE);
    epaper_driver_write_command(EPD_CMD_MASTER_ACTIVATION);
    epaper_driver_wait_busy();
    s_power_is_on = true;
}

#if EPD_POWER_OFF_AFTER_REFRESH
static void epaper_driver_power_off(void)
{
    if (!s_power_is_on) {
        return;
    }

    epaper_driver_write_command(EPD_CMD_DISPLAY_UPDATE_CONTROL_2);
    epaper_driver_write_data(EPD_POWER_OFF_SEQUENCE);
    epaper_driver_write_command(EPD_CMD_MASTER_ACTIVATION);
    epaper_driver_wait_busy();
    s_power_is_on = false;
}
#endif

static void epaper_driver_write_ram(uint8_t command, const uint8_t *buffer, size_t length)
{
    epaper_driver_set_memory_pointer();
    epaper_driver_write_command(command);
    epaper_bus_write_data_buffer(buffer, length);
}

static void epaper_driver_write_ram_filled(uint8_t command, uint8_t data, size_t length)
{
    epaper_driver_set_memory_pointer();
    epaper_driver_write_command(command);
    for (size_t i = 0; i < length; ++i) {
        epaper_driver_write_data(data);
    }
}

void epaper_driver_init(void)
{
    epaper_bus_init();
    epaper_bus_reset();
    s_power_is_on = false;
}

void epaper_driver_write_command(uint8_t command)
{
    epaper_bus_write_command(command);
}

void epaper_driver_write_data(uint8_t data)
{
    epaper_bus_write_data(data);
}

void epaper_driver_wait_busy(void)
{
    uint32_t elapsed_ms = 0;

    /* BUSY 拉高表示屏幕内部还在处理；加超时避免接线错误时主循环卡死。 */
    while (epaper_bus_is_busy() == EPD_BUSY_LEVEL) {
        if (elapsed_ms >= EPD_BUSY_TIMEOUT_MS) {
            printf("epaper: busy wait timeout\n");
            return;
        }
        epaper_bus_delay_ms(10);
        elapsed_ms += 10;
    }
}

void epaper_driver_write_framebuffer(const uint8_t *buffer, size_t length)
{
    epaper_driver_write_framebuffers(buffer, NULL, length);
}

void epaper_driver_write_framebuffers(const uint8_t *black_buffer, const uint8_t *red_buffer, size_t length)
{
    /*
     * 对齐 GxEPD::update()：每次全刷前重新初始化显示、上电，再写黑白 RAM 和第二 RAM。
     * 三色屏的 0x26 是红色平面；没有红色内容时写 0x00，避免仿真停在红底。
     */
    epaper_driver_init_display();
    epaper_driver_power_on();
    epaper_driver_write_ram(EPD_CMD_WRITE_RAM_BW, black_buffer, length);
    if (red_buffer != NULL) {
        epaper_driver_write_ram(EPD_CMD_WRITE_RAM_COLOR, red_buffer, length);
    } else {
        epaper_driver_write_ram_filled(EPD_CMD_WRITE_RAM_COLOR, 0x00, length);
    }
}

void epaper_driver_refresh(void)
{
    /* 写完显存后触发全刷，等待 BUSY 释放才算刷新完成。 */
    epaper_driver_write_command(EPD_CMD_DISPLAY_UPDATE_CONTROL_2);
    epaper_driver_write_data(EPD_FULL_UPDATE_SEQUENCE);
    epaper_driver_write_command(EPD_CMD_MASTER_ACTIVATION);
    epaper_driver_wait_busy();
#if EPD_POWER_OFF_AFTER_REFRESH
    epaper_driver_power_off();
#endif
}

void epaper_driver_sleep(void)
{
    epaper_driver_write_command(EPD_CMD_DEEP_SLEEP_MODE);
    epaper_driver_write_data(0x01);
    epaper_bus_delay_ms(100);
}
