#include "pico_hdmi/hdmi_audio_8ch.h"

#include "pico_hdmi/hstx_data_island_queue.h"
#include "pico_hdmi/hstx_packet.h"

#include "pico.h"

#if PICO_HDMI_AUDIO_8CH

// IEC 60958 frame position (0..191) of the next frame encoded.
static int iec_frame_count;

// An odd frame waiting for a partner: islands carry two frames.
static audio_sample_8ch_t held_frame;
static bool have_held_frame;

// Encode two frames as one island directly into the queue. False if full.
static bool __not_in_flash_func(push_pair)(const audio_sample_8ch_t *a, const audio_sample_8ch_t *b)
{
    hstx_data_island2_t *slot = hstx_di_queue_reserve_8ch();
    if (!slot) {
        return false;
    }
    const uint32_t rate = hstx_di_queue_get_sample_rate();
    hstx_packet_t first;
    hstx_packet_t second;
    iec_frame_count = hstx_packet_set_audio_sample_8ch(&first, a, iec_frame_count, rate);
    iec_frame_count = hstx_packet_set_audio_sample_8ch(&second, b, iec_frame_count, rate);
    hstx_encode_data_island2(slot, &first, &second, false, hstx_di_queue_get_hsync_active());
    hstx_di_queue_commit_8ch();
    return true;
}

uint32_t __not_in_flash_func(hdmi_audio_8ch_write)(const audio_sample_8ch_t *frames, uint32_t count)
{
    uint32_t taken = 0;

    if (have_held_frame && count > 0) {
        if (!push_pair(&held_frame, &frames[0])) {
            return 0;
        }
        have_held_frame = false;
        taken = 1;
    }
    while (count - taken >= 2) {
        if (!push_pair(&frames[taken], &frames[taken + 1])) {
            return taken;
        }
        taken += 2;
    }
    if (count - taken == 1) {
        held_frame = frames[taken];
        have_held_frame = true;
        taken++;
    }
    return taken;
}

uint32_t hdmi_audio_8ch_free_frames(void)
{
    const uint32_t free_frames = hstx_di_queue_get_free() * HSTX_DI_AUDIO_SAMPLES_PER_ISLAND;
    return have_held_frame ? free_frames - 1 : free_frames;
}

uint32_t hdmi_audio_8ch_queued_frames(void)
{
    return (hstx_di_queue_get_level() * HSTX_DI_AUDIO_SAMPLES_PER_ISLAND) + (have_held_frame ? 1 : 0);
}

uint32_t hdmi_audio_8ch_frames_sent(void)
{
    return hstx_di_queue_islands_sent * HSTX_DI_AUDIO_SAMPLES_PER_ISLAND;
}

uint32_t hdmi_audio_8ch_underruns(void)
{
    return hstx_di_queue_silence_count;
}

#endif // PICO_HDMI_AUDIO_8CH
