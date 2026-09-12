/*
    net.ll is an open-source network library
    for the dot-ll-collection.
    Copyright (C) 2022, Julianno F. C. Silva (@juliannojungle)

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License as published
    by the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/agpl-3.0.html>.
*/

/* RP2040 WiFi through the CYW43439, which the pico-sdk drives over a
 * PIO-implemented half-duplex gSPI, using cyw43_arch from pico-SDK. */

#include "WiFi.h"

#include <stdio.h>
#include <string.h>

#include "pico/cyw43_arch.h"
#include "pico/time.h"

typedef struct {
    WiFiNetwork *Networks;
    uint16_t MaxNetworks;
    uint16_t FoundNetworks;
} ScanTarget;

#define SCAN_TIMEOUT_MS 15000

static bool wifiInitialized = false;

/* A scan result's auth_mode is NOT one of the CYW43_AUTH_* constants, despite what
 * the driver's own comment suggests: those are 32-bit values used when connecting,
 * while this field is a uint8_t bitmask the driver assembles from the beacon's
 * information elements (cyw43_ll.c) -- 1 = WEP/privacy, 2 = WPA, 4 = WPA2. Comparing
 * it against CYW43_AUTH_* silently never matches.
 *
 * The driver does not detect WPA3 on this path, and carries TODOs about not telling
 * TKIP, AES and enterprise apart, so this is as precise as an RP2040 scan gets. */
#define CYW43_SCAN_SECURITY_WEP 0x01
#define CYW43_SCAN_SECURITY_WPA 0x02
#define CYW43_SCAN_SECURITY_WPA2 0x04

static WiFiAuthMode AuthModeFromCyw43(uint8_t authMode) {
    if (authMode & CYW43_SCAN_SECURITY_WPA2) {
        return WIFI_AUTH_MODE_WPA2;
    }
    if (authMode & CYW43_SCAN_SECURITY_WPA) {
        return WIFI_AUTH_MODE_WPA;
    }
    if (authMode & CYW43_SCAN_SECURITY_WEP) {
        return WIFI_AUTH_MODE_WEP;
    }
    return WIFI_AUTH_MODE_OPEN;
}

static int ScanResultReceived(void *environment, const cyw43_ev_scan_result_t *result) {
    ScanTarget *target = (ScanTarget *)environment;

    if (target == NULL || result == NULL || target->FoundNetworks >= target->MaxNetworks) {
        return 0;
    }

    WiFiNetwork *network = &target->Networks[target->FoundNetworks];
    memset(network, 0, sizeof(*network));

    size_t ssidLength = result->ssid_len;
    if (ssidLength > WIFI_SSID_MAX_LENGTH) {
        ssidLength = WIFI_SSID_MAX_LENGTH;
    }
    memcpy(network->Ssid, result->ssid, ssidLength);
    network->Ssid[ssidLength] = '\0';

    memcpy(network->Bssid, result->bssid, WIFI_BSSID_LENGTH);
    network->Channel = result->channel;
    network->AuthMode = AuthModeFromCyw43(result->auth_mode);
    network->Rssi = result->rssi;

    target->FoundNetworks++;
    return 0;
}

bool WiFiInitialize(void) {
    if (wifiInitialized) {
        return true;
    }

    if (cyw43_arch_init() != 0) {
        printf("WiFi: cyw43_arch_init failed, is this board wired to a radio?\n");
        return false;
    }

    cyw43_arch_enable_sta_mode();
    wifiInitialized = true;
    return true;
}

void WiFiDeinitialize(void) {
    if (!wifiInitialized) {
        return;
    }

    cyw43_arch_deinit();
    wifiInitialized = false;
}

bool WiFiScan(WiFiNetwork networks[], uint16_t maxNetworks, uint16_t *foundNetworks) {
    if (networks == NULL || foundNetworks == NULL || maxNetworks == 0) {
        return false;
    }

    *foundNetworks = 0;

    if (!wifiInitialized) {
        printf("WiFi: WiFiInitialize must succeed before scanning\n");
        return false;
    }

    ScanTarget target = {.Networks = networks, .MaxNetworks = maxNetworks, .FoundNetworks = 0};
    cyw43_wifi_scan_options_t options = {0};

    if (cyw43_wifi_scan(&cyw43_state, &options, &target, ScanResultReceived) != 0) {
        printf("WiFi: cyw43_wifi_scan failed to start\n");
        return false;
    }

    /* Synchronous by construction: nothing services the driver in poll mode, so
     * the wait itself has to pump it. */
    absolute_time_t deadline = make_timeout_time_ms(SCAN_TIMEOUT_MS);
    while (cyw43_wifi_scan_active(&cyw43_state)) {
        if (absolute_time_diff_us(get_absolute_time(), deadline) <= 0) {
            printf("WiFi: scan timed out\n");
            return false;
        }

        cyw43_arch_poll();
        sleep_ms(10);
    }

    *foundNetworks = target.FoundNetworks;

    return true;
}

bool WiFiAccessPointStart(const char *ssid, const char *password) {
    (void)ssid;
    (void)password;
    printf("WiFiAccessPointStart: not implemented on RP2040 yet (needs a radio and lwIP enabled)\n");
    return false;
}

bool WiFiAccessPointStop(void) {
    return true;
}

bool WiFiAccessPointIsRunning(void) {
    printf("WiFiAccessPointIsRunning: not implemented on RP2040 yet (needs a radio and lwIP enabled)\n");
    return false;
}

bool WiFiStationConnect(const char *ssid, const char *password) {
    (void)ssid;
    (void)password;
    printf("WiFiStationConnect: not implemented on RP2040 yet (needs a radio and lwIP enabled)\n");
    return false;
}
