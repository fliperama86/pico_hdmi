#include "pico_hdmi/video_output.h"

#include "pico_hdmi/hstx_data_island_queue.h"
#include "pico_hdmi/hstx_packet.h"
#include "pico_hdmi/hstx_pins.h"

#include "pico/stdlib.h"

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/structs/bus_ctrl.h"
#include "hardware/structs/clocks.h"
#include "hardware/structs/hstx_ctrl.h"
#include "hardware/structs/hstx_fifo.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#ifndef PICO_HDMI_LINE_BUFFER_IN_SCRATCH_Y
#define PICO_HDMI_LINE_BUFFER_IN_SCRATCH_Y 0
#endif

#if PICO_HDMI_LINE_BUFFER_IN_SCRATCH_Y
#define PICO_HDMI_LINE_BUFFER_ATTR __scratch_y("pico_hdmi_line_buffer")
#else
#define PICO_HDMI_LINE_BUFFER_ATTR
#endif

#ifndef PICO_HDMI_LEGACY_240P_AVI_INFOFRAME
#define PICO_HDMI_LEGACY_240P_AVI_INFOFRAME 0
#endif

// ============================================================================
// DVI/HSTX Constants
// ============================================================================

#define TMDS_CTRL_00 0x354u // vsync=0 hsync=0
#define TMDS_CTRL_01 0x0abu // vsync=0 hsync=1
#define TMDS_CTRL_10 0x154u // vsync=1 hsync=0
#define TMDS_CTRL_11 0x2abu // vsync=1 hsync=1

// Map semantic "V asserted / V inactive" and "H asserted / H inactive" to TMDS bits.
// Negative polarity (default, VIC 1 etc.): asserted = 0 on wire.
// Positive polarity (720p60 VIC 4, etc.): asserted = 1 on wire.
#ifdef MODE_SYNC_POSITIVE
#define _TMDS_VON_HON TMDS_CTRL_11
#define _TMDS_VON_HOFF TMDS_CTRL_10
#define _TMDS_VOFF_HON TMDS_CTRL_01
#define _TMDS_VOFF_HOFF TMDS_CTRL_00
#else
#define _TMDS_VON_HON TMDS_CTRL_00
#define _TMDS_VON_HOFF TMDS_CTRL_01
#define _TMDS_VOFF_HON TMDS_CTRL_10
#define _TMDS_VOFF_HOFF TMDS_CTRL_11
#endif

// Sync symbols: Lane 0 carries sync, Lanes 1&2 are always CTRL_00
// Naming: V0 = vsync asserted, V1 = vsync inactive; H0 = hsync asserted, H1 = hsync inactive
#define SYNC_V0_H0 (_TMDS_VON_HON | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V0_H1 (_TMDS_VON_HOFF | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V1_H0 (_TMDS_VOFF_HON | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V1_H1 (_TMDS_VOFF_HOFF | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))

// Data Island preamble: Lane 0 = sync, Lanes 1&2 = CTRL_01 pattern
// Per HDMI 1.3a Table 5-2: CTL0=1, CTL1=0, CTL2=1, CTL3=0
#define PREAMBLE_V0_H0 (_TMDS_VON_HON | (TMDS_CTRL_01 << 10) | (TMDS_CTRL_01 << 20))
#define PREAMBLE_V1_H0 (_TMDS_VOFF_HON | (TMDS_CTRL_01 << 10) | (TMDS_CTRL_01 << 20))
#define PREAMBLE_V0_H1 (_TMDS_VON_HOFF | (TMDS_CTRL_01 << 10) | (TMDS_CTRL_01 << 20))
#define PREAMBLE_V1_H1 (_TMDS_VOFF_HOFF | (TMDS_CTRL_01 << 10) | (TMDS_CTRL_01 << 20))

// Video preamble: Lane 0 = sync, Lane 1 = CTRL_01, Lane 2 = CTRL_00
// Per HDMI 1.3a Table 5-2: CTL0=1, CTL1=0, CTL2=0, CTL3=0
#define VIDEO_PREAMBLE_V0_H1 (_TMDS_VON_HOFF | (TMDS_CTRL_01 << 10) | (TMDS_CTRL_00 << 20))
#define VIDEO_PREAMBLE_V1_H1 (_TMDS_VOFF_HOFF | (TMDS_CTRL_01 << 10) | (TMDS_CTRL_00 << 20))

// Video guard band: Per HDMI 1.3a Table 5-5
// CH0 = 0b1011001100 (0x2CC), CH1 = 0b0100110011 (0x133), CH2 = 0b1011001100 (0x2CC)
#define VIDEO_GUARD_BAND (0x2CCu | (0x133u << 10) | (0x2CCu << 20))

#define HSTX_CMD_RAW (0x0u << 12)
#define HSTX_CMD_RAW_REPEAT (0x1u << 12)
#define HSTX_CMD_TMDS (0x2u << 12)
#define HSTX_CMD_TMDS_REPEAT (0x3u << 12)
#define HSTX_CMD_NOP (0xfu << 12)

// Data Island placement:
//   - If hsync is wide enough (e.g. 640x480 hsync=96), place DI inside the hsync pulse.
//     This is the original layout — DI is encoded with hsync_active=true.
//   - Otherwise (e.g. 720p60 hsync=40), place DI in the back porch after hsync.
//     DI is encoded with hsync_active=false.
#if MODE_H_SYNC_WIDTH >= (W_PREAMBLE + W_DATA_ISLAND)
#define DI_IN_HSYNC 1
#define SYNC_AFTER_DI (MODE_H_SYNC_WIDTH - W_PREAMBLE - W_DATA_ISLAND)
#else
#define DI_IN_HSYNC 0
#endif

// Video preamble and guard band widths (HDMI 1.3a Section 5.2.2)
#define W_VIDEO_PREAMBLE 8
#define W_VIDEO_GUARD_BAND 2

// ============================================================================
// Audio/Video State
// ============================================================================

uint16_t frame_width = 0;
uint16_t frame_height = 0;
volatile uint32_t video_frame_count = 0;

// DVI mode: when true, disables all HDMI Data Islands (pure DVI output, no audio)
// Some monitors have trouble syncing with HDMI Data Islands
static bool dvi_mode = false; // Default to HDMI mode (full features with audio)

static uint16_t PICO_HDMI_LINE_BUFFER_ATTR line_buffer[MODE_H_ACTIVE_PIXELS] __attribute__((aligned(4)));
static uint32_t v_scanline = 2;
static bool vactive_cmdlist_posted = false;
static bool dma_pong = false;

static video_output_task_fn background_task = NULL;
static video_output_scanline_cb_t scanline_callback = NULL;
static video_output_scanline_ptr_cb_t scanline_pointer_callback = NULL;
static video_output_vsync_cb_t vsync_callback = NULL;
static const uint32_t *active_data_ptr = (const uint32_t *)line_buffer;

#define DMACH_PING 0
#define DMACH_PONG 1

bool video_output_in_vertical_blanking(void)
{
    return v_scanline < (MODE_V_TOTAL_LINES - MODE_V_ACTIVE_LINES);
}

// ============================================================================
// Command Lists
// ============================================================================

// Pure DVI command lists (no Data Islands)
static uint32_t vblank_line_vsync_off[] = {HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH,
                                           SYNC_V1_H1,
                                           HSTX_CMD_NOP,
                                           HSTX_CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH,
                                           SYNC_V1_H0,
                                           HSTX_CMD_NOP,
                                           HSTX_CMD_RAW_REPEAT | (MODE_H_BACK_PORCH + MODE_H_ACTIVE_PIXELS),
                                           SYNC_V1_H1,
                                           HSTX_CMD_NOP};

static uint32_t vblank_line_vsync_on[] = {HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH,
                                          SYNC_V0_H1,
                                          HSTX_CMD_NOP,
                                          HSTX_CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH,
                                          SYNC_V0_H0,
                                          HSTX_CMD_NOP,
                                          HSTX_CMD_RAW_REPEAT | (MODE_H_BACK_PORCH + MODE_H_ACTIVE_PIXELS),
                                          SYNC_V0_H1,
                                          HSTX_CMD_NOP};

// Active video line for DVI mode (no Data Island, just sync + pixels)
static uint32_t vactive_line_dvi[] = {
    HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH, SYNC_V1_H1, HSTX_CMD_NOP,
    HSTX_CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH,  SYNC_V1_H0, HSTX_CMD_NOP,
    HSTX_CMD_RAW_REPEAT | MODE_H_BACK_PORCH,  SYNC_V1_H1, HSTX_CMD_TMDS | MODE_H_ACTIVE_PIXELS};

static uint32_t vactive_di_ping[128], vactive_di_pong[128], vactive_di_null[128];
static uint32_t vblank_di_ping[128], vblank_di_pong[128], vblank_di_null[128];
static uint32_t vblank_di_len, vblank_di_null_len;
static uint32_t vactive_di_len, vactive_di_null_len;

#ifndef PICO_HDMI_PRECOMPOSED_ACTIVE_LINES
#define PICO_HDMI_PRECOMPOSED_ACTIVE_LINES 0
#endif

#if PICO_HDMI_PRECOMPOSED_ACTIVE_LINES
static uint32_t build_line_with_di(uint32_t *buf, const uint32_t *di_words, bool vsync, bool active);

// Pre-composed active-line headers. The header content is STATIC (sync +
// preamble + control + video preamble); only the 36 island words change per
// line. Headers are built ONCE (first compose_service call); per line, the
// scanline ISR pops a pre-ENCODED island from the di queue and patches it
// into the entry it is about to post (~36 word copies, <1.5 us). This keeps
// the audio schedule inside the ISR, so it is immune to ANY background-task
// stall shorter than the di queue's cushion (~1 frame at 200 packets).
//
// The previous design composed entries ahead of the beam in the background
// task: every per-frame background section longer than the ring's 2.5 ms
// lead (frame init, RGB565 rebuild tail, overlay redraw, a 7 ms UART
// printf) staled entries and dropped their already-dequeued audio packets
// -- measured as +-60 Hz sidebands all over a pure sine (ST grew ~7
// lines/frame). Do not resurrect ahead-of-beam composition for audio.
static video_output_precomposed_line_t *compose_ring;
static uint32_t compose_ring_entries;
static bool compose_ring_built;
static uint32_t active_line_global; // counted by the ISR (Core 1 only)
// Offset of the 36 island words inside a built active line: front porch
// (3 words) + island preamble (3) + the RAW command word (1).
#define PRECOMPOSED_DI_OFFSET 7
// Cached null-island payload for lines with no audio packet due.
static const uint32_t *precomposed_null_di;
// Length of the once-built blanking templates (vblank_di_ping/pong).
static uint32_t precomposed_vblank_len;
// Active lines posted via the static fallback because the headers were not
// built yet (startup only). Must not grow while running.
volatile uint32_t video_output_precomposed_stale_count;

// 16-bit active-data transfers: the pointer callback returns the native
// (half-width) line; the bus replicates each half-word across the 32-bit
// FIFO write and the HSTX expander doubles it. Per-post ctrl writes swap the
// transfer size between header (32-bit) and pixel data (16-bit) posts.
static bool native_pixel_mode;
static uint32_t dma_ctrl32[2], dma_ctrl16[2];

void video_output_set_native_pixel_mode(bool enabled)
{
    native_pixel_mode = enabled;
}

void video_output_set_compose_ring(video_output_precomposed_line_t *ring, uint32_t entries)
{
    active_line_global = 0;
    compose_ring_entries = entries;
    compose_ring_built = false;
    __compiler_memory_barrier();
    compose_ring = ring;
}

void video_output_compose_service(void)
{
    if (!compose_ring || compose_ring_built || dvi_mode) {
        return;
    }
    // One-time header build (requires the null island, i.e. audio config
    // done -- this runs from the background task, after video_output init).
    const uint32_t *null_di = hstx_get_null_data_island(false, DI_HSYNC_ACTIVE);
    for (uint32_t i = 0; i < compose_ring_entries; i++) {
        compose_ring[i].len = (uint16_t)build_line_with_di(compose_ring[i].buf, null_di, false, true);
        compose_ring[i].tag = i;
    }
    // Blanking templates for the ISR's island patching (non-vsync lines).
    precomposed_vblank_len = build_line_with_di(vblank_di_ping, null_di, false, false);
    (void)build_line_with_di(vblank_di_pong, null_di, false, false);
    precomposed_null_di = null_di;
    __compiler_memory_barrier();
    compose_ring_built = true;
}
#endif // PICO_HDMI_PRECOMPOSED_ACTIVE_LINES

static uint32_t vblank_acr_vsync_on[64], vblank_acr_vsync_on_len;
static uint32_t vblank_acr_vsync_off[64], vblank_acr_vsync_off_len;
static uint32_t vblank_infoframe_vsync_on[64], vblank_infoframe_vsync_on_len;
static uint32_t vblank_infoframe_vsync_off[64], vblank_infoframe_vsync_off_len;
static uint32_t vblank_avi_infoframe[64], vblank_avi_infoframe_len;

#if defined(PICO_HDMI_DUMP_COMMAND_LISTS) && PICO_HDMI_DUMP_COMMAND_LISTS
static void dump_command_list(const char *name, const uint32_t *words, uint32_t len)
{
    printf("pico_hdmi %s len=%lu", name, (unsigned long)len);
    for (uint32_t i = 0; i < len; ++i) {
        printf(" %08lx", (unsigned long)words[i]);
    }
    printf("\n");
}

static void dump_static_command_lists(void)
{
    printf("pico_hdmi mode h=%u,%u,%u,%u v=%u,%u,%u,%u hstx_clk_div=%u csr_clkdiv=%u di_in_hsync=%u\n",
           MODE_H_FRONT_PORCH, MODE_H_SYNC_WIDTH, MODE_H_BACK_PORCH, MODE_H_ACTIVE_PIXELS, MODE_V_FRONT_PORCH,
           MODE_V_SYNC_WIDTH, MODE_V_BACK_PORCH, MODE_V_ACTIVE_LINES, MODE_HSTX_CLK_DIV, MODE_HSTX_CSR_CLKDIV,
           DI_IN_HSYNC);
    dump_command_list("vblank_line_vsync_off", vblank_line_vsync_off, count_of(vblank_line_vsync_off));
    dump_command_list("vblank_line_vsync_on", vblank_line_vsync_on, count_of(vblank_line_vsync_on));
    dump_command_list("vactive_line_dvi", vactive_line_dvi, count_of(vactive_line_dvi));
}
#endif

// ============================================================================
// HSTX Resync - Reset output to sync with input VSYNC
// ============================================================================

static void __scratch_x("") hstx_resync(void)
{
    // 1. Abort DMA chains
    dma_channel_abort(DMACH_PING);
    dma_channel_abort(DMACH_PONG);

    // 2. Disable HSTX (resets shift register, clock generator, and flushes FIFO)
    hstx_ctrl_hw->csr &= ~HSTX_CTRL_CSR_EN_BITS;

    // Small delay to ensure HSTX fully stops
    __asm volatile("nop\nnop\nnop\nnop");

    // 3. Reset state to start of frame
    v_scanline = 0;
    vactive_cmdlist_posted = false;
    dma_pong = false;

    // 4. Clear any pending DMA interrupts
    dma_hw->ints0 = (1U << DMACH_PING) | (1U << DMACH_PONG);

    // 5. Configure DMA PING to start from beginning of frame (Line 0)
    dma_channel_hw_t *ch_ping = &dma_hw->ch[DMACH_PING];
#if PICO_HDMI_PRECOMPOSED_ACTIVE_LINES
    if (native_pixel_mode) {
        // A resync can interrupt mid-line; restore 32-bit command transfers.
        dma_hw->ch[DMACH_PING].al1_ctrl = dma_ctrl32[DMACH_PING];
        dma_hw->ch[DMACH_PONG].al1_ctrl = dma_ctrl32[DMACH_PONG];
    }
#endif
    ch_ping->read_addr = (uintptr_t)vblank_line_vsync_off;
    ch_ping->transfer_count = count_of(vblank_line_vsync_off);

    // 6. Configure DMA PONG for the NEXT line (Line 1)
    // This ensures that when PING finishes and chains to PONG, PONG is ready.
    dma_channel_hw_t *ch_pong = &dma_hw->ch[DMACH_PONG];
    ch_pong->read_addr = (uintptr_t)vblank_line_vsync_off; // Line 1 is also blank
    ch_pong->transfer_count = count_of(vblank_line_vsync_off);

    // 7. Re-enable HSTX then start DMA
    hstx_ctrl_hw->csr |= HSTX_CTRL_CSR_EN_BITS;
    dma_channel_start(DMACH_PING);
}

// ============================================================================
// Internal Helpers
// ============================================================================

static uint32_t build_line_with_di(uint32_t *buf, const uint32_t *di_words, bool vsync, bool active)
{
    uint32_t *p = buf;
    uint32_t sync_h0 = vsync ? SYNC_V0_H0 : SYNC_V1_H0;
    uint32_t sync_h1 = vsync ? SYNC_V0_H1 : SYNC_V1_H1;

#if DI_IN_HSYNC
    // DI inside the hsync pulse (original layout, wide-hsync modes)
    uint32_t preamble = vsync ? PREAMBLE_V0_H0 : PREAMBLE_V1_H0;

    *p++ = HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH;
    *p++ = sync_h1;
    *p++ = HSTX_CMD_NOP;

    *p++ = HSTX_CMD_RAW_REPEAT | W_PREAMBLE;
    *p++ = preamble;
    *p++ = HSTX_CMD_NOP;

    *p++ = HSTX_CMD_RAW | W_DATA_ISLAND;
    for (int i = 0; i < W_DATA_ISLAND; i++)
        *p++ = di_words[i];
    *p++ = HSTX_CMD_NOP;

    *p++ = HSTX_CMD_RAW_REPEAT | SYNC_AFTER_DI;
    *p++ = sync_h0;
    *p++ = HSTX_CMD_NOP;

    if (active) {
        uint32_t video_preamble = vsync ? VIDEO_PREAMBLE_V0_H1 : VIDEO_PREAMBLE_V1_H1;

        *p++ = HSTX_CMD_RAW_REPEAT | (MODE_H_BACK_PORCH - W_VIDEO_PREAMBLE - W_VIDEO_GUARD_BAND);
        *p++ = sync_h1;
        *p++ = HSTX_CMD_NOP;

        *p++ = HSTX_CMD_RAW_REPEAT | W_VIDEO_PREAMBLE;
        *p++ = video_preamble;
        *p++ = HSTX_CMD_NOP;

        *p++ = HSTX_CMD_RAW_REPEAT | W_VIDEO_GUARD_BAND;
        *p++ = VIDEO_GUARD_BAND;

        *p++ = HSTX_CMD_TMDS | MODE_H_ACTIVE_PIXELS;
    } else {
        *p++ = HSTX_CMD_RAW_REPEAT | (MODE_H_BACK_PORCH + MODE_H_ACTIVE_PIXELS);
        *p++ = sync_h1;
        *p++ = HSTX_CMD_NOP;
    }
#else
    // DI in back porch, after hsync pulse (narrow-hsync modes like 720p60)
    uint32_t preamble = vsync ? PREAMBLE_V0_H1 : PREAMBLE_V1_H1;

    // Front porch
    *p++ = HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH;
    *p++ = sync_h1;
    *p++ = HSTX_CMD_NOP;

    // HSync pulse (clean, no embedded DI)
    *p++ = HSTX_CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH;
    *p++ = sync_h0;
    *p++ = HSTX_CMD_NOP;

    // DI preamble (H inactive, lanes 1&2 = CTRL_01)
    *p++ = HSTX_CMD_RAW_REPEAT | W_PREAMBLE;
    *p++ = preamble;
    *p++ = HSTX_CMD_NOP;

    // DI packet
    *p++ = HSTX_CMD_RAW | W_DATA_ISLAND;
    for (int i = 0; i < W_DATA_ISLAND; i++)
        *p++ = di_words[i];
    *p++ = HSTX_CMD_NOP;

    if (active) {
        uint32_t video_preamble = vsync ? VIDEO_PREAMBLE_V0_H1 : VIDEO_PREAMBLE_V1_H1;

        // Remainder of back porch before video preamble
        *p++ = HSTX_CMD_RAW_REPEAT |
               (MODE_H_BACK_PORCH - W_PREAMBLE - W_DATA_ISLAND - W_VIDEO_PREAMBLE - W_VIDEO_GUARD_BAND);
        *p++ = sync_h1;
        *p++ = HSTX_CMD_NOP;

        *p++ = HSTX_CMD_RAW_REPEAT | W_VIDEO_PREAMBLE;
        *p++ = video_preamble;
        *p++ = HSTX_CMD_NOP;

        *p++ = HSTX_CMD_RAW_REPEAT | W_VIDEO_GUARD_BAND;
        *p++ = VIDEO_GUARD_BAND;

        *p++ = HSTX_CMD_TMDS | MODE_H_ACTIVE_PIXELS;
    } else {
        // Remainder of back porch + full active area as control period
        *p++ = HSTX_CMD_RAW_REPEAT | (MODE_H_BACK_PORCH - W_PREAMBLE - W_DATA_ISLAND + MODE_H_ACTIVE_PIXELS);
        *p++ = sync_h1;
        *p++ = HSTX_CMD_NOP;
    }
#endif
    return (uint32_t)(p - buf);
}

typedef struct {
    bool vsync_active;
    bool front_porch;
    bool back_porch;
    bool active_video;
    bool send_acr;
    uint32_t active_line;
} scanline_state_t;

static inline void __scratch_x("") get_scanline_state(uint32_t v_scanline, scanline_state_t *state)
{
    state->vsync_active = (v_scanline >= MODE_V_FRONT_PORCH && v_scanline < (MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH));
    state->front_porch = (v_scanline < MODE_V_FRONT_PORCH);
    state->back_porch = (v_scanline >= MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH &&
                         v_scanline < MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH + MODE_V_BACK_PORCH);
    state->active_video = (!state->vsync_active && !state->front_porch && !state->back_porch);

    state->send_acr = (v_scanline >= (MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH) &&
                       v_scanline < (MODE_V_TOTAL_LINES - MODE_V_ACTIVE_LINES) && (v_scanline % 4 == 0));

    if (state->active_video) {
        state->active_line = v_scanline - (MODE_V_TOTAL_LINES - MODE_V_ACTIVE_LINES);
    } else {
        state->active_line = 0;
    }
}

static inline void __scratch_x("") video_output_handle_vsync(dma_channel_hw_t *ch, uint32_t v_scanline)
{
    if (dvi_mode) {
        // Pure DVI: simple vsync line without Data Islands
        ch->read_addr = (uintptr_t)vblank_line_vsync_on;
        ch->transfer_count = count_of(vblank_line_vsync_on);
        if (v_scanline == MODE_V_FRONT_PORCH) {
            video_frame_count++;
            if (vsync_callback)
                vsync_callback();
        }
    } else {
        if (v_scanline == MODE_V_FRONT_PORCH) {
            ch->read_addr = (uintptr_t)vblank_acr_vsync_on;
            ch->transfer_count = vblank_acr_vsync_on_len;
            video_frame_count++;
            if (vsync_callback)
                vsync_callback();
        } else {
            ch->read_addr = (uintptr_t)vblank_infoframe_vsync_on;
            ch->transfer_count = vblank_infoframe_vsync_on_len;
        }
    }
}

static inline void __scratch_x("")
    video_output_handle_active_start(dma_channel_hw_t *ch, uint32_t v_scanline, uint32_t active_line, bool dma_pong)
{
    uint32_t *dst32 = (uint32_t *)line_buffer;

    if (scanline_pointer_callback) {
        const uint32_t *scanline = scanline_pointer_callback(v_scanline, active_line);
        active_data_ptr = scanline ? scanline : (const uint32_t *)line_buffer;
    } else if (scanline_callback) {
        scanline_callback(v_scanline, active_line, dst32);
        active_data_ptr = (const uint32_t *)line_buffer;
    } else {
        // If no callback, just output black pixels
        for (uint32_t i = 0; i < MODE_H_ACTIVE_PIXELS / 2; i++) {
            dst32[i] = 0;
        }
        active_data_ptr = (const uint32_t *)line_buffer;
    }

    if (dvi_mode) {
        // Pure DVI: simple active line without Data Islands
        ch->read_addr = (uintptr_t)vactive_line_dvi;
        ch->transfer_count = count_of(vactive_line_dvi);
    } else {
#if PICO_HDMI_PRECOMPOSED_ACTIVE_LINES
        if (compose_ring_built) {
            uint32_t g = active_line_global++;
            video_output_precomposed_line_t *e = &compose_ring[g % compose_ring_entries];
            // Pop a pre-encoded island (or null/silence) and patch it into
            // the static header about to be posted. The schedule lives in
            // this ISR, so the background task can never starve it.
            const uint32_t *di = hstx_di_queue_get_audio_packet();
            if (!di) {
                di = precomposed_null_di;
            }
            uint32_t *dst = &e->buf[PRECOMPOSED_DI_OFFSET];
            for (int i = 0; i < W_DATA_ISLAND; i++) {
                dst[i] = di[i];
            }
            ch->read_addr = (uintptr_t)e->buf;
            ch->transfer_count = e->len;
        } else {
            ch->read_addr = (uintptr_t)vactive_di_null;
            ch->transfer_count = vactive_di_null_len;
            video_output_precomposed_stale_count++;
        }
#else
        uint32_t *buf = dma_pong ? vactive_di_ping : vactive_di_pong;
        const uint32_t *di_words = hstx_di_queue_get_audio_packet();
        if (di_words) {
            vactive_di_len = build_line_with_di(buf, di_words, false, true);
            ch->read_addr = (uintptr_t)buf;
            ch->transfer_count = vactive_di_len;
        } else {
            ch->read_addr = (uintptr_t)vactive_di_null;
            ch->transfer_count = vactive_di_null_len;
        }
#endif
    }
}

static inline void __scratch_x("")
    video_output_handle_blanking(dma_channel_hw_t *ch, uint32_t v_scanline, bool send_acr, bool dma_pong)
{
    if (dvi_mode) {
        // Pure DVI: simple blanking line without Data Islands
        (void)send_acr;
        (void)dma_pong;
        (void)v_scanline;
        ch->read_addr = (uintptr_t)vblank_line_vsync_off;
        ch->transfer_count = count_of(vblank_line_vsync_off);
    } else {
        if (send_acr) {
            ch->read_addr = (uintptr_t)vblank_acr_vsync_off;
            ch->transfer_count = vblank_acr_vsync_off_len;
        } else if (v_scanline == 0) {
            ch->read_addr = (uintptr_t)vblank_avi_infoframe;
            ch->transfer_count = vblank_avi_infoframe_len;
        } else {
#if PICO_HDMI_PRECOMPOSED_ACTIVE_LINES
            // Blanking lines carry their share of the audio schedule too
            // (see the tick comment in dma_irq_handler): patch the island
            // into a static ping/pong blanking template, same trick as the
            // active lines.
            const uint32_t *di = compose_ring_built ? hstx_di_queue_get_audio_packet() : NULL;
            if (di) {
                uint32_t *buf = dma_pong ? vblank_di_ping : vblank_di_pong;
                uint32_t *dst = &buf[PRECOMPOSED_DI_OFFSET];
                for (int i = 0; i < W_DATA_ISLAND; i++) {
                    dst[i] = di[i];
                }
                ch->read_addr = (uintptr_t)buf;
                ch->transfer_count = precomposed_vblank_len;
            } else {
                ch->read_addr = (uintptr_t)vblank_di_null;
                ch->transfer_count = vblank_di_null_len;
            }
#else
            const uint32_t *di_words = hstx_di_queue_get_audio_packet();
            if (di_words) {
                uint32_t *buf = dma_pong ? vblank_di_ping : vblank_di_pong;
                vblank_di_len = build_line_with_di(buf, di_words, false, false);
                ch->read_addr = (uintptr_t)buf;
                ch->transfer_count = vblank_di_len;
            } else {
                ch->read_addr = (uintptr_t)vblank_di_null;
                ch->transfer_count = vblank_di_null_len;
            }
#endif
        }
    }
}

static inline void __scratch_x("") video_output_handle_active_data(dma_channel_hw_t *ch)
{
    ch->read_addr = (uintptr_t)active_data_ptr;
    // 32-bit mode: words of pre-doubled pixels. Native 16-bit mode: half-word
    // transfers of native pixels (bus-replicated) -- same transfer count.
    ch->transfer_count = (MODE_H_ACTIVE_PIXELS * sizeof(uint16_t)) / sizeof(uint32_t);
}

// ============================================================================
// DMA IRQ Handler
// ============================================================================

static void __scratch_x("") dma_irq_handler()
{
    uint32_t ch_num = dma_pong ? DMACH_PONG : DMACH_PING;
    dma_channel_hw_t *ch = &dma_hw->ch[ch_num];
    dma_hw->intr = 1U << ch_num;
    dma_pong = !dma_pong;

    // Advance audio/data-island scheduler exactly once per scanline (HDMI
    // mode only). All 525 lines tick: audio packets must be spread across
    // the whole frame INCLUDING blanking -- delivering only on active lines
    // leaves a 45-line gap every frame, i.e. 60 Hz delivery modulation that
    // sinks audibly resample around (+-60 Hz sidebands on a pure tone).
    if (!dvi_mode && !vactive_cmdlist_posted) {
        hstx_di_queue_tick();
    }

    scanline_state_t state;
    get_scanline_state(v_scanline, &state);

    bool posting_active_data = state.active_video && vactive_cmdlist_posted;
#if PICO_HDMI_PRECOMPOSED_ACTIVE_LINES
    if (native_pixel_mode) {
        ch->al1_ctrl = posting_active_data ? dma_ctrl16[ch_num] : dma_ctrl32[ch_num];
    }
#endif

    if (state.vsync_active) {
        video_output_handle_vsync(ch, v_scanline);
    } else if (state.active_video && !vactive_cmdlist_posted) {
        video_output_handle_active_start(ch, v_scanline, state.active_line, dma_pong);
        vactive_cmdlist_posted = true;
    } else if (posting_active_data) {
        video_output_handle_active_data(ch);
        vactive_cmdlist_posted = false;
    } else {
        video_output_handle_blanking(ch, v_scanline, state.send_acr, dma_pong);
    }
    if (!vactive_cmdlist_posted)
        v_scanline = (v_scanline + 1) % MODE_V_TOTAL_LINES;
}

// ============================================================================
// Public Interface
// ============================================================================

// ACR N values from HDMI spec Table 7-1; CTS computed from actual pixel clock.
// Formula: f_audio = f_TMDS * N / (128 * CTS)  =>  CTS = f_TMDS * N / (128 * f_audio)
static uint32_t get_acr_n(uint32_t sample_rate)
{
    switch (sample_rate) {
        case 32000:
            return 4096;
        case 44100:
            return 6272;
        case 48000:
            return 6144;
        case 88200:
            return 12544;
        case 96000:
            return 12288;
        case 176400:
            return 25088;
        case 192000:
            return 24576;
        default:
            return 6144; // fallback to 48kHz
    }
}

static void get_acr_params(uint32_t sample_rate, uint32_t *n, uint32_t *cts)
{
    *n = get_acr_n(sample_rate);
    uint32_t pixel_clock = clock_get_hz(clk_hstx) / MODE_HSTX_CSR_CLKDIV;
    *cts = (uint32_t)(((uint64_t)pixel_clock * *n) / (128ULL * sample_rate));
}

static void configure_audio_packets(uint32_t sample_rate)
{
    hstx_di_queue_set_sample_rate(sample_rate);

    // Derive the audio packet cadence from the actual pixel clock instead of
    // assuming exactly 60.000 Hz. 720p currently runs from 372 MHz sys_clk
    // (74.4 MHz pixel clock), so frame rate is slightly above nominal CEA 60 Hz.
    // ACR still advertises 48 kHz; if we also emit 800 samples every video
    // frame, stricter sinks eventually overflow their audio clock domain.
    uint32_t pixel_clock_hz = clock_get_hz(clk_hstx) / MODE_HSTX_CSR_CLKDIV;
    uint32_t spl_fp = (uint32_t)(((uint64_t)sample_rate * MODE_H_TOTAL_PIXELS << 16) / pixel_clock_hz);
    hstx_di_queue_set_samples_per_line_fp(spl_fp);

    hstx_packet_t packet;
    hstx_data_island_t island;

    uint32_t acr_n;
    uint32_t acr_cts;
    get_acr_params(sample_rate, &acr_n, &acr_cts);
    hstx_packet_set_acr(&packet, acr_n, acr_cts);
    hstx_encode_data_island(&island, &packet, true, DI_HSYNC_ACTIVE);
    vblank_acr_vsync_on_len = build_line_with_di(vblank_acr_vsync_on, island.words, true, false);
    hstx_encode_data_island(&island, &packet, false, DI_HSYNC_ACTIVE);
    vblank_acr_vsync_off_len = build_line_with_di(vblank_acr_vsync_off, island.words, false, false);

    hstx_packet_set_audio_infoframe(&packet, sample_rate, 2, 16);
    hstx_encode_data_island(&island, &packet, true, DI_HSYNC_ACTIVE);
    vblank_infoframe_vsync_on_len = build_line_with_di(vblank_infoframe_vsync_on, island.words, true, false);
    hstx_encode_data_island(&island, &packet, false, DI_HSYNC_ACTIVE);
    vblank_infoframe_vsync_off_len = build_line_with_di(vblank_infoframe_vsync_off, island.words, false, false);
}

void video_output_init(uint16_t width, uint16_t height)
{
    frame_width = width;
    frame_height = height;

#if defined(PICO_HDMI_DUMP_COMMAND_LISTS) && PICO_HDMI_DUMP_COMMAND_LISTS
    dump_static_command_lists();
#endif

    // Configure clk_hstx for the current video mode
    // After set_sys_clock_khz(), clk_hstx needs to be reconfigured
    uint32_t sys_freq = clock_get_hz(clk_sys);

    clock_configure_int_divider(clk_hstx,
                                0, // No glitchless mux
                                CLOCKS_CLK_HSTX_CTRL_AUXSRC_VALUE_CLK_SYS, sys_freq, MODE_HSTX_CLK_DIV);

    // Claim DMA channels for HSTX (channels 0 and 1)
    dma_channel_claim(DMACH_PING);
    dma_channel_claim(DMACH_PONG);

    // Set v_total for audio packet timing
    hstx_di_queue_set_v_total(MODE_V_TOTAL_LINES);

    // Initialize HDMI audio packets (default 48kHz)
    configure_audio_packets(48000);

    hstx_packet_t packet;
    hstx_data_island_t island;

    // VIC=1 for 640x480, VIC=4 for 720p60, VIC=0 for non-standard timings (e.g. 240p)
    uint8_t vic = (height == 480) ? 1 : (height == 720) ? 4 : 0;
#if defined(VIDEO_MODE_320x240) && !PICO_HDMI_LEGACY_240P_AVI_INFOFRAME
    uint8_t pixel_repetition = 3;
#else
    uint8_t pixel_repetition = 0;
#endif
    hstx_packet_set_avi_infoframe(&packet, vic, pixel_repetition);
    hstx_encode_data_island(&island, &packet, false, DI_HSYNC_ACTIVE);
    vblank_avi_infoframe_len = build_line_with_di(vblank_avi_infoframe, island.words, false, false);

    vblank_di_null_len =
        build_line_with_di(vblank_di_null, hstx_get_null_data_island(false, DI_HSYNC_ACTIVE), false, false);
    vactive_di_null_len =
        build_line_with_di(vactive_di_null, hstx_get_null_data_island(false, DI_HSYNC_ACTIVE), false, true);

    vblank_di_len = build_line_with_di(vblank_di_ping, hstx_get_null_data_island(false, DI_HSYNC_ACTIVE), false, false);
    memcpy(vblank_di_pong, vblank_di_ping, sizeof(vblank_di_ping));
}

void video_output_set_background_task(video_output_task_fn task)
{
    background_task = task;
}

bool video_output_get_dvi_mode(void)
{
    return dvi_mode;
}

void video_output_set_dvi_mode(bool enabled)
{
    dvi_mode = enabled;
}

void video_output_set_scanline_callback(video_output_scanline_cb_t cb)
{
    scanline_callback = cb;
}

void video_output_set_scanline_pointer_callback(video_output_scanline_ptr_cb_t cb)
{
    scanline_pointer_callback = cb;
}

void video_output_set_vsync_callback(video_output_vsync_cb_t cb)
{
    vsync_callback = cb;
}

void video_output_core1_run(void)
{
    // HSTX Hardware Setup
    hstx_ctrl_hw->expand_tmds = 4 << HSTX_CTRL_EXPAND_TMDS_L2_NBITS_LSB | 8 << HSTX_CTRL_EXPAND_TMDS_L2_ROT_LSB |
                                5 << HSTX_CTRL_EXPAND_TMDS_L1_NBITS_LSB | 3 << HSTX_CTRL_EXPAND_TMDS_L1_ROT_LSB |
                                4 << HSTX_CTRL_EXPAND_TMDS_L0_NBITS_LSB | 29 << HSTX_CTRL_EXPAND_TMDS_L0_ROT_LSB;

    hstx_ctrl_hw->expand_shift =
        2 << HSTX_CTRL_EXPAND_SHIFT_ENC_N_SHIFTS_LSB | 16 << HSTX_CTRL_EXPAND_SHIFT_ENC_SHIFT_LSB |
        1 << HSTX_CTRL_EXPAND_SHIFT_RAW_N_SHIFTS_LSB | 0 << HSTX_CTRL_EXPAND_SHIFT_RAW_SHIFT_LSB;

    hstx_ctrl_hw->csr = 0;
    hstx_ctrl_hw->csr = HSTX_CTRL_CSR_EXPAND_EN_BITS | (uint32_t)MODE_HSTX_CSR_CLKDIV << HSTX_CTRL_CSR_CLKDIV_LSB |
                        5U << HSTX_CTRL_CSR_N_SHIFTS_LSB | 2U << HSTX_CTRL_CSR_SHIFT_LSB | HSTX_CTRL_CSR_EN_BITS;

    hstx_ctrl_hw->bit[0] = HSTX_CTRL_BIT0_CLK_BITS | HSTX_CTRL_BIT0_INV_BITS;
    hstx_ctrl_hw->bit[1] = HSTX_CTRL_BIT0_CLK_BITS;
    for (uint lane = 0; lane < 3; ++lane) {
        int bit = 2 + (lane * 2);
        uint32_t lane_data_sel_bits = (lane * 10) << HSTX_CTRL_BIT0_SEL_P_LSB | ((lane * 10) + 1)
                                                                                    << HSTX_CTRL_BIT0_SEL_N_LSB;
        hstx_ctrl_hw->bit[bit] = lane_data_sel_bits | HSTX_CTRL_BIT0_INV_BITS;
        hstx_ctrl_hw->bit[bit + 1] = lane_data_sel_bits;
    }

    // Set GPIO 12-19 to HSTX function (function 0 on RP2350)
    for (int i = PIN_HSTX_CLK; i <= PIN_HSTX_D2 + 1; ++i) {
        gpio_set_function(i, 0);
        gpio_set_slew_rate(i, GPIO_SLEW_RATE_FAST);
        gpio_set_drive_strength(i, GPIO_DRIVE_STRENGTH_12MA);
    }

    // DMA Setup
    dma_channel_config c = dma_channel_get_default_config(DMACH_PING);
    channel_config_set_chain_to(&c, DMACH_PONG);
    channel_config_set_dreq(&c, DREQ_HSTX);
    dma_channel_configure(DMACH_PING, &c, &hstx_fifo_hw->fifo, vblank_line_vsync_off, count_of(vblank_line_vsync_off),
                          false);

    c = dma_channel_get_default_config(DMACH_PONG);
    channel_config_set_chain_to(&c, DMACH_PING);
    channel_config_set_dreq(&c, DREQ_HSTX);
    dma_channel_configure(DMACH_PONG, &c, &hstx_fifo_hw->fifo, vblank_line_vsync_off, count_of(vblank_line_vsync_off),
                          false);

#if PICO_HDMI_PRECOMPOSED_ACTIVE_LINES
    for (int i = 0; i < 2; i++) {
        uint32_t ch_num = (i == 0) ? DMACH_PING : DMACH_PONG;
        uint32_t ctrl = dma_hw->ch[ch_num].al1_ctrl;
        dma_ctrl32[ch_num] = ctrl;
        dma_ctrl16[ch_num] =
            (ctrl & ~DMA_CH0_CTRL_TRIG_DATA_SIZE_BITS) | ((uint32_t)DMA_SIZE_16 << DMA_CH0_CTRL_TRIG_DATA_SIZE_LSB);
    }
#endif

    dma_hw->ints0 = (1U << DMACH_PING) | (1U << DMACH_PONG);
    dma_hw->inte0 = (1U << DMACH_PING) | (1U << DMACH_PONG);
    irq_set_exclusive_handler(DMA_IRQ_0, dma_irq_handler);
    irq_set_priority(DMA_IRQ_0, 0);
    irq_set_enabled(DMA_IRQ_0, true);

    bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_W_BITS | BUSCTRL_BUS_PRIORITY_DMA_R_BITS;
    dma_channel_start(DMACH_PING);

    while (1) {
        if (background_task) {
            background_task();
        }
        tight_loop_contents();
    }
}

void pico_hdmi_set_audio_sample_rate(uint32_t sample_rate)
{
    configure_audio_packets(sample_rate);
}

volatile uint32_t video_output_resync_count;

void video_output_force_resync(void)
{
    // Full HSTX + DMA restart from frame start. Recovers from a desynced
    // HSTX command stream (one corrupted/mis-sized command word makes the
    // expander misinterpret everything after it, permanently -- symptom:
    // sink loses lock while scanlines "complete" at bus speed because the
    // FIFO no longer back-pressures). Safe to call from Core 1 thread
    // context; the scanline ISR is held off during the reset.
    irq_set_enabled(DMA_IRQ_0, false);
    hstx_resync();
    irq_set_enabled(DMA_IRQ_0, true);
    video_output_resync_count++;
}
