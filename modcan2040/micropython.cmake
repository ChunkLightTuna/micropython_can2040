# MicroPython user C module: can2040 software CAN bus for RP2040/RP2350.
#
# Usage (from micropython/ports/rp2):
#   cmake -S . -B build -G Ninja -DMICROPY_BOARD=RPI_PICO \
#         -DUSER_C_MODULES=/path/to/modcan2040/micropython.cmake
#
# Variables:
#   CAN2040_DIR     path to the upstream can2040 checkout (default: ../can2040)
#   CAN2040_IN_RAM  place can2040 code + tables in RAM for low IRQ latency (default ON)

if(NOT CAN2040_DIR)
    get_filename_component(CAN2040_DIR "${CMAKE_CURRENT_LIST_DIR}/../can2040" ABSOLUTE)
endif()
if(NOT EXISTS "${CAN2040_DIR}/src/can2040.c")
    message(FATAL_ERROR "can2040 sources not found at ${CAN2040_DIR} (set CAN2040_DIR)")
endif()
option(CAN2040_IN_RAM "Run the can2040 core from RAM instead of flash" ON)

# The can2040 core is built as its own static library so it can get -O2 (as
# upstream recommends) and, optionally, have its sections renamed so the
# pico-sdk linker script places them in RAM (.time_critical* -> .data).
add_library(can2040_core STATIC ${CAN2040_DIR}/src/can2040.c)
target_include_directories(can2040_core PUBLIC ${CAN2040_DIR}/src)
target_link_libraries(can2040_core PRIVATE hardware_structs_headers)
target_compile_options(can2040_core PRIVATE -O2)
if(PICO_RP2350)
    target_compile_definitions(can2040_core PRIVATE PICO_RP2350=1)
endif()
if(CAN2040_IN_RAM)
    add_custom_command(TARGET can2040_core POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} --prefix-alloc-sections=.time_critical $<TARGET_FILE:can2040_core>
        COMMENT "Relocating can2040 core into RAM (.time_critical)"
        VERBATIM)
endif()

add_library(usermod_can2040 INTERFACE)
target_sources(usermod_can2040 INTERFACE ${CMAKE_CURRENT_LIST_DIR}/modcan2040.c)
target_include_directories(usermod_can2040 INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CAN2040_DIR}/src
)
target_link_libraries(usermod_can2040 INTERFACE can2040_core)
# Hook the port's soft-reset path (see can2040_mp_hooks.h).
target_compile_options(usermod_can2040 INTERFACE
    $<$<COMPILE_LANGUAGE:C>:-include>
    $<$<COMPILE_LANGUAGE:C>:${CMAKE_CURRENT_LIST_DIR}/can2040_mp_hooks.h>)

target_link_libraries(usermod INTERFACE usermod_can2040)
