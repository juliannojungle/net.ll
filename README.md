# net.ll

> 🦖 **Part of [dot-ll-collection](https://github.com/topics/dot-ll-collection)**

> ⚠️ **This project is under active development. The documentation is growing along the project as it's a work-in-progress.**

This is a lightweight, bare-metal network library for embedded systems.

It does two things: lists the WiFi access points in range, and downloads a file over HTTP or HTTPS
straight to storage. Both are **synchronous**, and the library starts no threads of its own.

One codebase targets three platforms: **RP2040** (via pico-sdk), **ESP32** (via ESP-IDF) and a
**Simulator** that runs on a Linux desktop.

## Features

- 📡 **WiFi scan** — one entry per access point, with SSID, BSSID, channel, authentication mode and signal
  strength, filled into an array you own. No callbacks.
- ⬇️ **HTTP/HTTPS download to storage** — the body is streamed straight to the file system rather than
  buffered, because embedded targets have little RAM to spare.
- 🔄 **Same API on every platform** — the platform layer is swapped at build time, so consuming code needs
  no `#ifdef`.
- 🖥️ **Runs on the desktop too** — the Simulator has no radio, so under WSL it borrows the Windows host's
  to report the real networks in range.

## Using it

Copy `net.ll.cmake` into your project and `include()` it after `project()`. It resolves net.ll through
`NET_LL_PATH`, downloading it when that directory is not populated, and appends to `SOURCES` /
`INCLUDE_DIRS` for your own target. It brings [fs.ll](https://github.com/juliannojungle/fs.ll) along,
which the download needs, and [hal.ll](https://github.com/juliannojungle/hal.ll) with it.

```c
#include "WiFi.h"

WiFiNetwork networks[20];
uint16_t found = 0;

if (WiFiInitialize() && WiFiScan(networks, 20, &found)) {
    for (uint16_t i = 0; i < found; i++) {
        printf("%s (%d dBm)\n", networks[i].Ssid, networks[i].Rssi);
    }
}

WiFiDeinitialize();
```

An empty result is a success, not a failure: a scan is not repeatable, and consecutive calls legitimately
report different networks.

## Building the example

`src/Sample.c` lists the access points in range. It needs no credentials and no SD card, so it runs on a
bare board straight out of USB.

```bash
# Simulator (desktop)
cmake -B build -DPLATFORM_NAME=Simulator && cmake --build build
./build/net.ll

# RP2040 (needs pico-sdk at ~/pico-sdk or PICO_SDK_PATH; defaults to PICO_BOARD=pico_w)
cmake -B build -DPLATFORM_NAME=RP2040 && cmake --build build

# ESP32 (needs ESP-IDF exported in the shell)
source ~/esp-idf/export.sh && idf.py -DPLATFORM_NAME=ESP32 build
```

## Status

All three platforms build clean, and the Simulator sample runs. **Connecting to a network is not
implemented yet**, and the download is implemented on the Simulator only — on RP2040 and ESP32 it is still
a stub. Neither firmware has been flashed. See `AGENTS.md` for what is verified and what is not.
