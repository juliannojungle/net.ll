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
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#define WIFI_ACCESS_POINT_CHANNEL 1
#define WIFI_ACCESS_POINT_MAX_CONNECTIONS 4
#define WIFI_ACCESS_POINT_NETMASK "255.255.255.0"
#define WIFI_STATION_CONNECT_TIMEOUT_MILLISECONDS 15000
#define WIFI_STATION_CONNECT_POLL_MILLISECONDS 100

static bool wifiInitialized = false;
static bool accessPointRunning = false;
static esp_netif_t *stationNetif = NULL;
static esp_netif_t *accessPointNetif = NULL;

static WiFiAuthMode AuthModeFromEsp(wifi_auth_mode_t authMode) {
    switch (authMode) {
    case WIFI_AUTH_OPEN: return WIFI_AUTH_MODE_OPEN;
    case WIFI_AUTH_WEP: return WIFI_AUTH_MODE_WEP;
    case WIFI_AUTH_WPA_PSK: return WIFI_AUTH_MODE_WPA;
    case WIFI_AUTH_WPA2_PSK:
    case WIFI_AUTH_WPA_WPA2_PSK: return WIFI_AUTH_MODE_WPA2;
    case WIFI_AUTH_WPA3_PSK:
    case WIFI_AUTH_WPA2_WPA3_PSK: return WIFI_AUTH_MODE_WPA3;
    default: return WIFI_AUTH_MODE_UNKNOWN;
    }
}

static bool InitializeNvs(void) {
    /* The WiFi driver stores calibration data here, so NVS comes first. A fresh
     * or resized partition needs erasing before it can be used. */
    esp_err_t result = nvs_flash_init();

    if (result == ESP_ERR_NVS_NO_FREE_PAGES || result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        return nvs_flash_erase() == ESP_OK && nvs_flash_init() == ESP_OK;
    }

    return result == ESP_OK;
}

static bool InitializeNetworkStack(void) {
    if (esp_netif_init() != ESP_OK) {
        printf("WiFi: esp_netif_init failed\n");
        return false;
    }

    /* Already created is not an error: a consumer may have its own event loop. */
    esp_err_t result = esp_event_loop_create_default();

    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
        printf("WiFi: esp_event_loop_create_default failed\n");
        return false;
    }

    if (stationNetif == NULL) {
        stationNetif = esp_netif_create_default_wifi_sta();
    }

    return true;
}

static bool StartRadioInStationMode(void) {
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

    return true;
}

bool WiFiInitialize(void) {
    if (wifiInitialized) {
        return true;
    }

    if (!InitializeNvs()) {
        printf("WiFi: could not initialize NVS\n");
        return false;
    }

    if (!InitializeNetworkStack() || !StartRadioInStationMode()) {
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
    accessPointRunning = false;
    wifiInitialized = false;
}

static void CopyApRecord(WiFiNetwork *network, const wifi_ap_record_t *record) {
    memset(network, 0, sizeof(*network));
    snprintf(network->Ssid, sizeof(network->Ssid), "%s", (const char *)record->ssid);
    memcpy(network->Bssid, record->bssid, WIFI_BSSID_LENGTH);
    network->Channel = record->primary;
    network->AuthMode = AuthModeFromEsp(record->authmode);
    network->Rssi = record->rssi;
}

static bool CollectScanRecords(
    WiFiNetwork networks[], uint16_t maxNetworks, uint16_t *foundNetworks, uint16_t availableRecords) {
    uint16_t recordsToRead = (availableRecords < maxNetworks) ? availableRecords : maxNetworks;
    wifi_ap_record_t *records = calloc(recordsToRead, sizeof(wifi_ap_record_t));

    if (records == NULL) {
        printf("WiFi: out of memory reading %u scan records\n", recordsToRead);
        esp_wifi_clear_ap_list();
        return false;
    }

    bool scanned = esp_wifi_scan_get_ap_records(&recordsToRead, records) == ESP_OK;

    if (scanned) {
        for (uint16_t i = 0; i < recordsToRead; i++) {
            CopyApRecord(&networks[i], &records[i]);
        }

        *foundNetworks = recordsToRead;
    } else {
        printf("WiFi: esp_wifi_scan_get_ap_records failed\n");
    }

    free(records);
    return scanned;
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

    /* An empty result is a success: a scan is not repeatable. */
    if (availableRecords == 0) {
        return true;
    }

    return CollectScanRecords(networks, maxNetworks, foundNetworks, availableRecords);
}

static bool ConfigureAccessPointAddress(void) {
    esp_netif_ip_info_t addressInfo;
    memset(&addressInfo, 0, sizeof(addressInfo));
    addressInfo.ip.addr = esp_ip4addr_aton(WIFI_ACCESS_POINT_ADDRESS);
    addressInfo.gw.addr = addressInfo.ip.addr;
    addressInfo.netmask.addr = esp_ip4addr_aton(WIFI_ACCESS_POINT_NETMASK);

    /* The DHCP server latches the interface address when it starts, so it has to
     * be down while the address changes -- otherwise the gateway handed to the
     * phone stays ESP-IDF's 192.168.4.1 default. */
    esp_netif_dhcps_stop(accessPointNetif);

    if (esp_netif_set_ip_info(accessPointNetif, &addressInfo) != ESP_OK) {
        printf("WiFi: could not set the access point address to %s\n", WIFI_ACCESS_POINT_ADDRESS);
        esp_netif_dhcps_start(accessPointNetif);
        return false;
    }

    if (esp_netif_dhcps_start(accessPointNetif) != ESP_OK) {
        printf("WiFi: could not start the DHCP server\n");
        return false;
    }

    return true;
}

static bool EnsureAccessPointNetif(void) {
    if (accessPointNetif != NULL) {
        return true;
    }

    accessPointNetif = esp_netif_create_default_wifi_ap();

    if (accessPointNetif == NULL) {
        printf("WiFi: esp_netif_create_default_wifi_ap failed\n");
        return false;
    }

    return true;
}

static void BuildAccessPointConfig(wifi_config_t *config, const char *ssid, const char *password) {
    bool open = (password == NULL || password[0] == '\0');

    memset(config, 0, sizeof(*config));
    snprintf((char *)config->ap.ssid, sizeof(config->ap.ssid), "%s", ssid);
    config->ap.ssid_len = (uint8_t)strlen((const char *)config->ap.ssid);
    config->ap.channel = WIFI_ACCESS_POINT_CHANNEL;
    config->ap.max_connection = WIFI_ACCESS_POINT_MAX_CONNECTIONS;
    config->ap.authmode = open ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

    if (!open) {
        snprintf((char *)config->ap.password, sizeof(config->ap.password), "%s", password);
    }
}

static bool ApplyAccessPointConfig(const wifi_config_t *config) {
    if (esp_wifi_set_mode(WIFI_MODE_AP) != ESP_OK) {
        printf("WiFi: could not switch the radio to access point mode\n");
        return false;
    }

    if (esp_wifi_set_config(WIFI_IF_AP, config) != ESP_OK) {
        printf("WiFi: esp_wifi_set_config failed for the access point\n");
        return false;
    }

    esp_err_t result = esp_wifi_start(); // Already started is not an error.

    if (result != ESP_OK && result != ESP_ERR_WIFI_CONN) {
        printf("WiFi: could not start the access point\n");
        return false;
    }

    return ConfigureAccessPointAddress();
}

bool WiFiAccessPointStart(const char *ssid, const char *password) {
    if (ssid == NULL || ssid[0] == '\0') {
        return false;
    }

    if (!wifiInitialized) {
        printf("WiFi: WiFiInitialize must succeed before starting an access point\n");
        return false;
    }

    if (accessPointRunning) {
        return true;
    }

    if (!EnsureAccessPointNetif()) {
        return false;
    }

    wifi_config_t config;
    BuildAccessPointConfig(&config, ssid, password);

    if (!ApplyAccessPointConfig(&config)) {
        esp_wifi_set_mode(WIFI_MODE_STA);
        return false;
    }

    accessPointRunning = true;
    return true;
}

bool WiFiAccessPointStop(void) {
    if (!accessPointRunning) {
        return true;
    }

    if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK) {
        printf("WiFi: could not switch the radio back to station mode\n");
        return false;
    }

    accessPointRunning = false;
    return true;
}

bool WiFiAccessPointIsRunning(void) { return accessPointRunning; }

static bool StartStationConnection(const char *ssid, const char *password) {
    wifi_config_t config;
    memset(&config, 0, sizeof(config));
    snprintf((char *)config.sta.ssid, sizeof(config.sta.ssid), "%s", ssid);

    if (password != NULL && password[0] != '\0') {
        snprintf((char *)config.sta.password, sizeof(config.sta.password), "%s", password);
    }

    if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK) {
        printf("WiFi: could not switch the radio to station mode\n");
        return false;
    }

    if (esp_wifi_set_config(WIFI_IF_STA, &config) != ESP_OK) {
        printf("WiFi: esp_wifi_set_config failed for the station\n");
        return false;
    }

    if (esp_wifi_connect() != ESP_OK) {
        printf("WiFi: esp_wifi_connect failed\n");
        return false;
    }

    return true;
}

static bool StationHasAddress(void) {
    wifi_ap_record_t connectedTo;
    esp_netif_ip_info_t addressInfo;

    return esp_wifi_sta_get_ap_info(&connectedTo) == ESP_OK &&
           esp_netif_get_ip_info(stationNetif, &addressInfo) == ESP_OK && addressInfo.ip.addr != 0;
}

/* Polled rather than event-driven so the call returns only once the outcome is
 * known, without this library owning a thread to wait on. An address is what
 * makes the link usable, so association alone is not enough. */
static bool WaitForStationAddress(void) {
    for (uint32_t waited = 0; waited < WIFI_STATION_CONNECT_TIMEOUT_MILLISECONDS;
        waited += WIFI_STATION_CONNECT_POLL_MILLISECONDS) {
        if (StationHasAddress()) {
            return true;
        }

        vTaskDelay(pdMS_TO_TICKS(WIFI_STATION_CONNECT_POLL_MILLISECONDS));
    }

    return false;
}

bool WiFiStationConnect(const char *ssid, const char *password) {
    if (ssid == NULL || ssid[0] == '\0') {
        return false;
    }

    if (!wifiInitialized) {
        printf("WiFi: WiFiInitialize must succeed before connecting\n");
        return false;
    }

    if (accessPointRunning) {
        printf("WiFi: cannot connect while the access point is running\n");
        return false;
    }

    if (!StartStationConnection(ssid, password)) {
        return false;
    }

    if (WaitForStationAddress()) {
        return true;
    }

    printf("WiFi: could not connect to %s\n", ssid);
    esp_wifi_disconnect();
    return false;
}
