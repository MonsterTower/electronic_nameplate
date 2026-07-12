# Hardware Bring-up

此目录是 ESP32-S3 真实硬件的独立 ESP-IDF 项目，与根目录的 Wokwi 仿真项目互不干扰。

当前第一版只验证按键驱动对应 LED：

| 按键 | 模组管脚 / GPIO | LED 模组管脚 / GPIO |
| --- | --- | --- |
| BOOT | 27 / GPIO0 | 23 / GPIO21 |
| BUT- | 32 / GPIO39 | 21 / GPIO13 |
| BUT+ | 33 / GPIO40 | 22 / GPIO14 |

按键使用内部上拉，按下为低电平；LED 默认高电平点亮。程序使用 30ms 防抖，并在串口输出稳定的按下和释放事件。

```powershell
cd hardware_bringup
. D:\esp\v6.0.2\esp-idf\export.ps1
idf.py build
```
