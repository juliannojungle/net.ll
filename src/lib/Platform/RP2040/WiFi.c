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
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "dhcpserver.h"

typedef struct {
    WiFiNetwork *Networks;
    uint16_t MaxNetworks;
    uint16_t FoundNetworks;
} ScanTarget;

#define SCAN_TIMEOUT_MS 15000
#define STATION_CONNECT_TIMEOUT_MS 15000
#define STATION_CONNECT_POLL_MS 100
#define ACCESS_POINT_NETMASK "255.255.255.0"

static bool wifiInitialized = false;
static bool accessPointRunning = false;
static dhcp_server_t dhcpServer;

bool WiFiAccessPointStop(void);

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

    WiFiAccessPointStop();
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

static void StartAccessPointDhcp(void) {
    ip4_addr_t gateway;
    ip4_addr_t netmask;
    ip4addr_aton(WIFI_ACCESS_POINT_ADDRESS, &gateway);
    ip4addr_aton(ACCESS_POINT_NETMASK, &netmask);

    struct netif *accessPointNetif = &cyw43_state.netif[CYW43_ITF_AP];
    netif_set_addr(accessPointNetif, &gateway, &netmask, &gateway);

    /* lwIP carries only a DHCP client, so clients get their address from the vendored
     * MicroPython server, handing out leases in the gateway's subnet. */
    dhcp_server_init(&dhcpServer, accessPointNetif, &gateway, &netmask);
}

bool WiFiAccessPointStart(const char *ssid, const char *password) {
    if (ssid == NULL || ssid[0] == '\0') return false;

    if (!wifiInitialized) {
        printf("WiFi: WiFiInitialize must succeed before starting an access point\n");
        return false;
    }

    if (accessPointRunning) return true;

    uint32_t authMode = (password == NULL || password[0] == '\0') ? CYW43_AUTH_OPEN : CYW43_AUTH_WPA2_AES_PSK;
    cyw43_arch_enable_ap_mode(ssid, password, authMode);

    StartAccessPointDhcp();
    accessPointRunning = true;
    return true;
}

bool WiFiAccessPointStop(void) {
    if (!accessPointRunning) return true;

    dhcp_server_deinit(&dhcpServer);
    cyw43_arch_disable_ap_mode();
    accessPointRunning = false;
    return true;
}

bool WiFiAccessPointIsRunning(void) {
    return accessPointRunning;
}

static bool StationHasAddress(void) {
    const struct netif *stationNetif = &cyw43_state.netif[CYW43_ITF_STA];

    return cyw43_wifi_link_status(&cyw43_state, CYW43_ITF_STA) == CYW43_LINK_UP &&
           !ip4_addr_isany_val(*netif_ip4_addr(stationNetif));
}

/* Polled rather than event-driven so the call returns only once the outcome is known,
 * without this library owning a thread to wait on. An address is what makes the link
 * usable, so association alone is not enough. The wait pumps the driver itself. */
static bool WaitForStationAddress(void) {
    absolute_time_t deadline = make_timeout_time_ms(STATION_CONNECT_TIMEOUT_MS);

    while (absolute_time_diff_us(get_absolute_time(), deadline) > 0) {
        if (StationHasAddress()) return true;

        cyw43_arch_poll();
        sleep_ms(STATION_CONNECT_POLL_MS);
    }

    return false;
}

bool WiFiStationConnect(const char *ssid, const char *password) {
    if (ssid == NULL || ssid[0] == '\0') return false;

    if (!wifiInitialized) {
        printf("WiFi: WiFiInitialize must succeed before connecting\n");
        return false;
    }

    /* One radio, and it cannot hold both modes. */
    if (accessPointRunning) {
        printf("WiFi: cannot connect while the access point is running\n");
        return false;
    }

    bool open = (password == NULL || password[0] == '\0');
    uint32_t authMode = open ? CYW43_AUTH_OPEN : CYW43_AUTH_WPA2_AES_PSK;

    if (cyw43_arch_wifi_connect_async(ssid, open ? NULL : password, authMode) != 0) {
        printf("WiFi: cyw43_arch_wifi_connect_async failed\n");
        return false;
    }

    if (WaitForStationAddress()) return true;

    printf("WiFi: could not connect to %s\n", ssid);
    return false;
}
