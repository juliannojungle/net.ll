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

/* RP2040 download over lwIP's own HTTP client (httpc_get_file_dns), driven in poll mode
 * so the call stays synchronous and net.ll owns no thread. HTTP only: HTTPS is out of
 * scope, so plain http:// URLs are expected here. The body streams to storage through
 * fs.ll, never buffered whole -- the chip has little RAM. */

#include "HttpClient.h"
#include "FileSystem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico/cyw43_arch.h"
#include "pico/time.h"
#include "lwip/apps/http_client.h"
#include "lwip/pbuf.h"
#include "lwip/altcp.h"

#define DOWNLOAD_TIMEOUT_MS 30000
#define DOWNLOAD_POLL_MS 10
#define HOST_MAX_LENGTH 256

#ifndef NET_LL_USER_AGENT
#define NET_LL_USER_AGENT "net.ll/0.alpha (RP2040)"
#endif

typedef struct {
    FIL file;
    bool fileOpened;
    bool done;
    bool success;
} DownloadState;

static bool ParseHttpUrl(const char *url, char *host, size_t hostLength, uint16_t *port, const char **uri) {
    const char *cursor = url;

    if (strncmp(cursor, "http://", 7) == 0) cursor += 7;

    const char *pathStart = strchr(cursor, '/');
    const char *portStart = strchr(cursor, ':');
    const char *hostEnd = pathStart;

    if (portStart != NULL && (pathStart == NULL || portStart < pathStart)) {
        *port = (uint16_t)atoi(portStart + 1);
        hostEnd = portStart;
    } else {
        *port = 80;
    }

    size_t length = hostEnd != NULL ? (size_t)(hostEnd - cursor) : strlen(cursor);

    if (length == 0 || length >= hostLength) return false;

    memcpy(host, cursor, length);
    host[length] = '\0';
    *uri = pathStart != NULL ? pathStart : "/";
    return true;
}

static err_t BodyReceived(void *argument, struct altcp_pcb *connection, struct pbuf *buffer, err_t error) {
    DownloadState *state = (DownloadState *)argument;

    if (buffer == NULL || error != ERR_OK) return ERR_OK;

    for (struct pbuf *segment = buffer; segment != NULL; segment = segment->next)
        WriteFile(&state->file, segment->payload, segment->len);

    altcp_recved(connection, buffer->tot_len);
    pbuf_free(buffer);
    return ERR_OK;
}

static void TransferFinished(void *argument, httpc_result_t result, u32_t receivedLength, u32_t serverStatus,
                             err_t error) {
    DownloadState *state = (DownloadState *)argument;
    (void)receivedLength;

    state->success = (result == HTTPC_RESULT_OK && error == ERR_OK && serverStatus == 200);
    state->done = true;
}

static bool WaitForTransfer(DownloadState *state) {
    absolute_time_t deadline = make_timeout_time_ms(DOWNLOAD_TIMEOUT_MS);

    while (!state->done) {
        if (absolute_time_diff_us(get_absolute_time(), deadline) <= 0) {
            printf("HttpClient: download timed out\n");
            return false;
        }

        cyw43_arch_poll();
        sleep_ms(DOWNLOAD_POLL_MS);
    }

    return state->success;
}

bool HttpDownloadFile(const char *url, const char *filePath) {
    if (url == NULL || filePath == NULL) return false;

    char host[HOST_MAX_LENGTH];
    uint16_t port = 80;
    const char *uri = NULL;

    if (!ParseHttpUrl(url, host, sizeof(host), &port, &uri)) {
        printf("HttpClient: could not parse URL %s\n", url);
        return false;
    }

    CreatePathDirectories(filePath);

    DownloadState state = {.fileOpened = false, .done = false, .success = false};

    if (!OpenFile(&state.file, filePath)) {
        printf("HttpClient: could not open file %s\n", filePath);
        return false;
    }

    state.fileOpened = true;

    httpc_connection_t settings = {0};
    settings.result_fn = TransferFinished;
    httpc_state_t *connection = NULL;

    err_t error = httpc_get_file_dns(host, port, uri, &settings, BodyReceived, &state, &connection);

    bool success = (error == ERR_OK) && WaitForTransfer(&state);

    if (!success && error != ERR_OK) printf("HttpClient: httpc_get_file_dns failed to start (%d)\n", error);

    CloseFile(&state.file);
    return success;
}
