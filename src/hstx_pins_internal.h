#ifndef HSTX_PINS_INTERNAL_H
#define HSTX_PINS_INTERNAL_H

#include <stdbool.h>

// Configure the HSTX bit crossbar from the selected pinout and prevent any
// subsequent pinout changes.
void pico_hdmi_hstx_configure_pinout(void);

// Connect or disconnect all eight HSTX-capable GPIOs. Initial connection also
// configures the pad slew rate and drive strength; resync reconnection does not
// need to repeat those writes.
void pico_hdmi_hstx_connect_pins(bool configure_pads);
void pico_hdmi_hstx_disconnect_pins(void);

#endif // HSTX_PINS_INTERNAL_H
