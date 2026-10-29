#ifndef RIOOS_CONSOLE_FONT_H
#define RIOOS_CONSOLE_FONT_H

#include "types.h"

/* Fixed-pitch terminal font: DejaVu Sans Mono rendered at 9x18 pixels. */
#define CONSOLE_FONT_W 9
#define CONSOLE_FONT_H 18
#define CONSOLE_FONT_FIRST 32
#define CONSOLE_FONT_GLYPHS 95

extern const uint8_t console_font_glyphs[CONSOLE_FONT_GLYPHS][CONSOLE_FONT_W * CONSOLE_FONT_H];

#endif
