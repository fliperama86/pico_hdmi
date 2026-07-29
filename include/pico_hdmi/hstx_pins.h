/**
 * pico_hdmi Pin Configuration
 *
 * HSTX output pin mapping for the pico_hdmi library.
 */

#ifndef HSTX_PINS_H
#define HSTX_PINS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// =============================================================================
// DVI/HSTX Output Pins (GPIO 12-19)
// =============================================================================
// These are the default pins for the RP2350's HSTX peripheral.
#define PIN_HSTX_CLK 12 // Clock pair base (12=CLK-, 13=CLK+)
#define PIN_HSTX_D0 14  // Data 0 pair base (14=D0-, 15=D0+)
#define PIN_HSTX_D1 16  // Data 1 pair base (16=D1-, 17=D1+)
#define PIN_HSTX_D2 18  // Data 2 pair base (18=D2-, 19=D2+)

/**
 * The GPIO carrying one side of an HSTX pseudo-differential pair.
 *
 * RP2350 can route each HSTX clock or data output independently to any of
 * GPIO12 through GPIO19. Naming the two pins by signal polarity makes pair
 * swapping explicit and avoids a separate inversion flag.
 */
typedef struct {
    uint8_t positive_gpio;
    uint8_t negative_gpio;
} pico_hdmi_hstx_pair_t;

/**
 * Complete HDMI HSTX pinout. data[0] through data[2] are the logical TMDS
 * data lanes, regardless of which physical GPIO pair carries each lane.
 */
typedef struct {
    pico_hdmi_hstx_pair_t clock;
    pico_hdmi_hstx_pair_t data[3];
} pico_hdmi_hstx_pinout_t;

/**
 * Override the default HSTX pinout.
 *
 * The default remains CLK-/CLK+ on GPIO12/13 and D0-/D0+ through D2-/D2+
 * on GPIO14 through GPIO19. Existing applications do not need to call this
 * function.
 *
 * A custom pinout must use every GPIO from 12 through 19 exactly once. Call
 * this before video_output_core1_run(). The configuration is copied, so the
 * supplied object does not need to remain alive.
 *
 * @return true if the pinout was accepted, false if it was invalid or HSTX
 *         output configuration has already started.
 */
bool video_output_set_hstx_pinout(const pico_hdmi_hstx_pinout_t *pinout);

#ifdef __cplusplus
}
#endif

#endif // HSTX_PINS_H
