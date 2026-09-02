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

/* Usage example: list the access points in range.
 *
 * A scan is what this sample does because it needs no credentials and no SD
 * card, so it runs on a bare board straight out of USB. Downloading is left to
 * a consumer, and to a later sample once there is hardware with storage to test
 * it on. */

#include "HAL.h"
#include "WiFi.h"

#include <stdio.h>

#define MAX_NETWORKS 20

static const char *AuthModeName(WiFiAuthMode authMode) {
    switch (authMode) {
        case WIFI_AUTH_MODE_OPEN:
            return "open";
        case WIFI_AUTH_MODE_WEP:
            return "WEP";
        case WIFI_AUTH_MODE_WPA:
            return "WPA";
        case WIFI_AUTH_MODE_WPA2:
            return "WPA2";
        case WIFI_AUTH_MODE_WPA3:
            return "WPA3";
        default:
            return "unknown";
    }
}

static void PrintNetworks(const WiFiNetwork networks[], uint16_t count) {
    printf("%-34s %-18s %7s %6s %s\n", "SSID", "BSSID", "CHANNEL", "RSSI", "AUTH");

    for (uint16_t i = 0; i < count; i++) {
        const WiFiNetwork *network = &networks[i];
        printf("%-34s %02x:%02x:%02x:%02x:%02x:%02x %7u %6d %s\n",
               network->Ssid[0] != '\0' ? network->Ssid : "<hidden>",
               network->Bssid[0], network->Bssid[1], network->Bssid[2],
               network->Bssid[3], network->Bssid[4], network->Bssid[5],
               network->Channel, network->Rssi, AuthModeName(network->AuthMode));
    }
}

void app_entry(void) {
    STDIOInitAll();

    printf("net.ll sample: scanning for WiFi networks\n");

    if (!WiFiInitialize()) {
        printf("Could not bring the radio up, nothing to scan.\n");
        return;
    }

    WiFiNetwork networks[MAX_NETWORKS];
    uint16_t found = 0;

    if (WiFiScan(networks, MAX_NETWORKS, &found)) {
        printf("Found %u network(s).\n", found);
        if (found > 0) {
            PrintNetworks(networks, found);
        }
    } else {
        printf("Scan failed.\n");
    }

    WiFiDeinitialize();
}

#ifdef ESP_PLATFORM
void app_main(void) { app_entry(); }
#else
int main(void) { app_entry(); return 0; }
#endif
