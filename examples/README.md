# pico_hdmi Examples

Example projects demonstrating the pico_hdmi HDMI output library for RP2350.

## Prebuilt Release UF2s

The GitHub release assets include these ready-to-flash bouncing-box demos:

| Asset | Mode | HDMI path |
|-------|------|-----------|
| `bouncing_box.uf2` | 640x480 @ 60Hz | Non-RT / compile-time |
| `bouncing_box_720p_nonrt.uf2` | 1280x720 @ 60Hz | Non-RT / compile-time |
| `bouncing_box_720p_rt.uf2` | 1280x720 @ 60Hz | Runtime-mode (`video_output_rt.c`) |

Both 720p UF2s require 372 MHz sys_clk and 1.3V core voltage; the demos set this at startup.

## bouncing_box

Simple animated demo showing:

- 640x480 @ 60Hz HDMI output
- Scanline callback rendering
- 2x scaling from 320x240 framebuffer
- Basic animation loop

### Building

From the project root:

```bash
cd examples/bouncing_box
mkdir build && cd build
cmake ..
make
```

Flash the resulting `bouncing_box.uf2` to your Pico 2.

### 720p Non-RT Variant (experimental)

The demo can also be built at 1280x720 @ 60Hz. This requires overclocking to 372 MHz at 1.3V core voltage; validate it on the target sink before relying on it.

```bash
cd examples/bouncing_box
mkdir build && cd build
cmake -DBOUNCING_BOX_720P=ON -DPICO_HDMI_RUNTIME_MODES=OFF ..
make
```

In 720p mode audio remains enabled; the library places the HDMI Data Island in the back porch (hsync=40 px is too narrow to hold it) and uses positive sync polarity for CEA VIC 4. This is the non-RT / compile-time 720p path.

### Hardware

Requires HSTX pins connected to an HDMI connector:

- GPIO 12-13: Clock pair
- GPIO 14-15: Data 0 (Blue)
- GPIO 16-17: Data 1 (Green)
- GPIO 18-19: Data 2 (Red)

## bouncing_box_rt

Same bouncing-box visuals as `bouncing_box`, but driven through the
runtime-mode-switching variant of the library (`video_output_rt.c`) at
1280x720 @ 60Hz. Demonstrates using the rt API with a CEA mode that needs
positive sync polarity and back-porch Data Island placement.

The CMakeLists forces `PICO_HDMI_RUNTIME_MODES=ON` and defines
`VIDEO_MODE_1280x720` on **both** the library target and the executable —
`DI_HSYNC_ACTIVE` is a compile-time macro whose value depends on the active
`VIDEO_MODE_*`, and a mismatch between the library's view and the
executable's view corrupts the embedded HSYNC bits in audio Data Island
packets.

Requires overclocking to 372 MHz at 1.3V, same as the bouncing_box 720p
variant. Tested reliably on the Morph4K.

```bash
cd examples/bouncing_box_rt
mkdir build && cd build
cmake ..
make
```

## directvideo_240p

True 240p output for retro gaming scalers (Morph4K, RetroTINK 4K, OSSC, etc.):

- 1280x240 @ 60Hz with 4x pixel repetition (representing 320x240)
- Standard 25.2 MHz pixel clock (HDMI-compliant)
- AVI InfoFrame PR=3 tells scalers to treat as 320x240 @ 15kHz
- Compatible with scalers that recognize HDMI pixel repetition

### How It Works

HDMI has a minimum pixel clock of 25 MHz, but true 240p (320x240 @ 60Hz) would need only ~6.3 MHz. The solution is **pixel repetition**: we send 1280 pixels at 25.2 MHz (standard VGA rate) but set the HDMI Pixel Repetition field to 4x in the AVI InfoFrame. This tells the scaler that each group of 4 pixels is actually 1 logical pixel, achieving true 320x240 @ 15kHz scan rate semantics.

### Building

From the project root:

```bash
cd examples/directvideo_240p
mkdir build && cd build
cmake ..
make
```

Flash the resulting `directvideo_240p.uf2` to your Pico 2.

### Hardware

Same HSTX pinout as bouncing_box above.
