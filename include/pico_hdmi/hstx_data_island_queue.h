#ifndef HSTX_DATA_ISLAND_QUEUE_H
#define HSTX_DATA_ISLAND_QUEUE_H

#include "pico_hdmi/hstx_packet.h"

#include <stdbool.h>
#include <stdint.h>

/**
 * Initialize the Data Island queue and scheduler.
 */
void hstx_di_queue_init(void);

/**
 * Set the audio sample rate for packet timing.
 * @param sample_rate Audio sample rate in Hz (e.g. 44100, 48000)
 */
void hstx_di_queue_set_sample_rate(uint32_t sample_rate);

/**
 * Push a pre-encoded Data Island into the queue.
 * Returns true if successful, false if the queue is full.
 */
bool hstx_di_queue_push(const hstx_data_island_t *island);

/**
 * Get the current number of items in the queue.
 */
uint32_t hstx_di_queue_get_level(void);

/**
 * Times the queue ran dry at a scheduled audio slot and a silence packet
 * was inserted instead (each one stretches the real stream by 4 samples).
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
 * @return Pointer to 36-word HSTX data island, or NULL if no packet is due.
 */
const uint32_t *hstx_di_queue_get_audio_packet(void);

#endif // HSTX_DATA_ISLAND_QUEUE_H
