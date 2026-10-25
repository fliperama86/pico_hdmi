/**
 * pico_hdmi 8-Channel Audio Test
 *
 * Plays a test tone on each of the eight HDMI audio channels in turn:
 * 1.0 s of tone, 0.5 s of silence, then the next channel, forever.
 * - 640x480 @ 60Hz HDMI output
 * - 8-channel 48 kHz 24-bit L-PCM (PICO_HDMI_AUDIO_8CH), 7.1 allocation
 * - The screen is split into eight columns; the playing channel's column
 *   is lit (prebuilt lines handed to the video interrupt by pointer)
 * - The LED blinks once per second while video frames are being produced
 *   (driven by the frame counter the video interrupt advances)
 * - USB serial prints a status line every second: channel, video frame
 *   count, queue level, underruns and Core 0 audio load
 * - Build with -DAUDIO_8CH_TEST_VIDEO_ONLY=ON to send plain DVI video with no
 *   Data Islands, to tell a video fault from a sink rejecting the audio
 *
 * Target: RP2350 (Raspberry Pi Pico 2)
 */

#include "pico_hdmi/hstx_data_island_queue.h"
#include "pico_hdmi/hstx_packet.h"
#include "pico_hdmi/video_output.h"

#include "pico/multicore.h"
#include "pico/stdio_usb.h"
#include "pico/stdlib.h"

#include "hardware/clocks.h"

#include <math.h>
#include <stdio.h>

#if !PICO_HDMI_AUDIO_8CH
#error "audio_8ch_test needs the library built with PICO_HDMI_AUDIO_8CH=ON"
#endif

#ifndef AUDIO_8CH_TEST_VIDEO_ONLY
#define AUDIO_8CH_TEST_VIDEO_ONLY 0
#endif

// Diagnostic: print a marker at each startup step (waits up to 10 s for the
// USB serial port to be opened first so nothing is lost).
#ifndef AUDIO_8CH_TEST_TRACE
#define AUDIO_8CH_TEST_TRACE 0
#endif
#if AUDIO_8CH_TEST_TRACE
#define TRACE(...) printf("trace: " __VA_ARGS__)
#else
#define TRACE(...) ((void)0)
#endif

// ============================================================================
// Configuration
// ============================================================================

#define FRAME_WIDTH 640
#define FRAME_HEIGHT 480

#define AUDIO_SAMPLE_RATE 48000
#define NUM_CHANNELS HSTX_AUDIO_8CH_CHANNELS

#define TONE_HZ 440
// The LFE slot is low-passed by receivers (typically at 120 Hz or below), so
// it gets a low tone or it would be inaudible.
#define LFE_TONE_HZ 60
#define LFE_CHANNEL 2

#define TONE_SAMPLES (AUDIO_SAMPLE_RATE * 1)        // 1.0 s on
#define GAP_SAMPLES (AUDIO_SAMPLE_RATE / 2)         // 0.5 s off
#define STEP_SAMPLES (TONE_SAMPLES + GAP_SAMPLES)   // per channel
#define RAMP_SAMPLES (AUDIO_SAMPLE_RATE / 100)      // 10 ms fade in/out
#define TONE_AMPLITUDE (1 << 21)                    // -12 dBFS of 24-bit full scale

// Keep this many islands queued (the ring holds 255): ~10 ms of audio.
#define QUEUE_TARGET_LEVEL 240

// RGB565 column colours
#define COLOR_IDLE 0x2104   // Dark grey
#define COLOR_ACTIVE 0x07E0 // Green
#define COLOR_GAP 0x0000    // Black divider between columns

static const char *const channel_names[NUM_CHANNELS] = {
    "FL (front left)", "FR (front right)", "LFE (subwoofer)",     "FC (front centre)",
    "RL (rear left)",  "RR (rear right)",  "RLC (rear left ctr)", "RRC (rear right ctr)",
};

// ============================================================================
// Audio State
// ============================================================================

#define SINE_TABLE_BITS 10
#define SINE_TABLE_SIZE (1 << SINE_TABLE_BITS)
static int32_t sine_table[SINE_TABLE_SIZE];

static uint32_t audio_phase = 0;
static uint32_t step_position = 0; // Sample index within the current channel's step
static int tone_channel = 0;       // Channel the generator is producing
static int audio_frame_counter = 0;

// Channel whose tone is playing, or -1 during the gap. Written by Core 0, read
// by the scanline callback on Core 1. The generator runs ~10 ms ahead of the
// HDMI output, which is too little to see.
static volatile int display_channel = 0;

static void init_sine_table(void)
{
    for (int i = 0; i < SINE_TABLE_SIZE; i++) {
        float angle = (float)i * 2.0F * 3.14159265F / SINE_TABLE_SIZE;
        sine_table[i] = (int32_t)(sinf(angle) * TONE_AMPLITUDE);
    }
}

static inline uint32_t phase_increment_for(int channel)
{
    uint32_t hz = (channel == LFE_CHANNEL) ? LFE_TONE_HZ : TONE_HZ;
    return (uint32_t)(((uint64_t)hz << 32) / AUDIO_SAMPLE_RATE);
}

// Produce one 8-channel frame: the tone on tone_channel during the first
// TONE_SAMPLES of each step, silence on every other channel.
static void next_frame(audio_sample_8ch_t *frame)
{
    for (int c = 0; c < NUM_CHANNELS; c++) {
        frame->ch[c] = 0;
    }

    if (step_position < TONE_SAMPLES) {
        int32_t s = sine_table[audio_phase >> (32 - SINE_TABLE_BITS)];
        audio_phase += phase_increment_for(tone_channel);

        // Linear fade at both ends of the tone so it starts and stops
        // without a click.
        uint32_t from_end = TONE_SAMPLES - 1 - step_position;
        uint32_t ramp = step_position < from_end ? step_position : from_end;
        if (ramp < RAMP_SAMPLES) {
            s = (int32_t)(((int64_t)s * (int32_t)ramp) / RAMP_SAMPLES);
        }
        frame->ch[tone_channel] = s;
    }

    if (++step_position >= STEP_SAMPLES) {
        step_position = 0;
        audio_phase = 0;
        tone_channel = (tone_channel + 1) % NUM_CHANNELS;
    }
}

// Top the Data Island queue up. Each island carries two 8-channel frames.
// Returns the number of islands pushed.
static int generate_audio(void)
{
    int pushed_now = 0;
#if AUDIO_8CH_TEST_TRACE
    // Bounded so the trace heartbeat keeps running even if the queue never fills.
    for (int n = 0; n < 16 && hstx_di_queue_get_level() < QUEUE_TARGET_LEVEL; n++) {
#else
    while (hstx_di_queue_get_level() < QUEUE_TARGET_LEVEL) {
#endif
        audio_sample_8ch_t frame;
        hstx_packet_t first;
        hstx_packet_t second;

        next_frame(&frame);
        audio_frame_counter = hstx_packet_set_audio_sample_8ch(&first, &frame, audio_frame_counter, AUDIO_SAMPLE_RATE);
        next_frame(&frame);
        audio_frame_counter =
            hstx_packet_set_audio_sample_8ch(&second, &frame, audio_frame_counter, AUDIO_SAMPLE_RATE);

        hstx_data_island2_t island;
        hstx_encode_data_island2(&island, &first, &second, false, DI_HSYNC_ACTIVE);
        hstx_di_queue_push_8ch(&island);
        pushed_now++;
#if AUDIO_8CH_TEST_TRACE
        static uint32_t pushed;
        if (++pushed == 241) {
            TRACE("pushed %lu, queue %lu\n", (unsigned long)pushed, (unsigned long)hstx_di_queue_get_level());
        }
#endif

        display_channel = (step_position < TONE_SAMPLES) ? tone_channel : -1;
    }
    return pushed_now;
}

// ============================================================================
// Scanline Pointer Callback (runs on Core 1)
// ============================================================================

#define COLUMN_WORDS (FRAME_WIDTH / NUM_CHANNELS / 2) // 40 words = 80 pixels
#define LINE_WORDS (FRAME_WIDTH / 2)

// Every active line is identical, and there are only nine screens: no channel
// lit (the gap), or one of eight. All nine lines are drawn once at startup and
// the video interrupt just picks one. Filling a line inside the interrupt
// costs ~1300 cycles, which together with the 8-channel Data Island copy
// overran the interrupt's deadline at 126 MHz and broke the HSTX stream.
static uint32_t channel_lines[NUM_CHANNELS + 1][LINE_WORDS];

static void init_channel_lines(void)
{
    const uint32_t idle = COLOR_IDLE | (COLOR_IDLE << 16);
    const uint32_t lit = COLOR_ACTIVE | (COLOR_ACTIVE << 16);
    const uint32_t gap = COLOR_GAP | (COLOR_GAP << 16);

    // channel_lines[0] has no column lit; channel_lines[n + 1] lights channel n.
    for (int active = -1; active < NUM_CHANNELS; active++) {
        uint32_t *dst = channel_lines[active + 1];
        for (int c = 0; c < NUM_CHANNELS; c++) {
            const uint32_t color = (c == active) ? lit : idle;
            for (int i = 0; i < COLUMN_WORDS - 1; i++) {
                *dst++ = color;
            }
            *dst++ = gap; // 2-pixel divider
        }
    }
}

static const uint32_t *__scratch_x("") scanline_pointer(uint32_t v_scanline, uint32_t active_line)
{
    (void)v_scanline;
    (void)active_line;
    return channel_lines[display_channel + 1];
}

// ============================================================================
// Main (Core 0)
// ============================================================================

int main(void)
{
    // 480p60: set system clock to 126 MHz for HSTX timing (25.2 MHz pixel clock).
    set_sys_clock_khz(126000, true);

    stdio_init_all();

    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);

    sleep_ms(1000);
#if AUDIO_8CH_TEST_TRACE
    for (int i = 0; i < 100 && !stdio_usb_connected(); i++) {
        sleep_ms(100);
    }
#endif
    TRACE("start\n");

    init_sine_table();
    init_channel_lines();
    TRACE("sine table done\n");

    // Initialize HDMI output
    hstx_di_queue_init();
    TRACE("queue init done\n");
    video_output_init(FRAME_WIDTH, FRAME_HEIGHT);
    TRACE("video_output_init done\n");
    pico_hdmi_set_audio_sample_rate(AUDIO_SAMPLE_RATE);
    video_output_set_scanline_pointer_callback(scanline_pointer);
    TRACE("audio rate set\n");

#if AUDIO_8CH_TEST_VIDEO_ONLY
    video_output_set_dvi_mode(true);
#else
    // Pre-fill audio buffer
    uint64_t prefill_start = time_us_64();
    while (hstx_di_queue_get_level() < QUEUE_TARGET_LEVEL) {
        generate_audio();
    }
    TRACE("prefill done, queue %lu, %lu us\n", (unsigned long)hstx_di_queue_get_level(),
          (unsigned long)(time_us_64() - prefill_start));
#endif

    // Launch Core 1 for HSTX output
    multicore_launch_core1(video_output_core1_run);
    TRACE("core 1 launched\n");

    // Main loop: keep the queue fed. Time spent generating is measured so the
    // status line can report how much of Core 0 the audio path uses.
    uint64_t window_start = time_us_64();
    uint64_t busy_us = 0;
    uint32_t last_led_frame = 0;

    while (1) {
#if !AUDIO_8CH_TEST_VIDEO_ONLY
        // Only calls that produced audio count towards the load: a call that
        // finds the queue full returns in well under the timer's 1 us tick.
        uint64_t t0 = time_us_64();
        if (generate_audio() > 0) {
            busy_us += time_us_64() - t0;
        }
#endif

        // Heartbeat from the video interrupt's frame counter: the LED only
        // blinks while Core 1 is producing frames.
        uint32_t frames = video_frame_count;
        if (frames - last_led_frame >= 30) {
            last_led_frame = frames;
            gpio_xor_mask(1U << PICO_DEFAULT_LED_PIN);
        }

        uint64_t now = time_us_64();
#if AUDIO_8CH_TEST_TRACE
        static uint64_t last_trace;
        static uint32_t loops, traces;
        loops++;
        if (traces < 60 && now - last_trace >= 100000) {
            last_trace = now;
            traces++;
            TRACE("loop %lu frames %lu queue %lu underruns %lu\n", (unsigned long)loops, (unsigned long)frames,
                  (unsigned long)hstx_di_queue_get_level(), (unsigned long)hstx_di_queue_silence_count);
        }
#endif
        if (now - window_start >= 1000000) {
            uint32_t load_pct = (uint32_t)((busy_us * 100) / (now - window_start));
            int ch = display_channel;
            printf("frames %lu | queue %lu | underruns %lu | core0 audio load %lu%% | %s%s\n", (unsigned long)frames,
                   (unsigned long)hstx_di_queue_get_level(), (unsigned long)hstx_di_queue_silence_count,
                   (unsigned long)load_pct, ch >= 0 ? "playing " : "gap", ch >= 0 ? channel_names[ch] : "");
            window_start = now;
            busy_us = 0;
        }
    }

    return 0;
}
