#include "pico_hdmi/video_output_rt.h"

#include "pico_hdmi/hstx_data_island_queue.h"
#include "pico_hdmi/hstx_packet.h"
#include "pico_hdmi/video_output.h" // for default sync polarity and DI placement

#include "pico/stdlib.h"

#include "hardware/clocks.h"
#include "hardware/dma.h"
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

#include "hstx_pins_internal.h"

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

#ifndef PICO_HDMI_ACTIVE_LINE_DOUBLE_BUFFER
#define PICO_HDMI_ACTIVE_LINE_DOUBLE_BUFFER 0
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

// Elastic-blanking htrim registration/patching (see htrim_register_all() and
// video_output_set_vblank_htrim_px()). Off by default: in the default
// (non-genlock) build, scratch_x is exactly full, so no new code may reach
// any __scratch_x/__scratch_y function unless this is explicitly enabled.
#ifndef PICO_HDMI_VBLANK_HTRIM
#define PICO_HDMI_VBLANK_HTRIM 0
#endif

// HSTX clock divider for the 480p mode descriptor. Default 1 (stock 126 MHz
// sys_clk -> 25.2 MHz pixel). A consumer overclocking 480p (e.g. 252 MHz for
// scanline-IRQ headroom) sets this to 2 to keep the pixel clock unchanged.
#ifndef PICO_HDMI_480P_HSTX_CLK_DIV
#define PICO_HDMI_480P_HSTX_CLK_DIV 1
#endif

// Same mechanism as PICO_HDMI_480P_HSTX_CLK_DIV, for 240p. Default 1 (stock
// 126 MHz sys_clk -> 25.2 MHz pixel). A consumer overclocking 240p (e.g. 252
// MHz, to give Core 0's per-pixel conversion more headroom per line -- the
// DARK/SHADOW hotfix, or the 32-bit RGB888 scanout path) sets this to 2 to
// keep the pixel clock unchanged (clk_hstx stays 126 MHz; only clk_sys
// speeds up).
#ifndef PICO_HDMI_240P_HSTX_CLK_DIV
#define PICO_HDMI_240P_HSTX_CLK_DIV 1
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

static uint32_t lane0_sync_symbol(bool hsync_positive, bool vsync_positive, bool vsync_active, bool hsync_active)
{
    const bool vsync_wire = (vsync_active == vsync_positive);
    const bool hsync_wire = (hsync_active == hsync_positive);
    return tmds_ctrl_symbol(vsync_wire, hsync_wire);
}

static void cache_sync_symbols(const video_mode_t *mode)
{
    const uint32_t von_hon = lane0_sync_symbol(mode->hsync_positive, mode->vsync_positive, true, true);
    const uint32_t von_hoff = lane0_sync_symbol(mode->hsync_positive, mode->vsync_positive, true, false);
    const uint32_t voff_hon = lane0_sync_symbol(mode->hsync_positive, mode->vsync_positive, false, true);
    const uint32_t voff_hoff = lane0_sync_symbol(mode->hsync_positive, mode->vsync_positive, false, false);

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
    .hsync_positive = false,
    .vsync_positive = false,
    .data_island_in_hsync = true,
};

const video_mode_t video_mode_240_p = {
    .h_front_porch = 32,
    .h_sync_width = 192,
#if PICO_HDMI_VBLANK_HTRIM
    // Genlock: hardware testing showed the elastic/uniform htrim trim
    // distribution was never the cause of the 240p artifact (byte-identical
    // symptom across broken-trim, uniform-trim, and elastic-trim builds).
    // Root cause: the ORIGINAL 1600x262 base raster, once genlock stretches
    // v_total up to ~266, is a nonstandard raster (real 240p is 262/263
    // lines at ~15.7 kHz H) that falls outside the window sinks key 240p
    // detection on. Fix: retime the base mode itself to look like ordinary
    // NTSC 240p: 1613 x 264 @ 25.2 MHz = 59.178 Hz, H = 15.62 kHz --
    // numerically the MVS's own raster (264 lines, ~64.0 us/line) -- without
    // needing to stretch v_total beyond standard. Residual vs the 59.1856 Hz
    // MVS source is +2.1 us/frame, well inside uniform htrim (~-2 px), so
    // elastic single-line trim (htrim_elastic_mode) is retired/dormant.
    .h_back_porch = 109,
#else
    .h_back_porch = 96,
#endif
    .h_active_pixels = 1280,
    .v_front_porch = 4,
    .v_sync_width = 4,
#if PICO_HDMI_VBLANK_HTRIM
    .v_back_porch = 16,
#else
    .v_back_porch = 14,
#endif
    .v_active_lines = 240,
#if PICO_HDMI_VBLANK_HTRIM
    .h_total_pixels = 1613,
    .v_total_lines = 264,
#else
    .h_total_pixels = 1600,
    .v_total_lines = 262,
#endif
    // Pixel = sys_clk / (hstx_clk_div * hstx_csr_clkdiv); 1*5 -> 25.2 MHz at
    // the stock 126 MHz. A consumer overclocking 240p (e.g. 252 MHz, same
    // reasoning as 480p above) defines PICO_HDMI_240P_HSTX_CLK_DIV=2 to keep
    // pixel/signal identical (clk_hstx stays 126 MHz; only clk_sys speeds up).
    .hstx_clk_div = PICO_HDMI_240P_HSTX_CLK_DIV,
    .hstx_csr_clkdiv = 5,
    .hsync_positive = false,
    .vsync_positive = false,
    .data_island_in_hsync = true,
};

// Exact-clock 1280x720 CVT reduced blanking. The normal runtime firmware uses
// a 320 MHz system/HSTX clock, so CSR /5 gives an exact 64 MHz pixel clock.
// This is a non-CTA 59.979 Hz timing with H+/V- sync and VIC 0 metadata.
const video_mode_t video_mode_720_p = {
    .h_front_porch = 48,
    .h_sync_width = 32,
    .h_back_porch = 80,
    .h_active_pixels = 1280,
    .v_front_porch = 3,
    .v_sync_width = 5,
    .v_back_porch = 13,
    .v_active_lines = 720,
    .h_total_pixels = 1440,
    .v_total_lines = 741,
    .hstx_clk_div = 1,
    .hstx_csr_clkdiv = 5,
    .hsync_positive = true,
    .vsync_positive = false,
    .data_island_in_hsync = false,
};

// VESA CVT 960x720 @ 60Hz, adjusted only to the exact 56.0 MHz pixel clock
// available from a 280 MHz RP2350 PLL. The active raster is exactly the 3x
// 320x240 image, so no active-area borders are required.
const video_mode_t video_mode_960x720_p = {
    .h_front_porch = 48,
    .h_sync_width = 96,
    .h_back_porch = 144,
    .h_active_pixels = 960,
    .v_front_porch = 3,
    .v_sync_width = 4,
    .v_back_porch = 21,
    .v_active_lines = 720,
    .h_total_pixels = 1248,
    .v_total_lines = 748,
    .hstx_clk_div = 1,
    .hstx_csr_clkdiv = 5,
    .hsync_positive = false,
    .vsync_positive = true,
    .data_island_in_hsync = true,
};

// 1024x768 PC timing near VESA DMT. A 324 MHz HSTX clock gives a 64.8 MHz
// pixel clock; reducing the standard 160-pixel back porch to 156 keeps the
// horizontal and vertical scan rates at 48.358 kHz and 59.998 Hz. The 960x720
// image is centered inside the active raster with 32/24-pixel black borders.
const video_mode_t video_mode_1024x768_p = {
    .h_front_porch = 24,
    .h_sync_width = 136,
    .h_back_porch = 156,
    .h_active_pixels = 1024,
    .v_front_porch = 3,
    .v_sync_width = 6,
    .v_back_porch = 29,
    .v_active_lines = 768,
    .h_total_pixels = 1340,
    .v_total_lines = 806,
    .hstx_clk_div = 1,
    .hstx_csr_clkdiv = 5,
    .hsync_positive = false,
    .vsync_positive = false,
    .data_island_in_hsync = true,
};

const video_mode_t *video_output_active_mode = &video_mode_480_p;

static video_output_hstx_source_t hstx_clock_source = VIDEO_OUTPUT_HSTX_SOURCE_CLK_SYS;
static uint32_t hstx_clock_source_hz;

static void configure_hstx_clock(const video_mode_t *mode)
{
    uint32_t auxsrc = CLOCKS_CLK_HSTX_CTRL_AUXSRC_VALUE_CLK_SYS;
    uint32_t source_hz = clock_get_hz(clk_sys);
    if (hstx_clock_source == VIDEO_OUTPUT_HSTX_SOURCE_PLL_USB) {
        auxsrc = CLOCKS_CLK_HSTX_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB;
        source_hz = hstx_clock_source_hz;
    }
    hard_assert(source_hz != 0U);
    clock_configure_int_divider(clk_hstx, 0, auxsrc, source_hz, mode->hstx_clk_div);
}

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

// Max active pixels across all modes (1280 for 240p/720p).
// FEASIBILITY SPIKE (PICO_HDMI_PIXEL_FORMAT_RGB888, default OFF): one 32-bit
// RGB888 word per pixel instead of two packed 16-bit RGB565 pixels per word,
// so the buffer element type/width doubles in bytes (1280 x uint32_t vs
// 1280 x uint16_t). Element count stays 1280 either way -- it is a pixel
// count in the RGB888 case and a halfword (2px) count in the RGB565 case.
#if PICO_HDMI_PIXEL_FORMAT_RGB888
static uint32_t PICO_HDMI_LINE_BUFFER_ATTR line_buffer[1280] __attribute__((aligned(4)));
#else
static uint16_t PICO_HDMI_LINE_BUFFER_ATTR line_buffer[1280] __attribute__((aligned(4)));
#endif

#if PICO_HDMI_ACTIVE_LINE_DOUBLE_BUFFER
// ============================================================================
// Pipelined active-line double buffering (feasibility spike, default OFF).
//
// Only engages for the mode descriptor whose v_active_lines==720 (3x
// vertical scale, e.g. runtime 720p); 2x/4x modes always stay on the single
// `line_buffer` path above, even when this option is compiled in.
//
// While DMA scans db_buffers[db_front_idx] ("front") for the 3 physical
// lines of one group, the ISR arms ONE deferred fill request (at the
// group's first physical line) asking for the group 3 lines ahead to be
// rendered into the other buffer ("back"). Arming happens in
// video_output_handle_active_start() at the group boundary, but the request
// is only actually queued (db_fill_pending set, fill IRQ pended) later, from
// video_output_handle_active_data() for that SAME line -- see db_fill_armed
// below for why. That fill is serviced from the Core 1 background loop in
// video_output_core1_run() -- thread context, not this ISR -- so the
// (expensive) firmware scanline callback gets a full 3-line-period
// wall-clock budget instead of the single-segment budget every other ISR
// reprogram is bound to. Group 0 has no prior group to have
// pipelined it, so it is prefilled synchronously in the ISR during the last
// back-porch line, at the same single-line budget the non-pipelined path
// always had (see dma_irq_handler / video_output_prefill_group0()).
//
// Second buffer deliberately plain BSS, not PICO_HDMI_LINE_BUFFER_ATTR
// (scratch_y): at RGB888 each buffer is 1280 x uint32_t = 5120 bytes, and
// scratch_y is only 4 KB total -- two buffers would not fit.
#if PICO_HDMI_PIXEL_FORMAT_RGB888
static uint32_t line_buffer_b[1280] __attribute__((aligned(4)));
#else
static uint16_t line_buffer_b[1280] __attribute__((aligned(4)));
#endif
static uint32_t *const db_buffers[2] = {(uint32_t *)line_buffer, (uint32_t *)line_buffer_b};
// True only while the active mode's v_active_lines==720 (set in
// init_rt_from_mode()); other modes always take the single-buffer path.
static bool rt_double_buffer_active;
// Which of db_buffers[] DMA is currently scanning ("front"); updated only at
// the start of each 3-line group, synchronously in the ISR.
static uint8_t db_front_idx;
// Deferred-fill request: armed by the ISR at a group boundary, consumed by
// video_output_service_double_buffer_fill() from the Core 1 background loop.
static volatile bool db_fill_pending;
static uint32_t *db_fill_dst;
static uint32_t db_fill_v_scanline;
static uint32_t db_fill_active_line;
// Set by video_output_handle_active_start() at a group boundary once
// db_fill_dst/db_fill_v_scanline/db_fill_active_line are stashed, but before
// it is safe to let anything act on them: at that exact point db_fill_dst
// (the old front buffer, about to become the new back buffer) is still being
// drained by the in-flight DATA segment of the group's just-finished last
// line -- this ISR call always races that DMA transfer, a consequence of the
// ping-pong DMA scheduling in dma_irq_handler() below (each IRQ reprograms
// the channel that just finished for the job after next, while the other
// channel -- already carrying the previous job -- is running concurrently).
// Arming here instead of queuing directly avoids the fill IRQ overwriting it
// while DMA is still reading it. video_output_handle_active_data(), called
// on the very next ISR invocation for the same v_scanline (active_start and
// active_data always alternate one v_scanline apart by construction), turns
// an armed request into db_fill_pending + irq_set_pending() once that DATA
// segment has provably completed. Cleared there, and on resync.
static bool db_fill_armed;
// User IRQ that runs the deferred fill promptly, at low priority, instead of
// waiting for the next Core 1 background-loop iteration. That loop also runs
// the audio pipeline (polling PIO, SRC, TERC4), whose work items can block
// for tens of microseconds -- long enough to blow the fill's ~48us budget
// (fill ~6150 cycles, window 3 lines * 7200 cycles @ 320 MHz = 21600 cycles;
// start latency > ~15450 cycles misses the swap). A missed fill makes DMA
// scan a partially-updated buffer left-to-right, matching the observed
// left-half-biased tearing. Claimed once in video_output_core1_run(); user
// IRQs are core-local on RP2350 (Cortex-M33 per-core NVIC), so this can't
// collide with anything Core 0 claims. Priority is set clearly below
// DMA_IRQ_0 (0, highest) so the scanout ISR always preempts it.
static uint db_fill_irq_num = (uint)-1;
#endif // PICO_HDMI_ACTIVE_LINE_DOUBLE_BUFFER

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

#if PICO_HDMI_VBLANK_HTRIM
// 240p elastic single-line vblank trim template: a private copy of
// vblank_di_null whose tail RAW_REPEAT word alone carries the whole frame's
// htrim correction at 240p (see htrim_elastic_mode below).
static uint32_t vblank_elastic[128] HSTX_CMDLIST_ATTR;
static uint32_t vblank_elastic_len;
#endif

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
    pico_hdmi_hstx_disconnect_pins();

    // 3. Disable HSTX (resets shift register, clock generator, and flushes FIFO)
    hstx_ctrl_hw->csr &= ~HSTX_CTRL_CSR_EN_BITS;

    // Small delay to ensure HSTX fully stops
    __asm volatile("nop\nnop\nnop\nnop");

    // 4. Reset state to start of frame
    v_scanline = 0;
    vactive_cmdlist_posted = false;
    dma_pong = false;
#if PICO_HDMI_ACTIVE_LINE_DOUBLE_BUFFER
    // A resync can interrupt mid-frame: reset the pipelined double-buffer
    // state too, so the next frame doesn't scan a stale front buffer or
    // service a fill request queued against the old v_scanline timeline. The
    // group-0 prefill (dma_irq_handler / video_output_prefill_group0()) always
    // reruns before the next active_line==0 -- it fires on plain v_scanline
    // equality once per revolution -- so this just guarantees clean state in
    // the meantime. db_fill_armed must also be cleared: otherwise a request
    // armed by handle_active_start just before the resync would still fire
    // (from the next handle_active_data call after resync) against a
    // v_scanline timeline, and a db_front_idx, that the reset above just
    // invalidated.
    db_front_idx = 0;
    db_fill_pending = false;
    db_fill_armed = false;
#endif

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
    pico_hdmi_hstx_connect_pins(false);
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
#define HTRIM_LIMIT_PX 30
static uint32_t *htrim_word[HTRIM_MAX_SLOTS];
static uint16_t htrim_base[HTRIM_MAX_SLOTS];
static uint8_t htrim_slots;
static int16_t htrim_px;

#if PICO_HDMI_VBLANK_HTRIM
// ============================================================================
// 240p elastic single-line vblank trim (see video_output_set_vblank_htrim_px()
// and video_output_handle_blanking()).
//
// STATUS: RETIRED / DORMANT (see build_all_command_lists(), which now forces
// htrim_elastic_mode = false unconditionally). Hardware testing showed the
// 240p artifact was byte-identical across broken-trim, uniform-trim, and
// elastic-trim builds -- trim distribution was never the cause. Root cause
// was the nonstandard 266-line genlock raster; the fix was retiming
// video_mode_240_p's base raster to 1613x264 (see its definition above), at
// which uniform trim is only ~2 px and this machinery is unnecessary. Kept
// compiled (not deleted) for possible future reuse.
//
// Original rationale, kept for reference: at 240p the blanking region is
// only ~22-24 lines (vs. ~52 at 480p / more at 720p); spreading a uniform
// +-px trim across every one of them creates a visible H-period sawtooth
// that some sinks' H-PLL tracks (bottom-half image bounce, confirmed on
// hardware even through a re-timing scaler). Concentrating the whole
// per-frame correction (px * blank-line count) into ONE ordinary-blanking
// line instead keeps every other blanking line at exactly h_total px.
//
// 240p line map (retimed nominal rt_v_total_lines = 264; see
// get_scanline_state()):
//   v_scanline 0          -> AVI infoframe (video_output_handle_blanking() v_scanline==0 branch)
//   v_scanline 1..3       -> ordinary blanking (front-porch tail)  <- elastic line = 1
//   v_scanline 4..7       -> vsync (video_output_handle_vsync(), ACR-on / plain vsync)
//   v_scanline 8,12,16,20 -> ACR (back porch, send_acr)
//   v_scanline 9..23 (non-ACR) -> ordinary blanking (back porch)
//   v_scanline 24..263    -> active video
// Line 1 is the first ordinary-blanking line after active video (line 0 is
// reserved for the AVI infoframe) and is visited every frame regardless of
// the genlock's runtime +-1 rt_v_total_lines step: wraparound always revisits
// v_scanline 0..3 first (see dma_irq_handler()'s wrap logic), independent of
// where the genlock's extra/missing line lands at the bottom of the frame.
// ============================================================================
static bool htrim_elastic_mode;
static uint16_t htrim_elastic_scanline;
static uint16_t htrim_elastic_gain; // = mode->v_total_lines - mode->v_active_lines
static int32_t htrim_elastic_total; // = htrim_px * htrim_elastic_gain
static uint16_t htrim_elastic_base; // vblank_elastic's tail count before patching
#endif

// `slots` is a caller-owned counter, NOT the published `htrim_slots`: this
// lets htrim_register_all() build the whole table into htrim_word/htrim_base
// while `htrim_slots` (read by video_output_set_vblank_htrim_px(), which can
// race in from the vsync callback) stays at its old, fully-valid value until
// the rebuild is complete -- see htrim_register_all().
static void htrim_register(uint32_t *buf, uint32_t len, uint8_t *slots)
{
    // Mode-derived match window: the trailing "big repeat" segment of a
    // blanking line always carries the bulk of the line (back porch, plus
    // active pixels for non-active templates), so it is always > half a
    // line and always < one full line. A fixed 1200-1600 px window only
    // ever matched the (now-deleted) 720p timing; at 480p the big repeat is
    // 688 px and never matched, silently disabling the whole servo.
    const uint32_t rt_h_total =
        (uint32_t)rt_h_front_porch + rt_h_sync_width + rt_h_back_porch + rt_h_active_pixels;
    for (uint32_t i = 0; i < len && *slots < HTRIM_MAX_SLOTS; i++) {
        const uint32_t count = buf[i] & 0xFFFU;
        if ((buf[i] >> 12) == 0x1 && count >= (rt_h_total / 2) && count < rt_h_total) {
            htrim_word[*slots] = &buf[i];
            htrim_base[*slots] = (uint16_t)(buf[i] & 0xFFFU);
            (*slots)++;
        }
    }
}

static void htrim_register_all(void)
{
    // Preserve whatever trim the servo already applied: this function is
    // re-invoked mid-stream (build_all_command_lists(), video_output_update_acr())
    // whenever templates are rebuilt, and a rebuild must not reset the servo
    // to zero mid-genlock.
    const int prev_px = htrim_px;
    htrim_slots = 0;
    htrim_px = 0;
    uint8_t slots = 0;
    htrim_register(vblank_line_vsync_off, 9, &slots);
    htrim_register(vblank_line_vsync_on, 9, &slots);
    htrim_register(vblank_acr_vsync_on, vblank_acr_vsync_on_len, &slots);
    htrim_register(vblank_acr_vsync_off, vblank_acr_vsync_off_len, &slots);
    htrim_register(vblank_infoframe_vsync_on, vblank_infoframe_vsync_on_len, &slots);
    htrim_register(vblank_infoframe_vsync_off, vblank_infoframe_vsync_off_len, &slots);
    htrim_register(vblank_avi_infoframe, vblank_avi_infoframe_len, &slots);
    htrim_register(genlock_acr_vsync_on, genlock_acr_vsync_on_len, &slots);
    htrim_register(genlock_acr_vsync_off, genlock_acr_vsync_off_len, &slots);
#if PICO_HDMI_VBLANK_HTRIM
    // vblank_di_null is the silent-vblank line actually posted by the
    // non-precomposed HDMI path (video_output_handle_blanking()) whenever no
    // audio packet is pending; its tail repeat must track the servo too, or
    // silent lines fall back to the untrimmed period.
    htrim_register(vblank_di_null, vblank_di_null_len, &slots);
#endif
#if PICO_HDMI_PRECOMPOSED_ACTIVE_LINES
    // The DI vblank templates are persistent in precomposed mode (the ISR
    // patches island words in place, never the tail repeat) and MUST be
    // trimmed too: trimming only the non-DI lines imposes a line-to-line
    // H-period sawtooth inside every vblank (1650 vs 1650+trim px), which
    // sink H-PLLs visibly track once |trim| exceeds ~14 px (bench-observed).
    // Uniform trim removes the alternation and triples the authority.
    // False positives are impossible: TERC4 payload words always carry
    // high bits (3x10-bit lanes), so they can never match a bare
    // RAW_REPEAT command in the matched window.
    htrim_register(vblank_di_ping, precomposed_vblank_len, &slots);
    htrim_register(vblank_di_pong, precomposed_vblank_len, &slots);
    htrim_register(vblank_di_null, vblank_di_null_len, &slots);
#endif
    // Publish the rebuilt table atomically: __dmb() orders the htrim_word/
    // htrim_base writes above ahead of the htrim_slots publish below, so a
    // concurrent video_output_set_vblank_htrim_px() either still sees the old
    // (small, valid) slot count or the new one -- never a torn table.
    __dmb();
    htrim_slots = slots;
    if (prev_px) {
        video_output_set_vblank_htrim_px(prev_px);
    }
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
#if PICO_HDMI_VBLANK_HTRIM
    if (htrim_elastic_mode) {
        // 240p: the whole frame's correction lands on ONE line (see the
        // elastic-mode comment above) instead of being spread across every
        // registered template -- do not touch htrim_word[]/htrim_base[], so
        // all other blanking templates stay at their untrimmed base count.
        // 109 (h_back_porch) + 1280 (h_active_pixels) + 30 (HTRIM_LIMIT_PX) *
        // 24 (retimed 240p blank-line count, v_total 264 - v_active 240) =
        // 2109, well inside the RAW_REPEAT 12-bit count field (max 4095).
        htrim_elastic_total = (int32_t)htrim_px * (int32_t)htrim_elastic_gain;
        vblank_elastic[vblank_elastic_len - 3] =
            (0x1U << 12) | (uint32_t)((int32_t)htrim_elastic_base + htrim_elastic_total);
        return;
    }
#endif
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
#if PICO_HDMI_ACTIVE_LINE_DOUBLE_BUFFER
        if (rt_double_buffer_active) {
            if ((active_line % 3U) == 0U) {
                if (active_line != 0U) {
                    // Group advances: the back buffer (filled ahead of time
                    // by the background loop) becomes the new front.
                    db_front_idx = (uint8_t)(db_front_idx ^ 1U);
                }
                const uint32_t lookahead = active_line + 3U;
                if (lookahead < rt_v_active_lines) {
                    // Stash a deferred fill of the (new) back buffer with the
                    // NEXT group's content, but do NOT queue it yet: right
                    // here, db_buffers[db_front_idx ^ 1] (the fill target) is
                    // still being drained by the in-flight DATA segment of
                    // the group that just ended -- queuing now would let the
                    // fill IRQ start overwriting it while DMA is still
                    // reading it (see db_fill_armed's declaration comment).
                    // video_output_handle_active_data() turns this into the
                    // actual db_fill_pending + irq_set_pending() once that
                    // DATA segment has provably completed. Serviced from the
                    // Core 1 background loop (thread context), not this ISR,
                    // so it gets a full 3-line-period budget instead of 1.
                    db_fill_dst = db_buffers[db_front_idx ^ 1U];
                    db_fill_v_scanline = rt_blank_head + lookahead;
                    db_fill_active_line = lookahead;
                    __compiler_memory_barrier();
                    db_fill_armed = true;
                }
                // else: last group in the frame -- nothing further to prefetch.
            }
            // active_line % 3 != 0: the front buffer already holds this
            // group's content (filled 3 lines-worth of wall clock ago).
        } else
#endif
        {
            uint32_t *dst32 = (uint32_t *)line_buffer;
            if (scanline_callback) {
                scanline_callback(v_scanline, active_line, dst32);
            } else {
#if PICO_HDMI_PIXEL_FORMAT_RGB888
                // One 32-bit RGB888 word per pixel.
                for (uint32_t i = 0; i < rt_h_active_pixels; i++) {
                    dst32[i] = 0;
                }
#else
                for (uint32_t i = 0; i < rt_h_active_pixels / 2; i++) {
                    dst32[i] = 0;
                }
#endif
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
#if PICO_HDMI_VBLANK_HTRIM
                // This line is freshly built every call (unlike the static
                // templates htrim_register_all() patches directly), so its
                // tail repeat starts back at the untrimmed base count. Apply
                // the current servo trim here too, or every DI-bearing
                // vblank line reverts to the untrimmed period, producing an
                // H-period sawtooth versus the trimmed silent/template
                // lines. Tail is always the last 3 words for both
                // build_line_with_di() and build_line_with_di_backporch()
                // when active==false: [RAW_REPEAT|count, sync, NOP].
                //
                // 240p elastic mode: only the single designated line carries
                // the (much larger) whole-frame correction; every other
                // ordinary-blanking line -- including dynamically-built
                // audio-DI lines -- stays untouched at its base period.
                if (htrim_elastic_mode) {
                    if (v_scanline == htrim_elastic_scanline) {
                        buf[vblank_di_len - 3] += (uint32_t)htrim_elastic_total;
                    }
                } else {
                    buf[vblank_di_len - 3] += (uint32_t)(int32_t)htrim_px;
                }
#endif
                ch->read_addr = (uintptr_t)buf;
                ch->transfer_count = vblank_di_len;
            } else {
#if PICO_HDMI_VBLANK_HTRIM
                if (htrim_elastic_mode && v_scanline == htrim_elastic_scanline) {
                    ch->read_addr = (uintptr_t)vblank_elastic;
                    ch->transfer_count = vblank_elastic_len;
                } else
#endif
                {
                    ch->read_addr = (uintptr_t)vblank_di_null;
                    ch->transfer_count = vblank_di_null_len;
                }
            }
#endif
        }
    }
}

#if PICO_HDMI_ACTIVE_LINE_DOUBLE_BUFFER
// Turns an armed group-boundary fill request into the real db_fill_pending +
// irq_set_pending(). Deliberately out-of-line and __scratch_y (not
// __scratch_x): SCRATCH_X is shared with the Core 1 stack (.stack1_dummy)
// and was already within ~12 bytes of its link-time budget before this
// existed, so the state-transition body -- taken only once per 3-line group
// (240 times/frame at 720p), never on the other two lines -- is pushed to
// the roomier scratch_y bank instead. video_output_handle_active_data()
// keeps only the single flag test that guards the call.
static void __attribute__((noinline, noclone)) __scratch_y("") db_fill_arm_fire(void)
{
    db_fill_armed = false;
    __compiler_memory_barrier();
    db_fill_pending = true;
    // Wake the low-priority fill IRQ immediately rather than waiting for the
    // next background-loop poll (see db_fill_irq_num / db_fill_irq_handler).
    irq_set_pending(db_fill_irq_num);
}
#endif

static inline void __scratch_x("") video_output_handle_active_data(dma_channel_hw_t *ch)
{
#if PICO_HDMI_PRECOMPOSED_ACTIVE_LINES
    ch->read_addr = (uintptr_t)active_data_ptr;
#elif PICO_HDMI_ACTIVE_LINE_DOUBLE_BUFFER
    ch->read_addr = rt_double_buffer_active ? (uintptr_t)db_buffers[db_front_idx] : (uintptr_t)line_buffer;
#else
    ch->read_addr = (uintptr_t)line_buffer;
#endif
#if PICO_HDMI_PIXEL_FORMAT_RGB888
    // RGB888 feasibility spike: one 32-bit word per pixel, no packing.
    ch->transfer_count = rt_h_active_pixels;
#else
    // 32-bit mode: h_active/2 words of pre-expanded pixel pairs. Native
    // 16-bit mode: the same count as halfword transfers, bus-replicated.
    ch->transfer_count = (rt_h_active_pixels * sizeof(uint16_t)) / sizeof(uint32_t);
#endif
#if PICO_HDMI_ACTIVE_LINE_DOUBLE_BUFFER
    // By ping-pong construction, this call always runs exactly one ISR
    // invocation after the video_output_handle_active_start() that armed a
    // group-boundary fill (active_start/active_data for a given line always
    // fire on consecutive ISR calls, one v_scanline apart) -- i.e. right as
    // the DATA segment this very call is about to reprogram AWAY FROM (the
    // group's just-finished last line, occupying the fill's target buffer)
    // has provably completed. That makes this the first safe point to
    // publish db_fill_pending and wake the fill IRQ. Non-group lines pay one
    // flag test (the state transition itself lives in the out-of-line,
    // scratch_y db_fill_arm_fire() -- see its declaration comment).
    if (db_fill_armed) {
        db_fill_arm_fire();
    }
#endif
}

#if PICO_HDMI_ACTIVE_LINE_DOUBLE_BUFFER
// Fill db_buffers[0] with the first group's content (active_line 0),
// synchronously in the ISR, during the last back-porch line -- there is no
// prior group to have pipelined it ahead of time. This runs once per frame,
// at the same per-line budget the non-pipelined path always had; every other
// group gets the full 3-line-period budget via the deferred path below.
// Must run after video_pipeline_vsync_callback() has latched the new frame's
// ring start: that callback fires earlier, at v_scanline==rt_v_front_porch
// (see video_output_handle_vsync()), well before this (the last back-porch
// line), so group 0 reads the right frame's data.
static void __scratch_x("") video_output_prefill_group0(void)
{
    db_front_idx = 0;
    db_fill_pending = false;
    uint32_t *dst32 = db_buffers[0];
    if (scanline_callback) {
        scanline_callback(rt_blank_head, 0, dst32);
    } else {
#if PICO_HDMI_PIXEL_FORMAT_RGB888
        for (uint32_t i = 0; i < rt_h_active_pixels; i++) {
            dst32[i] = 0;
        }
#else
        for (uint32_t i = 0; i < rt_h_active_pixels / 2; i++) {
            dst32[i] = 0;
        }
#endif
    }
}

// Consume a deferred fill request queued by the ISR at a group boundary.
// Called from two contexts on the same core: the low-priority db_fill_irq_num
// IRQ (db_fill_irq_handler, below) -- the normal path, entered almost
// immediately after dma_irq_handler sets db_fill_pending and marks the IRQ
// pending -- and, as a belt-and-suspenders fallback, the Core 1 background
// loop in video_output_core1_run(). Either caller CAN be interrupted
// (repeatedly) by dma_irq_handler without missing any ISR deadline, since
// none of the fast per-segment ISR reprograms call into this. It only needs
// to finish before the group it is filling becomes the front buffer, ~3
// line-periods away.
static void video_output_service_double_buffer_fill(void)
{
    // Test-and-clear must be atomic across the two calling contexts above:
    // a plain "if (pending) { pending = false; ... }" could let the IRQ
    // interrupt the background-loop call between the check and the clear,
    // and both would then service (and clear) the same request. Same-core,
    // so a short critical section is enough -- no spinlock needed.
    uint32_t irq_state = save_and_disable_interrupts();
    bool pending = db_fill_pending;
    db_fill_pending = false;
    restore_interrupts(irq_state);
    if (!pending) {
        return;
    }
    __compiler_memory_barrier();
    uint32_t *dst32 = db_fill_dst;
    const uint32_t v = db_fill_v_scanline;
    const uint32_t al = db_fill_active_line;
    if (scanline_callback) {
        scanline_callback(v, al, dst32);
    } else {
#if PICO_HDMI_PIXEL_FORMAT_RGB888
        for (uint32_t i = 0; i < rt_h_active_pixels; i++) {
            dst32[i] = 0;
        }
#else
        for (uint32_t i = 0; i < rt_h_active_pixels / 2; i++) {
            dst32[i] = 0;
        }
#endif
    }
}

// Exclusive handler for db_fill_irq_num. Runs the deferred fill in IRQ
// context at low priority (below DMA_IRQ_0) -- which is the pre-double-buffer
// calling convention for the firmware scanline callback (it always ran from
// dma_irq_handler before this pipelining existed), so this is a return to
// known-safe territory for that callback, not new ground: no locks, no
// blocking calls, LUT reads + stores only, under RGB888.
static void db_fill_irq_handler(void)
{
    video_output_service_double_buffer_fill();
}
#endif // PICO_HDMI_ACTIVE_LINE_DOUBLE_BUFFER

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
#if PICO_HDMI_ACTIVE_LINE_DOUBLE_BUFFER
        if (rt_double_buffer_active && v_scanline == (uint32_t)(rt_blank_head - 1)) {
            video_output_prefill_group0();
        }
#endif
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
    uint32_t pixel_clock = clock_get_hz(clk_hstx) / video_output_active_mode->hstx_csr_clkdiv;
    *cts = (uint32_t)(((uint64_t)pixel_clock * *n) / (128ULL * sample_rate));
}

static void configure_audio_packets(uint32_t sample_rate)
{
    hstx_di_queue_set_sample_rate(sample_rate);

    // Override samples_per_line with pixel-clock-accurate value.
    // The default set_sample_rate() assumes exactly 60 Hz, which is wrong
    // for 240p (60.114 Hz). Derive from actual timing instead:
    //   samples_per_line = sample_rate * h_total_pixels / pixel_clock
    uint32_t pixel_clock_hz = clock_get_hz(clk_hstx) / video_output_active_mode->hstx_csr_clkdiv;
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
    hstx_packet_set_sync_polarity(mode->hsync_positive, mode->vsync_positive);
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
    rt_sync_after_di = mode->data_island_in_hsync ? mode->h_sync_width - W_PREAMBLE - W_DATA_ISLAND : 0;
#if PICO_HDMI_ACTIVE_LINE_DOUBLE_BUFFER
    // Gated on the mode descriptor, not a firmware "3x" concept the library
    // doesn't know about: only the 720p-class mode (v_active_lines==720) is
    // pipelined. 2x (480p) and 4x (240p) always take the single-buffer path.
    rt_double_buffer_active = (mode->v_active_lines == 720U);
#endif
}

static void build_all_command_lists(const video_mode_t *mode)
{
    build_dvi_command_lists();

#if defined(PICO_HDMI_DUMP_COMMAND_LISTS) && PICO_HDMI_DUMP_COMMAND_LISTS
    dump_runtime_command_lists(mode);
#endif

    // AVI InfoFrame: 480p remains CTA VIC 1. The 1280x720 descriptor is
    // exact-clock CVT-RB, so it uses VIC 0 with explicit 16:9 metadata.
    hstx_packet_t packet;
    hstx_data_island_t island;
    const bool mode_is_240p = mode->v_active_lines == 240 && mode->h_active_pixels == 1280;
    const bool mode_is_720p_rb = mode->v_active_lines == 720 && mode->h_active_pixels == 1280;
#if PICO_HDMI_VBLANK_HTRIM
    // Retired on hardware evidence: elastic single-line trim did not fix the
    // 240p artifact (symptom was byte-identical across broken-trim,
    // uniform-trim, and elastic-trim builds), so trim distribution was never
    // the cause -- the nonstandard genlock raster was (see video_mode_240_p
    // above, now retimed to 1613x264). Uniform trim at the retimed raster is
    // ~2 px, sub-threshold, so 240p reverts to the same uniform-trim path as
    // 480p/720p. Machinery kept compiled but dormant for possible reuse.
    htrim_elastic_mode = false;
    if (htrim_elastic_mode) {
        htrim_elastic_scanline = 1; // see line-map comment above htrim_elastic_mode
        htrim_elastic_gain = (uint16_t)(mode->v_total_lines - mode->v_active_lines);
    }
#endif
    uint8_t vic = (mode->v_active_lines == 480 && mode->h_active_pixels == 640) ? 1 : 0;
#if PICO_HDMI_LEGACY_240P_AVI_INFOFRAME
    uint8_t pixel_repetition = 0;
#else
    uint8_t pixel_repetition = mode_is_240p ? 3 : 0;
#endif
    const bool di_hsync_active = data_island_hsync_active();
    if (mode_is_240p) {
        hstx_packet_set_avi_infoframe(&packet, vic, pixel_repetition);
    } else {
        hstx_packet_set_avi_infoframe_aspect(&packet, vic, pixel_repetition, mode_is_720p_rb);
    }
    hstx_encode_data_island(&island, &packet, false, di_hsync_active);
    vblank_avi_infoframe_len = BUILD_LINE_WITH_DI(vblank_avi_infoframe, island.words, false, false);

    // Null DI command lists
    vblank_di_null_len =
        BUILD_LINE_WITH_DI(vblank_di_null, hstx_get_null_data_island(false, di_hsync_active), false, false);
    vactive_di_null_len =
        BUILD_LINE_WITH_DI(vactive_di_null, hstx_get_null_data_island(false, di_hsync_active), false, true);

    vblank_di_len = BUILD_LINE_WITH_DI(vblank_di_ping, hstx_get_null_data_island(false, di_hsync_active), false, false);
    memcpy(vblank_di_pong, vblank_di_ping, sizeof(vblank_di_ping));

#if PICO_HDMI_VBLANK_HTRIM
    // Elastic 240p vblank line: a private copy of vblank_di_null. Only its
    // tail word is ever patched (video_output_set_vblank_htrim_px()), and
    // only when htrim_elastic_mode is set; harmless to build unconditionally
    // at every mode apply.
    vblank_elastic_len = vblank_di_null_len;
    memcpy(vblank_elastic, vblank_di_null, sizeof(uint32_t) * vblank_di_null_len);
    htrim_elastic_base = (uint16_t)(vblank_elastic[vblank_elastic_len - 3] & 0xFFFU);

    // Re-scan every template just (re)built above for htrim slots. Runs on
    // mode apply / init only, never per-line.
    htrim_register_all();
#endif
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

    // Configure clk_hstx from the application's selected parent clock.
    configure_hstx_clock(initial_mode);

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
#if PICO_HDMI_PIXEL_FORMAT_RGB888
    // FEASIBILITY SPIKE: one 32-bit 0x00RRGGBB word per pixel (RP2350
    // datasheet Sec 12.11). NBITS=7 encodes 8 valid bits per lane; lane
    // assignment stays L0=blue, L1=green, L2=red. Each lane's NBITS bits end
    // at bit 7 of its right-rotated word, so for 0x00RRGGBB: L0 (bits 7:0)
    // ROT=0, L1 (bits 15:8) ROT=8, L2 (bits 23:16) ROT=16.
    hstx_ctrl_hw->expand_tmds = 7 << HSTX_CTRL_EXPAND_TMDS_L2_NBITS_LSB | 16 << HSTX_CTRL_EXPAND_TMDS_L2_ROT_LSB |
                                7 << HSTX_CTRL_EXPAND_TMDS_L1_NBITS_LSB | 8 << HSTX_CTRL_EXPAND_TMDS_L1_ROT_LSB |
                                7 << HSTX_CTRL_EXPAND_TMDS_L0_NBITS_LSB | 0 << HSTX_CTRL_EXPAND_TMDS_L0_ROT_LSB;

    // One pixel per FIFO word: ENC_N_SHIFTS=1, ENC_SHIFT=0. RAW_ fields are
    // for control-symbol (Data Island) words and are unchanged.
    hstx_ctrl_hw->expand_shift =
        1 << HSTX_CTRL_EXPAND_SHIFT_ENC_N_SHIFTS_LSB | 0 << HSTX_CTRL_EXPAND_SHIFT_ENC_SHIFT_LSB |
        1 << HSTX_CTRL_EXPAND_SHIFT_RAW_N_SHIFTS_LSB | 0 << HSTX_CTRL_EXPAND_SHIFT_RAW_SHIFT_LSB;
#else
    hstx_ctrl_hw->expand_tmds = 4 << HSTX_CTRL_EXPAND_TMDS_L2_NBITS_LSB | 8 << HSTX_CTRL_EXPAND_TMDS_L2_ROT_LSB |
                                5 << HSTX_CTRL_EXPAND_TMDS_L1_NBITS_LSB | 3 << HSTX_CTRL_EXPAND_TMDS_L1_ROT_LSB |
                                4 << HSTX_CTRL_EXPAND_TMDS_L0_NBITS_LSB | 29 << HSTX_CTRL_EXPAND_TMDS_L0_ROT_LSB;

    hstx_ctrl_hw->expand_shift =
        2 << HSTX_CTRL_EXPAND_SHIFT_ENC_N_SHIFTS_LSB | 16 << HSTX_CTRL_EXPAND_SHIFT_ENC_SHIFT_LSB |
        1 << HSTX_CTRL_EXPAND_SHIFT_RAW_N_SHIFTS_LSB | 0 << HSTX_CTRL_EXPAND_SHIFT_RAW_SHIFT_LSB;
#endif

    // NOTE: csr.N_SHIFTS/SHIFT below govern the TMDS *output* side (3 lanes x
    // 10-bit symbols, 2 raw bits shifted out per HSTX clock in DDR) and are
    // independent of expand_tmds/expand_shift, which govern the *input* pixel
    // expansion feeding the TMDS encoder. Do not change csr for pixel format.
    hstx_ctrl_hw->csr = 0;
    hstx_ctrl_hw->csr = HSTX_CTRL_CSR_EXPAND_EN_BITS |
                        (uint32_t)video_output_active_mode->hstx_csr_clkdiv << HSTX_CTRL_CSR_CLKDIV_LSB |
                        5U << HSTX_CTRL_CSR_N_SHIFTS_LSB | 2U << HSTX_CTRL_CSR_SHIFT_LSB | HSTX_CTRL_CSR_EN_BITS;

    pico_hdmi_hstx_configure_pinout();

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

#if PICO_HDMI_ACTIVE_LINE_DOUBLE_BUFFER
    // Deferred-fill IRQ (see db_fill_irq_num / db_fill_irq_handler above).
    // Claimed once per Core 1 launch. Priority 0xC0 is strictly below
    // DMA_IRQ_0's 0 (numerically lower = higher priority on this NVIC), so
    // DMA_IRQ_0 always preempts it, but it still beats waiting for the next
    // background-loop poll.
    db_fill_irq_num = (uint)user_irq_claim_unused(true);
    irq_set_exclusive_handler(db_fill_irq_num, db_fill_irq_handler);
    irq_set_priority(db_fill_irq_num, 0xC0);
    irq_set_enabled(db_fill_irq_num, true);
#endif

    bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_W_BITS | BUSCTRL_BUS_PRIORITY_DMA_R_BITS;
    dma_channel_start(DMACH_PING);

    // Wait for first DMA transfer to complete — HSTX serializes valid TMDS
    // internally but GPIO isn't connected yet, so TV sees nothing.
    while (dma_channel_is_busy(DMACH_PING))
        tight_loop_contents();

    // NOW connect GPIO — TV's first TMDS exposure is valid data
    pico_hdmi_hstx_connect_pins(true);

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

#if PICO_HDMI_ACTIVE_LINE_DOUBLE_BUFFER
        // Service any pending pipelined-scanout fill request before the
        // (firmware-supplied) background_task(), so it isn't starved by
        // whatever that does.
        video_output_service_double_buffer_fill();
#endif

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

void video_output_set_hstx_source(video_output_hstx_source_t source, uint32_t source_hz)
{
    hstx_clock_source = source;
    hstx_clock_source_hz = (source == VIDEO_OUTPUT_HSTX_SOURCE_PLL_USB) ? source_hz : 0U;
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
    configure_hstx_clock(video_output_active_mode);
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

#if PICO_HDMI_VBLANK_HTRIM
    // genlock_acr_vsync_on/off are built later than the mode-apply templates
    // (this is called after the sys_clk genlock adjustment, well after
    // build_all_command_lists()); re-scan now to pick up their htrim slots.
    // htrim_register_all() preserves whatever trim is already applied.
    htrim_register_all();
#endif
}

void video_output_request_resync(void)
{
    resync_requested = true;
    __dmb();
}
