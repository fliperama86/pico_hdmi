option(PICO_HDMI_RUNTIME_MODES "Use runtime video mode switching (video_output_rt)" OFF)
option(PICO_HDMI_RT_RUNTIME_MODE_ATTRS "Allow runtime modes with different sync polarity and Data Island placement" OFF)
option(PICO_HDMI_DUMP_COMMAND_LISTS "Print generated HDMI/HSTX command lists at startup" OFF)
option(PICO_HDMI_LINE_BUFFER_IN_SCRATCH_Y "Place the HDMI active-line DMA buffer in scratch Y RAM" OFF)
option(PICO_HDMI_PRECOMPOSED_ACTIVE_LINES "Enable precomposed HDMI active-line templates and native 16-bit scanout support" OFF)
option(PICO_HDMI_EXACT_AUDIO_PACING "Pace audio Data Islands with an exact rational accumulator (eliminates fixed-point drift vs ACR)" ON)
set(PICO_HDMI_HSTX_CLK_DIV "" CACHE STRING "Override MODE_HSTX_CLK_DIV; empty keeps mode default")

if(PICO_HDMI_RUNTIME_MODES)
    set(PICO_HDMI_VIDEO_OUTPUT_SRC src/video_output_rt.c)
else()
    set(PICO_HDMI_VIDEO_OUTPUT_SRC src/video_output.c)
endif()

add_library(pico_hdmi
    ${PICO_HDMI_VIDEO_OUTPUT_SRC}
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

target_compile_definitions(pico_hdmi PUBLIC
    PICO_HDMI_LINE_BUFFER_IN_SCRATCH_Y=$<BOOL:${PICO_HDMI_LINE_BUFFER_IN_SCRATCH_Y}>
    PICO_HDMI_PRECOMPOSED_ACTIVE_LINES=$<BOOL:${PICO_HDMI_PRECOMPOSED_ACTIVE_LINES}>
    PICO_HDMI_EXACT_AUDIO_PACING=$<BOOL:${PICO_HDMI_EXACT_AUDIO_PACING}>
)

if(PICO_HDMI_HSTX_CLK_DIV)
    target_compile_definitions(pico_hdmi PUBLIC MODE_HSTX_CLK_DIV=${PICO_HDMI_HSTX_CLK_DIV})
endif()

if(PICO_HDMI_DUMP_COMMAND_LISTS)
    target_compile_definitions(pico_hdmi PRIVATE PICO_HDMI_DUMP_COMMAND_LISTS=1)
endif()

if(PICO_HDMI_RT_RUNTIME_MODE_ATTRS)
    target_compile_definitions(pico_hdmi PRIVATE PICO_HDMI_RT_RUNTIME_MODE_ATTRS=1)
endif()
