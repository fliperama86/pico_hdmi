#include "pico_hdmi/video_output_rt.h"

#include "pico_hdmi/hstx_data_island_queue.h"
#include "pico_hdmi/hstx_packet.h"
#include "pico_hdmi/hstx_pins.h"
#include "pico_hdmi/video_output.h" // for default sync polarity and DI placement

#include "pico/stdlib.h"

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/structs/bus_ctrl.h"
#include "hardware/structs/clocks.h"
#include "hardware/structs/hstx_ctrl.h"
#include "hardware/structs/hstx_fifo.h"
#include "hardware/sync.h"
#include "hardware/timer.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#ifndef PICO_HDMI_RT_RUNTIME_MODE_ATTRS
#define PICO_HDMI_RT_RUNTIME_MODE_ATTRS 0
#endif

#ifndef PICO_HDMI_LINE_BUFFER_IN_SCRATCH_Y
#define PICO_HDMI_LINE_BUFFER_IN_SCRATCH_Y 0
#endif

#if PICO_HDMI_LINE_BUFFER_IN_SCRATCH_Y
#define PICO_HDMI_LINE_BUFFER_ATTR __scratch_y("pico_hdmi_line_buffer")
#else
#define PICO_HDMI_LINE_BUFFER_ATTR
#endif

// With precomposed active lines, the per-line DI builders are only invoked
// from the background task (one-time header builds), so they need not --
// and must not, for scratch-budget reasons -- occupy scratch sections.
#if PICO_HDMI_PRECOMPOSED_ACTIVE_LINES
#define DI_BUILDER_SECTION
#define DI_BUILDER_SECTION_Y
#else
#define DI_BUILDER_SECTION __scratch_x("")
#define DI_BUILDER_SECTION_Y __scratch_y("")
#endif

#ifndef PICO_HDMI_LEGACY_240P_AVI_INFOFRAME
#define PICO_HDMI_LEGACY_240P_AVI_INFOFRAME 0
#endif

// HSTX clock divider for the 480p mode descriptor. Default 1 (stock 126 MHz
// sys_clk -> 25.2 MHz pixel). A consumer overclocking 480p (e.g. 252 MHz for
// scanline-IRQ headroom) sets this to 2 to keep the pixel clock unchanged.
#ifndef PICO_HDMI_480P_HSTX_CLK_DIV
#define PICO_HDMI_480P_HSTX_CLK_DIV 1
#endif

#ifndef PICO_HDMI_ALIGN_DI_BUFFERS
#define PICO_HDMI_ALIGN_DI_BUFFERS 0
#endif

#if PICO_HDMI_ALIGN_DI_BUFFERS
#define HSTX_CMDLIST_ATTR __attribute__((aligned(16)))
#else
#define HSTX_CMDLIST_ATTR
#endif

// ============================================================================
// DVI/HSTX Constants
// ============================================================================

#define TMDS_CTRL_00 0x354u // vsync=0 hsync=0
#define TMDS_CTRL_01 0x0abu // vsync=0 hsync=1
#define TMDS_CTRL_10 0x154u // vsync=1 hsync=0
#define TMDS_CTRL_11 0x2abu // vsync=1 hsync=1

#define TMDS_SYNC_WORD(lane0) ((lane0) | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define TMDS_DI_PREAMBLE_WORD(lane0) ((lane0) | (TMDS_CTRL_01 << 10) | (TMDS_CTRL_01 << 20))
#define TMDS_VIDEO_PREAMBLE_WORD(lane0) ((lane0) | (TMDS_CTRL_01 << 10) | (TMDS_CTRL_00 << 20))

#define SYNC_NEG_V0_H0 TMDS_SYNC_WORD(TMDS_CTRL_00)
#define SYNC_NEG_V0_H1 TMDS_SYNC_WORD(TMDS_CTRL_01)
#define SYNC_NEG_V1_H0 TMDS_SYNC_WORD(TMDS_CTRL_10)
#define SYNC_NEG_V1_H1 TMDS_SYNC_WORD(TMDS_CTRL_11)
#define PREAMBLE_NEG_V0_H0 TMDS_DI_PREAMBLE_WORD(TMDS_CTRL_00)
#define PREAMBLE_NEG_V1_H0 TMDS_DI_PREAMBLE_WORD(TMDS_CTRL_10)
#define VIDEO_PREAMBLE_NEG_V0_H1 TMDS_VIDEO_PREAMBLE_WORD(TMDS_CTRL_01)
#define VIDEO_PREAMBLE_NEG_V1_H1 TMDS_VIDEO_PREAMBLE_WORD(TMDS_CTRL_11)

// Polarity indirection. Normal rt builds keep the compile-time path so scratch-X
// size stays stable; experimental 720p reboot switching enables runtime mode
// attributes and fills these symbols from video_mode_t.
#if !PICO_HDMI_RT_RUNTIME_MODE_ATTRS
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
#endif

#if PICO_HDMI_RT_RUNTIME_MODE_ATTRS
static uint32_t rt_sync_v0_h0;
static uint32_t rt_sync_v0_h1;
static uint32_t rt_sync_v1_h0;
static uint32_t rt_sync_v1_h1;
static uint32_t rt_preamble_v0_h0;
static uint32_t rt_preamble_v1_h0;
static uint32_t rt_preamble_v0_h1;
static uint32_t rt_preamble_v1_h1;
static uint32_t rt_video_preamble_v0_h1;
static uint32_t rt_video_preamble_v1_h1;

#define SYNC_V0_H0 rt_sync_v0_h0
#define SYNC_V0_H1 rt_sync_v0_h1
#define SYNC_V1_H0 rt_sync_v1_h0
#define SYNC_V1_H1 rt_sync_v1_h1
#define PREAMBLE_V0_H0 rt_preamble_v0_h0
#define PREAMBLE_V1_H0 rt_preamble_v1_h0
#define PREAMBLE_V0_H1 rt_preamble_v0_h1
#define PREAMBLE_V1_H1 rt_preamble_v1_h1
#define VIDEO_PREAMBLE_V0_H1 rt_video_preamble_v0_h1
#define VIDEO_PREAMBLE_V1_H1 rt_video_preamble_v1_h1

static uint16_t tmds_ctrl_symbol(bool vsync_wire, bool hsync_wire)
{
    switch ((vsync_wire ? 2U : 0U) | (hsync_wire ? 1U : 0U)) {
        case 0:
            return TMDS_CTRL_00;
        case 1:
            return TMDS_CTRL_01;
        case 2:
            return TMDS_CTRL_10;
        default:
            return TMDS_CTRL_11;
    }
}

static uint32_t lane0_sync_symbol(bool sync_positive, bool vsync_active, bool hsync_active)
{
    const bool vsync_wire = (vsync_active == sync_positive);
    const bool hsync_wire = (hsync_active == sync_positive);
    return tmds_ctrl_symbol(vsync_wire, hsync_wire);
}

static void cache_sync_symbols(const video_mode_t *mode)
{
    const bool sync_positive = mode->sync_positive;
    const uint32_t von_hon = lane0_sync_symbol(sync_positive, true, true);
    const uint32_t von_hoff = lane0_sync_symbol(sync_positive, true, false);
    const uint32_t voff_hon = lane0_sync_symbol(sync_positive, false, true);
    const uint32_t voff_hoff = lane0_sync_symbol(sync_positive, false, false);

    rt_sync_v0_h0 = von_hon | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20);
    rt_sync_v0_h1 = von_hoff | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20);
    rt_sync_v1_h0 = voff_hon | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20);
    rt_sync_v1_h1 = voff_hoff | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20);

    rt_preamble_v0_h0 = von_hon | (TMDS_CTRL_01 << 10) | (TMDS_CTRL_01 << 20);
    rt_preamble_v1_h0 = voff_hon | (TMDS_CTRL_01 << 10) | (TMDS_CTRL_01 << 20);
    rt_preamble_v0_h1 = von_hoff | (TMDS_CTRL_01 << 10) | (TMDS_CTRL_01 << 20);
    rt_preamble_v1_h1 = voff_hoff | (TMDS_CTRL_01 << 10) | (TMDS_CTRL_01 << 20);

    rt_video_preamble_v0_h1 = von_hoff | (TMDS_CTRL_01 << 10) | (TMDS_CTRL_00 << 20);
    rt_video_preamble_v1_h1 = voff_hoff | (TMDS_CTRL_01 << 10) | (TMDS_CTRL_00 << 20);
}
#endif

// Video guard band: Per HDMI 1.3a Table 5-5
// CH0 = 0b1011001100 (0x2CC), CH1 = 0b0100110011 (0x133), CH2 = 0b1011001100 (0x2CC)
#define VIDEO_GUARD_BAND (0x2CCu | (0x133u << 10) | (0x2CCu << 20))

#define HSTX_CMD_RAW (0x0u << 12)
#define HSTX_CMD_RAW_REPEAT (0x1u << 12)
#define HSTX_CMD_TMDS (0x2u << 12)
#define HSTX_CMD_TMDS_REPEAT (0x3u << 12)
#define HSTX_CMD_NOP (0xfu << 12)

// Video preamble and guard band widths (HDMI 1.3a Section 5.2.2)
#define W_VIDEO_PREAMBLE 8
#define W_VIDEO_GUARD_BAND 2

// ============================================================================
// Runtime Video Mode Definitions
// ============================================================================

const video_mode_t video_mode_480_p = {
    .h_front_porch = 16,
    .h_sync_width = 96,
    .h_back_porch = 48,
    .h_active_pixels = 640,
    .v_front_porch = 10,
    .v_sync_width = 2,
    .v_back_porch = 33,
    .v_active_lines = 480,
    .h_total_pixels = 800,
    .v_total_lines = 525,
    // Pixel = sys_clk / (hstx_clk_div * hstx_csr_clkdiv); 1*5 -> 25.2 MHz at the
    // stock 126 MHz. A consumer overclocking 480p (e.g. 252 MHz for scanline-IRQ
    // headroom) defines PICO_HDMI_480P_HSTX_CLK_DIV=2 to keep pixel/signal
    // identical (clk_hstx stays 126 MHz; only clk_sys speeds up).
    .hstx_clk_div = PICO_HDMI_480P_HSTX_CLK_DIV,
    .hstx_csr_clkdiv = 5,
    .sync_positive = false,
    .data_island_in_hsync = true,
};

const video_mode_t video_mode_240_p = {
    .h_front_porch = 32,
    .h_sync_width = 192,
    .h_back_porch = 96,
    .h_active_pixels = 1280,
    .v_front_porch = 4,
    .v_sync_width = 4,
    .v_back_porch = 14,
    .v_active_lines = 240,
    .h_total_pixels = 1600,
    .v_total_lines = 262,
    .hstx_clk_div = 1,
    .hstx_csr_clkdiv = 5,
    .sync_positive = false,
    .data_island_in_hsync = true,
};

// CEA VIC 4: 1280x720 @ 60Hz. Static builds use VIDEO_MODE_1280x720 so the
// compile-time sync polarity and DI placement match; runtime-attribute builds
// read those values from this descriptor.
// Pixel clock 74.25 MHz; expect sys_clk = 372 MHz and vreg = 1.30V.
const video_mode_t video_mode_720_p = {
    .h_front_porch = 110,
    .h_sync_width = 40,
    .h_back_porch = 220,
    .h_active_pixels = 1280,
    .v_front_porch = 5,
    .v_sync_width = 5,
    .v_back_porch = 20,
    .v_active_lines = 720,
    .h_total_pixels = 1650,
    .v_total_lines = 750,
    .hstx_clk_div = 1,
    .hstx_csr_clkdiv = 5,
    .sync_positive = true,
    .data_island_in_hsync = false,
};

const video_mode_t *video_output_active_mode = &video_mode_480_p;

// ============================================================================
// ISR-Cached Timing Variables (written by apply_mode, read by ISR)
// ============================================================================

static uint16_t rt_h_front_porch;
static uint16_t rt_h_sync_width;
static uint16_t rt_h_back_porch;
static uint16_t rt_h_active_pixels;
static uint16_t rt_v_front_porch;
static uint16_t rt_v_sync_width;
static uint16_t rt_v_back_porch;
static uint16_t rt_v_active_lines;
uint16_t rt_v_total_lines;
// Frame-constant vertical region boundaries, precomputed in init_rt_from_mode()
// so get_scanline_state() doesn't re-add them on every scanline IRQ.
static uint16_t rt_vsync_end;  // front_porch + sync_width
static uint16_t rt_blank_head; // front_porch + sync_width + back_porch (= active start)
static uint32_t rt_active_end; // blank_head + active_lines
static uint16_t rt_sync_after_di;
#if PICO_HDMI_RT_RUNTIME_MODE_ATTRS
static bool rt_di_in_hsync;
#endif

// ============================================================================
// Audio/Video State
// ============================================================================

uint16_t frame_width = 0;
uint16_t frame_height = 0;
volatile uint32_t video_frame_count = 0;

// DVI mode: when true, disables all HDMI Data Islands (pure DVI output, no audio)
static bool dvi_mode = false;

// Max active pixels across all modes (1280 for 240p)
static uint16_t PICO_HDMI_LINE_BUFFER_ATTR line_buffer[1280] __attribute__((aligned(4)));
static uint32_t v_scanline = 2;
static bool vactive_cmdlist_posted = false;
static bool dma_pong = false;

static video_output_task_fn background_task = NULL;
static video_output_scanline_cb_t scanline_callback = NULL;
static video_output_vsync_cb_t vsync_callback = NULL;

static uint32_t current_sample_rate = 48000;

#if PICO_HDMI_PRECOMPOSED_ACTIVE_LINES
// ============================================================================
// Precomposed active lines (2.1): static per-mode headers; the ISR pops a
// pre-encoded island from the di queue and patches it into the header being
// posted. Pixel data comes from a pointer callback (zero-copy / app-prepared
// rings); native 16-bit pixel mode is per-mode. Ported from video_output.c.
// ============================================================================
static video_output_scanline_ptr_cb_t scanline_ptr_callback = NULL;
static const uint32_t *active_data_ptr = NULL;
static bool native_pixel_mode = false;
static uint32_t dma_ctrl32[2], dma_ctrl16[2];

static video_output_precomposed_line_t *compose_ring;
static uint32_t compose_ring_entries;
static volatile bool compose_ring_built;
static uint32_t active_line_global;
static uint32_t precomposed_di_offset;
static uint32_t precomposed_di_words;
static uint32_t precomposed_vblank_di_offset;
static const uint32_t *precomposed_null_di;
static uint32_t precomposed_vblank_len;
volatile uint32_t video_output_precomposed_stale_count;

void video_output_set_scanline_pointer_callback(video_output_scanline_ptr_cb_t cb)
{
    scanline_ptr_callback = cb;
}

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

void video_output_force_resync(void)
{
    video_output_request_resync();
}
volatile uint32_t video_output_resync_count;
#endif // PICO_HDMI_PRECOMPOSED_ACTIVE_LINES

static inline bool data_island_hsync_active(void)
{
#if PICO_HDMI_RT_RUNTIME_MODE_ATTRS
    return rt_di_in_hsync;
#else
    return DI_HSYNC_ACTIVE;
#endif
}

// Mode switch / resync flags (set by Core 0, consumed by Core 1)
static volatile const video_mode_t *pending_mode = NULL;
static volatile bool resync_requested = false;

#define DMACH_PING 0
#define DMACH_PONG 1

// ============================================================================
// Command Lists (runtime-filled)
// ============================================================================

// Pure DVI command lists (9 words each)
static uint32_t vblank_line_vsync_off[9] HSTX_CMDLIST_ATTR;
static uint32_t vblank_line_vsync_on[9] HSTX_CMDLIST_ATTR;
static uint32_t vactive_line_dvi[9] HSTX_CMDLIST_ATTR;

static uint32_t vactive_di_ping[128] HSTX_CMDLIST_ATTR;
static uint32_t vactive_di_pong[128] HSTX_CMDLIST_ATTR;
static uint32_t vactive_di_null[128] HSTX_CMDLIST_ATTR;
static uint32_t vactive_di_len, vactive_di_null_len;

static uint32_t vblank_di_ping[128] HSTX_CMDLIST_ATTR;
static uint32_t vblank_di_pong[128] HSTX_CMDLIST_ATTR;
static uint32_t vblank_di_null[128] HSTX_CMDLIST_ATTR;
static uint32_t vblank_di_len, vblank_di_null_len;

static uint32_t vblank_acr_vsync_on[64] HSTX_CMDLIST_ATTR;
static uint32_t vblank_acr_vsync_off[64] HSTX_CMDLIST_ATTR;
static uint32_t vblank_infoframe_vsync_on[64] HSTX_CMDLIST_ATTR;
static uint32_t vblank_infoframe_vsync_off[64] HSTX_CMDLIST_ATTR;
static uint32_t vblank_avi_infoframe[64] HSTX_CMDLIST_ATTR;
static uint32_t vblank_acr_vsync_on_len, vblank_acr_vsync_off_len;
static uint32_t vblank_infoframe_vsync_on_len, vblank_infoframe_vsync_off_len;
static uint32_t vblank_avi_infoframe_len;

// ACR command lists for genlock (custom CTS values)
static uint32_t genlock_acr_vsync_on[64] HSTX_CMDLIST_ATTR;
static uint32_t genlock_acr_vsync_off[64] HSTX_CMDLIST_ATTR;
static uint32_t genlock_acr_vsync_on_len, genlock_acr_vsync_off_len;
static bool use_genlock_acr = false;

#if defined(PICO_HDMI_DUMP_COMMAND_LISTS) && PICO_HDMI_DUMP_COMMAND_LISTS
static void dump_command_list(const char *name, const uint32_t *words, uint32_t len)
{
    printf("pico_hdmi %s len=%lu", name, (unsigned long)len);
    for (uint32_t i = 0; i < len; ++i) {
        printf(" %08lx", (unsigned long)words[i]);
    }
    printf("\n");
}

static void dump_runtime_command_lists(const video_mode_t *mode)
{
    printf("pico_hdmi rt_mode h=%u,%u,%u,%u v=%u,%u,%u,%u hstx_clk_div=%u csr_clkdiv=%u di_in_hsync=%u\n",
           mode->h_front_porch, mode->h_sync_width, mode->h_back_porch, mode->h_active_pixels, mode->v_front_porch,
           mode->v_sync_width, mode->v_back_porch, mode->v_active_lines, mode->hstx_clk_div, mode->hstx_csr_clkdiv,
#if PICO_HDMI_RT_RUNTIME_MODE_ATTRS
           mode->data_island_in_hsync);
#else
           DI_IN_HSYNC);
#endif
    dump_command_list("vblank_line_vsync_off", vblank_line_vsync_off, 9);
    dump_command_list("vblank_line_vsync_on", vblank_line_vsync_on, 9);
    dump_command_list("vactive_line_dvi", vactive_line_dvi, 9);
}
#endif

// ============================================================================
// Build DVI Command Lists
// ============================================================================

static void build_dvi_command_lists(void)
{
    // vblank_line_vsync_off: vsync=1, hsync toggling
    vblank_line_vsync_off[0] = HSTX_CMD_RAW_REPEAT | rt_h_front_porch;
    vblank_line_vsync_off[1] = SYNC_V1_H1;
    vblank_line_vsync_off[2] = HSTX_CMD_NOP;
    vblank_line_vsync_off[3] = HSTX_CMD_RAW_REPEAT | rt_h_sync_width;
    vblank_line_vsync_off[4] = SYNC_V1_H0;
    vblank_line_vsync_off[5] = HSTX_CMD_NOP;
    vblank_line_vsync_off[6] = HSTX_CMD_RAW_REPEAT | (rt_h_back_porch + rt_h_active_pixels);
    vblank_line_vsync_off[7] = SYNC_V1_H1;
    vblank_line_vsync_off[8] = HSTX_CMD_NOP;

    // vblank_line_vsync_on: vsync=0, hsync toggling
    vblank_line_vsync_on[0] = HSTX_CMD_RAW_REPEAT | rt_h_front_porch;
    vblank_line_vsync_on[1] = SYNC_V0_H1;
    vblank_line_vsync_on[2] = HSTX_CMD_NOP;
    vblank_line_vsync_on[3] = HSTX_CMD_RAW_REPEAT | rt_h_sync_width;
    vblank_line_vsync_on[4] = SYNC_V0_H0;
    vblank_line_vsync_on[5] = HSTX_CMD_NOP;
    vblank_line_vsync_on[6] = HSTX_CMD_RAW_REPEAT | (rt_h_back_porch + rt_h_active_pixels);
    vblank_line_vsync_on[7] = SYNC_V0_H1;
    vblank_line_vsync_on[8] = HSTX_CMD_NOP;

    // vactive_line_dvi: active video for DVI (no Data Islands)
    vactive_line_dvi[0] = HSTX_CMD_RAW_REPEAT | rt_h_front_porch;
    vactive_line_dvi[1] = SYNC_V1_H1;
    vactive_line_dvi[2] = HSTX_CMD_NOP;
    vactive_line_dvi[3] = HSTX_CMD_RAW_REPEAT | rt_h_sync_width;
    vactive_line_dvi[4] = SYNC_V1_H0;
    vactive_line_dvi[5] = HSTX_CMD_NOP;
    vactive_line_dvi[6] = HSTX_CMD_RAW_REPEAT | rt_h_back_porch;
    vactive_line_dvi[7] = SYNC_V1_H1;
    vactive_line_dvi[8] = HSTX_CMD_TMDS | rt_h_active_pixels;
}

// ============================================================================
// HSTX Resync - Reset output to sync with input VSYNC
// ============================================================================

static void hstx_resync(void)
{
    // 1. Abort DMA chains
    dma_channel_abort(DMACH_PING);
    dma_channel_abort(DMACH_PONG);

    // 2. Disconnect GPIO before disabling HSTX (no garbage on pins)
    for (int i = PIN_HSTX_CLK; i <= PIN_HSTX_D2 + 1; ++i)
        gpio_set_function(i, GPIO_FUNC_SIO);

    // 3. Disable HSTX (resets shift register, clock generator, and flushes FIFO)
    hstx_ctrl_hw->csr &= ~HSTX_CTRL_CSR_EN_BITS;

    // Small delay to ensure HSTX fully stops
    __asm volatile("nop\nnop\nnop\nnop");

    // 4. Reset state to start of frame
    v_scanline = 0;
    vactive_cmdlist_posted = false;
    dma_pong = false;

    // 5. Clear any pending DMA interrupts
    dma_hw->ints0 = (1U << DMACH_PING) | (1U << DMACH_PONG);

    // 6. Configure DMA PING to start from beginning of frame (Line 0)
    dma_channel_hw_t *ch_ping = &dma_hw->ch[DMACH_PING];
#if PICO_HDMI_PRECOMPOSED_ACTIVE_LINES
    if (native_pixel_mode && dma_ctrl32[DMACH_PING]) {
        // A resync can interrupt mid-line; restore 32-bit command transfers.
        dma_hw->ch[DMACH_PING].al1_ctrl = dma_ctrl32[DMACH_PING];
        dma_hw->ch[DMACH_PONG].al1_ctrl = dma_ctrl32[DMACH_PONG];
    }
    video_output_resync_count++;
#endif
    ch_ping->read_addr = (uintptr_t)vblank_line_vsync_off;
    ch_ping->transfer_count = 9;

    // 7. Configure DMA PONG for the NEXT line (Line 1)
    dma_channel_hw_t *ch_pong = &dma_hw->ch[DMACH_PONG];
    ch_pong->read_addr = (uintptr_t)vblank_line_vsync_off;
    ch_pong->transfer_count = 9;

    // 8. Re-enable HSTX and start DMA (output goes nowhere — GPIO disconnected)
    hstx_ctrl_hw->csr |= HSTX_CTRL_CSR_EN_BITS;
    dma_channel_start(DMACH_PING);

    // 9. Wait for first valid line to serialize
    while (dma_channel_is_busy(DMACH_PING))
        tight_loop_contents();

    // 10. Reconnect GPIO — TV sees valid TMDS immediately
    for (int i = PIN_HSTX_CLK; i <= PIN_HSTX_D2 + 1; ++i)
        gpio_set_function(i, 0);
}

// ============================================================================
// Internal Helpers
// ============================================================================

static uint32_t DI_BUILDER_SECTION build_line_with_di(uint32_t *buf, const uint32_t *di_words, bool vsync, bool active)
    __attribute__((noinline, noclone));

static uint32_t DI_BUILDER_SECTION build_line_with_di(uint32_t *buf, const uint32_t *di_words, bool vsync, bool active)
{
    uint32_t *p = buf;
    uint32_t sync_h0;
    uint32_t sync_h1;
    uint32_t preamble;
    uint32_t video_preamble;
#if PICO_HDMI_RT_RUNTIME_MODE_ATTRS
    // Runtime-attribute builds still keep the 480p/240p hot builder specialized
    // to the stable negative-sync, DI-in-HSYNC layout. 720p uses a separate
    // back-porch builder selected by apply_mode().
    sync_h0 = vsync ? SYNC_NEG_V0_H0 : SYNC_NEG_V1_H0;
    sync_h1 = vsync ? SYNC_NEG_V0_H1 : SYNC_NEG_V1_H1;
    preamble = vsync ? PREAMBLE_NEG_V0_H0 : PREAMBLE_NEG_V1_H0;
    video_preamble = vsync ? VIDEO_PREAMBLE_NEG_V0_H1 : VIDEO_PREAMBLE_NEG_V1_H1;
#elif DI_IN_HSYNC
    // DI inside the hsync pulse (original layout, wide-hsync modes: 480p, 240p)
    sync_h0 = vsync ? SYNC_V0_H0 : SYNC_V1_H0;
    sync_h1 = vsync ? SYNC_V0_H1 : SYNC_V1_H1;
    preamble = vsync ? PREAMBLE_V0_H0 : PREAMBLE_V1_H0;
    video_preamble = vsync ? VIDEO_PREAMBLE_V0_H1 : VIDEO_PREAMBLE_V1_H1;
#else
    // DI in back porch, after hsync pulse (narrow-hsync modes: 720p60)
    sync_h0 = vsync ? SYNC_V0_H0 : SYNC_V1_H0;
    sync_h1 = vsync ? SYNC_V0_H1 : SYNC_V1_H1;
    preamble = vsync ? PREAMBLE_V0_H1 : PREAMBLE_V1_H1;
    video_preamble = vsync ? VIDEO_PREAMBLE_V0_H1 : VIDEO_PREAMBLE_V1_H1;
#endif

    *p++ = HSTX_CMD_RAW_REPEAT | rt_h_front_porch;
    *p++ = sync_h1;
    *p++ = HSTX_CMD_NOP;

#if PICO_HDMI_RT_RUNTIME_MODE_ATTRS || DI_IN_HSYNC
    *p++ = HSTX_CMD_RAW_REPEAT | W_PREAMBLE;
    *p++ = preamble;
    *p++ = HSTX_CMD_NOP;

    *p++ = HSTX_CMD_RAW | W_DATA_ISLAND;
    for (int i = 0; i < W_DATA_ISLAND; i++)
        *p++ = di_words[i];
    *p++ = HSTX_CMD_NOP;

    *p++ = HSTX_CMD_RAW_REPEAT | rt_sync_after_di;
    *p++ = sync_h0;
    *p++ = HSTX_CMD_NOP;

    if (active) {
        *p++ = HSTX_CMD_RAW_REPEAT | (rt_h_back_porch - W_VIDEO_PREAMBLE - W_VIDEO_GUARD_BAND);
        *p++ = sync_h1;
        *p++ = HSTX_CMD_NOP;

        *p++ = HSTX_CMD_RAW_REPEAT | W_VIDEO_PREAMBLE;
        *p++ = video_preamble;
        *p++ = HSTX_CMD_NOP;

        *p++ = HSTX_CMD_RAW_REPEAT | W_VIDEO_GUARD_BAND;
        *p++ = VIDEO_GUARD_BAND;

        *p++ = HSTX_CMD_TMDS | rt_h_active_pixels;
    } else {
        *p++ = HSTX_CMD_RAW_REPEAT | (rt_h_back_porch + rt_h_active_pixels);
        *p++ = sync_h1;
        *p++ = HSTX_CMD_NOP;
    }
#else
    *p++ = HSTX_CMD_RAW_REPEAT | rt_h_sync_width;
    *p++ = sync_h0;
    *p++ = HSTX_CMD_NOP;

    *p++ = HSTX_CMD_RAW_REPEAT | W_PREAMBLE;
    *p++ = preamble;
    *p++ = HSTX_CMD_NOP;

    *p++ = HSTX_CMD_RAW | W_DATA_ISLAND;
    for (int i = 0; i < W_DATA_ISLAND; i++)
        *p++ = di_words[i];
    *p++ = HSTX_CMD_NOP;

    if (active) {
        *p++ = HSTX_CMD_RAW_REPEAT |
               (rt_h_back_porch - W_PREAMBLE - W_DATA_ISLAND - W_VIDEO_PREAMBLE - W_VIDEO_GUARD_BAND);
        *p++ = sync_h1;
        *p++ = HSTX_CMD_NOP;

        *p++ = HSTX_CMD_RAW_REPEAT | W_VIDEO_PREAMBLE;
        *p++ = video_preamble;
        *p++ = HSTX_CMD_NOP;

        *p++ = HSTX_CMD_RAW_REPEAT | W_VIDEO_GUARD_BAND;
        *p++ = VIDEO_GUARD_BAND;

        *p++ = HSTX_CMD_TMDS | rt_h_active_pixels;
    } else {
        *p++ = HSTX_CMD_RAW_REPEAT | (rt_h_back_porch - W_PREAMBLE - W_DATA_ISLAND + rt_h_active_pixels);
        *p++ = sync_h1;
        *p++ = HSTX_CMD_NOP;
    }
#endif
    return (uint32_t)(p - buf);
}

#if PICO_HDMI_RT_RUNTIME_MODE_ATTRS
static uint32_t DI_BUILDER_SECTION_Y build_line_with_di_backporch(uint32_t *buf, const uint32_t *di_words, bool vsync,
                                                                  bool active) __attribute__((noinline, noclone));

static uint32_t __scratch_y("")
    build_line_with_di_backporch(uint32_t *buf, const uint32_t *di_words, bool vsync, bool active)
{
    uint32_t *p = buf;
    uint32_t sync_h0 = vsync ? SYNC_V0_H0 : SYNC_V1_H0;
    uint32_t sync_h1 = vsync ? SYNC_V0_H1 : SYNC_V1_H1;
    uint32_t preamble = vsync ? PREAMBLE_V0_H1 : PREAMBLE_V1_H1;
    uint32_t video_preamble = vsync ? VIDEO_PREAMBLE_V0_H1 : VIDEO_PREAMBLE_V1_H1;

    *p++ = HSTX_CMD_RAW_REPEAT | rt_h_front_porch;
    *p++ = sync_h1;
    *p++ = HSTX_CMD_NOP;

    *p++ = HSTX_CMD_RAW_REPEAT | rt_h_sync_width;
    *p++ = sync_h0;
    *p++ = HSTX_CMD_NOP;

    *p++ = HSTX_CMD_RAW_REPEAT | W_PREAMBLE;
    *p++ = preamble;
    *p++ = HSTX_CMD_NOP;

    *p++ = HSTX_CMD_RAW | W_DATA_ISLAND;
    for (int i = 0; i < W_DATA_ISLAND; i++)
        *p++ = di_words[i];
    *p++ = HSTX_CMD_NOP;

    if (active) {
        *p++ = HSTX_CMD_RAW_REPEAT |
               (rt_h_back_porch - W_PREAMBLE - W_DATA_ISLAND - W_VIDEO_PREAMBLE - W_VIDEO_GUARD_BAND);
        *p++ = sync_h1;
        *p++ = HSTX_CMD_NOP;

        *p++ = HSTX_CMD_RAW_REPEAT | W_VIDEO_PREAMBLE;
        *p++ = video_preamble;
        *p++ = HSTX_CMD_NOP;

        *p++ = HSTX_CMD_RAW_REPEAT | W_VIDEO_GUARD_BAND;
        *p++ = VIDEO_GUARD_BAND;

        *p++ = HSTX_CMD_TMDS | rt_h_active_pixels;
    } else {
        *p++ = HSTX_CMD_RAW_REPEAT | (rt_h_back_porch - W_PREAMBLE - W_DATA_ISLAND + rt_h_active_pixels);
        *p++ = sync_h1;
        *p++ = HSTX_CMD_NOP;
    }

    return (uint32_t)(p - buf);
}

typedef uint32_t (*di_line_builder_fn_t)(uint32_t *buf, const uint32_t *di_words, bool vsync, bool active);
static di_line_builder_fn_t rt_build_line_with_di = build_line_with_di;
#define BUILD_LINE_WITH_DI(buf, di_words, vsync, active) rt_build_line_with_di((buf), (di_words), (vsync), (active))
#else
#define BUILD_LINE_WITH_DI(buf, di_words, vsync, active) build_line_with_di((buf), (di_words), (vsync), (active))
#endif

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
    // Boundaries (rt_vsync_end / rt_blank_head / rt_active_end) are precomputed
    // once per mode in init_rt_from_mode() rather than re-summed every line.
    // Active video is pinned directly after the back porch so the vsync-to-active
    // distance NEVER depends on v_total: genlock vtotal steps land at the BOTTOM
    // of the frame (extra blanking before the front porch), not shifting the picture.
    const uint32_t blank_head = rt_blank_head;
    state->vsync_active = (v_scanline >= rt_v_front_porch && v_scanline < rt_vsync_end);
    state->front_porch = (v_scanline < rt_v_front_porch);
    state->back_porch = (v_scanline >= rt_vsync_end && v_scanline < blank_head);
    state->active_video = (v_scanline >= blank_head && v_scanline < rt_active_end);

    state->send_acr = (v_scanline >= rt_vsync_end && v_scanline < blank_head && (v_scanline % 4 == 0));

    if (state->active_video) {
        state->active_line = v_scanline - blank_head;
    } else {
        state->active_line = 0;
    }
}

// ============================================================================
// Elastic blanking: sub-line frame-period trim for genlock. Each blanking
// template ends in one large RAW_REPEAT (back porch + active, >1000 px);
// adding N pixels to those words stretches every blanking line by N pixel
// clocks. Granularity ~40 lines x 13.4 ns = ~0.5 us/frame per pixel -- 40x
// finer than a +-1 vtotal step, and invisible to sink line PLLs (<<1% of a
// line). vtotal dither steps (22 us) visibly disturb some sinks.
// ============================================================================
// ============================================================================
// Output perf probe: per-IRQ HSTX FIFO level minimum + max inter-IRQ gap.
// A FIFO dip toward empty or an IRQ gap spike is the objective signature of
// an output microburp (the thing sinks render as a split-second jolt).
// DIAGNOSTIC ONLY: two APB peripheral reads per scanline IRQ (timer + FIFO),
// which stall the core. Off by default to keep the per-line ISR lean; define
// PICO_HDMI_PERF_PROBE=1 to re-enable the FIFO/gap telemetry.
// ============================================================================
#ifndef PICO_HDMI_PERF_PROBE
#define PICO_HDMI_PERF_PROBE 0
#endif

#if PICO_HDMI_PERF_PROBE
static volatile uint32_t probe_fifo_min = 0xFFFFFFFFU;
static volatile uint32_t probe_irq_gap_max;
static uint32_t probe_last_irq_ts;
#endif

void video_output_perf_probe_read(uint32_t *fifo_min, uint32_t *irq_gap_max_us)
{
#if PICO_HDMI_PERF_PROBE
    *fifo_min = probe_fifo_min;
    *irq_gap_max_us = probe_irq_gap_max;
    probe_fifo_min = 0xFFFFFFFFU;
    probe_irq_gap_max = 0;
#else
    *fifo_min = 0;
    *irq_gap_max_us = 0;
#endif
}

#define HTRIM_MAX_SLOTS 16
#define HTRIM_MIN_BIG_REPEAT 1200
#define HTRIM_MAX_BIG_REPEAT 1600
#define HTRIM_LIMIT_PX 30
static uint32_t *htrim_word[HTRIM_MAX_SLOTS];
static uint16_t htrim_base[HTRIM_MAX_SLOTS];
static uint8_t htrim_slots;
static int16_t htrim_px;

static void htrim_register(uint32_t *buf, uint32_t len)
{
    for (uint32_t i = 0; i < len && htrim_slots < HTRIM_MAX_SLOTS; i++) {
        const uint32_t count = buf[i] & 0xFFFU;
        if ((buf[i] >> 12) == 0x1 && count >= HTRIM_MIN_BIG_REPEAT && count <= HTRIM_MAX_BIG_REPEAT) {
            htrim_word[htrim_slots] = &buf[i];
            htrim_base[htrim_slots] = (uint16_t)(buf[i] & 0xFFFU);
            htrim_slots++;
        }
    }
}

static void htrim_register_all(void)
{
    htrim_slots = 0;
    htrim_px = 0;
    htrim_register(vblank_line_vsync_off, 9);
    htrim_register(vblank_line_vsync_on, 9);
    htrim_register(vblank_acr_vsync_on, vblank_acr_vsync_on_len);
    htrim_register(vblank_acr_vsync_off, vblank_acr_vsync_off_len);
    htrim_register(vblank_infoframe_vsync_on, vblank_infoframe_vsync_on_len);
    htrim_register(vblank_infoframe_vsync_off, vblank_infoframe_vsync_off_len);
    htrim_register(vblank_avi_infoframe, vblank_avi_infoframe_len);
    htrim_register(genlock_acr_vsync_on, genlock_acr_vsync_on_len);
    htrim_register(genlock_acr_vsync_off, genlock_acr_vsync_off_len);
#if PICO_HDMI_PRECOMPOSED_ACTIVE_LINES
    // The DI vblank templates are persistent in precomposed mode (the ISR
    // patches island words in place, never the tail repeat) and MUST be
    // trimmed too: trimming only the non-DI lines imposes a line-to-line
    // H-period sawtooth inside every vblank (1650 vs 1650+trim px), which
    // sink H-PLLs visibly track once |trim| exceeds ~14 px (bench-observed).
    // Uniform trim removes the alternation and triples the authority.
    // False positives are impossible: TERC4 payload words always carry
    // high bits (3x10-bit lanes), so they can never match a bare
    // RAW_REPEAT command in the 1200-1600 px window.
    htrim_register(vblank_di_ping, precomposed_vblank_len);
    htrim_register(vblank_di_pong, precomposed_vblank_len);
    htrim_register(vblank_di_null, vblank_di_null_len);
#endif
}

int video_output_get_vblank_htrim_slots(void)
{
    return (int)htrim_slots;
}

int video_output_get_vblank_htrim_px(void)
{
    return (int)htrim_px;
}

// Trim every blanking line by `px` pixel clocks (clamped). Safe to call from
// the vsync callback: single aligned word writes, picked up by the lines
// posted later in the same frame.
void video_output_set_vblank_htrim_px(int px)
{
    if (px > HTRIM_LIMIT_PX)
        px = HTRIM_LIMIT_PX;
    if (px < -HTRIM_LIMIT_PX)
        px = -HTRIM_LIMIT_PX;
    if ((int16_t)px == htrim_px)
        return;
    htrim_px = (int16_t)px;
    for (uint32_t i = 0; i < htrim_slots; i++) {
        *htrim_word[i] = (0x1U << 12) | (uint32_t)(htrim_base[i] + px);
    }
}

#if PICO_HDMI_PRECOMPOSED_ACTIVE_LINES
void video_output_compose_service(void)
{
    if (!compose_ring || compose_ring_built || dvi_mode) {
        return;
    }
    const uint32_t *null_di = hstx_get_null_data_island(false, data_island_hsync_active());

    // Build entry 0 twice (null island, then a marker island) and diff to
    // discover the island patch offset and width for the ACTIVE mode --
    // robust against per-mode line layouts (in-hsync vs back-porch DI).
    static uint32_t marker[40];
    for (uint32_t i = 0; i < 40; i++) {
        marker[i] = 0xA5A50000u | i;
    }
    uint32_t len_null = BUILD_LINE_WITH_DI(compose_ring[0].buf, null_di, false, true);
    static uint32_t probe[sizeof(compose_ring[0].buf) / sizeof(uint32_t)];
    uint32_t len_probe = BUILD_LINE_WITH_DI(probe, marker, false, true);
    (void)len_probe;
    uint32_t first = 0, last = 0;
    for (uint32_t i = 0; i < len_null; i++) {
        if (compose_ring[0].buf[i] != probe[i]) {
            if (!first) {
                first = i;
            }
            last = i;
        }
    }
    precomposed_di_offset = first;
    precomposed_di_words = last - first + 1;

    compose_ring[0].len = (uint16_t)len_null;
    compose_ring[0].tag = 0;
    for (uint32_t i = 1; i < compose_ring_entries; i++) {
        compose_ring[i].len = (uint16_t)BUILD_LINE_WITH_DI(compose_ring[i].buf, null_di, false, true);
        compose_ring[i].tag = i;
    }
    // Blanking patch templates (non-vsync lines) with their own probed
    // offset -- the blanking layout differs from the active layout in
    // back-porch DI modes.
    precomposed_vblank_len = BUILD_LINE_WITH_DI(vblank_di_ping, null_di, false, false);
    (void)BUILD_LINE_WITH_DI(probe, marker, false, false);
    first = 0;
    for (uint32_t i = 0; i < precomposed_vblank_len; i++) {
        if (vblank_di_ping[i] != probe[i]) {
            first = i;
            break;
        }
    }
    precomposed_vblank_di_offset = first;
    (void)BUILD_LINE_WITH_DI(vblank_di_pong, null_di, false, false);
    precomposed_null_di = null_di;
    __compiler_memory_barrier();
    htrim_register_all();
    compose_ring_built = true;
}
#endif // PICO_HDMI_PRECOMPOSED_ACTIVE_LINES

static inline void __scratch_x("") video_output_handle_vsync(dma_channel_hw_t *ch, uint32_t v_scanline)
{
    if (dvi_mode) {
        ch->read_addr = (uintptr_t)vblank_line_vsync_on;
        ch->transfer_count = 9;
        if (v_scanline == rt_v_front_porch) {
            video_frame_count++;
            if (vsync_callback)
                vsync_callback();
        }
    } else {
        if (v_scanline == rt_v_front_porch) {
            if (use_genlock_acr) {
                ch->read_addr = (uintptr_t)genlock_acr_vsync_on;
                ch->transfer_count = genlock_acr_vsync_on_len;
            } else {
                ch->read_addr = (uintptr_t)vblank_acr_vsync_on;
                ch->transfer_count = vblank_acr_vsync_on_len;
            }
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
#if PICO_HDMI_PRECOMPOSED_ACTIVE_LINES
    if (scanline_ptr_callback) {
        active_data_ptr = scanline_ptr_callback(v_scanline, active_line);
    } else
#endif
    {
        uint32_t *dst32 = (uint32_t *)line_buffer;
        if (scanline_callback) {
            scanline_callback(v_scanline, active_line, dst32);
        } else {
            for (uint32_t i = 0; i < rt_h_active_pixels / 2; i++) {
                dst32[i] = 0;
            }
        }
#if PICO_HDMI_PRECOMPOSED_ACTIVE_LINES
        active_data_ptr = (const uint32_t *)line_buffer;
#endif
    }

    if (dvi_mode) {
        ch->read_addr = (uintptr_t)vactive_line_dvi;
        ch->transfer_count = 9;
    } else {
#if PICO_HDMI_PRECOMPOSED_ACTIVE_LINES
        if (compose_ring_built) {
            uint32_t g = active_line_global++;
            video_output_precomposed_line_t *e = &compose_ring[g % compose_ring_entries];
            // Pop a pre-encoded island (or null/silence) and patch it into
            // the static header about to be posted; the audio schedule lives
            // here and cannot be starved by background work.
            const uint32_t *di = hstx_di_queue_get_audio_packet();
            if (!di) {
                di = precomposed_null_di;
            }
            uint32_t *dst = &e->buf[precomposed_di_offset];
            for (uint32_t i = 0; i < precomposed_di_words; i++) {
                dst[i] = di[i];
            }
            ch->read_addr = (uintptr_t)e->buf;
            ch->transfer_count = e->len;
            return;
        }
        // Ring not built yet (startup / mode switch, a few ms): post the
        // static null line; never call the line builders from the ISR.
        video_output_precomposed_stale_count++;
        ch->read_addr = (uintptr_t)vactive_di_null;
        ch->transfer_count = vactive_di_null_len;
#else
        uint32_t *buf = dma_pong ? vactive_di_ping : vactive_di_pong;
        const uint32_t *di_words = hstx_di_queue_get_audio_packet();
        if (di_words) {
            vactive_di_len = BUILD_LINE_WITH_DI(buf, di_words, false, true);
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
        (void)send_acr;
        (void)dma_pong;
        (void)v_scanline;
        ch->read_addr = (uintptr_t)vblank_line_vsync_off;
        ch->transfer_count = 9;
    } else {
        if (send_acr) {
            if (use_genlock_acr) {
                ch->read_addr = (uintptr_t)genlock_acr_vsync_off;
                ch->transfer_count = genlock_acr_vsync_off_len;
            } else {
                ch->read_addr = (uintptr_t)vblank_acr_vsync_off;
                ch->transfer_count = vblank_acr_vsync_off_len;
            }
        } else if (v_scanline == 0) {
            ch->read_addr = (uintptr_t)vblank_avi_infoframe;
            ch->transfer_count = vblank_avi_infoframe_len;
        } else {
#if PICO_HDMI_PRECOMPOSED_ACTIVE_LINES
            if (compose_ring_built) {
                const uint32_t *di = hstx_di_queue_get_audio_packet();
                if (di) {
                    uint32_t *buf = dma_pong ? vblank_di_ping : vblank_di_pong;
                    uint32_t *dst = &buf[precomposed_vblank_di_offset];
                    for (uint32_t i = 0; i < precomposed_di_words; i++) {
                        dst[i] = di[i];
                    }
                    ch->read_addr = (uintptr_t)buf;
                    ch->transfer_count = precomposed_vblank_len;
                } else {
                    ch->read_addr = (uintptr_t)vblank_di_null;
                    ch->transfer_count = vblank_di_null_len;
                }
                return;
            }
            ch->read_addr = (uintptr_t)vblank_di_null;
            ch->transfer_count = vblank_di_null_len;
#else
            const uint32_t *di_words = hstx_di_queue_get_audio_packet();
            if (di_words) {
                uint32_t *buf = dma_pong ? vblank_di_ping : vblank_di_pong;
                vblank_di_len = BUILD_LINE_WITH_DI(buf, di_words, false, false);
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
#if PICO_HDMI_PRECOMPOSED_ACTIVE_LINES
    ch->read_addr = (uintptr_t)active_data_ptr;
#else
    ch->read_addr = (uintptr_t)line_buffer;
#endif
    // 32-bit mode: h_active/2 words of pre-expanded pixel pairs. Native
    // 16-bit mode: the same count as halfword transfers, bus-replicated.
    ch->transfer_count = (rt_h_active_pixels * sizeof(uint16_t)) / sizeof(uint32_t);
}

// ============================================================================
// DMA IRQ Handler
// ============================================================================

static void __scratch_x("") dma_irq_handler()
{
#if PICO_HDMI_PERF_PROBE
    {
        const uint32_t now = timer_hw->timerawl;
        const uint32_t gap = now - probe_last_irq_ts;
        probe_last_irq_ts = now;
        if (gap > probe_irq_gap_max && gap < 1000000U) {
            probe_irq_gap_max = gap;
        }
        const uint32_t lvl = (hstx_fifo_hw->stat & HSTX_FIFO_STAT_LEVEL_BITS) >> HSTX_FIFO_STAT_LEVEL_LSB;
        if (lvl < probe_fifo_min) {
            probe_fifo_min = lvl;
        }
    }
#endif
    uint32_t ch_num = dma_pong ? DMACH_PONG : DMACH_PING;
    dma_channel_hw_t *ch = &dma_hw->ch[ch_num];
    dma_hw->intr = 1U << ch_num;
    dma_pong = !dma_pong;

    // Advance audio/data-island scheduler exactly once per scanline (HDMI mode only)
    if (!dvi_mode && !vactive_cmdlist_posted) {
        hstx_di_queue_tick();
    }

    scanline_state_t state;
    get_scanline_state(v_scanline, &state);

#if PICO_HDMI_PRECOMPOSED_ACTIVE_LINES
    if (native_pixel_mode) {
        bool posting_active_data = state.active_video && vactive_cmdlist_posted;
        ch->al1_ctrl = posting_active_data ? dma_ctrl16[ch_num] : dma_ctrl32[ch_num];
    }
#endif

    if (state.vsync_active) {
        video_output_handle_vsync(ch, v_scanline);
    } else if (state.active_video && !vactive_cmdlist_posted) {
        video_output_handle_active_start(ch, v_scanline, state.active_line, dma_pong);
        vactive_cmdlist_posted = true;
    } else if (state.active_video && vactive_cmdlist_posted) {
        video_output_handle_active_data(ch);
        vactive_cmdlist_posted = false;
    } else {
        video_output_handle_blanking(ch, v_scanline, state.send_acr, dma_pong);
    }
    if (!vactive_cmdlist_posted) {
        // compare-and-reset instead of % (rt_v_total_lines is a non-power-of-2
        // runtime value -> real udiv; the wrap test is ~10-18 cyc cheaper/line).
        uint32_t next_scanline = v_scanline + 1U;
        v_scanline = (next_scanline >= rt_v_total_lines) ? 0U : next_scanline;
    }
}

// ============================================================================
// Apply Mode
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
    uint32_t pixel_clock = clock_get_hz(clk_sys) / ((uint32_t)video_output_active_mode->hstx_clk_div *
                                                    video_output_active_mode->hstx_csr_clkdiv);
    *cts = (uint32_t)(((uint64_t)pixel_clock * *n) / (128ULL * sample_rate));
}

static void configure_audio_packets(uint32_t sample_rate)
{
    hstx_di_queue_set_sample_rate(sample_rate);

    // Override samples_per_line with pixel-clock-accurate value.
    // The default set_sample_rate() assumes exactly 60 Hz, which is wrong
    // for 240p (60.114 Hz). Derive from actual timing instead:
    //   samples_per_line = sample_rate * h_total_pixels / pixel_clock
    uint32_t pixel_clock_hz = clock_get_hz(clk_sys) / ((uint32_t)video_output_active_mode->hstx_clk_div *
                                                       video_output_active_mode->hstx_csr_clkdiv);
    uint32_t h_total = video_output_active_mode->h_total_pixels;
#if PICO_HDMI_EXACT_AUDIO_PACING
    // The floor truncation below loses up to line_rate/65536 samples/s versus
    // the rate advertised over ACR, slowly draining the sink's audio buffer.
    // Keep the exact fractional remainder and feed it back in via the queue's
    // rational accumulator so long-term delivery is exactly sample_rate.
    uint64_t num = ((uint64_t)sample_rate * h_total) << 16;
    uint32_t spl = (uint32_t)(num / pixel_clock_hz);
    uint32_t rem = (uint32_t)(num - (uint64_t)spl * pixel_clock_hz);
    hstx_di_queue_set_samples_per_line_exact(spl, rem, pixel_clock_hz);
#else
    uint32_t spl_fp = (uint32_t)(((uint64_t)sample_rate * h_total << 16) / pixel_clock_hz);
    hstx_di_queue_set_samples_per_line_fp(spl_fp);
#endif

    hstx_packet_t packet;
    hstx_data_island_t island;

    uint32_t acr_n;
    uint32_t acr_cts;
    const bool di_hsync_active = data_island_hsync_active();
    get_acr_params(sample_rate, &acr_n, &acr_cts);
    hstx_packet_set_acr(&packet, acr_n, acr_cts);
    hstx_encode_data_island(&island, &packet, true, di_hsync_active);
    vblank_acr_vsync_on_len = BUILD_LINE_WITH_DI(vblank_acr_vsync_on, island.words, true, false);
    hstx_encode_data_island(&island, &packet, false, di_hsync_active);
    vblank_acr_vsync_off_len = BUILD_LINE_WITH_DI(vblank_acr_vsync_off, island.words, false, false);

    hstx_packet_set_audio_infoframe(&packet, sample_rate, 2, 16);
    hstx_encode_data_island(&island, &packet, true, di_hsync_active);
    vblank_infoframe_vsync_on_len = BUILD_LINE_WITH_DI(vblank_infoframe_vsync_on, island.words, true, false);
    hstx_encode_data_island(&island, &packet, false, di_hsync_active);
    vblank_infoframe_vsync_off_len = BUILD_LINE_WITH_DI(vblank_infoframe_vsync_off, island.words, false, false);
}

static void init_rt_from_mode(const video_mode_t *mode)
{
#if PICO_HDMI_RT_RUNTIME_MODE_ATTRS
    hstx_packet_set_sync_positive(mode->sync_positive);
    rt_di_in_hsync = mode->data_island_in_hsync;
    rt_build_line_with_di = rt_di_in_hsync ? build_line_with_di : build_line_with_di_backporch;
    hstx_di_queue_set_hsync_active(rt_di_in_hsync);
    cache_sync_symbols(mode);
#endif
    rt_h_front_porch = mode->h_front_porch;
    rt_h_sync_width = mode->h_sync_width;
    rt_h_back_porch = mode->h_back_porch;
    rt_h_active_pixels = mode->h_active_pixels;
    rt_v_front_porch = mode->v_front_porch;
    rt_v_sync_width = mode->v_sync_width;
    rt_v_back_porch = mode->v_back_porch;
    rt_v_active_lines = mode->v_active_lines;
    rt_v_total_lines = mode->v_total_lines;
    rt_vsync_end = (uint16_t)(rt_v_front_porch + rt_v_sync_width);
    rt_blank_head = (uint16_t)(rt_vsync_end + rt_v_back_porch);
    rt_active_end = (uint32_t)rt_blank_head + rt_v_active_lines;
    rt_sync_after_di = mode->h_sync_width - W_PREAMBLE - W_DATA_ISLAND;
}

static void build_all_command_lists(const video_mode_t *mode)
{
    build_dvi_command_lists();

#if defined(PICO_HDMI_DUMP_COMMAND_LISTS) && PICO_HDMI_DUMP_COMMAND_LISTS
    dump_runtime_command_lists(mode);
#endif

    // AVI InfoFrame: VIC=0 (non-standard) to avoid strict pixel clock
    // validation by sinks. Our 25.2 MHz is 0.1% off from standard 25.175 MHz.
    hstx_packet_t packet;
    hstx_data_island_t island;
    uint8_t vic = (mode->v_active_lines == 480) ? 1 : (mode->v_active_lines == 720) ? 4 : 0;
#if PICO_HDMI_LEGACY_240P_AVI_INFOFRAME
    uint8_t pixel_repetition = 0;
#else
    uint8_t pixel_repetition = (mode->v_active_lines == 240 && mode->h_active_pixels == 1280) ? 3 : 0;
#endif
    const bool di_hsync_active = data_island_hsync_active();
    hstx_packet_set_avi_infoframe(&packet, vic, pixel_repetition);
    hstx_encode_data_island(&island, &packet, false, di_hsync_active);
    vblank_avi_infoframe_len = BUILD_LINE_WITH_DI(vblank_avi_infoframe, island.words, false, false);

    // Null DI command lists
    vblank_di_null_len =
        BUILD_LINE_WITH_DI(vblank_di_null, hstx_get_null_data_island(false, di_hsync_active), false, false);
    vactive_di_null_len =
        BUILD_LINE_WITH_DI(vactive_di_null, hstx_get_null_data_island(false, di_hsync_active), false, true);

    vblank_di_len = BUILD_LINE_WITH_DI(vblank_di_ping, hstx_get_null_data_island(false, di_hsync_active), false, false);
    memcpy(vblank_di_pong, vblank_di_ping, sizeof(vblank_di_ping));
}

static void apply_mode(const video_mode_t *mode)
{
#if PICO_HDMI_PRECOMPOSED_ACTIVE_LINES
    // Headers are mode-specific: invalidate so the background task rebuilds
    // them (and re-probes DI offsets) for the new mode. The ISR falls back
    // to per-line builds until then.
    compose_ring_built = false;
    __compiler_memory_barrier();
#endif
    // 1. Disable DMA IRQ to prevent ISR firing with partial state
    irq_set_enabled(DMA_IRQ_0, false);

    // 2. Write all cached timing variables
    init_rt_from_mode(mode);

    // 3. Update public state
    video_output_active_mode = mode;
    frame_width = mode->h_active_pixels;
    frame_height = mode->v_active_lines;

    // 4. Build all command lists
    build_all_command_lists(mode);

    // 5. Update data island queue timing
    hstx_di_queue_set_v_total(mode->v_total_lines);

    // 6. Rebuild audio packets
    configure_audio_packets(current_sample_rate);

    // 7. Resync HSTX
    hstx_resync();

    // 8. Re-enable DMA IRQ
    irq_set_enabled(DMA_IRQ_0, true);
}

// ============================================================================
// Public Interface
// ============================================================================

void video_output_init(uint16_t width, uint16_t height)
{
    // Use pending_mode if set via video_output_set_mode() before init,
    // otherwise fall back to compile-time selection
    const video_mode_t *pm = (const video_mode_t *)pending_mode;
    const video_mode_t *initial_mode;
    if (pm) {
        pending_mode = NULL;
        initial_mode = pm;
    } else {
#ifdef VIDEO_MODE_1280x720
        initial_mode = &video_mode_720_p;
#elif defined(VIDEO_MODE_320x240)
        initial_mode = &video_mode_240_p;
#else
        initial_mode = &video_mode_480_p;
#endif
    }

    // Initialize rt_* cached variables and build command lists
    init_rt_from_mode(initial_mode);

    video_output_active_mode = initial_mode;
    frame_width = width;
    frame_height = height;

    build_all_command_lists(initial_mode);

    // Configure clk_hstx
    uint32_t sys_freq = clock_get_hz(clk_sys);
    clock_configure_int_divider(clk_hstx,
                                0, // No glitchless mux
                                CLOCKS_CLK_HSTX_CTRL_AUXSRC_VALUE_CLK_SYS, sys_freq, initial_mode->hstx_clk_div);

    // Claim DMA channels for HSTX (channels 0 and 1)
    dma_channel_claim(DMACH_PING);
    dma_channel_claim(DMACH_PONG);

    // Update data island queue timing
    hstx_di_queue_set_v_total(initial_mode->v_total_lines);

    // Initialize HDMI audio packets (default 48kHz)
    configure_audio_packets(48000);
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
    hstx_ctrl_hw->csr = HSTX_CTRL_CSR_EXPAND_EN_BITS |
                        (uint32_t)video_output_active_mode->hstx_csr_clkdiv << HSTX_CTRL_CSR_CLKDIV_LSB |
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

    // DMA Setup (configured before GPIO connection to avoid TMDS garbage)
    dma_channel_config c = dma_channel_get_default_config(DMACH_PING);
    channel_config_set_chain_to(&c, DMACH_PONG);
    channel_config_set_dreq(&c, DREQ_HSTX);
    dma_channel_configure(DMACH_PING, &c, &hstx_fifo_hw->fifo, vblank_line_vsync_off, 9, false);

    c = dma_channel_get_default_config(DMACH_PONG);
    channel_config_set_chain_to(&c, DMACH_PING);
    channel_config_set_dreq(&c, DREQ_HSTX);
    dma_channel_configure(DMACH_PONG, &c, &hstx_fifo_hw->fifo, vblank_line_vsync_off, 9, false);

#if PICO_HDMI_PRECOMPOSED_ACTIVE_LINES
    for (int i = 0; i < 2; i++) {
        uint32_t chn = (i == 0) ? DMACH_PING : DMACH_PONG;
        uint32_t ctrl = dma_hw->ch[chn].al1_ctrl;
        dma_ctrl32[chn] = ctrl;
        dma_ctrl16[chn] =
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

    // Wait for first DMA transfer to complete — HSTX serializes valid TMDS
    // internally but GPIO isn't connected yet, so TV sees nothing.
    while (dma_channel_is_busy(DMACH_PING))
        tight_loop_contents();

    // NOW connect GPIO — TV's first TMDS exposure is valid data
    for (int i = PIN_HSTX_CLK; i <= PIN_HSTX_D2 + 1; ++i) {
        gpio_set_function(i, 0);
        gpio_set_slew_rate(i, GPIO_SLEW_RATE_FAST);
        gpio_set_drive_strength(i, GPIO_DRIVE_STRENGTH_12MA);
    }

    while (1) {
        // Check for pending mode switch
        const video_mode_t *new_mode = (const video_mode_t *)pending_mode;
        if (new_mode) {
            pending_mode = NULL;
            apply_mode(new_mode);
        }

        // Check for resync request
        if (resync_requested) {
            resync_requested = false;
            irq_set_enabled(DMA_IRQ_0, false);
            hstx_resync();
            irq_set_enabled(DMA_IRQ_0, true);
        }

        if (background_task) {
            background_task();
        }
        tight_loop_contents();
    }
}

void pico_hdmi_set_audio_sample_rate(uint32_t sample_rate)
{
    current_sample_rate = sample_rate;
    configure_audio_packets(sample_rate);
}

void video_output_set_mode(const video_mode_t *mode)
{
    pending_mode = mode;
    __dmb();
}

uint16_t video_output_get_h_active_pixels(void)
{
    return rt_h_active_pixels;
}

uint16_t video_output_get_v_active_lines(void)
{
    return rt_v_active_lines;
}

void video_output_reconfigure_clock(void)
{
    uint32_t sys_freq = clock_get_hz(clk_sys);
    clock_configure_int_divider(clk_hstx, 0, CLOCKS_CLK_HSTX_CTRL_AUXSRC_VALUE_CLK_SYS, sys_freq,
                                video_output_active_mode->hstx_clk_div);
}

void video_output_update_acr(uint32_t pixel_clock_hz)
{
    // CTS = pixel_clock_hz * N / (128 * sample_rate)
    uint32_t acr_n = get_acr_n(current_sample_rate);
    uint32_t custom_cts = (uint32_t)(((uint64_t)pixel_clock_hz * acr_n) / (128ULL * current_sample_rate));

    hstx_packet_t packet;
    hstx_data_island_t island;
    const bool di_hsync_active = data_island_hsync_active();

    hstx_packet_set_acr(&packet, acr_n, custom_cts);
    hstx_encode_data_island(&island, &packet, true, di_hsync_active);
    genlock_acr_vsync_on_len = BUILD_LINE_WITH_DI(genlock_acr_vsync_on, island.words, true, false);
    hstx_encode_data_island(&island, &packet, false, di_hsync_active);
    genlock_acr_vsync_off_len = BUILD_LINE_WITH_DI(genlock_acr_vsync_off, island.words, false, false);

    __dmb();
    use_genlock_acr = true;
}

void video_output_request_resync(void)
{
    resync_requested = true;
    __dmb();
}
