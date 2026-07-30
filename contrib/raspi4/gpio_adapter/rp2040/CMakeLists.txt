# SPDX-License-Identifier: GPL-2.0-or-later

cmake_minimum_required(VERSION 3.13)

if(NOT PICO_SDK_PATH AND DEFINED ENV{PICO_SDK_PATH})
    set(PICO_SDK_PATH "$ENV{PICO_SDK_PATH}" CACHE PATH "Pico SDK path")
endif()
if(NOT PICO_SDK_PATH)
    message(FATAL_ERROR "PICO_SDK_PATH must name an installed Raspberry Pi Pico SDK")
endif()
include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(qemu_rpi_qgpio_adapter C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(QGPIO_RP2040_ALLOWED_MASK "0x1c7fffff" CACHE STRING
    "Bit mask of RP2040 GPIOs the adapter may configure")

pico_sdk_init()
if(PICO_SDK_VERSION_STRING VERSION_LESS "2.3.0")
    message(FATAL_ERROR "QGPIO reference firmware requires Pico SDK 2.3.0+")
endif()
set(QGPIO_PICO_SDK_COMMIT
    "98a542c1a62fb549ffb5d66a3e5892b06276b670"
    CACHE STRING "Required Pico SDK source commit")
find_package(Git REQUIRED)
execute_process(
    COMMAND ${GIT_EXECUTABLE} -C ${PICO_SDK_PATH} rev-parse HEAD
    OUTPUT_VARIABLE QGPIO_PICO_SDK_ACTUAL_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE QGPIO_PICO_SDK_GIT_RESULT
)
if(NOT QGPIO_PICO_SDK_GIT_RESULT EQUAL 0 OR
   NOT QGPIO_PICO_SDK_ACTUAL_COMMIT STREQUAL QGPIO_PICO_SDK_COMMIT)
    message(FATAL_ERROR
        "Pico SDK must be exact commit ${QGPIO_PICO_SDK_COMMIT}; "
        "found ${QGPIO_PICO_SDK_ACTUAL_COMMIT}")
endif()

add_executable(qgpio_adapter
    main.c
    ../qgpio_core.c
)
target_include_directories(qgpio_adapter PRIVATE ..)
target_compile_options(qgpio_adapter PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(qgpio_adapter PRIVATE
    PICO_STDIO_USB_STDOUT_TIMEOUT_US=50000
    PICO_NO_BI_PROGRAM_BUILD_DATE=1
    QGPIO_RP2040_ALLOWED_MASK=${QGPIO_RP2040_ALLOWED_MASK}
)
target_link_libraries(qgpio_adapter PRIVATE
    pico_stdlib
    pico_stdio_usb
    hardware_gpio
    hardware_watchdog
)

pico_enable_stdio_uart(qgpio_adapter 0)
pico_enable_stdio_usb(qgpio_adapter 1)
pico_set_program_name(qgpio_adapter "QEMU Raspberry Pi GPIO adapter")
pico_set_program_version(qgpio_adapter "1.0")
pico_add_extra_outputs(qgpio_adapter)
