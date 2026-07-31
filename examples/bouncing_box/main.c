/**
 * pico_hdmi Bouncing Box Example
 *
 * Demonstrates basic usage of the pico_hdmi library:
 * - 640x480 @ 60Hz HDMI output
 * - Scanline callback for rendering
 * - HDMI audio (Für Elise melody)
 * - Simple animation
 *
 * Target: RP2350 (Raspberry Pi Pico 2)
 */

#include "pico_hdmi/hstx_data_island_queue.h"
#include "pico_hdmi/hstx_packet.h"
#include "pico_hdmi/video_output.h"

#include "pico/multicore.h"
#include "pico/stdlib.h"

#include "hardware/clocks.h"
#include "hardware/vreg.h"

#include <math.h>
#include <string.h>

#include "audio.h"
// ============================================================================
// Configuration
// ============================================================================

#ifdef VIDEO_MODE_1280x720
#define FRAME_WIDTH 1280
#define FRAME_HEIGHT 720
#define BOX_SIZE 64
#else
#define FRAME_WIDTH 640
#define FRAME_HEIGHT 480
#define BOX_SIZE 32
#endif
#define BG_COLOR 0x0010  // Dark blue (RGB565)
#define BOX_COLOR 0xFFE0 // Yellow (RGB565)

// Audio configuration
#ifndef BOUNCING_BOX_AUDIO_SAMPLE_RATE
#define BOUNCING_BOX_AUDIO_SAMPLE_RATE 48000
#endif
#define AUDIO_SAMPLE_RATE BOUNCING_BOX_AUDIO_SAMPLE_RATE
#define TONE_AMPLITUDE 6000

// ============================================================================
// Animation State
// ============================================================================

static volatile int box_x = 50, box_y = 50;
#ifdef VIDEO_MODE_1280x720
static int box_dx = 4, box_dy = 2;
#else
static int box_dx = 2, box_dy = 1;
#endif

// ============================================================================
// Audio State - Für Elise
// ============================================================================

#define SINE_TABLE_SIZE 256
static int16_t sine_table[SINE_TABLE_SIZE];
static uint32_t audio_phase = 0;
static uint32_t phase_increment = 0;
static int audio_frame_counter = 0;

// Use Korobeiniki for the demo (Für Elise kept for reference)

static int current_melody_length = KOROBEINIKI_LENGTH;

static int melody_index = 0;
static int note_frames_remaining = 0;
#ifdef VIDEO_MODE_50HZ
static uint32_t melody_tick_accum = 0;
#endif

static void init_sine_table(void)
{
    for (int i = 0; i < SINE_TABLE_SIZE; i++) {
        float angle = (float)i * 2.0F * 3.14159265F / SINE_TABLE_SIZE;
        sine_table[i] = (int16_t)(sinf(angle) * TONE_AMPLITUDE);
    }
}

static void advance_melody(void)
{
    if (--note_frames_remaining <= 0) {
        melody_index = (melody_index + 1) % current_melody_length;

        note_frames_remaining = current_melody[melody_index].duration;
        uint16_t freq = current_melody[melody_index].freq;
        if (freq > 0) {
            phase_increment = (uint32_t)(((uint64_t)freq << 32) / AUDIO_SAMPLE_RATE);
        } else {
            phase_increment = 0; // Rest
        }
    }
}

static void advance_melody_for_video_frame(void)
{
#ifdef VIDEO_MODE_50HZ
    // The melody durations are expressed in 60 Hz video-frame ticks. Advance
    // six melody ticks per five 50 Hz frames so the 50 Hz diagnostic plays
    // the same notes for the same wall-clock duration as the 60 Hz control.
    melody_tick_accum += 6;
    while (melody_tick_accum >= 5) {
        melody_tick_accum -= 5;
        advance_melody();
    }
#else
    advance_melody();
#endif
}

static inline int16_t get_sine_sample(void)
{
    if (phase_increment == 0)
        return 0; // Rest
    int16_t s = sine_table[(audio_phase >> 24) & 0xFF];
    audio_phase += phase_increment;
    return s;
}

static void generate_audio(void)
{
    // Keep the audio queue fed
    while (hstx_di_queue_get_level() < 200) {
        audio_sample_t samples[4];
        for (int i = 0; i < 4; i++) {
            int16_t s = get_sine_sample();
            samples[i].left = s;
            samples[i].right = s;
        }

        hstx_packet_t packet;
        audio_frame_counter =
            hstx_packet_set_audio_samples_cs_rate(&packet, samples, 4, audio_frame_counter, AUDIO_SAMPLE_RATE);

        hstx_data_island_t island;
        hstx_encode_data_island(&island, &packet, false, DI_HSYNC_ACTIVE);
        hstx_di_queue_push(&island);
    }
}

// ============================================================================
// Scanline Callback (runs on Core 1)
// ============================================================================

static void __scratch_x("") scanline_callback(uint32_t v_scanline, uint32_t active_line, uint32_t *dst)
{
    (void)v_scanline;

    int fb_line = active_line;

    // Read current box position
    int bx = box_x;
    int by = box_y;

    uint32_t bg = BG_COLOR | (BG_COLOR << 16);
    uint32_t box = BOX_COLOR | (BOX_COLOR << 16);

    // Check if this line intersects the box vertically
    if (fb_line >= by && fb_line < by + BOX_SIZE) {
        // Three regions: before box, box, after box
        int i = 0;

        // Region 1: before box
        // Note: iterating by 2 pixels at a time (1 uint32_t)
        for (; i < bx / 2; i++) {
            dst[i] = bg;
        }

        // Region 2: box
        for (; i < (bx + BOX_SIZE) / 2 && i < FRAME_WIDTH / 2; i++) {
            dst[i] = box;
        }

        // Region 3: after box
        for (; i < FRAME_WIDTH / 2; i++) {
            dst[i] = bg;
        }
    } else {
        // Fast path: entire line is background
        for (int i = 0; i < FRAME_WIDTH / 2; i++) {
            dst[i] = bg;
        }
    }
}

// ============================================================================
// Main (Core 0)
// ============================================================================

static void update_box(void)
{
    int x = box_x + box_dx;
    int y = box_y + box_dy;

    if (x <= 0 || x + BOX_SIZE >= FRAME_WIDTH) {
        box_dx = -box_dx;
        x = box_x + box_dx;
    }
    if (y <= 0 || y + BOX_SIZE >= FRAME_HEIGHT) {
        box_dy = -box_dy;
        y = box_y + box_dy;
    }

    box_x = x;
    box_y = y;
}

int main(void)
{
#ifdef VIDEO_MODE_720P_RB
    // Exact-clock 720p60 CVT reduced blanking: 320 MHz / 5 = 64 MHz
    // pixel clock. This removes the standard mode's 0.2% clock offset.
    vreg_set_voltage(VREG_VOLTAGE_1_20);
    sleep_ms(10);
    set_sys_clock_khz(320000, true);
#elif defined(VIDEO_MODE_1280x720)
    // 720p50/60: 372 MHz at 1.3V. Closest achievable to 371.25 MHz with
    // 12 MHz XOSC (0.2% high -> 74.4 MHz pixel clock).
    vreg_set_voltage(VREG_VOLTAGE_1_30);
    sleep_ms(10);
    set_sys_clock_khz(372000, true);
#else
    // 480p60: set system clock to 126 MHz for HSTX timing (25.2 MHz pixel clock).
    set_sys_clock_khz(126000, true);
#endif

    stdio_init_all();

    // Initialize LED for heartbeat
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);

    sleep_ms(1000);

#ifndef BOUNCING_BOX_DVI_ONLY
    // Initialize audio
    init_sine_table();
    note_frames_remaining = current_melody[0].duration;
    phase_increment = (uint32_t)(((uint64_t)current_melody[0].freq << 32) / AUDIO_SAMPLE_RATE);
#endif

    // Initialize HDMI output
    hstx_di_queue_init();
    video_output_init(FRAME_WIDTH, FRAME_HEIGHT);
#ifndef BOUNCING_BOX_DVI_ONLY
    // Keep ACR, Audio InfoFrame, packet cadence, and IEC 60958 channel status
    // consistent with the generated sample stream.
    pico_hdmi_set_audio_sample_rate(AUDIO_SAMPLE_RATE);
#endif
#ifdef BOUNCING_BOX_DVI_ONLY
    video_output_set_dvi_mode(true);
#endif

    // Register scanline callback
    video_output_set_scanline_callback(scanline_callback);

#ifndef BOUNCING_BOX_DVI_ONLY
    // Pre-fill audio buffer
    generate_audio();
#endif

    // Launch Core 1 for HSTX output
    multicore_launch_core1(video_output_core1_run);
    sleep_ms(100);

    // Main loop - animation + audio
    uint32_t last_frame = 0;
    bool led_state = false;

    while (1) {
#ifndef BOUNCING_BOX_DVI_ONLY
        // Keep audio buffer fed
        generate_audio();
#endif

        while (video_frame_count == last_frame) {
#ifndef BOUNCING_BOX_DVI_ONLY
            generate_audio();
#endif
            tight_loop_contents();
        }
        last_frame = video_frame_count;

        // Update animation
        update_box();

#ifndef BOUNCING_BOX_DVI_ONLY
        // Keep melody timing constant between the 50 Hz diagnostic and the
        // normal 60 Hz control.
        advance_melody_for_video_frame();
#endif

        // LED heartbeat
        if ((video_frame_count % 30) == 0) {
            led_state = !led_state;
            gpio_put(PICO_DEFAULT_LED_PIN, led_state);
        }
    }

    return 0;
}
