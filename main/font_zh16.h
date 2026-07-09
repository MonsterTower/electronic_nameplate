#ifndef FONT_ZH16_H
#define FONT_ZH16_H

#include <stdbool.h>
#include <stdint.h>

#define FONT_ZH16_WIDTH 16
#define FONT_ZH16_HEIGHT 16

bool font_zh16_get_glyph(uint32_t codepoint, const uint16_t **rows);
const uint16_t *font_zh16_get_missing_glyph(void);

#endif
