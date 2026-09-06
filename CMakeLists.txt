cmake_minimum_required(VERSION 3.16)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON) # Generate compile_commands.json (consumed by clangd for code intelligence)
set(PROJECT_NAME "net.ll")

# In-tree build: this repository is the net.ll root, so no lookup/download is needed.
set(NET_LL_PATH "${CMAKE_SOURCE_DIR}")

set(PLATFORMS "Simulator" "RP2040" "ESP32")
if(NOT PLATFORM_NAME)
    set(PLATFORM_NAME "Simulator" CACHE STRING "Build platform, one of: ${PLATFORMS}")
endif()

if(DEFINED DEBUGMSGS) # Optional debug messages: pass -DDEBUGMSGS (no value needed)
    add_compile_definitions(DEBUGMSGS)
endif()

# Build system per platform
if(PLATFORM_NAME STREQUAL "ESP32")
    # ESP-IDF orchestrates the build: EXTRA_COMPONENT_DIRS adds our component keeping ESP-IDF built-in components.
    set(EXTRA_COMPONENT_DIRS "${CMAKE_SOURCE_DIR}/src/lib/Platform/ESP32")
    include($ENV{IDF_PATH}/tools/cmake/project.cmake)
    project(${PROJECT_NAME})

elseif(PLATFORM_NAME STREQUAL "Simulator")
    set(CMAKE_C_STANDARD 11)
    project(${PROJECT_NAME})
    include(${CMAKE_SOURCE_DIR}/net.ll.cmake)
    set(SOURCES "${CMAKE_SOURCE_DIR}/src/Sample.c" ${SOURCES})
    add_executable(${PROJECT_NAME} ${SOURCES})
    # Passing /src to include hal.ll's empty HalMock.h
    include_directories(${INCLUDE_DIRS} "${HAL_LL_PATH}/src")

    find_package(OpenSSL REQUIRED) # HTTPS in the Simulator HTTP client
    target_link_libraries(${PROJECT_NAME} PUBLIC
        ${PLATFORM_LIBRARIES} OpenSSL::SSL OpenSSL::Crypto)

elseif(PLATFORM_NAME STREQUAL "RP2040")
    set(CMAKE_C_STANDARD 11)
    set(CMAKE_CXX_STANDARD 14)
    set(PICO_TOOLCHAIN_PATH /usr/bin)

    # Enable radio on RP2040.
    if(NOT DEFINED PICO_BOARD)
        set(PICO_BOARD pico_w)
    endif()

    if(NOT DEFINED PICO_SDK_PATH)
        set(PICO_SDK_PATH ~/pico-sdk)
    endif()
    include(${CMAKE_SOURCE_DIR}/src/Dependency/pico_sdk_import.cmake)

    project(${PROJECT_NAME})
    pico_sdk_init()
    include(${CMAKE_SOURCE_DIR}/net.ll.cmake)
    set(SOURCES "${CMAKE_SOURCE_DIR}/src/Sample.c" ${SOURCES})
    add_executable(${PROJECT_NAME} ${SOURCES})
    include_directories(${INCLUDE_DIRS})

    add_compile_definitions(${PLATFORM_DEFINITIONS})
    target_link_libraries(${PROJECT_NAME} PUBLIC ${PLATFORM_LIBRARIES})

    pico_enable_stdio_usb(${PROJECT_NAME} 1)  # enable usb output
    pico_enable_stdio_uart(${PROJECT_NAME} 0) # disable uart output
    pico_add_extra_outputs(${PROJECT_NAME})   # create map/bin/hex/uf2 files in addition to elf

else()
    string(REPLACE ";" ", " PLATFORMS_PRETTY "${PLATFORMS}")
    message(FATAL_ERROR "PLATFORM_NAME must be one of: ${PLATFORMS_PRETTY} (got '${PLATFORM_NAME}')")
endif()
