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

set(BOUNCING_BOX_AUDIO_SAMPLE_RATE "48000" CACHE STRING "HDMI audio sample rate (44100 or 48000 Hz)")
set_property(CACHE BOUNCING_BOX_AUDIO_SAMPLE_RATE PROPERTY STRINGS 44100 48000)
if (NOT BOUNCING_BOX_AUDIO_SAMPLE_RATE STREQUAL "44100" AND
    NOT BOUNCING_BOX_AUDIO_SAMPLE_RATE STREQUAL "48000")
    message(FATAL_ERROR "BOUNCING_BOX_AUDIO_SAMPLE_RATE must be 44100 or 48000")
endif()
target_compile_definitions(bouncing_box PRIVATE
    BOUNCING_BOX_AUDIO_SAMPLE_RATE=${BOUNCING_BOX_AUDIO_SAMPLE_RATE}
)

# Optional: build at 1280x720 @ 60 Hz (requires overclock to 372 MHz @ 1.3V).
# Default is 640x480 @ 60 Hz at the stock 126 MHz clock.
# Enable with: cmake -DBOUNCING_BOX_720P=ON ..
option(BOUNCING_BOX_720P "Build bouncing_box in 1280x720 mode (requires overclock)" OFF)
option(BOUNCING_BOX_720P50 "Build bouncing_box in CEA VIC 19 1280x720p50 mode (requires overclock)" OFF)
option(BOUNCING_BOX_720P_RB "Build bouncing_box in exact-clock 1280x720p60 CVT reduced-blanking mode" OFF)

set(BOUNCING_BOX_720P_MODE_COUNT 0)
foreach(BOUNCING_BOX_MODE BOUNCING_BOX_720P BOUNCING_BOX_720P50 BOUNCING_BOX_720P_RB)
    if (${BOUNCING_BOX_MODE})
        math(EXPR BOUNCING_BOX_720P_MODE_COUNT "${BOUNCING_BOX_720P_MODE_COUNT} + 1")
    endif()
endforeach()
if (BOUNCING_BOX_720P_MODE_COUNT GREATER 1)
    message(FATAL_ERROR "BOUNCING_BOX_720P, BOUNCING_BOX_720P50, and BOUNCING_BOX_720P_RB are mutually exclusive")
endif()

if (BOUNCING_BOX_720P OR BOUNCING_BOX_720P50 OR BOUNCING_BOX_720P_RB)
    target_compile_definitions(bouncing_box PRIVATE VIDEO_MODE_1280x720)
    target_compile_definitions(pico_hdmi PRIVATE VIDEO_MODE_1280x720)
    if (BOUNCING_BOX_720P50)
        target_compile_definitions(bouncing_box PRIVATE VIDEO_MODE_50HZ)
        target_compile_definitions(pico_hdmi PRIVATE VIDEO_MODE_50HZ)
    elseif (BOUNCING_BOX_720P_RB)
        target_compile_definitions(bouncing_box PRIVATE VIDEO_MODE_720P_RB)
        target_compile_definitions(pico_hdmi PRIVATE VIDEO_MODE_720P_RB)
    endif()
    # 720p overclocks sys_clk to 372 MHz, at which XIP flash reads corrupt (the
    # boot-clock QMI timing no longer samples correctly) and the core hard-faults.
    # Execute from RAM so flash is only read at the safe boot clock. Robust and
    # flash-chip-independent. (480p stays the default flash/XIP build.)
    pico_set_binary_type(bouncing_box copy_to_ram)
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
