cmake_minimum_required(VERSION 3.13)

# RP2350 Raspberry Pi Pico 2
set(PICO_BOARD pico2 CACHE STRING "Board type")

# Pull in SDK
include($ENV{PICO_SDK_PATH}/pico_sdk_init.cmake)

project(directvideo_240p C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# True 240p mode for retro scalers (Morph4K, RetroTINK)
# Uses native 320x240@60Hz timing with low pixel clock (~6.3 MHz)
add_compile_definitions(VIDEO_MODE_320x240=1)

# Add pico_hdmi library (MUST be after add_compile_definitions)
add_subdirectory(../.. pico_hdmi)

add_executable(directvideo_240p
    main.c
)

target_link_libraries(directvideo_240p
    pico_stdlib
    pico_multicore
    hardware_clocks
    hardware_dma
    hardware_irq
    pico_hdmi
)

# Enable USB output, disable UART
pico_enable_stdio_usb(directvideo_240p 1)
pico_enable_stdio_uart(directvideo_240p 0)

pico_add_extra_outputs(directvideo_240p)
