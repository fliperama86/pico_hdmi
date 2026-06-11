[![Discord](https://img.shields.io/discord/1469402044845527253)](https://discord.gg/NqK9m8CBPT)

# PicoHDMI
An HSTX-native HDMI output library for the RP2350 (Raspberry Pi Pico 2).

> **2.0 beta notice**: the `2.0-beta` branch/tag contains the newer opt-in scanout/audio pipeline described below. It is intended for testing and performance-sensitive projects, but it is not yet the recommended stable baseline for every application. If you need the older stable API/behavior, use [`v0.0.6`](https://github.com/fliperama86/pico_hdmi/releases/tag/v0.0.6) or the [`v0.0.6` source tree](https://github.com/fliperama86/pico_hdmi/tree/v0.0.6).

PicoHDMI leverages the RP2350's dedicated **HSTX (High-Speed Transmit)** peripheral with hardware TMDS encoding. No bit-banging, no overclocking required: just near-zero CPU overhead for video output.

## Overview

`pico_hdmi` provides a high-performance video and audio output pipeline using the RP2350's HSTX hardware. It is designed to be decoupled from specific application logic, focusing strictly on the generation of stable TMDS signals and Data Island injection (e.g., for audio).

## Key Features

- **HSTX Hardware TMDS Encoding**: Uses the native TMDS encoder for zero-CPU video serialization.
- **Audio Data Islands**: Built-in support for TERC4 encoding and scheduled injection of audio samples.
- **Data Island Queue**: Lock-free queue for asynchronous packet posting from other cores.
- **Double-Buffered DMA**: Stable video output with minimal jitter.
- **True 240p DirectVideo Mode**: 320x240 output with HDMI pixel repetition for retro gaming scalers (Morph4K, RetroTINK 4K, OSSC).
- **Configurable Audio Sample Rate**: Default 48kHz, with runtime support for 32kHz, 44.1kHz, and other standard HDMI rates. ACR N/CTS values follow the HDMI spec (Table 7-1/7-2).
- **720p60 Mode (experimental)**: Opt-in 1280x720 @ 60Hz (CEA VIC 4) at 372 MHz system clock and 1.3V core voltage. HSTX pins are configured for fast slew and 12mA drive to improve sink margin at 720p. Enable by defining `VIDEO_MODE_1280x720` when compiling the library.
- **2.0 beta precomposed scanout path (opt-in)**: Static HDMI active-line templates reduce scanline-ISR work; the ISR patches only the current Data Island payload.
- **Native 16-bit pixel scanout (opt-in)**: A pointer callback can return native RGB565 half-width scanlines for 2x modes; DMA/HSTX performs the horizontal pixel duplication.
- **Resync API (opt-in use)**: Applications can force a full HSTX/DMA restart and track resync count for watchdog-style recovery.

## Video Modes

The library selects a mode at compile time via a single define:

| Define                 | Resolution | Pixel Clock | sys_clk | Notes                                   |
|------------------------|------------|-------------|---------|-----------------------------------------|
| *(none)*               | 640x480    | 25.2 MHz    | 126 MHz | Default — CEA VIC 1, negative sync      |
| `VIDEO_MODE_320x240`   | 1280x240*  | 25.2 MHz    | 126 MHz | True 240p, 4x pixel repetition          |
| `VIDEO_MODE_1280x720`  | 1280x720   | 74.4 MHz    | 372 MHz | CEA VIC 4, positive sync, needs 1.3V    |

The library automatically handles sync polarity and Data Island placement per mode. For modes with a narrow hsync pulse (e.g. 720p60's 40-px hsync), the HDMI Data Island is placed in the back porch rather than inside the hsync pulse. This is transparent to callers.

The HSTX output pins are configured for fast slew and 12mA drive. This gives the RP2350 enough edge margin for 720p60 on stricter sinks; weaker default pad settings can produce corruption or sync loss at 720p even when 480p is stable.

## Prebuilt Demo UF2s

GitHub Releases include ready-to-flash bouncing-box firmware for Pico 2:

| Asset | Mode | HDMI path | Notes |
|-------|------|-----------|-------|
| `bouncing_box.uf2` | 640x480 @ 60Hz | Non-RT / compile-time | Default stable VGA/480p demo |
| `bouncing_box_720p_nonrt.uf2` | 1280x720 @ 60Hz | Non-RT / compile-time | Uses `VIDEO_MODE_1280x720`; recommended for validating the compile-time 720p path |
| `bouncing_box_720p_rt.uf2` | 1280x720 @ 60Hz | Runtime-mode (`video_output_rt.c`) | Uses the runtime-mode HDMI backend at CEA VIC 4 |

Both 720p builds require the RP2350 to run at 372 MHz with 1.3V core voltage. The demo configures those clocks at startup.

## 2.0 Beta: Opt-in Low-Jitter Scanout

The `2.0-beta` branch adds a compatibility-preserving path for applications that are close to the scanline timing limit. The legacy defaults remain enabled unless you opt in with CMake options or API calls.

### When to use it

Use the 2.0 beta path if your app does heavy per-line work, overlays, 320→640 scaling, audio Data Islands, or live capture while HDMI is running. The main benefit is reducing h-blank/ISR pressure and jitter. Simple apps that already fit comfortably in the classic scanline callback do not need it.

### CMake options

| Option | Default | Purpose |
|--------|---------|---------|
| `PICO_HDMI_PRECOMPOSED_ACTIVE_LINES` | `OFF` | Enable precomposed active-line templates and the native 16-bit scanout API. |
| `PICO_HDMI_HSTX_CLK_DIV` | empty | Override `MODE_HSTX_CLK_DIV`; empty keeps the mode default. Useful for 252 MHz sysclk + div2 480p. |
| `PICO_HDMI_LINE_BUFFER_IN_SCRATCH_Y` | `OFF` | Place the active-line buffer in scratch Y RAM. |
| `PICO_HDMI_RUNTIME_MODES` | `OFF` | Use `video_output_rt.c` for runtime mode switching. |
| `PICO_HDMI_RT_RUNTIME_MODE_ATTRS` | `OFF` | Allow runtime modes with different sync polarity/Data Island placement. |
| `PICO_HDMI_DUMP_COMMAND_LISTS` | `OFF` | Print generated command lists at startup for debugging. |

Example 480p beta build using a 252 MHz system clock and HSTX div2:

```bash
cmake -S examples/bouncing_box -B build-2beta -G Ninja \
  -DPICO_BOARD=pico2 \
  -DPICO_PLATFORM=rp2350-arm-s \
  -DPICO_HDMI_PRECOMPOSED_ACTIVE_LINES=ON \
  -DPICO_HDMI_HSTX_CLK_DIV=2
cmake --build build-2beta -j
```

Your application must set a matching system clock. For example, 640x480p60 can use either the stable default `126 MHz / div1 / CSR div5`, or the beta-friendly `252 MHz / div2 / CSR div5`; both produce a 25.2 MHz effective pixel clock.

### New APIs

```c
video_output_set_scanline_pointer_callback(my_ptr_cb);
video_output_set_native_pixel_mode(true);
video_output_set_compose_ring(ring, ring_entries);
video_output_compose_service();
video_output_in_vertical_blanking();
video_output_force_resync();
```

For a 2x 480p path, native pixel mode lets the pointer callback return a 320-pixel RGB565 line while HSTX/DMA duplicates each pixel horizontally to 640 output pixels. This is especially useful for emulators, capture pipelines, and OSD composition where the source is already 320-wide.

Minimal pattern:

```c
static video_output_precomposed_line_t compose_ring[64];

video_output_init(320, 240);
video_output_set_compose_ring(compose_ring, 64);
video_output_set_scanline_pointer_callback(my_native_line_cb);
video_output_set_native_pixel_mode(true);

// Run often from Core 1 background context, before/while scanout is active.
video_output_compose_service();
```

The beta path is additive: old fill-buffer callbacks, existing Data Island queue usage, and default compile-time modes still work without changes when `PICO_HDMI_PRECOMPOSED_ACTIVE_LINES=OFF`.

## Audio Sample Rate

By default, HDMI audio is configured for 48kHz. To use a different sample rate (44100 Hz for example), call `pico_hdmi_set_audio_sample_rate()` after `video_output_init()`:

```c
video_output_init(FRAME_WIDTH, FRAME_HEIGHT);
pico_hdmi_set_audio_sample_rate(44100);
```

Supported rates: 32000, 44100, 48000, 88200, 96000, 176400, 192000 Hz.

## Scanline Callback Timing

The scanline callback runs during h-blank with very limited time:

| Clock    | H-Blank Window | CPU Cycles | ~Instructions |
|----------|----------------|------------|---------------|
| 126 MHz  | ~6 µs          | ~800       | 500-700       |
| 252 MHz  | ~6 µs          | ~1600      | 1000-1400     |

A simple 320→640 pixel copy/double alone takes ~400-600 cycles. This leaves almost no room for additional processing.

**Guidelines:**
- Do heavy lifting elsewhere (different core or outside the callback)
- Use the callback only to feed pre-computed data into the DMA buffer
- Avoid per-pixel branching; use loop splitting instead
- Process 2 pixels per iteration (32-bit ops)
- Keep callback code in zero-wait-state RAM (`__scratch_x`)

The callback exists for flexibility (e.g., upscale from a smaller source buffer on-the-fly) rather than for processing. Pre-render everything, then just copy.

## Directory Structure

- `include/pico_hdmi/`: Public headers. Use `#include <pico_hdmi/...>` in your project.
- `src/`: Implementation files.
- `CMakeLists.txt`: Build configuration.

## Usage

1. Add this directory to your project's `lib` folder.
2. Add `add_subdirectory(path/to/pico_hdmi)` to your `CMakeLists.txt`.
3. Link against `pico_hdmi`.
4. Initialize with `video_output_init()` and run the output loop on Core 1 with `video_output_core1_run()`.

## Development

This project uses `clang-format` and `clang-tidy` to maintain code quality.

### Prerequisites

- **pre-commit**: To automatically run checks before each commit.

On macOS, you can install it via Homebrew:

```bash
brew install pre-commit
```

On Linux:

```bash
pip install pre-commit
```

### Setup Hooks

To activate the git pre-commit hooks, run:

```bash
pre-commit install
```

Once installed, the hooks will automatically format your code and run static analysis whenever you commit.

> **Note**: If a hook fails and modifies your files (e.g., `clang-format`), you will need to `git add` those changes and commit again.

To manually run the checks on all files:

```bash
pre-commit run --all-files
```

## License

Unlicense
