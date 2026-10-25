#ifndef HSTX_PACKET_H
#define HSTX_PACKET_H

#include <stdbool.h>
#include <stdint.h>

// Data Island timing constants
#define W_GUARDBAND 2                                             // Guard band: 2 pixel clocks
#define W_PREAMBLE 8                                              // Preamble: 8 pixel clocks
#define W_DATA_PACKET 32                                          // Packet data: 32 pixel clocks
#define W_DATA_ISLAND (W_GUARDBAND + W_DATA_PACKET + W_GUARDBAND) // Total: 36

// HSTX outputs 1 symbol per word
#define HSTX_DATA_ISLAND_WORDS W_DATA_ISLAND // 36 words for HSTX

// A Data Island carrying two back-to-back packets: one leading guard band,
// two 32-pixel packets, one trailing guard band (68 words).
#define W_DATA_ISLAND_2 (W_GUARDBAND + (2 * W_DATA_PACKET) + W_GUARDBAND)

// Packet structure (same as DVI/HSTX spec)
typedef struct {
    uint8_t header[4];       // 3 bytes header + 1 byte BCH parity
    uint8_t subpacket[4][8]; // 4 subpackets, each 7 bytes + 1 byte BCH parity
} hstx_packet_t;

// Pre-encoded data island for HSTX (36 words)
typedef struct {
    uint32_t words[HSTX_DATA_ISLAND_WORDS];
} hstx_data_island_t;

// Pre-encoded two-packet data island for HSTX (68 words)
typedef struct {
    uint32_t words[W_DATA_ISLAND_2];
} hstx_data_island2_t;

// Audio sample structure
typedef struct {
    int16_t left;
    int16_t right;
} audio_sample_t;

// One 8-channel audio frame. Each entry is a signed 24-bit sample held in the
// low 24 bits of an int32_t (range -8388608..8388607). Channel n goes to
// HDMI audio slot n+1; with channel allocation 0x13 (7.1) the slots are
// FL, FR, LFE, FC, RL, RR, RLC, RRC.
#define HSTX_AUDIO_8CH_CHANNELS 8
typedef struct {
    int32_t ch[HSTX_AUDIO_8CH_CHANNELS];
} audio_sample_8ch_t;

// CTA-861 Audio InfoFrame channel allocation (CA) codes.
#define HSTX_AUDIO_CA_STEREO 0x00
#define HSTX_AUDIO_CA_7_1 0x13 // FL FR LFE FC RL RR RLC RRC

// ============================================================================
// Packet creation functions
// ============================================================================

void hstx_packet_init(hstx_packet_t *packet);
void hstx_packet_set_acr(hstx_packet_t *packet, uint32_t n, uint32_t cts);
void hstx_packet_set_audio_infoframe(hstx_packet_t *packet, uint32_t sample_rate, uint8_t channels,
                                     uint8_t bits_per_sample);
// As above, with an explicit channel allocation code (PB4), which multichannel
// streams need so the sink knows which speaker each slot feeds.
void hstx_packet_set_audio_infoframe_ca(hstx_packet_t *packet, uint32_t sample_rate, uint8_t channels,
                                        uint8_t bits_per_sample, uint8_t channel_allocation);
void hstx_packet_set_avi_infoframe(hstx_packet_t *packet, uint8_t vic, uint8_t pixel_repetition);
void hstx_packet_set_avi_infoframe_aspect(hstx_packet_t *packet, uint8_t vic, uint8_t pixel_repetition,
                                          bool aspect_16_9);
// SPD (Source Product Description) InfoFrame, CTA-861: an 8-character vendor
// name, a 16-character product description and a Source Device Information
// code. Sinks show them as the input's name/icon; some also pick a game or PC
// picture mode from the device code. Longer strings are truncated, shorter
// ones zero-padded.
#define HSTX_SPD_DEVICE_UNKNOWN 0x00
#define HSTX_SPD_DEVICE_DIGITAL_STB 0x01
#define HSTX_SPD_DEVICE_DVD_PLAYER 0x02
#define HSTX_SPD_DEVICE_GAME 0x08
#define HSTX_SPD_DEVICE_PC 0x09
void hstx_packet_set_spd_infoframe(hstx_packet_t *packet, const char *vendor, const char *product, uint8_t device_info);
// DV1 ("Direct Video") SPD InfoFrame: the MiSTer convention a RetroTINK-4K
// reads to undo pixel repetition and crop padding on a native-rate signal.
// It reuses the SPD packet with a different payload: PB1..PB3 "DV1", PB4
// flags, PB5 pixel repetition (output pixels per source pixel), PB6..PB7
// de_h (output pixels from the end of hsync to the picture's first pixel),
// PB8..PB9 de_v (lines from the end of vsync to the picture's first line, plus
// one, as MiSTer counts them), PB10..PB13 the picture's width and height in
// source pixels and lines, all little-endian, then a name of up to 14
// characters from PB14. Like MiSTer's, characters 13 and 14 sit past the
// 25-byte length the checksum covers.
#define HSTX_DV1_FLAG_INTERLACED 0x01
#define HSTX_DV1_FLAG_MENU 0x04
#define HSTX_DV1_FLAG_ROTATED 0x08
void hstx_packet_set_spd_dv1_infoframe(hstx_packet_t *packet, uint8_t flags, uint8_t pixel_repetition, uint16_t de_h,
                                       uint16_t de_v, uint16_t width, uint16_t height, const char *name);
int hstx_packet_set_audio_samples(hstx_packet_t *packet, const audio_sample_t *samples, int num_samples,
                                  int frame_count);
// As above, but with a proper IEC 60958 channel status bit sequence
// (consumer L-PCM, 48 kHz) and parity covering the VUC bits. Strict
// receivers (e.g. ones that re-encode audio) may require this.
int hstx_packet_set_audio_samples_cs(hstx_packet_t *packet, const audio_sample_t *samples, int num_samples,
                                     int frame_count);
// Sample-rate-aware form used when the stream is not 48 kHz.
int hstx_packet_set_audio_samples_cs_rate(hstx_packet_t *packet, const audio_sample_t *samples, int num_samples,
                                          int frame_count, uint32_t sample_rate);
// Layout 1 audio sample packet: one 8-channel, 24-bit L-PCM frame, channel
// pairs 1-2, 3-4, 5-6, 7-8 in subpackets 0-3. Carries IEC 60958 consumer
// channel status (L-PCM, given sample rate, 24-bit word length). Returns the
// next frame_count (frame_count + 1, modulo 192).
int hstx_packet_set_audio_sample_8ch(hstx_packet_t *packet, const audio_sample_8ch_t *sample, int frame_count,
                                     uint32_t sample_rate);
void hstx_packet_set_null(hstx_packet_t *packet);

// ============================================================================
// TERC4 encoding for HSTX
// ============================================================================

void hstx_packet_set_sync_positive(bool positive);
bool hstx_packet_get_sync_positive(void);
void hstx_packet_set_sync_polarity(bool hsync_positive, bool vsync_positive);
void hstx_encode_data_island(hstx_data_island_t *out, const hstx_packet_t *packet, bool vsync, bool hsync);
// Encode two packets as one Data Island. Only the island's first pixel has
// TERC4 channel 0 bit 3 clear; the second packet is encoded with it set.
void hstx_encode_data_island2(hstx_data_island2_t *out, const hstx_packet_t *first, const hstx_packet_t *second,
                              bool vsync, bool hsync);
const uint32_t *hstx_get_null_data_island(bool vsync, bool hsync);

#endif // HSTX_PACKET_H
