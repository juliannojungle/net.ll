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

#include "HttpServer.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "lwip/errno.h"
#include "lwip/sockets.h"

#define RESPONSE_HEADER_SIZE 256
#define DEFAULT_CONTENT_TYPE "text/html"

typedef struct {
    HttpMethod Method;
    const char *Path;
    HttpEndpointCallback Callback;
    void *Context;
} Route;

static int listenSocket = -1;
static Route routes[HTTP_SERVER_MAX_ROUTES];
static uint16_t routeCount = 0;
static Route defaultRoute;
static bool useDefaultRoute = false;

/* The request lives here for the duration of one poll, so the whole server's RAM
 * footprint is this buffer regardless of how many requests are served. The last
 * byte is reserved for the NUL that terminates the body in place. */
static char requestBuffer[HTTP_SERVER_MAX_REQUEST_SIZE];

static const char *ReasonPhrase(uint16_t statusCode) {
    switch (statusCode) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 413: return "Payload Too Large";
    case 500: return "Internal Server Error";
    default: return "OK";
    }
}

static bool WriteAll(int socketHandle, const char *data, size_t length) {
    size_t written = 0;

    while (written < length) {
        int result = send(socketHandle, data + written, length - written, 0);

        if (result <= 0) {
            if (result < 0 && errno == EINTR) {
                continue;
            }

            return false;
        }

        written += (size_t)result;
    }

    return true;
}

static bool WriteResponse(int socketHandle, const HttpResponse *response) {
    char header[RESPONSE_HEADER_SIZE];
    const char *customHeader = (response->CustomHeader == NULL || response->CustomHeader[0] == '\0')
        ? "" : response->CustomHeader;
    int headerLength = snprintf(header, sizeof(header),
        "HTTP/1.1 %u %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %u\r\n"
        "Connection: close\r\n"
        "%s"
        "\r\n",
        (unsigned)response->StatusCode, ReasonPhrase(response->StatusCode),
        response->ContentType == NULL ? DEFAULT_CONTENT_TYPE : response->ContentType, (unsigned)response->BodyLength,
        customHeader);

    if (headerLength <= 0 || (size_t)headerLength >= sizeof(header)) {
        return false;
    }

    if (!WriteAll(socketHandle, header, (size_t)headerLength)) {
        return false;
    }

    if (response->Body == NULL || response->BodyLength == 0) {
        return true;
    }

    return WriteAll(socketHandle, response->Body, response->BodyLength);
}

static bool WriteStatusOnly(int socketHandle, uint16_t statusCode) {
    HttpResponse response;
    response.StatusCode = statusCode;
    response.ContentType = "text/plain";
    response.Body = ReasonPhrase(statusCode);
    response.BodyLength = (uint32_t)strlen(response.Body);
    return WriteResponse(socketHandle, &response);
}

static size_t ContentLengthOf(const char *headers) {
    const char *line = headers;

    while (line != NULL && *line != '\0') {
        if (strncasecmp(line, "Content-Length:", 15) == 0) {
            unsigned long value = 0;
            const char *cursor = line + 15;

            while (*cursor == ' ' || *cursor == '\t') {
                cursor++;
            }

            while (*cursor >= '0' && *cursor <= '9') {
                value = (value * 10) + (unsigned long)(*cursor - '0');
                cursor++;
            }

            return (size_t)value;
        }

        line = strstr(line, "\r\n");

        if (line != NULL) {
            line += 2;
        }
    }

    return 0;
}

/* Fills the buffer until the headers are complete and the announced body has
 * arrived. Returns false once the request cannot fit, so the caller answers 413
 * and closes instead of buffering the remainder. */
static bool ReadRequest(int socketHandle, size_t *totalRead, size_t *bodyOffset, size_t *bodyLength) {
    const size_t capacity = sizeof(requestBuffer) - 1;
    size_t received = 0;
    size_t headerEnd = 0;
    size_t announcedBody = 0;
    bool headersComplete = false;

    while (received < capacity) {
        int result = recv(socketHandle, requestBuffer + received, capacity - received, 0);

        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }

            return false;
        }

        if (result == 0) {
            break;
        }

        received += (size_t)result;
        requestBuffer[received] = '\0';

        if (!headersComplete) {
            const char *separator = strstr(requestBuffer, "\r\n\r\n");

            if (separator == NULL) {
                continue;
            }

            headersComplete = true;
            headerEnd = (size_t)(separator - requestBuffer) + 4;
            announcedBody = ContentLengthOf(requestBuffer);

            if (announcedBody > capacity - headerEnd) {
                return false;
            }
        }

        if (received >= headerEnd + announcedBody) {
            break;
        }
    }

    if (!headersComplete) {
        return false;
    }

    *totalRead = received;
    *bodyOffset = headerEnd;
    *bodyLength = received - headerEnd < announcedBody ? received - headerEnd : announcedBody;
    return true;
}

static bool ParseMethod(const char *token, HttpMethod *method) {
    if (strcmp(token, "GET") == 0) {
        *method = HTTP_METHOD_GET;
        return true;
    }

    if (strcmp(token, "POST") == 0) {
        *method = HTTP_METHOD_POST;
        return true;
    }

    return false;
}

/* Splits the request line in place: NUL-terminates method and path, and detaches
 * the query string. Returns false when the line is malformed. */
static bool ParseRequestLine(char **method, char **path, char **query) {
    char *methodEnd = strchr(requestBuffer, ' ');

    if (methodEnd == NULL) {
        return false;
    }

    *methodEnd = '\0';
    char *pathStart = methodEnd + 1;
    char *pathEnd = strpbrk(pathStart, " \r\n");

    if (pathEnd == NULL) {
        return false;
    }

    *pathEnd = '\0';
    char *queryStart = strchr(pathStart, '?');

    if (queryStart != NULL) {
        *queryStart = '\0';
        queryStart++;
    }

    *method = requestBuffer;
    *path = pathStart;
    *query = queryStart;
    return true;
}

static const Route *MatchRoute(const char *path, HttpMethod method, bool methodKnown, bool *pathExists) {
    *pathExists = false;

    for (uint16_t index = 0; index < routeCount; index++) {
        if (strcmp(routes[index].Path, path) != 0) {
            continue;
        }

        *pathExists = true;

        if (methodKnown && routes[index].Method == method) {
            return &routes[index];
        }
    }

    return useDefaultRoute ? &defaultRoute : NULL;
}

static void DispatchRoute(int socketHandle, const Route *match, HttpMethod method, const char *path, const char *query,
    size_t bodyOffset, size_t bodyLength) {
    char *body = requestBuffer + bodyOffset;
    body[bodyLength] = '\0';

    HttpRequest request;
    request.Method = method;
    request.Path = path;
    request.Query = query;
    request.Body = body;
    request.BodyLength = (uint16_t)bodyLength;

    HttpResponse response;
    response.StatusCode = 200;
    response.ContentType = DEFAULT_CONTENT_TYPE;
    response.Body = NULL;
    response.BodyLength = 0;

    match->Callback(&request, &response, match->Context);
    WriteResponse(socketHandle, &response);
}

static void ServeConnection(int socketHandle) {
    size_t totalRead = 0;
    size_t bodyOffset = 0;
    size_t bodyLength = 0;

    if (!ReadRequest(socketHandle, &totalRead, &bodyOffset, &bodyLength)) {
        WriteStatusOnly(socketHandle, 413);
        return;
    }

    char *methodToken = NULL;
    char *path = NULL;
    char *query = NULL;

    if (!ParseRequestLine(&methodToken, &path, &query)) {
        WriteStatusOnly(socketHandle, 400);
        return;
    }

    HttpMethod method = HTTP_METHOD_GET;
    bool methodKnown = ParseMethod(methodToken, &method);
    bool pathExists = false;
    const Route *match = MatchRoute(path, method, methodKnown, &pathExists);

    if (match == NULL) {
        WriteStatusOnly(socketHandle, pathExists ? 405 : 404);
        return;
    }

    DispatchRoute(socketHandle, match, method, path, query, bodyOffset, bodyLength);
}

static bool BindAndListen(int handle, uint16_t port) {
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);

    if (bind(handle, (struct sockaddr *)&address, sizeof(address)) < 0) {
        printf("[net.ll] HTTP server: cannot bind port %u.\n", (unsigned)port);
        return false;
    }

    if (listen(handle, 1) < 0) {
        printf("[net.ll] HTTP server: cannot listen on port %u.\n", (unsigned)port);
        return false;
    }

    return true;
}

bool HttpServerStart(uint16_t port) {
    if (listenSocket >= 0) {
        return true;
    }

    int handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    if (handle < 0) {
        printf("[net.ll] HTTP server: cannot create the listening socket.\n");
        return false;
    }

    int reuse = 1;
    setsockopt(handle, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    if (!BindAndListen(handle, port)) {
        close(handle);
        return false;
    }

    listenSocket = handle;
    return true;
}

bool HttpServerSetDefaultEndpoint(HttpMethod method, const char *path, HttpEndpointCallback callback, void *context) {
    if (path == NULL || callback == NULL || routeCount >= HTTP_SERVER_MAX_ROUTES) {
        return false;
    }

    defaultRoute.Method = method;
    defaultRoute.Path = path;
    defaultRoute.Callback = callback;
    defaultRoute.Context = context;
    useDefaultRoute = true;
    return true;
}

bool HttpServerRegisterEndpoint(HttpMethod method, const char *path, HttpEndpointCallback callback, void *context) {
    if (path == NULL || callback == NULL || routeCount >= HTTP_SERVER_MAX_ROUTES) {
        return false;
    }

    routes[routeCount].Method = method;
    routes[routeCount].Path = path;
    routes[routeCount].Callback = callback;
    routes[routeCount].Context = context;
    routeCount++;
    return true;
}

bool HttpServerPoll(uint32_t timeoutMilliseconds) {
    if (listenSocket < 0) {
        return false;
    }

    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(listenSocket, &readable);

    struct timeval timeout;
    timeout.tv_sec = (time_t)(timeoutMilliseconds / 1000u);
    timeout.tv_usec = (suseconds_t)((timeoutMilliseconds % 1000u) * 1000u);

    if (select(listenSocket + 1, &readable, NULL, NULL, &timeout) <= 0) {
        return false;
    }

    int connection = accept(listenSocket, NULL, NULL);

    if (connection < 0) {
        return false;
    }

    ServeConnection(connection);
    close(connection);
    return true;
}

void HttpServerStop(void) {
    if (listenSocket >= 0) {
        close(listenSocket);
        listenSocket = -1;
    }

    routeCount = 0;
}
