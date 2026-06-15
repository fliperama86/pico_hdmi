cmake_minimum_required(VERSION 3.13)

# RP2350 Raspberry Pi Pico 2
set(PICO_BOARD pico2 CACHE STRING "Board type")

# Pull in SDK
include($ENV{PICO_SDK_PATH}/pico_sdk_init.cmake)

project(bouncing_box_rt C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# Use the runtime-mode-switching variant of pico_hdmi (video_output_rt.c).
set(PICO_HDMI_RUNTIME_MODES ON CACHE BOOL "" FORCE)

# Add pico_hdmi library
add_subdirectory(../.. pico_hdmi)

add_executable(bouncing_box_rt
    main.c
)

# This example runs at 1280x720 @ 60 Hz (CEA VIC 4) on the rt path.
# VIDEO_MODE_1280x720 MUST be defined on BOTH the library target and the
# executable target — DI_HSYNC_ACTIVE resolves at compile time in each
# translation unit, and a mismatch corrupts the embedded HSYNC bits in
# audio Data Island packets.
target_compile_definitions(pico_hdmi PRIVATE VIDEO_MODE_1280x720)
target_compile_definitions(bouncing_box_rt PRIVATE VIDEO_MODE_1280x720)

# 720p overclocks sys_clk to 372 MHz. At that speed the QMI flash read timing
# (set by boot stage2 for the boot clock) no longer samples correctly, so XIP
# instruction fetches corrupt and the core hard-faults. Execute from RAM: flash
# is read only once at the safe boot clock, never at 372 MHz. Flash-chip-
# independent; the robust fix for any 720p build.
pico_set_binary_type(bouncing_box_rt copy_to_ram)

target_link_libraries(bouncing_box_rt
    pico_stdlib
    pico_multicore
    hardware_clocks
    hardware_dma
    hardware_irq
    pico_hdmi
)

# Enable USB output, disable UART
pico_enable_stdio_usb(bouncing_box_rt 1)
pico_enable_stdio_uart(bouncing_box_rt 0)

pico_add_extra_outputs(bouncing_box_rt)
