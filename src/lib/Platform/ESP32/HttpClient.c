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

/* ESP32 download over esp_http_client, whose esp_http_client_perform blocks until the
 * transfer finishes -- synchronous, and net.ll starts no thread of its own. HTTP only:
 * HTTPS is out of scope, so plain http:// URLs are expected. The body streams to storage
 * through fs.ll on each ON_DATA event, never buffered whole -- the chip has little RAM. */

#include "HttpClient.h"
#include "FileSystem.h"

#include <stdio.h>

#include "esp_http_client.h"

#ifndef NET_LL_USER_AGENT
#define NET_LL_USER_AGENT "net.ll/0.alpha (ESP32)"
#endif

typedef struct {
    FIL file;
    bool fileOpened;
} DownloadState;

static esp_err_t OnHttpEvent(esp_http_client_event_t *event) {
    DownloadState *state = (DownloadState *)event->user_data;

    if (event->event_id == HTTP_EVENT_ON_DATA && event->data_len > 0)
        WriteFile(&state->file, event->data, (unsigned int)event->data_len);

    return ESP_OK;
}

bool HttpDownloadFile(const char *url, const char *filePath) {
    if (url == NULL || filePath == NULL) return false;

    CreatePathDirectories(filePath);

    DownloadState state = {.fileOpened = false};

    if (!OpenFile(&state.file, filePath)) {
        printf("HttpClient: could not open file %s\n", filePath);
        return false;
    }

    state.fileOpened = true;

    esp_http_client_config_t config = {
        .url = url,
        .user_agent = NET_LL_USER_AGENT,
        .event_handler = OnHttpEvent,
        .user_data = &state,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);

    if (client == NULL) {
        printf("HttpClient: esp_http_client_init failed\n");
        CloseFile(&state.file);
        return false;
    }

    esp_err_t error = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    bool success = (error == ESP_OK) && (status == 200);

    if (!success) printf("HttpClient: download failed (err %d, status %d)\n", error, status);

    esp_http_client_cleanup(client);
    CloseFile(&state.file);
    return success;
}
