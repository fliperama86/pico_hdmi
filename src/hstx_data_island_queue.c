#include "pico_hdmi/hstx_data_island_queue.h"

#include "pico_hdmi/video_output.h"

#include <string.h>

#include "pico.h"

#include "hardware/sync.h"

#ifndef PICO_HDMI_RAM_DI_QUEUE_PUSH
#define PICO_HDMI_RAM_DI_QUEUE_PUSH 0
#endif

#if PICO_HDMI_AUDIO_8CH
typedef hstx_data_island2_t di_entry_t;
#else
typedef hstx_data_island_t di_entry_t;
#endif

#define DI_SAMPLES_FP (HSTX_DI_AUDIO_SAMPLES_PER_ISLAND << 16)

#define DI_RING_BUFFER_SIZE 256
static di_entry_t di_ring_buffer[DI_RING_BUFFER_SIZE];
static volatile uint32_t di_ring_head = 0;
static volatile uint32_t di_ring_tail = 0;

// Single pre-encoded silent audio packet (no B flag; see init).
static di_entry_t silence_packet;
static bool di_hsync_active = DI_HSYNC_ACTIVE;
static uint32_t current_sample_rate = 48000;
// Underrun insertions of the silence packet (see get_audio_packet).
volatile uint32_t hstx_di_queue_silence_count;
// Every audio island handed to the scanline ISR, queued or silence.
volatile uint32_t hstx_di_queue_islands_sent;

// Audio timing state (default 48kHz, 525 lines for 480p)
static uint32_t audio_sample_accum = 0; // Fixed-point accumulator
static uint32_t cached_v_total_lines = 525;
#define DEFAULT_SAMPLES_PER_FRAME (48000 / 60)
static uint32_t samples_per_line_fp = (DEFAULT_SAMPLES_PER_FRAME << 16) / 525;

#if PICO_HDMI_EXACT_AUDIO_PACING
// Exact-rational pacing: recovers the samples/s lost to samples_per_line_fp's
// floor truncation. rem/den is the fractional remainder of
// (sample_rate * h_total << 16) / pixel_clock; rem_accum carries it forward
// so long-term delivery is exactly sample_rate instead of drifting low.
static uint32_t audio_pacing_rem = 0;
static uint32_t audio_pacing_den = 1;
static uint32_t audio_pacing_rem_accum = 0;
#endif

// Cap on the audio backlog (samples due but not yet sent), in 16.16. Lines
// that carry no audio island (vsync, ACR, InfoFrames) leave a few islands of
// backlog each frame, which later lines pay back; 16 islands is far above
// that. Without the cap the accumulator keeps growing whenever lines tick
// without taking audio (for example precomposed lines before the compose
// ring is built), then puts an island on every line until the backlog is
// paid off; at 480p it wraps after about 1.4 s.
#define MAX_AUDIO_ACCUM (16 * DI_SAMPLES_FP)

static void hstx_di_queue_build_silence_packet(void)
{
    // frame_count=4 (NOT 0): frame 0 would set the IEC block-start B flag,
    // so every underrun insertion would reset the sink's channel-status block
    // sync mid-stream.
#if PICO_HDMI_AUDIO_8CH
    hstx_packet_t first;
    hstx_packet_t second;
    audio_sample_8ch_t sample = {0};
    int fc = hstx_packet_set_audio_sample_8ch(&first, &sample, 4, current_sample_rate);
    (void)hstx_packet_set_audio_sample_8ch(&second, &sample, fc, current_sample_rate);
    hstx_encode_data_island2(&silence_packet, &first, &second, false, di_hsync_active);
#else
    hstx_packet_t packet;
    audio_sample_t samples[4] = {0};
    (void)hstx_packet_set_audio_samples_cs_rate(&packet, samples, 4, 4, current_sample_rate);
    hstx_encode_data_island(&silence_packet, &packet, false, di_hsync_active);
#endif
}

void hstx_di_queue_init(void)
{
    di_ring_head = 0;
    di_ring_tail = 0;
    hstx_di_queue_silence_count = 0;
    hstx_di_queue_islands_sent = 0;
    audio_sample_accum = 0;
#if PICO_HDMI_EXACT_AUDIO_PACING
    audio_pacing_rem_accum = 0;
#endif
    hstx_di_queue_build_silence_packet();
}

void hstx_di_queue_set_sample_rate(uint32_t sample_rate)
{
    current_sample_rate = sample_rate;
    uint32_t samples_per_frame = sample_rate / 60;
    samples_per_line_fp = (samples_per_frame << 16) / cached_v_total_lines;
    hstx_di_queue_build_silence_packet();
}

void hstx_di_queue_set_v_total(uint32_t v_total)
{
    cached_v_total_lines = v_total;
}

void hstx_di_queue_set_hsync_active(bool hsync_active)
{
    if (di_hsync_active != hsync_active) {
        di_ring_head = 0;
        di_ring_tail = 0;
        audio_sample_accum = 0;
#if PICO_HDMI_EXACT_AUDIO_PACING
        audio_pacing_rem_accum = 0;
#endif
    }
    di_hsync_active = hsync_active;
    hstx_di_queue_build_silence_packet();
}

bool __not_in_flash_func(hstx_di_queue_get_hsync_active)(void)
{
    return di_hsync_active;
}

void hstx_di_queue_set_samples_per_line_fp(uint32_t value)
{
    samples_per_line_fp = value;
}

#if PICO_HDMI_EXACT_AUDIO_PACING
void hstx_di_queue_set_samples_per_line_exact(uint32_t spl_fp, uint32_t rem, uint32_t den)
{
    samples_per_line_fp = spl_fp;
    audio_pacing_rem = rem;
    audio_pacing_den = den;
    audio_pacing_rem_accum = 0;
}
#endif

#if PICO_HDMI_AUDIO_8CH
#if PICO_HDMI_RAM_DI_QUEUE_PUSH
bool __not_in_flash_func(hstx_di_queue_push_8ch)(const hstx_data_island2_t *island)
#else
bool hstx_di_queue_push_8ch(const hstx_data_island2_t *island)
#endif
#else
#if PICO_HDMI_RAM_DI_QUEUE_PUSH
bool __not_in_flash_func(hstx_di_queue_push)(const hstx_data_island_t *island)
#else
bool hstx_di_queue_push(const hstx_data_island_t *island)
#endif
#endif
{
    uint32_t next_head = (di_ring_head + 1) % DI_RING_BUFFER_SIZE;
    if (next_head == di_ring_tail)
        return false;

    di_ring_buffer[di_ring_head] = *island;
    di_ring_head = next_head;
    return true;
}

#if PICO_HDMI_AUDIO_8CH
hstx_data_island2_t *__not_in_flash_func(hstx_di_queue_reserve_8ch)(void)
{
    uint32_t next_head = (di_ring_head + 1) % DI_RING_BUFFER_SIZE;
    if (next_head == di_ring_tail)
        return NULL;
    return &di_ring_buffer[di_ring_head];
}

void __not_in_flash_func(hstx_di_queue_commit_8ch)(void)
{
    // The island's words must be visible to the other core before the
    // scanline ISR can see the new head.
    __dmb();
    di_ring_head = (di_ring_head + 1) % DI_RING_BUFFER_SIZE;
}
#endif

uint32_t hstx_di_queue_get_free(void)
{
    return (DI_RING_BUFFER_SIZE - 1) - hstx_di_queue_get_level();
}

uint32_t __not_in_flash_func(hstx_di_queue_get_sample_rate)(void)
{
    return current_sample_rate;
}

uint32_t hstx_di_queue_get_level(void)
{
    uint32_t head = di_ring_head;
    uint32_t tail = di_ring_tail;
    if (head >= tail)
        return head - tail;
    return DI_RING_BUFFER_SIZE + head - tail;
}

void __scratch_x("") hstx_di_queue_tick(void)
{
    audio_sample_accum += samples_per_line_fp;
    if (audio_sample_accum > MAX_AUDIO_ACCUM)
        audio_sample_accum = MAX_AUDIO_ACCUM;
#if PICO_HDMI_EXACT_AUDIO_PACING
    audio_pacing_rem_accum += audio_pacing_rem;
    if (audio_pacing_rem_accum >= audio_pacing_den) {
        audio_pacing_rem_accum -= audio_pacing_den;
        audio_sample_accum += 1;
    }
#endif
}

const uint32_t *__scratch_x("") hstx_di_queue_get_audio_packet(void)
{
    // Check if it's time to send the next audio island: every ~2.6 lines
    // for 4-sample stereo packets at 480p, every ~1.3 lines for 8-channel
    // two-frame islands.
    if (audio_sample_accum >= DI_SAMPLES_FP) {
        audio_sample_accum -= DI_SAMPLES_FP;
        hstx_di_queue_islands_sent++;
        if (di_ring_tail != di_ring_head) {
            const uint32_t *words = di_ring_buffer[di_ring_tail].words;
            di_ring_tail = (di_ring_tail + 1) % DI_RING_BUFFER_SIZE;
            return words;
        }
        // Queue is empty: return a pre-encoded silent packet to keep HDMI
        // audio active. Every insertion stretches the real sample stream by
        // one island's worth of samples. If the counter climbs, the
        // producer is starving.
        hstx_di_queue_silence_count++;
        return silence_packet.words;
    }
    return NULL;
}
