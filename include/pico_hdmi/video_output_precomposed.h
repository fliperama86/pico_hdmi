// Precomposed active-line support, shared by both library paths
// (video_output.c and video_output_rt.c). See PICO_HDMI_PRECOMPOSED_ACTIVE_LINES.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Pre-composed active-line support (compile with
 * PICO_HDMI_PRECOMPOSED_ACTIVE_LINES=1). Static active-line headers (sync +
 * preamble + video preamble) are built ONCE by the first
 * video_output_compose_service() call; per line, the scanline ISR pops a
 * pre-encoded island from the di queue and patches its 36 words into the
 * entry being posted (~1.5 us). Audio pacing therefore lives in the ISR and
 * cannot be starved by background work. All audio rides the active lines
 * (pace the queue accordingly, e.g.
 * hstx_di_queue_set_samples_per_line_fp((samples_per_frame<<16)/active_lines)).
 */
typedef struct {
    volatile uint32_t tag; // global active-line index this entry was composed for
    uint16_t len;
    uint16_t _pad;
    uint32_t buf[76];
} video_output_precomposed_line_t;

void video_output_set_compose_ring(video_output_precomposed_line_t *ring, uint32_t entries);
void video_output_compose_service(void);

/**
 * Active lines posted via the static fallback before the headers were
 * built (startup only). Must not grow while running.
 */
extern volatile uint32_t video_output_precomposed_stale_count;

/**
 * Full scanout restart (recovers a desynced HSTX command stream). On the
 * runtime-modes path this defers to the core-1 loop's resync machinery.
 */
void video_output_force_resync(void);
extern volatile uint32_t video_output_resync_count;

#ifdef __cplusplus
}
#endif
