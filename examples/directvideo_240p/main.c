/**
 * pico_hdmi True 240p DirectVideo Example
 *
 * Demonstrates true 240p output for retro gaming scalers:
 * - 1280x240 @ 60Hz with 4x pixel repetition (representing 320x240)
 * - Standard 25.2 MHz pixel clock (HDMI-compliant)
 * - AVI InfoFrame PR=3 tells scalers to treat as 320x240 @ 15kHz
 * - Compatible with Morph4K, RetroTINK 4K, and other scalers
 *
 * The trick: We send 1280 pixels at 25.2 MHz (standard VGA rate) but
 * set the HDMI Pixel Repetition field to 4x, so the scaler knows each
 * group of 4 pixels is actually 1 logical pixel.
 *
 * Target: RP2350 (Raspberry Pi Pico 2)
 */

#include "pico_hdmi/video_output.h"

#include "pico/multicore.h"
#include "pico/stdlib.h"

#include "hardware/clocks.h"

// ============================================================================
// Configuration
// ============================================================================

// True 240p: 1280x240 (Pixel Quadrupled from 320x240)
// This uses a 25.2 MHz pixel clock (HDMI standard minimum)
#define FRAME_WIDTH 1280
#define FRAME_HEIGHT 240

// Color bars (RGB565)
#define WHITE 0xFFFF
#define YELLOW 0xFFE0
#define CYAN 0x07FF
#define GREEN 0x07E0
#define MAGENTA 0xF81F
#define RED 0xF800
#define BLUE 0x001F
#define BLACK 0x0000

static const uint16_t color_bars[8] = {WHITE, YELLOW, CYAN, GREEN, MAGENTA, RED, BLUE, BLACK};

// ============================================================================
// Scanline Callback (runs on Core 1)
// ============================================================================

void __scratch_x("") scanline_callback(uint32_t v_scanline, uint32_t active_line, uint32_t *dst)
{
    (void)v_scanline;
    (void)active_line;

    // Draw 8 color bars, each 160 pixels wide (1280 / 8 = 160)
    for (int bar = 0; bar < 8; bar++) {
        uint16_t color = color_bars[bar];
        // Pack two pixels into each uint32_t
        uint32_t packed = color | (color << 16);

        // 160 pixels per bar -> 80 uint32_t words
        int start = bar * 160 / 2;
        int end = (bar + 1) * 160 / 2;
        for (int i = start; i < end; i++) {
            dst[i] = packed;
        }
    }
}

// ============================================================================
// Main (Core 0)
// ============================================================================

int main(void)
{
    // Set system clock to 126 MHz (gives 25.2 MHz pixel clock)
    set_sys_clock_khz(126000, true);

    // Initialize LED for heartbeat
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);

    // Initialize video output
    video_output_init(FRAME_WIDTH, FRAME_HEIGHT);

    // Use HDMI mode to send AVI InfoFrame with pixel repetition
    video_output_set_dvi_mode(false);

    // Register scanline callback
    video_output_set_scanline_callback(scanline_callback);

    // Launch Core 1 for HSTX output
    multicore_launch_core1(video_output_core1_run);

    // Main loop
    uint32_t last_frame = 0;
    bool led_state = false;

    while (1) {
        while (video_frame_count == last_frame) {
            tight_loop_contents();
        }
        last_frame = video_frame_count;

        // LED heartbeat (toggle every 30 frames = ~0.5s)
        if ((video_frame_count % 30) == 0) {
            led_state = !led_state;
            gpio_put(PICO_DEFAULT_LED_PIN, led_state);
        }
    }

    return 0;
}
