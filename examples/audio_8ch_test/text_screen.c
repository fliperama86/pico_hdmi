#include "text_screen.h"

#include "pico.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "font8x8_basic.h"

#define LINE_WORDS (TEXT_COLS * 8 / 2) // 640 RGB565 pixels, two per word

// Text and attributes. Plain RAM, written by Core 0, read by the interrupt.
static char text[TEXT_ROWS][TEXT_COLS];
static uint8_t row_palette[TEXT_ROWS];

// For each palette and each 4-pixel slice of a glyph row (bit 0 = leftmost),
// the two words of RGB565 pixels it becomes. The left pixel of each pair is
// the low halfword.
static uint32_t nibble_lut[TEXT_PAL_COUNT][16][2];

// The interrupt renders line N+1 while DMA still sends line N, so lines
// alternate between two buffers.
static uint32_t line_bufs[2][LINE_WORDS];

static const uint16_t palette_colors[TEXT_PAL_COUNT][2] = {
    // {foreground, background}
    [TEXT_PAL_NORMAL] = {0xC618, 0x0000},
    [TEXT_PAL_TITLE] = {0xFFFF, 0x0010},
    [TEXT_PAL_HEADING] = {0xFFE0, 0x0000},
    [TEXT_PAL_HIGHLIGHT] = {0x0000, 0x07E0},
    [TEXT_PAL_DIM] = {0x7BEF, 0x0000},
};

void text_screen_init(void)
{
    for (int p = 0; p < TEXT_PAL_COUNT; p++) {
        const uint32_t fg = palette_colors[p][0];
        const uint32_t bg = palette_colors[p][1];
        for (int n = 0; n < 16; n++) {
            uint32_t px[4];
            for (int i = 0; i < 4; i++) {
                px[i] = (n >> i) & 1 ? fg : bg;
            }
            nibble_lut[p][n][0] = px[0] | (px[1] << 16);
            nibble_lut[p][n][1] = px[2] | (px[3] << 16);
        }
    }
    memset(text, ' ', sizeof(text));
    memset(row_palette, TEXT_PAL_NORMAL, sizeof(row_palette));
}

void text_screen_printf(int row, text_palette_t palette, const char *fmt, ...)
{
    if (row < 0 || row >= TEXT_ROWS) {
        return;
    }
    char buf[TEXT_COLS + 1];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (n < 0) {
        n = 0;
    } else if (n > TEXT_COLS) {
        n = TEXT_COLS;
    }
    memcpy(text[row], buf, (size_t)n);
    memset(&text[row][n], ' ', (size_t)(TEXT_COLS - n));
    row_palette[row] = (uint8_t)palette;
}

void text_screen_dump(void)
{
    for (int r = 0; r < TEXT_ROWS; r++) {
        printf("screen %02d %d |%.*s|\n", r, row_palette[r], TEXT_COLS, text[r]);
    }
}

const uint32_t *__scratch_x("") text_screen_scanline(uint32_t v_scanline, uint32_t active_line)
{
    (void)v_scanline;
    uint32_t *dst = line_bufs[active_line & 1];
    uint32_t *const line = dst;

    const uint32_t row = active_line >> 4;      // 16 scanlines per text row
    const uint32_t glyph_y = (active_line >> 1) & 7; // each glyph row shown twice
    const char *chars = text[row];
    const uint32_t(*lut)[2] = nibble_lut[row_palette[row]];

    for (int c = 0; c < TEXT_COLS; c++) {
        const uint8_t bits = font8x8_basic[chars[c] & 0x7F][glyph_y];
        const uint32_t *lo = lut[bits & 0xF];
        const uint32_t *hi = lut[bits >> 4];
        dst[0] = lo[0];
        dst[1] = lo[1];
        dst[2] = hi[0];
        dst[3] = hi[1];
        dst += 4;
    }
    return line;
}
