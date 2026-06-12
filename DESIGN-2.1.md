# pico_hdmi 2.1 — precomposed architecture for the runtime-modes path

Goal (ratified by NeoPico-HD "Scenario B"): one firmware, all modes
(480p/240p/720p), every mode on the tiny-ISR precomposed architecture —
the only configuration that has survived XIP layout shifts, RAM execution,
and feature stacks. Endgame: delete the copy-model ISR entirely.

## Per-mode pixel strategy

Native pixel mode (16-bit DMA + bus halfword replication + HSTX 2x
expander) can only produce even-duplicated streams: each input halfword
becomes two identical pixels. Hence:

| Mode | Active | Scale | Pixel source |
|------|--------|-------|--------------|
| 480p | 640    | 2x    | NATIVE: zero-copy 320px line (proven in 2.0 fixed path) |
| 240p | 1280   | 4x    | NATIVE: app pre-doubles 320 -> 640 halfwords in a small ring outside the ISR; hardware does the final 2x |
| 720p | 1280   | 3x    | 32-BIT: 3x is not expressible via halfword replication (AAA BBB pairs as (AA)(AB)(BB)); app pre-expands 320 -> 640 words (960px centered + pillarbox) in a ring outside the ISR |

`native_pixel_mode` therefore becomes a PER-MODE property, constant within
a mode.

## Swap-free DMA (hardening; suspected root of residual desyncs)

2.0's native mode switches one channel's transfer size per post (header=32,
pixels=16) from the ISR; a late/coalesced IRQ can mis-size a post and
permanently desync the HSTX command stream (suspect in rp2350-doom's
"brownouts", issue #1 there). 2.1 eliminates the switching: a dedicated
pixel channel fixed at 16-bit (or 32-bit per mode), header ping/pong
channels fixed at 32-bit, chained header->pixel->header. No channel ever
changes width at runtime; the race ceases to exist structurally.

## Precomposed machinery (port from video_output.c)

- Header ring built ONCE PER MODE (lengths/DI placement are mode-dependent:
  480p/240p islands in-hsync, 720p islands in the back-porch idle region;
  the island patch offset is recorded at header-build time).
- ISR pops pre-encoded islands from the di queue and patches 36 words into
  the header being posted; audio pacing lives in the ISR (starvation-proof,
  proven for hours on two consumers).
- Blanking lines: ping/pong patch templates (as in the fixed path).
- Mode switch: invalidate ring (compose_ring_built=false), rebuild headers
  + sync symbol cache + di pacing for the new mode in the background task,
  then resync from frame top.

## Staging

- S1: 480p precomposed on the RT path (single mode), validated on NeoPico
  against the proven fixed-path behavior.
- S2: swap-free DMA channel topology.
- S3: 240p + 720p pre-expanded ring support (lib: per-mode pixel source
  width; app: prep rings + OSD composes at native res BEFORE expansion).
- S4: runtime mode switching + the menu's Resolution entry on this path.
