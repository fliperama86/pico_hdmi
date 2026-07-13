#include "pico_hdmi/hstx_data_island_queue.h"

#include "pico_hdmi/video_output.h"

#include <string.h>

#include "pico.h"

#ifndef PICO_HDMI_RAM_DI_QUEUE_PUSH
#define PICO_HDMI_RAM_DI_QUEUE_PUSH 0
#endif

#define DI_RING_BUFFER_SIZE 256
static hstx_data_island_t di_ring_buffer[DI_RING_BUFFER_SIZE];
static volatile uint32_t di_ring_head = 0;
static volatile uint32_t di_ring_tail = 0;

// Single pre-encoded silent audio packet (no B flag; see init).
static hstx_data_island_t silence_packet;
static bool di_hsync_active = DI_HSYNC_ACTIVE;
// Underrun insertions of the silence packet (see get_audio_packet).
volatile uint32_t hstx_di_queue_silence_count;

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

// Limit accumulator to avoid overflow if we run dry.
// Clamping to 1 packet (plus a tiny margin is implicit) ensures we don't burst.
#define MAX_AUDIO_ACCUM (4 << 16)

static void hstx_di_queue_build_silence_packet(void)
{
    // frame_count=4 (NOT 0): frame 0 would set the IEC block-start B flag,
    // so every underrun insertion would reset the sink's channel-status block
    // sync mid-stream.
    hstx_packet_t packet;
    audio_sample_t samples[4] = {0};
    (void)hstx_packet_set_audio_samples(&packet, samples, 4, 4);
    hstx_encode_data_island(&silence_packet, &packet, false, di_hsync_active);
}

void hstx_di_queue_init(void)
{
    di_ring_head = 0;
    di_ring_tail = 0;
    hstx_di_queue_silence_count = 0;
    audio_sample_accum = 0;
#if PICO_HDMI_EXACT_AUDIO_PACING
    audio_pacing_rem_accum = 0;
#endif
    hstx_di_queue_build_silence_packet();
}

void hstx_di_queue_set_sample_rate(uint32_t sample_rate)
{
    uint32_t samples_per_frame = sample_rate / 60;
    samples_per_line_fp = (samples_per_frame << 16) / cached_v_total_lines;
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

bool hstx_di_queue_get_hsync_active(void)
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

#if PICO_HDMI_RAM_DI_QUEUE_PUSH
bool __not_in_flash_func(hstx_di_queue_push)(const hstx_data_island_t *island)
#else
bool hstx_di_queue_push(const hstx_data_island_t *island)
#endif
{
    uint32_t next_head = (di_ring_head + 1) % DI_RING_BUFFER_SIZE;
    if (next_head == di_ring_tail)
        return false;

    di_ring_buffer[di_ring_head] = *island;
    di_ring_head = next_head;
    return true;
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
    // Check if it's time to send a 4-sample audio packet (every ~2.6 lines)
    if (audio_sample_accum >= (4 << 16)) {
        audio_sample_accum -= (4 << 16);
        if (di_ring_tail != di_ring_head) {
            const uint32_t *words = di_ring_buffer[di_ring_tail].words;
            di_ring_tail = (di_ring_tail + 1) % DI_RING_BUFFER_SIZE;
            return words;
        }
        // Queue is empty: return a pre-encoded silent packet to keep HDMI
        // audio active. Every insertion stretches the real sample stream by
        // 4 samples -- if the counter climbs, the producer is starving.
        hstx_di_queue_silence_count++;
        return silence_packet.words;
    }
    return NULL;
}
