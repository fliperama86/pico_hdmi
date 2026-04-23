# pico_hdmi Examples

Example projects demonstrating the pico_hdmi HDMI output library for RP2350.

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

### 720p Variant (experimental)

The demo can also be built at 1280x720 @ 60Hz. This requires overclocking to 372 MHz at 1.3V core voltage and has only been verified on the Morph4K — other displays may not lock.

```bash
cd examples/bouncing_box
mkdir build && cd build
cmake -DBOUNCING_BOX_720P=ON ..
make
```

In 720p mode audio remains enabled; the library places the HDMI Data Island in the back porch (hsync=40 px is too narrow to hold it) and uses positive sync polarity for CEA VIC 4.

### Hardware

Requires HSTX pins connected to an HDMI connector:

- GPIO 12-13: Clock pair
- GPIO 14-15: Data 0 (Blue)
- GPIO 16-17: Data 1 (Green)
- GPIO 18-19: Data 2 (Red)

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
