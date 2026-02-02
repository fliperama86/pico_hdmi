add_library(pico_hdmi
    src/video_output.c
    src/hstx_data_island_queue.c
    src/hstx_packet.c
)

target_include_directories(pico_hdmi PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}/include
)

target_link_libraries(pico_hdmi
    pico_stdlib
    hardware_clocks
    hardware_dma
    hardware_irq
    hardware_gpio
)
