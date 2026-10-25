/**
 * pico_hdmi 8-Channel Audio Test
 *
 * Plays a test tone on each of the eight HDMI audio channels in turn:
 * 1.0 s of tone, 0.5 s of silence, then the next channel, forever.
 * - 640x480 @ 60Hz HDMI output (25.2 MHz pixel clock)
 * - 252 MHz system clock at 1.15 V, HSTX clock divided by 2: twice the CPU
 *   headroom of the stock 126 MHz setup with the same standard video timing
 * - The frame rate is measured against the crystal over 10 s and shown in
 *   ppm, so any pull on the system PLL is visible
 * - 8-channel 48 kHz 24-bit L-PCM (PICO_HDMI_AUDIO_8CH), 7.1 allocation
 * - An 80x30 text status screen: stream and video format, a table of the
 *   eight slots with the playing one highlighted, the current tone and its
 *   progress, and live statistics (samples sent, queue, underruns, frame
 *   rate, CPU load). Rendered line by line in the video interrupt.
 * - The LED blinks once per second while video frames are being produced
 *   (driven by the frame counter the video interrupt advances)
 * - USB serial prints a status line every second: channel, video frame
 *   count, queue level, underruns and Core 0 audio load
 * - Build with -DAUDIO_8CH_TEST_VIDEO_ONLY=ON to send plain DVI video with no
 *   Data Islands, to tell a video fault from a sink rejecting the audio
 *
 * Target: RP2350 (Raspberry Pi Pico 2)
 */

#include "pico_hdmi/hdmi_audio_8ch.h"
#include "pico_hdmi/hstx_data_island_queue.h"
#include "pico_hdmi/hstx_packet.h"
#include "pico_hdmi/video_output.h"

#include "pico/multicore.h"
#include "pico/stdio_usb.h"
#include "pico/stdlib.h"

#include "hardware/clocks.h"
#include "hardware/structs/qmi.h"
#include "hardware/structs/timer.h"
#include "hardware/vreg.h"

#include <math.h>
#include <stdio.h>

#include "text_screen.h"

#if !PICO_HDMI_AUDIO_8CH
#error "audio_8ch_test needs the library built with PICO_HDMI_AUDIO_8CH=ON"
#endif

// 252 MHz / HSTX div 2 / CSR div 5 = 25.2 MHz pixel clock. The CMakeLists sets
// PICO_HDMI_HSTX_CLK_DIV=2 for the library to match.
#define SYS_CLOCK_KHZ 252000
#if MODE_HSTX_CLK_DIV != 2
#error "audio_8ch_test runs at 252 MHz and needs the library built with PICO_HDMI_HSTX_CLK_DIV=2"
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
#define QUEUE_TARGET_FRAMES 480
// Frames generated per hdmi_audio_8ch_write() call.
#define GEN_BLOCK_FRAMES 32

// How often Core 0 refreshes the status screen.
#define SCREEN_UPDATE_US 100000

// CTA-861 speaker codes and names for channel allocation 0x13 (7.1).
static const char *const channel_codes[NUM_CHANNELS] = {"FL", "FR", "LFE", "FC", "RL", "RR", "RLC", "RRC"};
static const char *const channel_names[NUM_CHANNELS] = {
    "Front left", "Front right", "Low frequency (sub)", "Front centre",
    "Rear left",  "Rear right",  "Rear left centre",    "Rear right centre",
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
static uint64_t samples_generated = 0;            // 8-channel frames generated so far
static uint32_t times_played[NUM_CHANNELS] = {0}; // Tones started per channel

// Channel whose tone is playing, or -1 during the gap, for the serial status
// line. The generator runs ~10 ms ahead of the HDMI output.
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

    if (step_position == 0) {
        times_played[tone_channel]++;
    }
    samples_generated++;

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

// Keep QUEUE_TARGET_FRAMES frames queued ahead of the HDMI output, generated
// in blocks and handed over through the library's PCM interface. Returns the
// number of frames written.
static uint32_t generate_audio(void)
{
    uint32_t written = 0;
    for (;;) {
        const uint32_t queued = hdmi_audio_8ch_queued_frames();
        if (queued >= QUEUE_TARGET_FRAMES) {
            break;
        }
        // Never generate more than the queue can take, so no frame is lost.
        uint32_t n = QUEUE_TARGET_FRAMES - queued;
        const uint32_t room = hdmi_audio_8ch_free_frames();
        if (n > room) {
            n = room;
        }
        if (n > GEN_BLOCK_FRAMES) {
            n = GEN_BLOCK_FRAMES;
        }
        if (n == 0) {
            break;
        }
        audio_sample_8ch_t block[GEN_BLOCK_FRAMES];
        for (uint32_t i = 0; i < n; i++) {
            next_frame(&block[i]);
        }
        written += hdmi_audio_8ch_write(block, n);
        display_channel = (step_position < TONE_SAMPLES) ? tone_channel : -1;
#if AUDIO_8CH_TEST_TRACE
        // One block per call so the trace heartbeat keeps running even if
        // the queue never fills.
        break;
#endif
    }
    return written;
}

// ============================================================================
// Status Screen (written by Core 0, rendered by the video interrupt)
// ============================================================================

// Live figures the main loop measures for the screen.
typedef struct {
    uint32_t frames;
    uint32_t fps;
    uint32_t core0_load_pct;
    uint32_t render_ns_per_line; // measured once at startup
    bool rate_valid;
    int32_t frame_rate_mhz;  // measured frame rate, millihertz
    int32_t frame_ppm_x10;   // measured vs nominal frame rate, 0.1 ppm units
    const char *hook_status; // from audio_8ch_test_hook, or NULL
} screen_stats_t;

// ============================================================================
// Optional test hook
// ============================================================================

// Called on every main-loop pass. A test build can link a strong definition
// (for example the soft VCXO pull test in clock_tests/) and return one line of
// status for the screen and the serial log, or NULL for none.
__attribute__((weak)) const char *audio_8ch_test_hook(uint64_t now_us)
{
    (void)now_us;
    return NULL;
}

// ============================================================================
// Frame-rate measurement
// ============================================================================

// The vsync callback stamps each frame with the crystal-driven microsecond
// timer. Comparing the frame count against those stamps over 10 s measures
// the real pixel clock to about 0.1 ppm, including any pull on the PLL.
static volatile uint32_t vsync_frame;
static volatile uint32_t vsync_time_us;

static void __scratch_x("") on_vsync(void)
{
    vsync_time_us = timer_hw->timerawl;
    vsync_frame = video_frame_count;
}

#define RATE_WINDOW_S 10
static uint32_t rate_hist_frame[RATE_WINDOW_S + 1];
static uint32_t rate_hist_us[RATE_WINDOW_S + 1];
static uint32_t rate_hist_count;

// Call once a second. Returns true once a full window is available.
static bool measure_frame_rate(screen_stats_t *st)
{
    uint32_t f, t;
    do {
        f = vsync_frame;
        t = vsync_time_us;
    } while (f != vsync_frame);

    const uint32_t slot = rate_hist_count % (RATE_WINDOW_S + 1);
    rate_hist_frame[slot] = f;
    rate_hist_us[slot] = t;
    rate_hist_count++;
    if (rate_hist_count <= RATE_WINDOW_S) {
        return false;
    }
    const uint32_t old = rate_hist_count % (RATE_WINDOW_S + 1);
    const double frames = (double)(f - rate_hist_frame[old]);
    const double seconds = (double)(t - rate_hist_us[old]) * 1e-6;
    const double measured = frames / seconds;
    const double nominal =
        (double)(clock_get_hz(clk_hstx) / MODE_HSTX_CSR_CLKDIV) / (MODE_H_TOTAL_PIXELS * MODE_V_TOTAL_LINES);
    st->frame_rate_mhz = (int32_t)(measured * 1000.0 + 0.5);
    st->frame_ppm_x10 = (int32_t)((measured / nominal - 1.0) * 1e7 + (measured >= nominal ? 0.5 : -0.5));
    st->rate_valid = true;
    return true;
}

#define PROGRESS_WIDTH 64

static void draw_static_rows(void)
{
    text_screen_printf(2, TEXT_PAL_HEADING, " STREAM");
    text_screen_printf(3, TEXT_PAL_NORMAL,
                       "  L-PCM, 8 channels, 48 kHz, 24-bit    HDMI Layout 1 packets, 2 per island");
    // CTS = f_pixel * N / (128 * 48000) with N = 6144: f_pixel / 1000 exactly.
    const uint32_t pixel_khz = clock_get_hz(clk_hstx) / MODE_HSTX_CSR_CLKDIV / 1000;
    text_screen_printf(4, TEXT_PAL_NORMAL, "  Channel allocation 0x13 (7.1)        ACR N=6144 CTS=%lu",
                       (unsigned long)pixel_khz);
    text_screen_printf(5, TEXT_PAL_NORMAL,
                       "  Video 640x480 VIC %d, %lu.%02lu MHz      System %lu.%lu MHz @ 1.15 V, HSTX div %d",
                       MODE_CEA_VIC, (unsigned long)(pixel_khz / 1000), (unsigned long)(pixel_khz % 1000 / 10),
                       (unsigned long)(SYS_CLOCK_KHZ / 1000), (unsigned long)(SYS_CLOCK_KHZ % 1000 / 100),
                       MODE_HSTX_CLK_DIV);
    // The boot ROM's flash divider stays in force on RP2350 (the SDK's
    // PICO_FLASH_SPI_CLKDIV is not applied); show what the chip really uses.
    const uint32_t flash_div = (qmi_hw->m[0].timing & QMI_M0_TIMING_CLKDIV_BITS) >> QMI_M0_TIMING_CLKDIV_LSB;
    text_screen_printf(6, TEXT_PAL_DIM,
                       "  Flash clock divider %lu (%lu MHz)       Line %d px x %d lines, %lu.%03lu Hz nominal",
                       (unsigned long)flash_div, (unsigned long)(flash_div ? SYS_CLOCK_KHZ / 1000 / flash_div : 0),
                       MODE_H_TOTAL_PIXELS, MODE_V_TOTAL_LINES,
                       (unsigned long)(pixel_khz * 1000 / (MODE_H_TOTAL_PIXELS * MODE_V_TOTAL_LINES)),
                       (unsigned long)((uint64_t)pixel_khz * 1000000 / (MODE_H_TOTAL_PIXELS * MODE_V_TOTAL_LINES) %
                                       1000));
    text_screen_printf(7, TEXT_PAL_HEADING, " SLOT  CODE  SPEAKER               TONE     STATE          TIMES PLAYED");
    text_screen_printf(17, TEXT_PAL_HEADING, " NOW");
    text_screen_printf(21, TEXT_PAL_HEADING, " STATISTICS");
    text_screen_printf(28, TEXT_PAL_DIM, "  Sequence: 1.0 s tone at -12 dBFS, 0.5 s gap, slots 1 to 8 in turn.");
    text_screen_printf(29, TEXT_PAL_DIM, "  A status line also goes out over USB serial once a second.");
}

static void update_screen(const screen_stats_t *st)
{
    // Snapshot the generator; Core 0 owns these, so they are consistent here.
    const uint32_t pos = step_position;
    const int ch = tone_channel;
    const bool in_tone = pos < TONE_SAMPLES;

    uint32_t up_s = (uint32_t)(time_us_64() / 1000000);
    text_screen_printf(0, TEXT_PAL_TITLE, "  PICO_HDMI  8-CHANNEL HDMI AUDIO TEST %*s uptime %02lu:%02lu:%02lu  ", 20,
                       "", (unsigned long)(up_s / 3600), (unsigned long)(up_s / 60 % 60), (unsigned long)(up_s % 60));

    for (int c = 0; c < NUM_CHANNELS; c++) {
        const bool playing = in_tone && c == ch;
        const bool next = !in_tone && c == (ch + 1) % NUM_CHANNELS;
        text_screen_printf(8 + c, playing ? TEXT_PAL_HIGHLIGHT : TEXT_PAL_NORMAL,
                           "  %d    %-4s  %-20s  %3d Hz   %-13s  %lu", c + 1, channel_codes[c], channel_names[c],
                           c == LFE_CHANNEL ? LFE_TONE_HZ : TONE_HZ,
                           playing ? ">> PLAYING <<" : (next ? "next" : ""), (unsigned long)times_played[c]);
    }

    if (in_tone) {
        uint32_t left_ms = (TONE_SAMPLES - pos) / (AUDIO_SAMPLE_RATE / 1000);
        text_screen_printf(18, TEXT_PAL_NORMAL, "  Slot %d %s (%s): %d Hz tone, -12 dBFS        %lu.%02lu s left",
                           ch + 1, channel_codes[ch], channel_names[ch], ch == LFE_CHANNEL ? LFE_TONE_HZ : TONE_HZ,
                           (unsigned long)(left_ms / 1000), (unsigned long)(left_ms % 1000 / 10));
    } else {
        int nx = (ch + 1) % NUM_CHANNELS;
        uint32_t left_ms = (STEP_SAMPLES - pos) / (AUDIO_SAMPLE_RATE / 1000);
        text_screen_printf(18, TEXT_PAL_DIM, "  Silence, next: slot %d %s (%s)        %lu.%02lu s left", nx + 1,
                           channel_codes[nx], channel_names[nx], (unsigned long)(left_ms / 1000),
                           (unsigned long)(left_ms % 1000 / 10));
    }

    char bar[PROGRESS_WIDTH + 1];
    const uint32_t filled = (uint32_t)(((uint64_t)pos * PROGRESS_WIDTH) / STEP_SAMPLES);
    const uint32_t tone_end = (TONE_SAMPLES * PROGRESS_WIDTH) / STEP_SAMPLES;
    for (uint32_t i = 0; i < PROGRESS_WIDTH; i++) {
        bar[i] = i < filled ? (i < tone_end ? '#' : '=') : (i < tone_end ? '.' : ' ');
    }
    bar[PROGRESS_WIDTH] = '\0';
    text_screen_printf(19, TEXT_PAL_NORMAL, "  [%s]", bar);

    const uint32_t queue = hdmi_audio_8ch_queued_frames() / 2;
    text_screen_printf(22, TEXT_PAL_NORMAL, "  Samples sent   %-14llu  IEC 60958 frame   %3d / 192",
                       (unsigned long long)samples_generated, (int)(samples_generated % 192));
    text_screen_printf(23, TEXT_PAL_NORMAL, "  Audio queue    %3lu / 255 islands (%lu.%lu ms)   Underruns         %lu",
                       (unsigned long)queue, (unsigned long)(queue * 2 * 10 / 480),
                       (unsigned long)(queue * 2 * 100 / 480 % 10), (unsigned long)hdmi_audio_8ch_underruns());
    text_screen_printf(24, TEXT_PAL_NORMAL, "  Video frames   %-14lu  Frame rate        %lu fps",
                       (unsigned long)st->frames, (unsigned long)st->fps);
    // Core 1 renders 480 text lines 60 times a second.
    const uint32_t core1_pct_x10 = (uint32_t)(((uint64_t)st->render_ns_per_line * 480 * 60) / 1000000);
    text_screen_printf(25, TEXT_PAL_NORMAL,
                       "  Core 0 audio   %lu %%            Core 1 text render %lu.%lu %% (%lu.%lu us/line)",
                       (unsigned long)st->core0_load_pct, (unsigned long)(core1_pct_x10 / 10),
                       (unsigned long)(core1_pct_x10 % 10), (unsigned long)(st->render_ns_per_line / 1000),
                       (unsigned long)(st->render_ns_per_line % 1000 / 100));
    if (st->rate_valid) {
        const int32_t ppm10 = st->frame_ppm_x10;
        const int32_t mag = ppm10 < 0 ? -ppm10 : ppm10;
        text_screen_printf(26, TEXT_PAL_NORMAL,
                           "  Measured rate  %ld.%03ld Hz      vs nominal %c%ld.%ld ppm (10 s window)",
                           (long)(st->frame_rate_mhz / 1000), (long)(st->frame_rate_mhz % 1000), ppm10 < 0 ? '-' : '+',
                           (long)(mag / 10), (long)(mag % 10));
    } else {
        text_screen_printf(26, TEXT_PAL_DIM, "  Measured rate  (needs 10 s)");
    }
    if (st->hook_status) {
        text_screen_printf(27, TEXT_PAL_HEADING, "  %s", st->hook_status);
    }
}

#if AUDIO_8CH_TEST_TRACE
// Split the per-island audio cost into its stages (trace builds only).
static void benchmark_audio_path(void)
{
    enum { N = 1000 };
    audio_sample_8ch_t frame = {{1, -2, 3, -4, 5, -6, 7, -8}};
    hstx_packet_t a, b;
    static hstx_data_island2_t island;
    int fc = 0;
    uint64_t t0 = time_us_64();
    for (int i = 0; i < N; i++) {
        frame.ch[0] = i;
        fc = hstx_packet_set_audio_sample_8ch(&a, &frame, fc, AUDIO_SAMPLE_RATE);
        fc = hstx_packet_set_audio_sample_8ch(&b, &frame, fc, AUDIO_SAMPLE_RATE);
    }
    uint64_t t1 = time_us_64();
    for (int i = 0; i < N; i++) {
        a.subpacket[0][0] = (uint8_t)i;
        hstx_encode_data_island2(&island, &a, &b, false, DI_HSYNC_ACTIVE);
    }
    uint64_t t2 = time_us_64();
    TRACE("per island: build 2 packets %lu ns, encode %lu ns\n", (unsigned long)((t1 - t0) * 1000 / N),
          (unsigned long)((t2 - t1) * 1000 / N));
}
#endif

// Time the scanline renderer over a full frame. Runs on Core 0 before Core 1
// starts, as an estimate of what the video interrupt spends per line.
static uint32_t measure_render_ns_per_line(void)
{
    uint64_t t0 = time_us_64();
    for (uint32_t line = 0; line < 480 * 4; line++) {
        (void)text_screen_scanline(0, line % FRAME_HEIGHT);
    }
    return (uint32_t)((time_us_64() - t0) * 1000 / (480 * 4));
}

// ============================================================================
// Main (Core 0)
// ============================================================================

int main(void)
{
    // 252 MHz needs a little more than the default 1.10 V core voltage. The
    // flash interface keeps the boot ROM's divider (shown on screen), well
    // within the Pico 2 flash's rating.
    vreg_set_voltage(VREG_VOLTAGE_1_15);
    sleep_ms(10);
    set_sys_clock_khz(SYS_CLOCK_KHZ, true);

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
    text_screen_init();
    draw_static_rows();
    screen_stats_t stats = {0};
    update_screen(&stats);
    stats.render_ns_per_line = measure_render_ns_per_line();
    TRACE("sine table and screen done, render %lu ns/line\n", (unsigned long)stats.render_ns_per_line);
#if AUDIO_8CH_TEST_TRACE
    benchmark_audio_path();
#endif

    // Initialize HDMI output
    hstx_di_queue_init();
    TRACE("queue init done\n");
    video_output_init(FRAME_WIDTH, FRAME_HEIGHT);
    TRACE("video_output_init done\n");
    pico_hdmi_set_audio_sample_rate(AUDIO_SAMPLE_RATE);
    video_output_set_scanline_pointer_callback(text_screen_scanline);
    video_output_set_vsync_callback(on_vsync);
    TRACE("audio rate set\n");

#if AUDIO_8CH_TEST_VIDEO_ONLY
    video_output_set_dvi_mode(true);
#else
    // Pre-fill audio buffer
    uint64_t prefill_start = time_us_64();
    while (hdmi_audio_8ch_queued_frames() < QUEUE_TARGET_FRAMES) {
        generate_audio();
    }
    TRACE("prefill done, %lu frames queued, %lu us\n", (unsigned long)hdmi_audio_8ch_queued_frames(),
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
    uint32_t window_frames = 0;
    uint64_t last_screen_update = 0;

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
        stats.hook_status = audio_8ch_test_hook(now);
#if AUDIO_8CH_TEST_TRACE
        static uint64_t last_trace;
        static uint32_t loops, traces;
        loops++;
        if (traces < 60 && now - last_trace >= 100000) {
            last_trace = now;
            traces++;
            TRACE("loop %lu frames %lu queue %lu underruns %lu\n", (unsigned long)loops, (unsigned long)frames,
                  (unsigned long)hdmi_audio_8ch_queued_frames(), (unsigned long)hdmi_audio_8ch_underruns());
            if (traces == 25) {
                text_screen_dump();
            }
        }
#endif
        if (now - last_screen_update >= SCREEN_UPDATE_US) {
            last_screen_update = now;
            stats.frames = frames;
            update_screen(&stats);
        }
        if (now - window_start >= 1000000) {
            uint32_t load_pct = (uint32_t)((busy_us * 100) / (now - window_start));
            stats.core0_load_pct = load_pct;
            stats.fps = (uint32_t)(((uint64_t)(frames - window_frames) * 1000000) / (now - window_start));
            window_frames = frames;
            const bool rate_ok = measure_frame_rate(&stats);
            int ch = display_channel;
            printf("frames %lu | queue %lu | underruns %lu | core0 audio load %lu%% | %s%s", (unsigned long)frames,
                   (unsigned long)hdmi_audio_8ch_queued_frames(), (unsigned long)hdmi_audio_8ch_underruns(),
                   (unsigned long)load_pct, ch >= 0 ? "playing " : "gap", ch >= 0 ? channel_names[ch] : "");
            if (rate_ok) {
                const int32_t mag = stats.frame_ppm_x10 < 0 ? -stats.frame_ppm_x10 : stats.frame_ppm_x10;
                printf(" | rate %ld.%03ld Hz %c%ld.%ld ppm", (long)(stats.frame_rate_mhz / 1000),
                       (long)(stats.frame_rate_mhz % 1000), stats.frame_ppm_x10 < 0 ? '-' : '+', (long)(mag / 10),
                       (long)(mag % 10));
            }
            if (stats.hook_status) {
                printf(" | %s", stats.hook_status);
            }
            printf("\n");
            window_start = now;
            busy_us = 0;
        }
    }

    return 0;
}
