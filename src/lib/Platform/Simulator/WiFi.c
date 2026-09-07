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
 * in range; on any other host the scan reports that it cannot be performed.
 *
 * netsh labels its output in the Windows UI language, so every label below is
 * matched in both Portuguese and English -- see LABEL_* and AGENTS.md. */

#include "WiFi.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* netsh writes its labels in the Windows UI language, so each one is matched in
 * both. The non-English spellings are input data from an external program, not
 * project prose, and the dev approved the exception explicitly.
 *
 * The Portuguese spellings are measured on the dev's host. The English ones are
 * NOT verified here -- this machine's Windows is pt-BR -- so treat them as the
 * expected wording rather than a confirmed fact. */
#define LABEL_AUTHENTICATION_PT "Autenticação"
#define LABEL_AUTHENTICATION_EN "Authentication"
#define LABEL_SIGNAL_PT "Sinal"
#define LABEL_SIGNAL_EN "Signal"
#define LABEL_CHANNEL_PT "Canal"
#define LABEL_CHANNEL_EN "Channel"

/* Used only when a label went unrecognised, which means an unexpected Windows UI
 * language. Never left at zero: a zeroed auth mode would read as "open network,
 * no password needed" and 0 dBm would read as an impossibly strong signal, so a
 * consumer would behave differently here than on hardware without noticing. */
#define FALLBACK_RSSI (-45)
#define FALLBACK_CHANNEL 36
#define FALLBACK_AUTH_MODE WIFI_AUTH_MODE_WPA2

/* `2>/dev/null` drops the interop noise Windows writes when the working
 * directory is a UNC path. `timeout` keeps a wedged interop from hanging the
 * caller -- a real state, see Toolchain/wsl.sh in gui.ll. */
#define NETSH_COMMAND "timeout 20 netsh.exe wlan show networks mode=bssid 2>/dev/null"

#define LINE_BUFFER_SIZE 512

static bool wifiInitialized = false;

static bool RunningUnderWsl(void) {
    if (getenv("WSL_INTEROP") != NULL || getenv("WSL_DISTRO_NAME") != NULL) {
        return true;
    }

    FILE *osRelease = fopen("/proc/sys/kernel/osrelease", "r");
    if (osRelease == NULL) {
        return false;
    }

    char release[LINE_BUFFER_SIZE];
    bool isWsl = false;
    if (fgets(release, sizeof(release), osRelease) != NULL) {
        isWsl = (strstr(release, "microsoft") != NULL) || (strstr(release, "WSL") != NULL);
    }

    fclose(osRelease);
    return isWsl;
}

static const char *SkipBlanks(const char *text) {
    while (*text == ' ' || *text == '\t') {
        text++;
    }
    return text;
}

/* Returns the text after the first ':', with surrounding blanks removed. */
static char *ValueAfterColon(char *line) {
    char *value = strchr(line, ':');
    if (value == NULL) {
        return NULL;
    }

    value++;
    while (*value == ' ' || *value == '\t') {
        value++;
    }

    size_t length = strlen(value);
    while (length > 0 && (value[length - 1] == '\n' || value[length - 1] == '\r' ||
                          value[length - 1] == ' ' || value[length - 1] == '\t')) {
        value[--length] = '\0';
    }

    return value;
}

/* True when the line is exactly "<label>   :", in either language.
 *
 * The label has to be anchored at the start AND followed by nothing but blanks
 * before the colon. A plain prefix test is not enough: the Bss Load subsection
 * carries "Channel utilization :", which starts with the English channel label
 * and would otherwise overwrite the channel with a utilisation percentage. */
static bool IsLabelledLine(const char *line, const char *labelPt, const char *labelEn) {
    const char *cursor = SkipBlanks(line);
    const char *label = NULL;

    if (strncmp(cursor, labelPt, strlen(labelPt)) == 0) {
        label = labelPt;
    } else if (strncmp(cursor, labelEn, strlen(labelEn)) == 0) {
        label = labelEn;
    } else {
        return false;
    }

    cursor = SkipBlanks(cursor + strlen(label));
    return (*cursor == ':');
}

/* Only the "SSID n" and "BSSID n" prefixes are parsed for identity: those two
 * acronyms survive translation, so they anchor the structure regardless of the
 * Windows language. */
static bool IsPrefixedIndexLine(const char *line, const char *keyword) {
    const char *cursor = SkipBlanks(line);

    size_t keywordLength = strlen(keyword);
    if (strncmp(cursor, keyword, keywordLength) != 0) {
        return false;
    }

    cursor += keywordLength;
    if (*cursor != ' ') {
        return false;
    }

    cursor = SkipBlanks(cursor);

    /* An index must follow, otherwise "SSID" alone would match. */
    return (*cursor >= '0' && *cursor <= '9');
}

static void ParseBssid(const char *text, uint8_t bssid[WIFI_BSSID_LENGTH]) {
    unsigned int octets[WIFI_BSSID_LENGTH] = {0};
    if (sscanf(text, "%x:%x:%x:%x:%x:%x",
               &octets[0], &octets[1], &octets[2],
               &octets[3], &octets[4], &octets[5]) != WIFI_BSSID_LENGTH) {
        return;
    }

    for (int i = 0; i < WIFI_BSSID_LENGTH; i++) {
        bssid[i] = (uint8_t)octets[i];
    }
}

/* The protocol acronyms inside the value are not translated even though the
 * label is, so they are what gets matched. Checked strongest first, since
 * "WPA2-Personal" contains "WPA" too. A recognised label with no acronym in it
 * means an open network -- Windows spells that one in its own language. */
static WiFiAuthMode AuthModeFromText(const char *text) {
    if (strstr(text, "WPA3") != NULL) {
        return WIFI_AUTH_MODE_WPA3;
    }
    if (strstr(text, "WPA2") != NULL) {
        return WIFI_AUTH_MODE_WPA2;
    }
    if (strstr(text, "WPA") != NULL) {
        return WIFI_AUTH_MODE_WPA;
    }
    if (strstr(text, "WEP") != NULL) {
        return WIFI_AUTH_MODE_WEP;
    }
    return WIFI_AUTH_MODE_OPEN;
}

/* netsh reports signal as a quality percentage, not dBm. Windows derives that
 * percentage from RSSI over a documented linear scale where 0% is -100 dBm and
 * 100% is -50 dBm, so inverting it recovers an approximation of the original
 * figure -- close, but reconstructed rather than measured. */
static int16_t RssiFromSignalPercentage(const char *text) {
    int percentage = atoi(text);

    if (percentage < 0) {
        percentage = 0;
    } else if (percentage > 100) {
        percentage = 100;
    }

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

void WiFiDeinitialize(void) {
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

    FILE *netsh = popen(NETSH_COMMAND, "r");
    if (netsh == NULL) {
        printf("WiFi: could not run netsh.exe\n");
        return false;
    }

    /* netsh nests the output: the name and authentication belong to an SSID
     * block, then each BSSID under it opens an entry whose signal and channel
     * follow on the lines after. So the SSID-level values are remembered and
     * copied into every entry below them, and the entry stays addressable while
     * its own lines are still arriving -- one entry per BSS, matching what a
     * hardware scan reports. */
    char currentSsid[WIFI_SSID_MAX_LENGTH + 1] = {0};
    WiFiAuthMode currentAuthMode = FALLBACK_AUTH_MODE;
    bool haveSsid = false;
    WiFiNetwork *currentNetwork = NULL;
    char line[LINE_BUFFER_SIZE];

    while (fgets(line, sizeof(line), netsh) != NULL) {
        if (IsPrefixedIndexLine(line, "BSSID")) {
            currentNetwork = NULL;

            if (!haveSsid || *foundNetworks >= maxNetworks) {
                continue;
            }

            const char *value = ValueAfterColon(line);
            if (value == NULL) {
                continue;
            }

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
            if (value != NULL) {
                currentAuthMode = AuthModeFromText(value);
            }
        } else if (currentNetwork != NULL &&
                   IsLabelledLine(line, LABEL_SIGNAL_PT, LABEL_SIGNAL_EN)) {
            const char *value = ValueAfterColon(line);
            if (value != NULL) {
                currentNetwork->Rssi = RssiFromSignalPercentage(value);
            }
        } else if (currentNetwork != NULL &&
                   IsLabelledLine(line, LABEL_CHANNEL_PT, LABEL_CHANNEL_EN)) {
            const char *value = ValueAfterColon(line);
            if (value != NULL) {
                currentNetwork->Channel = (uint16_t)atoi(value);
            }
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

/* There is no radio here, so the access point is a successful no-op: it only
 * records the state, which lets a consumer run the whole provisioning flow on
 * the desktop while the HTTP server listens on the host's loopback. */
static bool accessPointRunning = false;

bool WiFiAccessPointStart(const char *ssid, const char *password) {
    (void)password;
    printf("WiFi: no radio on the Simulator, the access point \"%s\" is a no-op\n",
           ssid != NULL ? ssid : "");
    accessPointRunning = true;
    return true;
}

bool WiFiAccessPointStop(void) {
    accessPointRunning = false;
    return true;
}

bool WiFiAccessPointIsRunning(void) {
    return accessPointRunning;
}

bool WiFiStationConnect(const char *ssid, const char *password) {
    (void)ssid;
    (void)password;
    printf("WiFi: connecting as a station is not implemented on the Simulator\n");
    return false;
}
