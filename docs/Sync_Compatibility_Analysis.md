# Sync Compatibility Analysis

Investigation of sync failures with Acer XB271HU and Samsung Q80 TV.

## Status: RESOLVED ✓

| Display | Status | Fix Required |
|---------|--------|--------------|
| Morph4K | ✓ Working | Video Guard Band |
| Acer XB271HU | ✓ Working | Video Guard Band |
| Samsung Q80 TV | ✓ Working | Video Guard Band + DDC bus init |

## Fixes Applied

| Issue | Commit | Notes |
|-------|--------|-------|
| Video Preamble/Guard Band | `83d9692` | Required by HDMI 1.3a spec |
| DDC Bus Termination | TBD | Samsung Q80 needed DDC pull-ups |

## Remaining Improvements (Optional)

| Issue | Priority | Status |
|-------|----------|--------|
| Minimal AVI InfoFrame | Low | Open |
| No General Control Packet (GCP) | Low | Open |

## Samsung Q80 Investigation - RESOLVED ✓

**Root Cause:** Floating DDC bus lines. The TV requires the DDC (I2C) bus to be properly terminated.

**Solution:** DDC bus needs to be terminated - not left floating.

**Hardware fix (no code needed):**
- Add 4.7k pull-up resistors from DDC_SCL (HDMI pin 15) and DDC_SDA (HDMI pin 16) to 3.3V or 5V

**Alternative - software fix:**
```c
// Initialize I2C with internal pull-ups
i2c_init(i2c1, 100000);
gpio_set_function(9, GPIO_FUNC_I2C);   // SDA
gpio_set_function(10, GPIO_FUNC_I2C);  // SCL
gpio_pull_up(9);
gpio_pull_up(10);
```

**Result:** Samsung Q80 TV now syncs correctly with video and audio!

---

## Critical Issue: Missing Video Preamble and Guard Band

### Spec Requirement (HDMI 1.3a Section 5.2.2)

Before **every** Video Data Period, the spec requires:

1. **Video Preamble** (8 pixels minimum)
   - CH0: Encodes HSYNC/VSYNC as usual
   - CH1: CTL0=1, CTL1=0 → CTRL_01 (0x0AB)
   - CH2: CTL2=0, CTL3=0 → CTRL_00 (0x354)

2. **Video Leading Guard Band** (2 pixels)
   - CH0: 0b1011001100 (0x2CC)
   - CH1: 0b0100110011 (0x133)
   - CH2: 0b1011001100 (0x2CC)

### Current Implementation

**DVI Mode** (`vactive_line_dvi`):
```
Front Porch (16px) → Sync (96px) → Back Porch (48px) → TMDS Pixels
                     [All using CTRL_00 on CH1/CH2]
```

**HDMI Mode** (active lines with Data Island):
```
Front Porch → DI Preamble → Data Island → Control Period → Back Porch → TMDS Pixels
                                          [CTRL_00 on CH1/CH2]
```

**Problem**: Both modes go directly from control period (CH1/CH2 = CTRL_00) to TMDS video pixels without:
- Video Preamble (CH1=CTRL_01, CH2=CTRL_00 for 8 pixels)
- Video Guard Band (2 pixels with special symbols)

### DVI Compatibility Exception

From HDMI 1.3a Appendix C (DVI Backward Compatibility):
> "No Video Guard Bands shall be used."

**Important**: When in pure DVI mode (no Data Islands), video guard bands are NOT required. But the current implementation defaults to HDMI mode with Data Islands enabled, so guard bands ARE required.

### Proposed Fix

Add Video Preamble and Guard Band before active video in HDMI mode:

```c
// New symbols
#define VIDEO_PREAMBLE_V1_H1 (TMDS_CTRL_11 | (TMDS_CTRL_01 << 10) | (TMDS_CTRL_00 << 20))
#define VIDEO_GUARD_BAND     (0x2CCu | (0x133u << 10) | (0x2CCu << 20))

// In build_line_with_di() for active lines:
// Before: Back porch (48px) → TMDS pixels
// After:  Back porch (38px) → Video Preamble (8px) → Video Guard Band (2px) → TMDS pixels
```

### Timing Budget

For 640x480@60Hz:
- Back porch: 48 pixels
- Video Preamble: 8 pixels
- Video Guard Band: 2 pixels
- Remaining control period: 48 - 8 - 2 = 38 pixels (spec minimum is 12)

**Plenty of margin available.**

---

## Medium Priority: AVI InfoFrame Completeness

### Current Implementation

```c
packet->subpacket[0][1] = 0x00;  // Y=RGB, A=0, B=0, S=0
packet->subpacket[0][2] = 0x08;  // ITC=1, rest=0
packet->subpacket[0][3] = 0x00;
packet->subpacket[0][4] = vic;
packet->subpacket[0][5] = 0x00;
```

### Missing Fields That May Help

| Field | Byte | Bits | Current | Recommended |
|-------|------|------|---------|-------------|
| A (Active Format) | 1 | 4 | 0 | 1 (valid) |
| S (Scan Info) | 1 | 1:0 | 00 | 00 (no data) or 01 (overscan) |
| M (Aspect Ratio) | 2 | 5:4 | 00 | 01 (4:3) for VIC 1 |
| R (Active Portion) | 2 | 3:0 | 0 | 8 (same as coded) |
| Q (Quantization) | 3 | 7:6 | 00 | 00 (default) or 01 (limited) |

### Proposed Improvement

```c
void hstx_packet_set_avi_infoframe(hstx_packet_t *packet, uint8_t vic) {
    hstx_packet_init(packet);
    packet->header[0] = 0x82;
    packet->header[1] = 0x02;
    packet->header[2] = 0x0D;

    // Byte 1: Y1:Y0=00 (RGB), A0=1, B1:B0=00, S1:S0=00
    packet->subpacket[0][1] = 0x10;  // A=1 (active format valid)

    // Byte 2: C1:C0=00, M1:M0=01 (4:3), R3:R0=1000 (same as coded)
    packet->subpacket[0][2] = 0x18;  // M=4:3, R=8

    // Byte 3: ITC=0, EC=0, Q=0, SC=0
    packet->subpacket[0][3] = 0x00;

    // Byte 4: VIC
    packet->subpacket[0][4] = vic;

    // Byte 5: YQ=0, CN=0, PR=0
    packet->subpacket[0][5] = 0x00;

    compute_infoframe_checksum(packet);
    compute_all_parity(packet);
}
```

---

## Low Priority: General Control Packet (GCP)

### Purpose

- AVMUTE: Mute audio/video during mode changes
- Color Depth: Indicate 24/30/36/48-bit color
- Default_Phase: Phase alignment

### Why It May Help

Some displays may wait for Clear_AVMUTE before displaying video. Sending a GCP with Clear_AVMUTE at startup could help.

### Proposed Implementation

Send GCP with Clear_AVMUTE once during vertical blanking at startup:

```c
void hstx_packet_set_gcp(hstx_packet_t *packet, bool set_avmute, bool clear_avmute) {
    hstx_packet_init(packet);
    packet->header[0] = 0x03;  // GCP type

    uint8_t sb0 = 0;
    if (set_avmute) sb0 |= 0x01;
    if (clear_avmute) sb0 |= 0x10;
    sb0 |= (0x04 << 4);  // CD = 4 (24-bit color, 8 bits per component)

    packet->subpacket[0][0] = sb0;
    // Replicate to all subpackets
    for (int i = 1; i < 4; i++)
        memcpy(packet->subpacket[i], packet->subpacket[0], 7);

    compute_all_parity(packet);
}
```

---

## Implementation Priority

1. **First**: Add Video Preamble and Video Guard Band to HDMI mode
   - This is the most likely cause of sync failures
   - The spec is explicit about requiring this before video data

2. **Second**: Improve AVI InfoFrame
   - Add Active Format Valid flag
   - Add proper aspect ratio for VIC 1

3. **Third**: Add GCP with Clear_AVMUTE
   - Send once at startup to clear any mute state

---

## Testing Plan

After each change:
1. Test on Morph4K analyzer (verify no errors)
2. Test on Acer XB271HU (target problematic display)
3. Test on Samsung Q80 TV (target problematic display)
4. Test on known-working displays to verify no regressions

---

## References

- HDMI 1.3a Section 5.2.1.1 (Preamble)
- HDMI 1.3a Section 5.2.2 (Video Data Period)
- HDMI 1.3a Section 5.2.2.1 (Video Guard Band)
- HDMI 1.3a Appendix C (DVI Backward Compatibility)
- hdl-util/hdmi reference implementation (tmds_channel.sv)
