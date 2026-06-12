#ifndef VIDEO_OUTPUT_H
#define VIDEO_OUTPUT_H

#include "pico_hdmi/hstx_packet.h"

#include <stdbool.h>
#include <stdint.h>

// ============================================================================
// Video Output Configuration
// ============================================================================

// Video mode selection (define VIDEO_MODE_320x240 or VIDEO_MODE_1280x720 before including this header)
#ifdef VIDEO_MODE_320x240

// 1280x240 @ 60Hz - True 240p (Quad Clock Rate)
// Pixel Clock: 25.2 MHz (15kHz scan rate)
// H_TOTAL = 1600 pixels, V_TOTAL = 262 lines
// Active: 1280 (320x4), Front: 32 (8x4), Sync: 192 (48x4), Back: 96 (24x4)
#define MODE_H_FRONT_PORCH 32
#define MODE_H_SYNC_WIDTH 192
#define MODE_H_BACK_PORCH 96
#define MODE_H_ACTIVE_PIXELS 1280

#define MODE_V_FRONT_PORCH 4
#define MODE_V_SYNC_WIDTH 4
#define MODE_V_BACK_PORCH 14
#define MODE_V_ACTIVE_LINES 240

#ifndef MODE_HSTX_CLK_DIV
// HSTX clock divider: clk_sys / 1 = 126 MHz -> 25.2 MHz pixel clock (with CSR_CLKDIV=5).
#define MODE_HSTX_CLK_DIV 1
#endif
#define MODE_HSTX_CSR_CLKDIV 5

#elif defined(VIDEO_MODE_1280x720)

// 1280x720 @ 60Hz (CEA VIC 4)
// Pixel clock: 74.25 MHz (using sys_clk=372 MHz gives 74.4 MHz, within HDMI tolerance)
// Sync polarity: both H and V are POSITIVE (unlike VIC 1's negative)
#define MODE_H_FRONT_PORCH 110
#define MODE_H_SYNC_WIDTH 40
#define MODE_H_BACK_PORCH 220
#define MODE_H_ACTIVE_PIXELS 1280

#define MODE_V_FRONT_PORCH 5
#define MODE_V_SYNC_WIDTH 5
#define MODE_V_BACK_PORCH 20
#define MODE_V_ACTIVE_LINES 720

#ifndef MODE_HSTX_CLK_DIV
// HSTX clock: sys_clk / 1 = 372 MHz -> 74.4 MHz pixel clock (with CSR_CLKDIV=5)
#define MODE_HSTX_CLK_DIV 1
#endif
#define MODE_HSTX_CSR_CLKDIV 5

// 720p60 uses positive sync polarity on both H and V
#define MODE_SYNC_POSITIVE 1

#else

// 640x480 @ 60Hz (VIC 1) - Default mode
// Pixel clock: 25.2 MHz
#define MODE_H_FRONT_PORCH 16
#define MODE_H_SYNC_WIDTH 96
#define MODE_H_BACK_PORCH 48
#define MODE_H_ACTIVE_PIXELS 640

#define MODE_V_FRONT_PORCH 10
#define MODE_V_SYNC_WIDTH 2
#define MODE_V_BACK_PORCH 33
#define MODE_V_ACTIVE_LINES 480

#ifndef MODE_HSTX_CLK_DIV
// HSTX clock divider: 1 (stock 126 MHz sys_clk -> 25.2 MHz pixel clock).
#define MODE_HSTX_CLK_DIV 1
#endif
#define MODE_HSTX_CSR_CLKDIV 5

#endif

#define MODE_H_TOTAL_PIXELS (MODE_H_FRONT_PORCH + MODE_H_SYNC_WIDTH + MODE_H_BACK_PORCH + MODE_H_ACTIVE_PIXELS)
#define MODE_V_TOTAL_LINES (MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH + MODE_V_BACK_PORCH + MODE_V_ACTIVE_LINES)

// Data Island placement: true if the DI fits inside the hsync pulse (480p, 240p);
// false if it must be placed in the back porch instead (720p60).
// Callers of hstx_encode_data_island pass this as the hsync_active parameter.
#if MODE_H_SYNC_WIDTH >= (W_PREAMBLE + W_DATA_ISLAND)
#define DI_IN_HSYNC 1
#define DI_HSYNC_ACTIVE true
#else
#define DI_IN_HSYNC 0
#define DI_HSYNC_ACTIVE false
#endif

// Frame dimensions (set via video_output_init)
extern uint16_t frame_width;
extern uint16_t frame_height;

// ============================================================================
// Global State
// ============================================================================

extern volatile uint32_t video_frame_count;

/**
 * True while the current vertical position is outside the active picture.
 * Background work that performs RAM-heavy preparation can use this to avoid
 * competing with active-video DMA fetches.
 */
bool video_output_in_vertical_blanking(void);

// ============================================================================
// Public Interface
// ============================================================================

typedef void (*video_output_task_fn)(void);
typedef void (*video_output_vsync_cb_t)(void);

/**
 * Scanline Callback:
 * Called by the video output system when it needs pixel data for a scanline.
 *
 * @param v_scanline The current vertical scanline (0 to MODE_V_TOTAL_LINES - 1)
 * @param active_line The current active video line (0 to MODE_V_ACTIVE_LINES - 1),
 *                    only valid if active_video is true.
 * @param line_buffer Buffer to fill with MODE_H_ACTIVE_PIXELS RGB565 pixels (packed as uint32_t pairs).
 *                    The buffer MUST be filled with (MODE_H_ACTIVE_PIXELS / 2) uint32_t words.
 *                    - 640x480 mode: 640 pixels = 320 uint32_t words
 *                    - 320x240 mode: 1280 pixels = 640 uint32_t words (4x pixel repetition)
 */
typedef void (*video_output_scanline_cb_t)(uint32_t v_scanline, uint32_t active_line, uint32_t *line_buffer);

/**
 * Scanline Pointer Callback:
 * Called by the video output system when it needs the address of already
 * prepared RGB565 pixel data for a scanline.
 *
 * @param v_scanline The current vertical scanline (0 to MODE_V_TOTAL_LINES - 1)
 * @param active_line The current active video line (0 to MODE_V_ACTIVE_LINES - 1)
 * @return Pointer to (MODE_H_ACTIVE_PIXELS / 2) uint32_t words, or NULL to use
 *         the internal line buffer. In native pixel mode the pointer refers to
 *         MODE_H_ACTIVE_PIXELS / 2 RGB565 halfwords.
 */
typedef const uint32_t *(*video_output_scanline_ptr_cb_t)(uint32_t v_scanline, uint32_t active_line);

/**
 * Initialize HSTX and DMA for video output.
 * @param width Framebuffer width in pixels (e.g., 320)
 * @param height Framebuffer height in pixels (e.g., 240)
 */
void video_output_init(uint16_t width, uint16_t height);

/**
 * Register the scanline callback.
 */
void video_output_set_scanline_callback(video_output_scanline_cb_t cb);

/**
 * Register a prepared scanline pointer callback.
 * When non-NULL, this takes precedence over the fill-buffer callback.
 */
void video_output_set_scanline_pointer_callback(video_output_scanline_ptr_cb_t cb);

/**
 * Register a VSYNC callback, called once per frame at the start of vertical sync.
 */
void video_output_set_vsync_callback(video_output_vsync_cb_t cb);

/**
 * Register a background task to run in the Core 1 loop.
 * This is typically used for audio processing.
 */
void video_output_set_background_task(video_output_task_fn task);

/**
 * Get DVI mode status.
 * @return true if DVI mode (no HDMI audio), false if HDMI mode
 */
bool video_output_get_dvi_mode(void);

/**
 * Set DVI mode.
 * When enabled, disables all HDMI Data Islands (no audio output).
 * Some monitors have trouble syncing with HDMI Data Islands.
 * Default: false (HDMI mode with audio).
 * @param enabled true for DVI mode, false for HDMI mode with audio
 */
void video_output_set_dvi_mode(bool enabled);

#include "pico_hdmi/video_output_precomposed.h"

/**
 * Native pixel mode (requires PICO_HDMI_PRECOMPOSED_ACTIVE_LINES): the
 * scanline pointer callback returns the native half-width line (e.g. 320
 * RGB565 pixels for 640-wide output); the active-data DMA runs 16-bit
 * transfers and the bus/HSTX expander double each pixel in hardware.
 */
void video_output_set_native_pixel_mode(bool enabled);

/**
 * Core 1 entry point for video output.
 * This function does not return.
 */
void video_output_core1_run(void);

/**
 * Reconfigure HDMI audio for a different sample rate.
 * Updates ACR, Audio InfoFrame, and packet timing.
 * Can be called after video_output_init() to override the default 48kHz.
 * @param sample_rate Audio sample rate in Hz (e.g. 32000, 44100, 48000)
 */
void pico_hdmi_set_audio_sample_rate(uint32_t sample_rate);

#endif // VIDEO_OUTPUT_H
