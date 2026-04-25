[![Discord](https://img.shields.io/discord/1469402044845527253)](https://discord.gg/NqK9m8CBPT)

# PicoHDMI
An HSTX-native HDMI output library for the RP2350 (Raspberry Pi Pico 2).

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
