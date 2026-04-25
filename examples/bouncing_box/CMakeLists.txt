cmake_minimum_required(VERSION 3.13)

# RP2350 Raspberry Pi Pico 2
set(PICO_BOARD pico2 CACHE STRING "Board type")

# Pull in SDK
include($ENV{PICO_SDK_PATH}/pico_sdk_init.cmake)

project(bouncing_box C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# Add pico_hdmi library
add_subdirectory(../.. pico_hdmi)

add_executable(bouncing_box
    main.c
)

# Optional: build at 1280x720 @ 60 Hz (requires overclock to 372 MHz @ 1.3V).
# Default is 640x480 @ 60 Hz at the stock 126 MHz clock.
# Enable with: cmake -DBOUNCING_BOX_720P=ON ..
option(BOUNCING_BOX_720P "Build bouncing_box in 1280x720 mode (requires overclock)" OFF)
if (BOUNCING_BOX_720P)
    target_compile_definitions(bouncing_box PRIVATE VIDEO_MODE_1280x720)
    target_compile_definitions(pico_hdmi PRIVATE VIDEO_MODE_1280x720)
endif()

option(BOUNCING_BOX_DVI_ONLY "Build bouncing_box without HDMI Data Islands/audio" OFF)
if (BOUNCING_BOX_DVI_ONLY)
    target_compile_definitions(bouncing_box PRIVATE BOUNCING_BOX_DVI_ONLY)
endif()

target_link_libraries(bouncing_box
    pico_stdlib
    pico_multicore
    hardware_clocks
    hardware_dma
    hardware_irq
    pico_hdmi
)

# Enable USB output, disable UART
pico_enable_stdio_usb(bouncing_box 1)
pico_enable_stdio_uart(bouncing_box 0)

pico_add_extra_outputs(bouncing_box)
