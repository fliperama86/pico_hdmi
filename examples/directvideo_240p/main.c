/**
 * pico_hdmi True 240p DirectVideo Example
 *
 * Demonstrates true 240p output for retro gaming scalers:
 * - Native 320x240@60Hz timing with ~6.3 MHz pixel clock
 * - Compatible with Morph4K, RetroTINK 4K, and other scalers
 * - Scalers detect this as true 240p for optimal processing
 *
 * This uses a lower pixel clock than standard 640x480, achieved by
 * dividing the HSTX clock by 4 (126 MHz / 4 = 31.5 MHz HSTX clock).
 *
 * Target: RP2350 (Raspberry Pi Pico 2)
 */

#include "pico_hdmi/video_output.h"

#include "pico/multicore.h"
#include "pico/stdlib.h"

#include "hardware/clocks.h"

#include <stdio.h>
#include <string.h>

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
    // Set system clock to 126 MHz
    set_sys_clock_khz(126000, true);

    stdio_init_all();

    // Initialize LED for heartbeat - blink immediately to show we're alive
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    gpio_put(PICO_DEFAULT_LED_PIN, 1); // LED ON immediately

    sleep_ms(500);
    gpio_put(PICO_DEFAULT_LED_PIN, 0); // LED OFF
    sleep_ms(500);
    gpio_put(PICO_DEFAULT_LED_PIN, 1); // LED ON

    printf("=== BOOT ===\n");
    sleep_ms(100);
    printf("True 240p DirectVideo Example\n");
    printf("Resolution: %dx%d @ 60Hz\n", FRAME_WIDTH, FRAME_HEIGHT);
    printf("System clock: %lu Hz\n", clock_get_hz(clk_sys));
    printf("HSTX clock (before init): %lu Hz\n", clock_get_hz(clk_hstx));

    printf("Calling video_output_init...\n");
    // Initialize video output (configures clk_hstx divider)
    video_output_init(FRAME_WIDTH, FRAME_HEIGHT);
    printf("video_output_init done\n");

    printf("HSTX clock (after init): %lu Hz\n", clock_get_hz(clk_hstx));

    // Use HDMI mode (DVI=false) to include Guard Bands and InfoFrames
    // This improves compatibility with Morph4K and other scalers
    video_output_set_dvi_mode(false);
    printf("HDMI mode set (DVI disabled)\n");

    // Register scanline callback
    video_output_set_scanline_callback(scanline_callback);
    printf("Callback set, launching Core 1...\n");

    // Launch Core 1 for HSTX output
    multicore_launch_core1(video_output_core1_run);
    printf("Core 1 launched\n");
    sleep_ms(100);

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
