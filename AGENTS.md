# AGENTS.md

Working notes for AI agents and new contributors on **net.ll**.

---

## Ground rules

### 0. Every dot-ll project's README starts with the collection header

The two blockquotes right under the title must not be lost:

```markdown
# net.ll

> 🦖 **Part of [dot-ll-collection](https://github.com/topics/dot-ll-collection)**

> ⚠️ **This project is under active development. The documentation is growing along the project as it's a work-in-progress.**
```

More generally: **a file that already exists in the repository gets appended to, never rewritten from
scratch.** An agent destroyed hal.ll's header once by regenerating `README.md` wholesale.

### 1. Everything written to this repository is in English

Code, identifiers, string literals, comments, commit messages, `README.md`, this file. Not a single
variable or comment in another language. The **chat is separate**: Julianno, the lead dev, prefers to talk
in pt-BR, and that says nothing about what gets written to disk.

If you find non-English text anywhere outside the chat, stop and ask the dev.

**One approved exception, and it is narrow.** `src/lib/Platform/Simulator/WiFi.c` holds Portuguese label
spellings — `Autenticação`, `Sinal`, `Canal` — in the `LABEL_*_PT` macros. Those are **input data parsed
from an external program**, not prose this project authors: `netsh.exe` writes its labels in the Windows
UI language, and the dev's host is pt-BR. Each label is matched in Portuguese *or* English, so the parser
works on either host. **The dev approved this explicitly.** Do not "translate" or delete those strings,
and do not read the exception as license to write non-English anywhere else.

### 2. The dev takes the decisions, never the agent

Architectural choices, naming, trade-offs, scope, ambiguity in a requirement — **ask the dev**. Do not
decide and proceed, and do not present a decision as if it had already been made.

### 3. Do not make assumptions

Only two things count as true: what can be **validated in the code** and what the **dev states
explicitly**. Everything else is a question. Where this file states something unverified, it says so.

### 4. Comments only when essential

**The code has to speak for itself.** Write a comment only when it carries information the code cannot: a
non-obvious *why*, a hardware or spec quirk, units, a subtle invariant, or a short module header. Do not
restate what the code says, do not narrate step by step, and keep it to a line or two. Long rationale
belongs in this file.

The comments that earn their place here are the traps: why the auth-mode enum carries a prefix, why the
RP2040's scan `auth_mode` is not what the driver's own comment suggests, why only two of netsh's output
lines are parsed. Each one marks a bug that a naive edit would reintroduce.

### 5. Do not write tests

No unit tests, no test harness, no test build system. Verification is a clean build plus running the
sample on the Simulator or on the device. This is the dev's standing policy across the collection.

### 6. This library is 100% C

**No C++ anywhere in this repository**, and the same holds for every library in the dot-ll-collection.
C++ belongs to pedal.guru, the application, and nowhere else. fs.ll, gui.ll and hal.ll are C and consume
each other, so a C++ construct in a shared header would break them.

### 7. All hardware access goes through hal.ll — but net.ll needs none of it

The collection's rule is that nothing outside hal.ll touches hardware directly. **net.ll does not reach
hal.ll at all**, and that is not a violation: on every platform the radio arrives as a whole stack from
the platform SDK rather than as a bus (§5). The only exception is `src/Sample.c`, which calls
`STDIOInitAll()` so `printf` reaches USB on the RP2040 — that is hal.ll's, as the rule requires.

hal.ll still ends up in the build, because net.ll depends on fs.ll and `fs.ll.cmake` includes it.

---

## 1. What this project is

**net.ll** ("low level networking") is a small C library that gives an application, or a sibling library,
one API for the network: listing the WiFi access points in range, and downloading a file over HTTP or
HTTPS straight to storage.

It is a **library meant to be consumed by other projects**, not an application. `src/lib` is the library;
`src/Sample.c` is a usage example and the root `CMakeLists.txt` exists to build it.

Supported platforms: `Simulator` (Linux desktop, and Windows via WSL), `RP2040` (via pico-sdk) and
`ESP32` (via ESP-IDF).

It was split out of pedal.guru, where the HTTP client used to live under `src/Platform`. That is wave 4
of the five-wave migration whose plan and state live in pedal.guru's `AGENTS.md` §17.

## 2. Repository layout

```
CMakeLists.txt                      builds src/Sample.c, per platform
net.ll.cmake                        the build contract; copied into consumer projects
AGENTS.md                           this file
README.md                           user-facing overview
src/Sample.c                        usage example: lists the access points in range
src/lib/WiFi.h                      the WiFi API and its result type
src/lib/HttpClient.h                the download API
src/lib/Platform/<Platform>/        one folder per platform, same file names in each:
    WiFi.c                            bring-up and scan
    HttpClient.c                      the download
    CMakeLists.txt                    ESP32 only: ESP-IDF component registration
    lwipopts.h                        RP2040 only: lwIP config for pico_cyw43_arch_lwip_poll
    dhcpserver.{c,h}                  RP2040 only: vendored MIT DHCP server for the access point
src/Dependency/fs.ll.cmake          fs.ll's build contract, copied here
src/Dependency/fs.ll/               resolved through FS_LL_PATH — NOT a submodule, git-ignored
src/Dependency/pico_sdk_import.cmake  stock pico-sdk locator, used by the RP2040 build
```

## 3. The API

### WiFi (`src/lib/WiFi.h`)

```c
bool WiFiInitialize(void);
void WiFiDeinitialize(void);
bool WiFiScan(WiFiNetwork networks[], uint16_t maxNetworks, uint16_t *foundNetworks);
```

Bring-up is **explicit**, like fs.ll's `MountSdCard`, rather than hidden inside the scan: on ESP32 it is
expensive (NVS, netif, event loop, driver init, station mode, start) and doing it per scan would be waste.

`WiFiScan` fills the **caller's array** and reports how many entries it wrote. **No callbacks** — decided
by the dev. ESP32 already works that way; the RP2040 driver is callback-driven, so its implementation
accumulates into the array inside its own callback. The awkwardness belongs in the library, not in every
consumer.

**An empty result is a success, not a failure.** A scan is not repeatable: consecutive calls legitimately
report different networks, and that was observed in practice (one call returned 1 network, the next 3).

`WiFiNetwork` carries `Ssid`, `Bssid`, `Channel`, `AuthMode` and `Rssi` (dBm). **One entry per BSS, not
per name** — several access points can advertise the same SSID (a mesh, or one router publishing 2.4 GHz
and 5 GHz), so the same `Ssid` may appear more than once with a different `Bssid`. That mirrors what a
hardware scan reports; de-duplicating for display is the consumer's decision.

> **SSID and BSSID**, since both appear throughout: an SSID is the network's *name*, the string a user
> picks from a list. A BSSID is the MAC address of one specific access point radio. The SSID says *which
> network*, the BSSID says *which antenna you are actually talking to*.

### HTTP (`src/lib/HttpClient.h`)

```c
bool HttpDownloadFile(const char *url, const char *filePath);
```

Streams the body **straight to storage** rather than buffering it — the hardware has little RAM. There is
deliberately **no in-memory variant**: it was considered and rejected, and nothing needs one. If some
feature ever does, it gets added then.

The name lost the underscore it carried in pedal.guru (`HttpClient_DownloadFile`), which was legacy; the
dev cleared that up. Parameters and behaviour are otherwise unchanged.

### Every operation is synchronous, and net.ll starts no thread

**A hard rule from the dev.** Every call returns only once the work is done. net.ll creates no thread on
any platform.

The one honest limit: **ESP-IDF's WiFi driver runs internal tasks of its own**, and `esp_wifi_init`
requires the default event loop, which is a task too. That cannot be switched off — it is how `esp_wifi`
is built. Those tasks service the radio from the moment it is brought up, whether or not anything is
being transferred. So: **do not claim ESP32 is free of background threads; claim that net.ll creates
none.** No network operation here needs a thread of ours.

### Platform differences that matter

| | Simulator | RP2040 | ESP32 |
|---|---|---|---|
| radio | none — borrows the host's, see §4 | CYW43439 via pico-sdk | native, via `esp_wifi` |
| scan source | `netsh.exe` under WSL | `cyw43_wifi_scan` + poll loop | `esp_wifi_scan_start(block)` |
| scan fields | all real; RSSI reconstructed from a percentage (§4) | all real, but no WPA3 (§6) | all real |
| `HttpDownloadFile` | real: POSIX sockets + OpenSSL (HTTP + HTTPS) | real: lwIP's HTTP client, **HTTP only** | real: `esp_http_client`, **HTTP only** |
| entry point | `main()` | `main()` | `app_main()` (`ESP_PLATFORM` defined) |

**The download is implemented on all three platforms.** The Simulator does HTTP and HTTPS; the two
hardware platforms do **HTTP only** for now, since HTTPS would pull in mbedTLS (RP2040) or a TLS build
(ESP32) for RAM this project is trying to spare, and the OSM tile server answers plain HTTP anyway. The
RP2040 drives lwIP's own client (`httpc_get_file_dns`) in poll mode; the ESP32 uses `esp_http_client`,
whose `perform` blocks. Both stream the body to storage through fs.ll and stay synchronous. HTTPS on
hardware is the last open item (§11).

## 4. The Simulator has no radio, so it borrows the host's

There is no wireless hardware behind the Simulator, and the platform folders have to expose the same
function names. So the scan shells out: under **WSL**, `netsh.exe` is reachable and reports the real
networks in range. On any other host, `WiFiInitialize` reports that it cannot scan and returns `false`.

Several things about this are load-bearing:

- **WSL is detected**, not assumed: `WSL_INTEROP` or `WSL_DISTRO_NAME` in the environment, falling back to
  `microsoft`/`WSL` in `/proc/sys/kernel/osrelease`. Detection alone is not enough, so a failing
  `netsh.exe` is handled too — WSL interop genuinely breaks when `systemd-binfmt.service` is masked, which
  is why gui.ll ships `Toolchain/wsl.sh`. The call also carries a `timeout`, or a wedged interop would hang
  the caller.
- **The structure is anchored on `SSID` and `BSSID`; the value labels are matched in two languages.**
  Those two acronyms survive translation, so they carry the nesting regardless of the Windows UI language.
  Everything else is labelled in that language, so authentication, signal and channel are matched as
  `Autenticação`/`Authentication`, `Sinal`/`Signal` and `Canal`/`Channel` — the approved exception in
  ground rule 1. The Portuguese spellings are measured on the dev's host; **the English ones are not
  verified**, since this machine's Windows is pt-BR, so treat them as expected wording.

  **A label match has to be anchored *and* followed by nothing but blanks before the colon.** A plain
  prefix test is not enough, and this is a real trap rather than a hypothetical: the Bss Load subsection
  carries `Utilização do canal :` and, in English, `Channel utilization :` — the latter *starts with* the
  channel label, so a prefix test would overwrite the channel with a utilisation percentage. Verified
  against captured netsh output containing exactly that: channel came out 116 and 6, not 63 and 99.

- **The protocol acronyms inside a value are not translated, so those are what the auth parser matches** —
  `WPA3`, `WPA2`, `WPA`, `WEP`, checked strongest first because `WPA2-Personal` contains `WPA` too. A
  recognised label with none of them means an open network, which spares having to know how Windows spells
  "open" in every language.
- **`chcp` is not a way around the localisation, and that was measured.** It sets the console *code page*, i.e. character
  encoding, not the UI language: `cmd.exe /c "chcp 437 >nul && netsh wlan show networks"` still printed
  Portuguese labels, and so did `chcp 65001`. There was nothing to fix anyway — WSL interop already hands
  the output over as UTF-8, so an SSID with accents decodes correctly. And going through `cmd.exe` is
  worse than calling `netsh.exe` directly: from a WSL working directory it emits a UNC warning as noise,
  which came out mojibake with `chcp` in effect. **Do not retry this.**
- **Every field is real, and the mocks are only fallbacks now.** Authentication, channel and signal are
  parsed. `FALLBACK_RSSI`, `FALLBACK_CHANNEL` and `FALLBACK_AUTH_MODE` (WPA2) apply **only** when a label
  went unrecognised, which means an unexpected Windows UI language.

  **Why those fallbacks are not zero:** a field must never be left zero when zero *means* something. A
  zeroed auth mode reads as "open network, no password needed", and 0 dBm reads as an impossibly strong
  signal — either would make a consumer behave differently here than on hardware, silently.

  **Signal is reconstructed, not measured.** netsh reports a quality percentage, while the API carries RSSI
  in dBm. Windows derives that percentage from RSSI over a documented linear scale where 0% is -100 dBm and
  100% is -50 dBm, so the parser inverts it: `rssi = (percentage / 2) - 100`. Verified against real output
  — 91% came out -55 dBm, 16% came out -92. It is an approximation of the original figure, because
  inverting a lossy mapping cannot be anything else.

A hidden network reports an empty SSID, which is valid and has to be tolerated — the sample prints it as
`<hidden>`. netsh nests BSSIDs under each SSID, so the current SSID is remembered and copied into every
entry below it, which is what produces one entry per BSS.

## 5. Why net.ll does not go through hal.ll

The question was whether reaching a radio means going through a bus, which would put it behind hal.ll.
**It does not**, on any platform, and the RP2040 is the interesting case because its chip has no radio of
its own. Verified in the pico-sdk:

- **The bus is a half-duplex gSPI implemented in PIO, and the SDK owns the driver** —
  `src/rp2_common/pico_cyw43_driver/cyw43_bus_pio_spi.c` plus its `.pio` program. hal.ll exposes no PIO
  at all (GPIO, SPI, PWM, UART only), so routing this through it would mean adding a PIO abstraction *and*
  reimplementing a driver the SDK already ships.
- **The wire count gives it away**: in the Pico W board header, `CYW43_DEFAULT_PIN_WL_DATA_OUT`,
  `..._DATA_IN` and `..._HOST_WAKE` are **all GPIO 24** — one bidirectional data line. A conventional SPI
  abstraction with separate MOSI/MISO cannot model it. The rest: 23 = `WL_REG_ON`, 25 = `WL_CS`,
  29 = `WL_CLOCK`, all internal to the board.
- **What the SDK hands over is a stack, not a bus**: `cyw43_arch_init()`,
  `cyw43_arch_enable_sta_mode()`, and lwIP on top when TCP/IP is wanted.

### Poll mode, with lwIP on

The driver has to be *serviced* for its callbacks to fire, and the SDK offers two ways.
`PICO_CYW43_ARCH_THREADSAFE_BACKGROUND` services the chip from an interrupt behind the caller's back —
which the synchronous rule forbids, and which `pico_cyw43_arch_none` forces. So net.ll links
**`pico_cyw43_arch_lwip_poll`**, and every wait pumps `cyw43_arch_poll()` itself: the scan while
`cyw43_wifi_scan_active()` holds, and the station connect while no address has arrived. The `poll`
family creates no thread or task (verified: no `xTaskCreate`, no `multicore_launch`, no
`pthread_create`), and `pico_cyw43_arch_lwip_poll` links `pico_lwip_nosys` — lwIP in **`NO_SYS=1` mode,
no OS threads**, on the same `async_context`. **The synchronous rule survives lwIP intact.**

**Why lwIP is on, and why the target changed.** The scan works at the driver level and needs no TCP/IP,
but station connect and the access point do: an association without an address is not a usable link, and
the AP has to hand addresses out. `pico_cyw43_arch_poll` sets only `PICO_CYW43_ARCH_POLL=1`, and with
`CYW43_LWIP` left undefined the driver defaults to lwIP and then demands an `lwipopts.h` — so previously
the contract published `CYW43_LWIP=0` to keep the stack out while nothing called it. Now that connect and
the AP are implemented, the contract publishes **`CYW43_LWIP=1`** and links `pico_cyw43_arch_lwip_poll`,
and net.ll ships an `lwipopts.h` in `src/lib/Platform/RP2040/` (NO_SYS, DHCP client, values following the
public pico-examples common configuration). The contract puts that folder on `INCLUDE_DIRS` for the
RP2040 so lwIP finds the header; the consumer applies `CYW43_LWIP=1` at directory scope, as with any
`PLATFORM_DEFINITIONS` value.

**The access point's DHCP server is vendored.** lwIP carries only a DHCP *client*, and the ESP32 gets a
server for free from esp-netif (`esp_netif_dhcps_*`); the RP2040 has no equivalent in the SDK. So
`src/lib/Platform/RP2040/dhcpserver.{c,h}` is copied verbatim from the pico-examples access_point helper,
which is MicroPython's server under the **MIT licence (Copyright (c) 2018-2019 Damien P. George)** — its
header is left intact, and MIT composes with this repository's AGPL. It needs only UDP from the stack.
`WiFiAccessPointStart` sets the AP netif's address to `WIFI_ACCESS_POINT_ADDRESS` and starts that server,
mirroring the ESP32's stop-set-start-DHCP flow.

**The download now uses lwIP's own HTTP client on the RP2040.** `HttpDownloadFile` calls
`httpc_get_file_dns` (linked through `pico_lwip_http`), writes each received pbuf straight to storage in
the `recv_fn`, and waits for the `result_fn` while pumping `cyw43_arch_poll()` — the same poll discipline
as the scan and the connect. HTTP only: HTTPS would hang off `httpc_connection_t`'s `altcp_allocator` with
`pico_mbedtls`, which is deferred (§11).

### The board matters

The plain RP2040 has no radio, so the root `CMakeLists.txt` defaults `PICO_BOARD` to `pico_w`, which is
what pulls in the board header with the `CYW43_*` pins. Override it for a board wiring the module
elsewhere.

Worth keeping straight: pedal.guru's RP2040 target is the **Waveshare RP2040-LCD-1.28**, which has no
radio. The **Pico W** is a development vehicle — it works straight out of USB with nothing soldered. On
the real device the radio would be Raspberry Pi's **RM2**, the same CYW43439 sold separately, whose
documentation states full software compatibility with the Pico W SDK over the same gSPI interface. Its
pins on that board would differ, and every `CYW43_DEFAULT_PIN_WL_*` is `#ifndef`-guarded (with
`CYW43_PIN_WL_DYNAMIC` for runtime selection), so overriding them is supported. **Do not write any RM2 pin
assignment down until the dev confirms the wiring** — it is not wired yet.

## 6. Traps

Both of these were live bugs during wave 4, caught by the compiler and by reading the driver. They are
recorded because a naive edit reintroduces them.

- **`WiFiAuthMode`'s values carry a `WIFI_AUTH_MODE_` prefix out of necessity, not style.** ESP-IDF's own
  `wifi_auth_mode_t` already defines `WIFI_AUTH_OPEN` and `WIFI_AUTH_WEP`, and C enum values share the
  enclosing scope — the shorter names are a compile error on that platform.
- **The RP2040's scan `auth_mode` is not a `CYW43_AUTH_*` constant**, despite the driver's own comment
  pointing at them. Those are 32-bit values used when *connecting*; the scan field is a `uint8_t` bitmask
  the driver assembles from the beacon's information elements (`cyw43_ll.c`): `1` = WEP/privacy,
  `2` = WPA, `4` = WPA2. Comparing against `CYW43_AUTH_*` silently never matches —
  `-Wswitch-outside-range` is what exposed it. The driver detects **no WPA3** on this path and carries
  TODOs about not telling TKIP, AES and enterprise apart, so that mapping is as precise as an RP2040 scan
  gets.

Not a defect, recorded so it is not "fixed": the Simulator's `USER_AGENT` is
`NET_LL_USER_AGENT`, defaulting to `net.ll/...`. A library must not identify itself as one particular
application, and it used to say `PedalGuru`. A consumer that needs its own string — the OSM tile policy
asks for an identifiable agent — overrides the macro from its build.

## 7. Build system

There is no `add_library`. The contract publishes list variables that the consumer feeds into its own
target, and they are **appended to**, never overwritten, so several libraries following this architecture
accumulate into one build.

### `net.ll.cmake`

| variable | role |
|---|---|
| `NET_LL_PATH` | in/out. Root of the net.ll checkout. Accepted as a normal variable or an environment variable; relative paths resolve against `CMAKE_SOURCE_DIR`. Defaults to a `net.ll` folder next to the copied file. Ends up in the cache. |
| `PLATFORM_NAME` | in. `Simulator` (default), `RP2040` or `ESP32`. |
| `NET_LL_PLATFORM_DIR` | out. The resolved platform folder. |
| `SOURCES` / `INCLUDE_DIRS` | out. Appended with net.ll's, and with fs.ll's through its contract. |
| `PLATFORM_LIBRARIES` | out. What to pass to `target_link_libraries`. |
| `PLATFORM_REQUIRES` | out. What to pass to `idf_component_register`'s `REQUIRES`. |
| `PLATFORM_DEFINITIONS` | out. Compile definitions the consumer must apply. |

`PLATFORM_DEFINITIONS` is **new in this library** — hal.ll and fs.ll publish only the first two lists. It
exists because `CYW43_LWIP=1` has to reach the **pico-sdk's own** cyw43 sources, not just ours, so a
target-scoped definition is not enough and the consumer has to apply it at directory scope. The contract
cannot do that itself (see the rules below), so it publishes the value and the consumer applies it. In
this repository the root `CMakeLists.txt` does that in its RP2040 branch.

Resolution is two steps: default the path, then check the sentinel file `src/lib/WiFi.h`. If the sentinel
is missing, the directory is populated with a shallow `git clone` at configure time. The download is
deliberately **not** `FetchContent` — see fs.ll's `AGENTS.md` for the rationale.

**Three rules this file must obey**, all consequences of ESP-IDF's build model. ESP-IDF evaluates the
consumer's component `CMakeLists.txt` **twice**: first in **script mode** (`cmake -P`) purely to harvest
`REQUIRES`, then for real. In that first pass there is no project, no targets and **no cache**.

1. **Variables and messages only.** Directory- and target-scoped commands such as
   `add_compile_definitions` do not exist in script mode and abort the ESP32 configure. This is exactly
   why `PLATFORM_DEFINITIONS` is published rather than applied.
2. **Never clobber a `PLATFORM_NAME` the caller already set.** With no cache, an unguarded
   `set(... CACHE ...)` is *not* skipped and would silently reset the platform to `Simulator`. The
   consumer's component must also `set(PLATFORM_NAME "ESP32")` in the file itself, because `-D` arguments
   live in the cache and are invisible there.
3. **Publish what script mode needs, then `return()` before touching the filesystem.** That is why the
   link lists are set at the very top: they depend only on `PLATFORM_NAME`. The early return still has to
   `include` fs.ll's contract, or the rest of the chain's `REQUIRES` is lost:

   ```cmake
   if(DEFINED CMAKE_SCRIPT_MODE_FILE)
       include(${NET_LL_PATH}/src/Dependency/fs.ll.cmake)
       return()
   endif()
   ```

### Why fs.ll, and where hal.ll comes from

A download streams to storage, so `net.ll.cmake` ends by including its own versioned copy of
`fs.ll.cmake`. That copy also brings hal.ll in, since fs.ll's contract includes hal.ll's. So net.ll never
carries a copy of `hal.ll.cmake` and never resolves `HAL_LL_PATH` itself.

Include order between the sibling contracts does not matter. It used to, before the HAL was extracted;
there is now exactly one `HAL.h` and one `HALConfig.h` in any tree.

`src/Dependency/fs.ll` is therefore **not a git submodule** (it is git-ignored): the contract resolves or
downloads it at configure time. With nothing pinned, hal.ll lands *inside* that checkout, at
`src/Dependency/fs.ll/src/Dependency/hal.ll`, because fs.ll's copy of the contract defaults the path next
to itself.

### Root `CMakeLists.txt`

Sets `NET_LL_PATH` to `CMAKE_SOURCE_DIR` up front, because for an in-tree build this repository *is* the
net.ll root and nothing should be downloaded. Then it branches on `PLATFORM_NAME` and closes with an
`else()` raising `FATAL_ERROR` — without it an unknown platform matches no branch and cmake reports
success while writing a build system with no target.

The Simulator branch is the one that needs `find_package(OpenSSL)`, for HTTPS.

## 8. Building and running

`CMAKE_EXPORT_COMPILE_COMMANDS` is on and `.clangd` reads `build/compile_commands.json`, so **build into
`build/`** for working code intelligence. `.clangd` also strips the ARM flags so a desktop clangd can
parse the embedded builds, and defines `DEBUGMSGS`.

```bash
# Simulator
cmake -B build -DPLATFORM_NAME=Simulator && cmake --build build
./build/net.ll

# RP2040 (needs pico-sdk at ~/pico-sdk or PICO_SDK_PATH; defaults to PICO_BOARD=pico_w)
cmake -B build -DPLATFORM_NAME=RP2040 && cmake --build build

# ESP32 (needs ESP-IDF exported in the shell)
source ~/esp-idf/export.sh && idf.py -DPLATFORM_NAME=ESP32 build
```

The sample needs no SD card and no credentials, which is the whole point: it runs on a bare Pico W or a
bare ESP32-S3 straight out of USB.

## 9. Current status

**All three platforms configure, compile and link clean, with zero warnings.** Verified:

| platform | result |
|---|---|
| Simulator | builds, **and the sample runs correctly** — lists the real networks in range through `netsh.exe`, with real authentication, channel and signal, including a hidden one shown as `<hidden>` |
| RP2040 | builds and links, produces `net.ll.uf2` (~658 KB) for `PICO_BOARD=pico_w`, with lwIP on (`pico_cyw43_arch_lwip_poll` + `pico_lwip_http`) and the full API — scan, station connect, access point, and an HTTP download |
| ESP32 | builds and links, produces `net.ll.bin` (~767 KB) for `esp32s3`, with the full API including an HTTP download over `esp_http_client` |

Also verified: an external consumer with **nothing pinned**, which cloned fs.ll and (inside it) hal.ll at
configure time and then built and ran clean.

## 10. Not verified

- **Neither firmware has been flashed or run.** Both compile and link; nothing more is established. So
  the RP2040 and ESP32 `WiFi.c` — bring-up and scan alike — is **new code that has never executed**. That
  includes the poll loop, the auth-mode mapping and the ESP32 NVS/netif/event-loop sequence.
- **The RP2040 station connect and access point are new and unrun.** So is the vendored DHCP server on
  this platform, the `lwipopts.h`, and the connect poll-until-address loop. They compile and link against
  `pico_cyw43_arch_lwip_poll` with `PICO_BOARD=pico_w`; that is all that is established.
- **`HttpDownloadFile` has only ever run on the Simulator**, where it is the same code that already works
  inside pedal.guru. The RP2040 (`httpc_get_file_dns` + poll loop) and ESP32 (`esp_http_client`)
  implementations are new code that compiles and links but has never executed, so a clean build says
  nothing about downloading on a device.
- Nothing here is validated **on hardware**. Statements about the RP2040 and ESP32 come from reading the
  SDKs and compiling against them.

## 11. Open items

Each needs a decision from the dev. None is scheduled.

- **Connect on the Simulator is still a no-op**, and deliberately so. The RP2040 and ESP32 now implement
  `WiFiStationConnect`, but the Simulator has no radio to join a network with. The intended flow is that
  an application shows the scanned networks, the **user picks one**, and a failure moves on to the next.
  One finding kept so the research is not repeated: a Simulator connect that always succeeded would make
  that retry flow untestable, which defeats the purpose. `netsh.exe wlan show interfaces` reports the
  host's current `SSID` and `AP BSSID` (both locale-robust anchors), so the Simulator could succeed for
  the network the host is really on and fail for the others — self-consistent, and it makes the failure
  path real on the desktop.
- **Where WiFi credentials come from is not net.ll's question.** It has no answer here by design: the
  scan needs none, and the application owns that. **No submodule may know pedal.guru exists**, so pulling
  the application's settings type down into this library is not an option at any price.
- **A native-Linux scan.** Not written on purpose: the dev's Simulator host is Windows + WSL, so an
  nl80211/`libnl` implementation — or shelling to `nmcli`/`iw` — would be a second body of code, with
  privilege problems attached, for a host nobody currently runs. Add it the day a real Linux desktop
  matters.
- **The licence header wording in this repository was written by the agent** and has not been approved.
  It follows hal.ll's shape ("net.ll is an open-source network library for the dot-ll-collection") rather
  than pedal.guru's, which talks about cycle computers and does not fit a standalone library. Confirm or
  replace it. hal.ll has the same item open.
- **The `Toolchain/` folder does not exist here.** gui.ll's is the complete one and is reused across the
  collection; the dev expects it to become its own project. Do not start a copy.
- **HTTPS on the two hardware platforms — last, and priority zero.** The download is HTTP only on the
  RP2040 and ESP32. HTTPS would add mbedTLS on the RP2040 (an `mbedtls_config.h`, per-connection RAM on a
  264 KB chip, and a certificate-trust decision) and a TLS build on the ESP32. The OSM tile server answers
  plain HTTP, so this buys nothing today and costs the RAM the project is trying to reduce. The dev's
  call: leave it as the final item, done only when there is a concrete need.
