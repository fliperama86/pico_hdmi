#ifndef HSTX_DATA_ISLAND_QUEUE_H
#define HSTX_DATA_ISLAND_QUEUE_H

#include "pico_hdmi/hstx_packet.h"

#include <stdbool.h>
#include <stdint.h>

#ifndef PICO_HDMI_AUDIO_8CH
#define PICO_HDMI_AUDIO_8CH 0
#endif

// Audio Data Island geometry. The stereo build queues one-packet islands of
// four samples each. The 8-channel build (PICO_HDMI_AUDIO_8CH) queues
// two-packet islands, each packet one Layout 1 frame, so two samples each.
#if PICO_HDMI_AUDIO_8CH
#define HSTX_DI_AUDIO_ISLAND_WORDS W_DATA_ISLAND_2
#define HSTX_DI_AUDIO_SAMPLES_PER_ISLAND 2
#else
#define HSTX_DI_AUDIO_ISLAND_WORDS W_DATA_ISLAND
#define HSTX_DI_AUDIO_SAMPLES_PER_ISLAND 4
#endif

/**
 * Initialize the Data Island queue and scheduler.
 */
void hstx_di_queue_init(void);

/**
 * Set the audio sample rate for packet timing.
 * @param sample_rate Audio sample rate in Hz (e.g. 44100, 48000)
 */
void hstx_di_queue_set_sample_rate(uint32_t sample_rate);

#if PICO_HDMI_AUDIO_8CH
/**
 * Push a pre-encoded two-packet audio Data Island (two 8-channel frames,
 * see hstx_packet_set_audio_sample_8ch and hstx_encode_data_island2).
 * Returns true if successful, false if the queue is full.
 */
bool hstx_di_queue_push_8ch(const hstx_data_island2_t *island);

/**
 * Zero-copy push: return the next free slot to encode an island into, or
 * NULL if the queue is full. The slot goes out only after
 * hstx_di_queue_commit_8ch(). Single producer: do not mix with push calls
 * between a reserve and its commit.
 */
hstx_data_island2_t *hstx_di_queue_reserve_8ch(void);
void hstx_di_queue_commit_8ch(void);
#else
/**
 * Push a pre-encoded Data Island into the queue.
 * Returns true if successful, false if the queue is full.
 */
bool hstx_di_queue_push(const hstx_data_island_t *island);
#endif

/**
 * Get the current number of items in the queue.
 */
uint32_t hstx_di_queue_get_level(void);

/**
 * Get the number of items that can be pushed before the queue is full.
 */
uint32_t hstx_di_queue_get_free(void);

/**
 * Audio sample rate the queue is pacing for (set by hstx_di_queue_set_sample_rate).
 */
uint32_t hstx_di_queue_get_sample_rate(void);

/**
 * Audio islands the scheduler has sent, queued or silence. Advances at the
 * HDMI audio rate (HSTX_DI_AUDIO_SAMPLES_PER_ISLAND samples per island) for
 * as long as video runs; wraps at 2^32.
 */
extern volatile uint32_t hstx_di_queue_islands_sent;

/**
 * Times the queue ran dry at a scheduled audio slot and a silence packet
 * was inserted instead (each one stretches the real stream by
 * HSTX_DI_AUDIO_SAMPLES_PER_ISLAND samples).
 * Climbing while audio plays = producer starvation.
 */
extern volatile uint32_t hstx_di_queue_silence_count;

/**
 * Set the vertical total lines for audio packet timing calculation.
 * Must be called when the video mode changes.
 * @param v_total Total vertical lines (e.g. 525 for 480p, 262 for 240p)
 */
void hstx_di_queue_set_v_total(uint32_t v_total);

/**
 * Set whether queued/silent audio Data Islands are encoded during the hsync
 * pulse. Wide-hsync modes use true; narrow-hsync modes such as 720p use false.
 */
void hstx_di_queue_set_hsync_active(bool hsync_active);
bool hstx_di_queue_get_hsync_active(void);

/**
 * Override samples_per_line with a pixel-clock-accurate 16.16 fixed-point value.
 * Use this instead of set_sample_rate when the frame rate is not exactly 60 Hz.
 * @param value samples_per_line in 16.16 fixed-point
 */
void hstx_di_queue_set_samples_per_line_fp(uint32_t value);

#if PICO_HDMI_EXACT_AUDIO_PACING
/**
 * Set exact-rational audio pacing on top of the truncated 16.16 samples-per-line.
 * spl_fp is the truncated 16.16 samples-per-line (same value set_samples_per_line_fp
 * would take); rem/den is the exact fractional remainder of
 * (sample_rate * h_total << 16) / pixel_clock, so long-term delivery is exactly
 * sample_rate instead of drifting low from floor truncation.
 */
void hstx_di_queue_set_samples_per_line_exact(uint32_t spl_fp, uint32_t rem, uint32_t den);
#endif

/**
 * Advance the Data Island scheduler by one scanline.
 * Must be called exactly once per scanline in the DMA ISR.
 */
void hstx_di_queue_tick(void);

/**
 * Get the next audio Data Island packet if the scheduler determines it's time.
 *
 * @return Pointer to an HSTX_DI_AUDIO_ISLAND_WORDS-word data island, or NULL
 *         if no packet is due.
 */
const uint32_t *hstx_di_queue_get_audio_packet(void);

#endif // HSTX_DATA_ISLAND_QUEUE_H
