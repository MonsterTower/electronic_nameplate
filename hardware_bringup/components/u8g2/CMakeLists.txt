idf_component_register(
    SRCS
        "csrc/u8g2_buffer.c"
        "csrc/u8g2_font.c"
        "csrc/u8g2_hvline.c"
        "csrc/u8g2_intersection.c"
        "csrc/u8g2_kerning.c"
        "csrc/u8g2_ll_hvline.c"
        "csrc/u8g2_setup.c"
        "csrc/u8x8_8x8.c"
        "csrc/u8x8_display.c"
        "csrc/u8x8_setup.c"
        "fonts/u8g2_font_wqy16_t_gb2312.c"
    INCLUDE_DIRS "csrc"
)

# 生成的 GB2312 大字库文件不主动包含 u8g2.h，需要显式开启大字体定义。
target_compile_definitions(${COMPONENT_LIB} PUBLIC U8G2_USE_LARGE_FONTS)
