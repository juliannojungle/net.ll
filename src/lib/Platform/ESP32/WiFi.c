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

/* ESP32 WiFi through esp_wifi. Even a scan needs the whole driver brought up,
 * and the driver keeps calibration data in NVS -- hence nvs_flash here.
 *
 * This library starts no thread, and esp_wifi_scan_start is asked to block, so
 * the API is synchronous. ESP-IDF's own driver does run internal tasks; that is
 * how esp_wifi is built and cannot be switched off. */

#include "WiFi.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"

static bool wifiInitialized = false;

static WiFiAuthMode AuthModeFromEsp(wifi_auth_mode_t authMode) {
    switch (authMode) {
        case WIFI_AUTH_OPEN:
            return WIFI_AUTH_MODE_OPEN;
        case WIFI_AUTH_WEP:
            return WIFI_AUTH_MODE_WEP;
        case WIFI_AUTH_WPA_PSK:
            return WIFI_AUTH_MODE_WPA;
        case WIFI_AUTH_WPA2_PSK:
        case WIFI_AUTH_WPA_WPA2_PSK:
            return WIFI_AUTH_MODE_WPA2;
        case WIFI_AUTH_WPA3_PSK:
        case WIFI_AUTH_WPA2_WPA3_PSK:
            return WIFI_AUTH_MODE_WPA3;
        default:
            return WIFI_AUTH_MODE_UNKNOWN;
    }
}

bool WiFiInitialize(void) {
    if (wifiInitialized) {
        return true;
    }

    /* The WiFi driver stores calibration data here, so NVS comes first. A fresh
     * or resized partition needs erasing before it can be used. */
    esp_err_t result = nvs_flash_init();
    if (result == ESP_ERR_NVS_NO_FREE_PAGES || result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        if (nvs_flash_erase() != ESP_OK || nvs_flash_init() != ESP_OK) {
            printf("WiFi: could not initialize NVS\n");
            return false;
        }
    } else if (result != ESP_OK) {
        printf("WiFi: could not initialize NVS\n");
        return false;
    }

    if (esp_netif_init() != ESP_OK) {
        printf("WiFi: esp_netif_init failed\n");
        return false;
    }

    /* Already created is not an error: a consumer may have its own event loop. */
    result = esp_event_loop_create_default();
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
        printf("WiFi: esp_event_loop_create_default failed\n");
        return false;
    }

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t initConfig = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&initConfig) != ESP_OK) {
        printf("WiFi: esp_wifi_init failed\n");
        return false;
    }

    if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK || esp_wifi_start() != ESP_OK) {
        printf("WiFi: could not start the radio in station mode\n");
        esp_wifi_deinit();
        return false;
    }

    wifiInitialized = true;
    return true;
}

void WiFiDeinitialize(void) {
    if (!wifiInitialized) {
        return;
    }

    esp_wifi_stop();
    esp_wifi_deinit();
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

    /* block = true is what makes this synchronous. */
    if (esp_wifi_scan_start(NULL, true) != ESP_OK) {
        printf("WiFi: esp_wifi_scan_start failed\n");
        return false;
    }

    uint16_t availableRecords = 0;
    if (esp_wifi_scan_get_ap_num(&availableRecords) != ESP_OK) {
        printf("WiFi: esp_wifi_scan_get_ap_num failed\n");
        return false;
    }

    if (availableRecords == 0) {
        /* An empty result is a success: a scan is not repeatable. */
        return true;
    }

    uint16_t recordsToRead = (availableRecords < maxNetworks) ? availableRecords : maxNetworks;
    wifi_ap_record_t *records = calloc(recordsToRead, sizeof(wifi_ap_record_t));
    if (records == NULL) {
        printf("WiFi: out of memory reading %u scan records\n", recordsToRead);
        esp_wifi_clear_ap_list();
        return false;
    }

    bool scanned = false;
    if (esp_wifi_scan_get_ap_records(&recordsToRead, records) == ESP_OK) {
        for (uint16_t i = 0; i < recordsToRead; i++) {
            WiFiNetwork *network = &networks[i];
            memset(network, 0, sizeof(*network));
            snprintf(network->Ssid, sizeof(network->Ssid), "%s", (const char *)records[i].ssid);
            memcpy(network->Bssid, records[i].bssid, WIFI_BSSID_LENGTH);
            network->Channel = records[i].primary;
            network->AuthMode = AuthModeFromEsp(records[i].authmode);
            network->Rssi = records[i].rssi;
        }

        *foundNetworks = recordsToRead;
        scanned = true;
    } else {
        printf("WiFi: esp_wifi_scan_get_ap_records failed\n");
    }

    free(records);
    return scanned;
}
