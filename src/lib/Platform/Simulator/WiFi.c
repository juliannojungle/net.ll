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

/* Simulator WiFi: there is no radio here, so the scan borrows the host's.
 * Under WSL the Windows `netsh.exe` is reachable and reports the real networks
 * in range; on any other host the scan reports that it cannot be performed. */

#include "WiFi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* netsh writes its labels in the Windows UI language. Supporting EN and pt-BR */
#define LABEL_AUTHENTICATION_PT "Autenticação"
#define LABEL_AUTHENTICATION_EN "Authentication"
#define LABEL_SIGNAL_PT "Sinal"
#define LABEL_SIGNAL_EN "Signal"
#define LABEL_CHANNEL_PT "Canal"
#define LABEL_CHANNEL_EN "Channel"
#define FALLBACK_RSSI (-45)
#define FALLBACK_CHANNEL 36
#define FALLBACK_AUTH_MODE WIFI_AUTH_MODE_WPA2
#define NETSH_COMMAND "timeout 20 netsh.exe wlan show networks mode=bssid 2>/dev/null"
#define LINE_BUFFER_SIZE 512

static bool wifiInitialized = false;

static bool RunningUnderWsl(void) {
    if (getenv("WSL_INTEROP") != NULL || getenv("WSL_DISTRO_NAME") != NULL) return true;

    FILE *osRelease = fopen("/proc/sys/kernel/osrelease", "r");

    if (osRelease == NULL) return false;

    char release[LINE_BUFFER_SIZE];
    bool isWsl = false;

    if (fgets(release, sizeof(release), osRelease) != NULL)
        isWsl = (strstr(release, "microsoft") != NULL) || (strstr(release, "WSL") != NULL);

    fclose(osRelease);
    return isWsl;
}

static const char *SkipBlanks(const char *text) {
    while (*text == ' ' || *text == '\t') text++;

    return text;
}

static char *ValueAfterColon(char *line) {
    char *value = strchr(line, ':');

    if (value == NULL) return NULL;

    value++;

    while (*value == ' ' || *value == '\t') value++;

    size_t length = strlen(value);

    while (length > 0 && (value[length - 1] == '\n' || value[length - 1] == '\r' || value[length - 1] == ' ' ||
                             value[length - 1] == '\t')) {
        value[--length] = '\0';
    }

    return value;
}

/* True when the line is exactly "<label>   :" */
static bool IsLabelledLine(const char *line, const char *labelPt, const char *labelEn) {
    const char *cursor = SkipBlanks(line);
    const char *label = NULL;

    if (strncmp(cursor, labelPt, strlen(labelPt)) == 0) label = labelPt;
    else if (strncmp(cursor, labelEn, strlen(labelEn)) == 0) label = labelEn;
    else return false;

    cursor = SkipBlanks(cursor + strlen(label));
    return (*cursor == ':');
}

static bool IsPrefixedIndexLine(const char *line, const char *keyword) {
    const char *cursor = SkipBlanks(line);

    size_t keywordLength = strlen(keyword);

    if (strncmp(cursor, keyword, keywordLength) != 0) return false;

    cursor += keywordLength;

    if (*cursor != ' ') return false;

    cursor = SkipBlanks(cursor);
    /* An index must follow, otherwise "SSID" alone would match. */
    return (*cursor >= '0' && *cursor <= '9');
}

static void ParseBssid(const char *text, uint8_t bssid[WIFI_BSSID_LENGTH]) {
    unsigned int octets[WIFI_BSSID_LENGTH] = {0};

    if (sscanf(text, "%x:%x:%x:%x:%x:%x", &octets[0], &octets[1], &octets[2], &octets[3], &octets[4], &octets[5]) !=
        WIFI_BSSID_LENGTH) {
        return;
    }

    for (int i = 0; i < WIFI_BSSID_LENGTH; i++) bssid[i] = (uint8_t)octets[i];
}

static WiFiAuthMode AuthModeFromText(const char *text) {
    if (strstr(text, "WPA3") != NULL) return WIFI_AUTH_MODE_WPA3;
    if (strstr(text, "WPA2") != NULL) return WIFI_AUTH_MODE_WPA2;
    if (strstr(text, "WPA") != NULL) return WIFI_AUTH_MODE_WPA;
    if (strstr(text, "WEP") != NULL) return WIFI_AUTH_MODE_WEP;

    return WIFI_AUTH_MODE_OPEN;
}

static int16_t RssiFromSignalPercentage(const char *text) {
    int percentage = atoi(text);

    if (percentage < 0) percentage = 0;
    else if (percentage > 100) percentage = 100;

    return (int16_t)((percentage / 2) - 100);
}

bool WiFiInitialize(void) {
    if (!RunningUnderWsl()) {
        printf("WiFi: the Simulator can only scan through the Windows host under WSL\n");
        return false;
    }

    wifiInitialized = true;
    return true;
}

void WiFiDeinitialize(void) { wifiInitialized = false; }

bool WiFiScan(WiFiNetwork networks[], uint16_t maxNetworks, uint16_t *foundNetworks) {
    if (networks == NULL || foundNetworks == NULL || maxNetworks == 0) return false;

    *foundNetworks = 0;

    if (!wifiInitialized) {
        printf("WiFi: WiFiInitialize must succeed before scanning\n");
        return false;
    }

    FILE *netsh = popen(NETSH_COMMAND, "r");
    if (netsh == NULL) {
        printf("WiFi: could not run netsh.exe\n");
        return false;
    }

    char currentSsid[WIFI_SSID_MAX_LENGTH + 1] = {0};
    WiFiAuthMode currentAuthMode = FALLBACK_AUTH_MODE;
    bool haveSsid = false;
    WiFiNetwork *currentNetwork = NULL;
    char line[LINE_BUFFER_SIZE];

    while (fgets(line, sizeof(line), netsh) != NULL) {
        if (IsPrefixedIndexLine(line, "BSSID")) {
            currentNetwork = NULL;

            if (!haveSsid || *foundNetworks >= maxNetworks) continue;

            const char *value = ValueAfterColon(line);

            if (value == NULL) continue;

            currentNetwork = &networks[*foundNetworks];
            memset(currentNetwork, 0, sizeof(*currentNetwork));
            snprintf(currentNetwork->Ssid, sizeof(currentNetwork->Ssid), "%s", currentSsid);
            ParseBssid(value, currentNetwork->Bssid);
            currentNetwork->AuthMode = currentAuthMode;
            currentNetwork->Channel = FALLBACK_CHANNEL;
            currentNetwork->Rssi = FALLBACK_RSSI;
            (*foundNetworks)++;
        } else if (IsPrefixedIndexLine(line, "SSID")) {
            const char *value = ValueAfterColon(line);
            /* A hidden network reports an empty name, which is valid. */
            snprintf(currentSsid, sizeof(currentSsid), "%s", value != NULL ? value : "");
            currentAuthMode = FALLBACK_AUTH_MODE;
            haveSsid = true;
            currentNetwork = NULL;
        } else if (IsLabelledLine(line, LABEL_AUTHENTICATION_PT, LABEL_AUTHENTICATION_EN)) {
            const char *value = ValueAfterColon(line);

            if (value != NULL) currentAuthMode = AuthModeFromText(value);
        } else if (currentNetwork != NULL && IsLabelledLine(line, LABEL_SIGNAL_PT, LABEL_SIGNAL_EN)) {
            const char *value = ValueAfterColon(line);

            if (value != NULL) currentNetwork->Rssi = RssiFromSignalPercentage(value);
        } else if (currentNetwork != NULL && IsLabelledLine(line, LABEL_CHANNEL_PT, LABEL_CHANNEL_EN)) {
            const char *value = ValueAfterColon(line);

            if (value != NULL) currentNetwork->Channel = (uint16_t)atoi(value);
        }
    }

    int status = pclose(netsh);

    if (status != 0) {
        printf("WiFi: netsh.exe failed (status %d), is WSL interop working?\n", status);
        return false;
    }

    /* An empty result is a success: a scan is not repeatable. */
    return true;
}

/* There is no radio here, so the access point is a successful no-op. */
static bool accessPointRunning = false;

bool WiFiAccessPointStart(const char *ssid, const char *password) {
    (void)password;
    printf("WiFi: no radio on the Simulator, the access point \"%s\" is a no-op\n", ssid != NULL ? ssid : "");
    accessPointRunning = true;
    return true;
}

bool WiFiAccessPointStop(void) {
    accessPointRunning = false;
    return true;
}

bool WiFiAccessPointIsRunning(void) { return accessPointRunning; }

bool WiFiStationConnect(const char *ssid, const char *password) {
    (void)ssid;
    (void)password;
    printf("WiFi: connecting as a station is not implemented on the Simulator\n");
    return false;
}
