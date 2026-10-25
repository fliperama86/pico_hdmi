#ifndef HDMI_AUDIO_8CH_H
#define HDMI_AUDIO_8CH_H

/**
 * 8-channel PCM interface for the HDMI audio output (PICO_HDMI_AUDIO_8CH).
 *
 * Callers hand over 8-channel frames; the library builds the Layout 1 audio
 * sample packets (with IEC 60958 channel status), encodes them two per Data
 * Island straight into the island queue, and the scanline interrupt sends them
 * at the HDMI audio rate. No caller needs to know about packets or TERC4.
 *
 * Single producer: call these from one core only (the one feeding audio).
 * When the queue runs dry the output sends silence islands, so an input that
 * stops or stalls mutes the HDMI audio instead of breaking the stream.
 */

#include "pico_hdmi/hstx_packet.h"

#include <stdint.h>

#ifndef PICO_HDMI_AUDIO_8CH
#define PICO_HDMI_AUDIO_8CH 0
#endif

#if PICO_HDMI_AUDIO_8CH

/**
 * Queue frames for HDMI output. Never blocks: takes as many frames as fit and
 * returns that count. Frames go out in pairs, so an odd last frame is held
 * until the next call (it is counted as taken).
 */
uint32_t hdmi_audio_8ch_write(const audio_sample_8ch_t *frames, uint32_t count);

/**
 * Frames hdmi_audio_8ch_write() is guaranteed to take right now.
 */
uint32_t hdmi_audio_8ch_free_frames(void);

/**
 * Frames written but not yet sent: in the island queue, plus a held odd frame.
 */
uint32_t hdmi_audio_8ch_queued_frames(void);

/**
 * Running count of frame slots the HDMI output has sent, real audio or
 * silence. It advances at exactly the HDMI audio sample rate for as long as
 * video runs, so its rate against an input's frame count measures the clock
 * offset between them (for a clock servo or a USB feedback endpoint). Wraps at
 * 2^32 (about 24.8 hours at 48 kHz); compare differences, not values.
 */
uint32_t hdmi_audio_8ch_frames_sent(void);

/**
 * Silence islands inserted because the queue was empty (two frames each).
 */
uint32_t hdmi_audio_8ch_underruns(void);

#endif // PICO_HDMI_AUDIO_8CH

#endif // HDMI_AUDIO_8CH_H
