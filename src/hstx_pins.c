#include "pico_hdmi/hstx_pins.h"

#include "hardware/gpio.h"
#include "hardware/structs/hstx_ctrl.h"

#include "hstx_pins_internal.h"

#define HSTX_FIRST_GPIO 12U
#define HSTX_LAST_GPIO 19U

// This initializer reproduces the original hardcoded crossbar configuration
// exactly. In particular, the lower-numbered pin of each pair is inverted.
static pico_hdmi_hstx_pinout_t hstx_pinout = {
    .clock = {.positive_gpio = PIN_HSTX_CLK + 1, .negative_gpio = PIN_HSTX_CLK},
    .data = {{.positive_gpio = PIN_HSTX_D0 + 1, .negative_gpio = PIN_HSTX_D0},
             {.positive_gpio = PIN_HSTX_D1 + 1, .negative_gpio = PIN_HSTX_D1},
             {.positive_gpio = PIN_HSTX_D2 + 1, .negative_gpio = PIN_HSTX_D2}},
};

static bool hstx_pinout_locked;

static bool add_pin(uint32_t *used_pins, uint8_t gpio)
{
    if (gpio < HSTX_FIRST_GPIO || gpio > HSTX_LAST_GPIO) {
        return false;
    }

    const uint32_t mask = 1U << (gpio - HSTX_FIRST_GPIO);
    if (*used_pins & mask) {
        return false;
    }

    *used_pins |= mask;
    return true;
}

bool video_output_set_hstx_pinout(const pico_hdmi_hstx_pinout_t *pinout)
{
    if (!pinout || hstx_pinout_locked) {
        return false;
    }

    uint32_t used_pins = 0;
    if (!add_pin(&used_pins, pinout->clock.positive_gpio) || !add_pin(&used_pins, pinout->clock.negative_gpio)) {
        return false;
    }

    for (uint lane = 0; lane < 3; ++lane) {
        if (!add_pin(&used_pins, pinout->data[lane].positive_gpio) ||
            !add_pin(&used_pins, pinout->data[lane].negative_gpio)) {
            return false;
        }
    }

    // Eight unique pins in the eight-pin HSTX range necessarily use the full
    // GPIO12 through GPIO19 set.
    hstx_pinout = *pinout;
    return true;
}

static uint32_t gpio_to_hstx_bit(uint8_t gpio)
{
    return gpio - HSTX_FIRST_GPIO;
}

void pico_hdmi_hstx_configure_pinout(void)
{
    hstx_pinout_locked = true;

    hstx_ctrl_hw->bit[gpio_to_hstx_bit(hstx_pinout.clock.positive_gpio)] = HSTX_CTRL_BIT0_CLK_BITS;
    hstx_ctrl_hw->bit[gpio_to_hstx_bit(hstx_pinout.clock.negative_gpio)] =
        HSTX_CTRL_BIT0_CLK_BITS | HSTX_CTRL_BIT0_INV_BITS;

    for (uint lane = 0; lane < 3; ++lane) {
        const uint32_t lane_data_sel_bits = (lane * 10) << HSTX_CTRL_BIT0_SEL_P_LSB | ((lane * 10) + 1)
                                                                                          << HSTX_CTRL_BIT0_SEL_N_LSB;
        hstx_ctrl_hw->bit[gpio_to_hstx_bit(hstx_pinout.data[lane].positive_gpio)] = lane_data_sel_bits;
        hstx_ctrl_hw->bit[gpio_to_hstx_bit(hstx_pinout.data[lane].negative_gpio)] =
            lane_data_sel_bits | HSTX_CTRL_BIT0_INV_BITS;
    }
}

void pico_hdmi_hstx_connect_pins(bool configure_pads)
{
    for (uint gpio = HSTX_FIRST_GPIO; gpio <= HSTX_LAST_GPIO; ++gpio) {
        gpio_set_function(gpio, GPIO_FUNC_HSTX);
        if (configure_pads) {
            gpio_set_slew_rate(gpio, GPIO_SLEW_RATE_FAST);
            gpio_set_drive_strength(gpio, GPIO_DRIVE_STRENGTH_12MA);
        }
    }
}

void pico_hdmi_hstx_disconnect_pins(void)
{
    for (uint gpio = HSTX_FIRST_GPIO; gpio <= HSTX_LAST_GPIO; ++gpio) {
        gpio_set_function(gpio, GPIO_FUNC_SIO);
    }
}
