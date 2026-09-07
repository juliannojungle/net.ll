# net.ll.cmake
#
# Copy this file anywhere into your project and include() it from your CMakeLists.txt,
# after project(). It locates net.ll through NET_LL_PATH, downloading the project when
# that directory is not populated yet.
#
# NET_LL_PATH is cached, so several submodules of the same project (each one carrying its
# own copy of this file) share a single net.ll checkout: the first one to resolve it wins.
#
# net.ll builds on top of fs.ll, because a download streams straight to storage, and it
# includes its own copy of fs.ll.cmake. A consumer that also uses fs.ll directly carries
# that file too, which is what keeps it from being downloaded twice: whoever resolves
# FS_LL_PATH first wins and the other include reuses the cached checkout.
#
# net.ll does NOT reach hal.ll: on every platform the radio comes with a whole stack from
# the platform SDK, not a bus. hal.ll still ends up in the build, through fs.ll's contract.
#
# Inputs:
#   NET_LL_PATH    - path to the net.ll root directory (variable or environment).
#                    Relative paths are resolved against CMAKE_SOURCE_DIR.
#                    Defaults to a 'net.ll' folder next to this file.
#   PLATFORM_NAME  - Simulator (default), RP2040 or ESP32.
#
# Outputs:
#   NET_LL_PATH         - cached, absolute path to the net.ll root directory.
#   NET_LL_PLATFORM_DIR - the resolved platform folder.
#   SOURCES             - appended with the net.ll sources (and fs.ll's, through its contract).
#   INCLUDE_DIRS        - appended with the matching include directories.
#   PLATFORM_LIBRARIES  - appended with what the consumer must link.
#   PLATFORM_REQUIRES   - appended with the ESP-IDF components the consumer must REQUIRE.
#   PLATFORM_DEFINITIONS - appended with compile definitions the consumer must apply.
#
# On the Simulator, HTTPS needs OpenSSL: the consumer links OpenSSL::SSL and OpenSSL::Crypto.
#
# This file only sets variables. It must never call directory- or target-scoped commands
# such as add_compile_definitions: ESP-IDF evaluates the component that includes it in
# script mode (cmake -P), where those commands do not exist.

if(NOT DEFINED PLATFORM_NAME)
    set(PLATFORM_NAME "Simulator")
endif()

# What the consumer has to link, require or define. Declared before anything touches the
# filesystem because these lists depend only on the platform, never on the checkout.
if(PLATFORM_NAME STREQUAL "RP2040")
    # poll mode, not threadsafe_background: every network operation here is synchronous and
    # net.ll starts no thread, so nothing may service the radio behind the caller's back.
    set(PLATFORM_LIBRARIES ${PLATFORM_LIBRARIES} pico_cyw43_arch_poll)
    # There is no ready-made "poll without lwIP" target, and with CYW43_LWIP undefined the
    # driver defaults to using lwIP and then demands an lwipopts.h. Downloading on this
    # platform is not implemented yet, so the TCP/IP stack is left out until it is.
    set(PLATFORM_DEFINITIONS ${PLATFORM_DEFINITIONS} CYW43_LWIP=0)
elseif(PLATFORM_NAME STREQUAL "ESP32")
    # nvs_flash is not optional: the WiFi driver keeps calibration data there.
    # lwip carries the BSD socket headers the HTTP server is written against.
    set(PLATFORM_REQUIRES ${PLATFORM_REQUIRES} esp_wifi esp_netif esp_event nvs_flash lwip)
endif()

if(PLATFORM_LIBRARIES)
    list(REMOVE_DUPLICATES PLATFORM_LIBRARIES)
endif()
if(PLATFORM_REQUIRES)
    list(REMOVE_DUPLICATES PLATFORM_REQUIRES)
endif()
if(PLATFORM_DEFINITIONS)
    list(REMOVE_DUPLICATES PLATFORM_DEFINITIONS)
endif()

if(DEFINED ENV{NET_LL_PATH} AND (NOT NET_LL_PATH))
    set(NET_LL_PATH $ENV{NET_LL_PATH})
    message("Using NET_LL_PATH from environment ('${NET_LL_PATH}')")
endif()

if(NOT NET_LL_PATH)
    set(NET_LL_PATH "${CMAKE_CURRENT_LIST_DIR}/net.ll")
endif()

get_filename_component(NET_LL_PATH "${NET_LL_PATH}" REALPATH BASE_DIR "${CMAKE_SOURCE_DIR}")

# ESP-IDF evaluates the consumer's component twice, and the first pass runs in script
# mode (cmake -P) only to collect REQUIRES. The chain below net.ll publishes more of that
# list, so it still has to be reached, but nothing else here needs to run: there is no
# cache to read a caller-provided path from, and no source gets compiled in that pass.
if(DEFINED CMAKE_SCRIPT_MODE_FILE)
    include(${NET_LL_PATH}/src/Dependency/fs.ll.cmake)
    return()
endif()

# Sentinel file used to tell a populated checkout from an empty/missing directory.
set(NET_LL_SENTINEL_FILE "${NET_LL_PATH}/src/lib/WiFi.h")

if(NOT EXISTS "${NET_LL_SENTINEL_FILE}")
    find_package(Git QUIET)
    if(NOT Git_FOUND)
        message(FATAL_ERROR
            "net.ll was not found at '${NET_LL_PATH}' and git is not available to download it. "
            "Please install git or set NET_LL_PATH to an existing net.ll checkout.")
    endif()

    message("Downloading net.ll into '${NET_LL_PATH}'")
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" clone --branch main --depth 1
                https://github.com/juliannojungle/net.ll.git "${NET_LL_PATH}"
        RESULT_VARIABLE NET_LL_CLONE_RESULT
        ERROR_VARIABLE NET_LL_CLONE_ERROR)

    if(NOT NET_LL_CLONE_RESULT EQUAL 0)
        message(FATAL_ERROR "Failed to download net.ll into '${NET_LL_PATH}': ${NET_LL_CLONE_ERROR}")
    endif()

    if(NOT EXISTS "${NET_LL_SENTINEL_FILE}")
        message(FATAL_ERROR "Directory '${NET_LL_PATH}' does not appear to contain net.ll")
    endif()
endif()

set(NET_LL_PATH "${NET_LL_PATH}" CACHE PATH "Path to the net.ll root directory" FORCE)

set(NET_LL_LIB_DIR "${NET_LL_PATH}/src/lib")
set(NET_LL_PLATFORM_DIR "${NET_LL_LIB_DIR}/Platform/${PLATFORM_NAME}")

if(NOT EXISTS "${NET_LL_PLATFORM_DIR}")
    message(FATAL_ERROR "net.ll has no support for platform '${PLATFORM_NAME}' ('${NET_LL_PLATFORM_DIR}' not found)")
endif()

set(SOURCES
    ${SOURCES}
    "${NET_LL_PLATFORM_DIR}/WiFi.c"
    "${NET_LL_PLATFORM_DIR}/HttpClient.c"
    "${NET_LL_PLATFORM_DIR}/HttpServer.c")

set(INCLUDE_DIRS
    ${INCLUDE_DIRS}
    "${NET_LL_LIB_DIR}")

# Guards against the same net.ll being included by more than one sibling library.
list(REMOVE_DUPLICATES SOURCES)
list(REMOVE_DUPLICATES INCLUDE_DIRS)

# A download streams the body straight to storage rather than buffering it, so the file
# system comes from fs.ll. This include also brings hal.ll in, through fs.ll's contract.
include(${NET_LL_PATH}/src/Dependency/fs.ll.cmake)
