/**
 * 80x30 text screen for 640x480 HDMI output.
 *
 * Characters are 8x8 glyphs shown at double height (8x16 cells). Each row has
 * its own colour palette. The video interrupt renders each scanline from the
 * text buffer as it is needed (text_screen_scanline), so there is no
 * framebuffer: the whole screen costs 2.4 KB of text plus two line buffers.
 *
 * Core 0 writes rows with text_screen_printf at any time; a row caught
 * mid-update shows the mix for one frame at most.
 */

#ifndef TEXT_SCREEN_H
#define TEXT_SCREEN_H

#include <stdint.h>

#define TEXT_COLS 80
#define TEXT_ROWS 30

typedef enum {
    TEXT_PAL_NORMAL = 0, // light grey on black
    TEXT_PAL_TITLE,      // white on blue
    TEXT_PAL_HEADING,    // yellow on black
    TEXT_PAL_HIGHLIGHT,  // black on green
    TEXT_PAL_DIM,        // dark grey on black
    TEXT_PAL_COUNT
} text_palette_t;

// Build the palette lookup tables and clear the screen.
void text_screen_init(void);

// Replace a whole row: printf-style text, padded with spaces (or cut) to 80
// columns, drawn in the given palette.
void text_screen_printf(int row, text_palette_t palette, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

// Print the screen's text to stdout, one row per line (diagnostics).
void text_screen_dump(void);

// Scanline pointer callback for video_output_set_scanline_pointer_callback.
// Runs in the video interrupt on Core 1.
const uint32_t *text_screen_scanline(uint32_t v_scanline, uint32_t active_line);

#endif // TEXT_SCREEN_H
