#include <hardware/clocks.h>
#include <hardware/pio.h>
#include <pico/stdlib.h>
#include <stdio.h>

// Generated header from blink.pio
#include "blink.pio.h"

#define BLINK_PIN 25

void blink_pin_forever(PIO pio, uint sm, uint offset, uint pin, uint freq) {
  blink_program_init(pio, sm, offset, pin);
  pio_sm_set_enabled(pio, sm, true);

  printf("Blinking pin %d at %d Hz\n", pin, freq);

  // PIO counter program takes 3 more cycles in total than we pass as
  // @param freq: frequency (in Hz) to blink GPIO pin at
  pio->txf[sm] = (clock_get_hz(clk_sys) / (2 * freq)) - 3;
}

int main() {
  stdio_init_all();

  printf("PIO Blink Example - RP2350\n");

  // Choose which PIO instance to use (RP2350 has PIO0, PIO1, PIO2)
  PIO pio = pio0;

  // Load the blink program into the PIO instruction memory
  uint offset = pio_add_program(pio, &blink_program);

  // Find a free state machine on our chosen PIO
  uint sm = pio_claim_unused_sm(pio, true);

  // Configure the state machine to blink the LED at 3 Hz
  blink_pin_forever(pio, sm, offset, BLINK_PIN, 3);

  // Main loop - the PIO handles the blinking
  while (true) {
    tight_loop_contents();
  }

  return 0;
}
