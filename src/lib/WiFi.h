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

/* An SSID is at most 32 octets, and this array is NUL-terminated for printing. */
#define WIFI_SSID_MAX_LENGTH 32
#define WIFI_BSSID_LENGTH 6

/* The WIFI_AUTH_MODE_ prefix is not decoration: ESP-IDF's own wifi_auth_mode_t
 * already defines WIFI_AUTH_OPEN and WIFI_AUTH_WEP, and C enum values share the
 * enclosing scope, so the shorter names collide at compile time on that platform. */
typedef enum {
    WIFI_AUTH_MODE_UNKNOWN = 0,
    WIFI_AUTH_MODE_OPEN,
    WIFI_AUTH_MODE_WEP,
    WIFI_AUTH_MODE_WPA,
    WIFI_AUTH_MODE_WPA2,
    WIFI_AUTH_MODE_WPA3,
} WiFiAuthMode;

/* One entry per BSS, not per name: several access points may advertise the same
 * SSID (mesh, or one router publishing 2.4GHz and 5GHz), so the same Ssid can
 * appear more than once with a different Bssid. De-duplicating for display is
 * the caller's decision, not this library's. */
typedef struct {
    char Ssid[WIFI_SSID_MAX_LENGTH + 1];
    uint8_t Bssid[WIFI_BSSID_LENGTH];
    uint16_t Channel;
    WiFiAuthMode AuthMode;
    int16_t Rssi; /* dBm */
} WiFiNetwork;

/* Brings the radio up. Required before WiFiScan, and paired with WiFiDeinitialize. */
bool WiFiInitialize(void);
void WiFiDeinitialize(void);

/*
 * Scans for access points in range and fills up to maxNetworks entries of the
 * caller's array, writing how many were found to foundNetworks.
 *
 * Synchronous on every platform: it returns only once the scan has finished.
 * An empty result is a success, not a failure -- a scan is not repeatable, and
 * consecutive calls legitimately report different networks.
 *
 * Returns true when the scan completed, false when it could not be performed.
 */
bool WiFiScan(WiFiNetwork networks[], uint16_t maxNetworks, uint16_t *foundNetworks);

/* Overridable so a consumer whose own network overlaps this range can move it. */
#ifndef WIFI_ACCESS_POINT_ADDRESS
#define WIFI_ACCESS_POINT_ADDRESS "192.168.33.1"
#endif

/*
 * Starts an access point advertising ssid at WIFI_ACCESS_POINT_ADDRESS. A null or
 * empty password means an open access point.
 *
 * Requires a successful WiFiInitialize first, the same way WiFiScan does.
 */
bool WiFiAccessPointStart(const char *ssid, const char *password);
bool WiFiAccessPointStop(void);
bool WiFiAccessPointIsRunning(void);

/*
 * Joins ssid as a station, returning only once the connection is established or
 * has failed. A null or empty password means an open network. Fails immediately
 * while the access point is running.
 */
bool WiFiStationConnect(const char *ssid, const char *password);

#endif /* __WIFI_H_ */
