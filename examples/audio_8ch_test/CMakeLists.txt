cmake_minimum_required(VERSION 3.13)

# RP2350 Raspberry Pi Pico 2
set(PICO_BOARD pico2 CACHE STRING "Board type")

# Pull in SDK
include($ENV{PICO_SDK_PATH}/pico_sdk_init.cmake)

project(audio_8ch_test C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# 8-channel 48 kHz 24-bit audio. Must be set before the library is added.
set(PICO_HDMI_AUDIO_8CH ON CACHE BOOL "8-channel 48 kHz 24-bit HDMI audio (Layout 1 packets)" FORCE)
# The demo runs at 252 MHz; HSTX divides that by 2 to keep the 126 MHz
# (25.2 MHz pixel) clock of standard 640x480 @ 60 Hz.
set(PICO_HDMI_HSTX_CLK_DIV "2" CACHE STRING "Override MODE_HSTX_CLK_DIV; empty keeps mode default" FORCE)

# Add pico_hdmi library
add_subdirectory(../.. pico_hdmi)

add_executable(audio_8ch_test
    main.c
    text_screen.c
)

target_link_libraries(audio_8ch_test
    pico_stdlib
    pico_multicore
    hardware_clocks
    pico_hdmi
)

# Diagnostic: plain DVI video, no Data Islands or audio.
option(AUDIO_8CH_TEST_VIDEO_ONLY "Send video only (DVI mode), no audio" OFF)
if (AUDIO_8CH_TEST_VIDEO_ONLY)
    target_compile_definitions(audio_8ch_test PRIVATE AUDIO_8CH_TEST_VIDEO_ONLY=1)
endif()

# Diagnostic: print startup markers over USB serial.
option(AUDIO_8CH_TEST_TRACE "Print startup trace markers over USB serial" OFF)
if (AUDIO_8CH_TEST_TRACE)
    target_compile_definitions(audio_8ch_test PRIVATE AUDIO_8CH_TEST_TRACE=1)
endif()

# Status lines go out over USB serial.
pico_enable_stdio_usb(audio_8ch_test 1)
pico_enable_stdio_uart(audio_8ch_test 0)

pico_add_extra_outputs(audio_8ch_test)
