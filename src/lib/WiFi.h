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

#ifndef __WIFI_H_
#define __WIFI_H_

#include <stdbool.h>
#include <stdint.h>

#define WIFI_SSID_MAX_LENGTH 32
#define WIFI_BSSID_LENGTH 6
#define SCAN_MAX_NETWORKS 32

#ifndef WIFI_ACCESS_POINT_ADDRESS
#define WIFI_ACCESS_POINT_ADDRESS "192.168.33.1"
#endif

typedef enum {
    WIFI_AUTH_MODE_UNKNOWN = 0,
    WIFI_AUTH_MODE_OPEN,
    WIFI_AUTH_MODE_WEP,
    WIFI_AUTH_MODE_WPA,
    WIFI_AUTH_MODE_WPA2,
    WIFI_AUTH_MODE_WPA3,
} WiFiAuthMode;

typedef struct {
    char Ssid[WIFI_SSID_MAX_LENGTH + 1];
    uint8_t Bssid[WIFI_BSSID_LENGTH];
    uint16_t Channel;
    WiFiAuthMode AuthMode;
    int16_t Rssi; /* dBm */
} WiFiNetwork;

bool WiFiInitialize(void);
void WiFiDeinitialize(void);
bool WiFiScan(WiFiNetwork networks[], uint16_t maxNetworks, uint16_t *foundNetworks);
bool WiFiScanStart(void);
bool WiFiScanIsComplete(void);
bool WiFiScanGetResults(WiFiNetwork networks[], uint16_t maxNetworks, uint16_t *foundNetworks);
bool WiFiAccessPointStart(const char *ssid, const char *password);
bool WiFiAccessPointStop(void);
bool WiFiAccessPointIsRunning(void);
bool WiFiStationConnect(const char *ssid, const char *password);

#endif /* __WIFI_H_ */
