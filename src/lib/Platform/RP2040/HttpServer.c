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

#include "FreeRTOS.h"
#include "task.h"

#define LWIP_TIMEVAL_PRIVATE 0
#include "lwip/inet.h"
#include "lwip/sockets.h"

#define DEFAULT_CONTENT_TYPE "text/html"
#define RESPONSE_HEADER_SIZE 256
#define HTTP_SEND_CHUNK_SIZE TCP_MSS

typedef struct {
    HttpMethod Method;
    const char *Path;
    HttpEndpointCallback Callback;
    void *Context;
} Route;

typedef struct {
    size_t RequestLength;
    size_t HeaderLength;
    size_t BodyLength;
    size_t ExpectedBodyLength;
    bool HeadersComplete;
} ConnectionState;

typedef struct {
    int Socket;
    char RequestBuffer[HTTP_SERVER_MAX_REQUEST_SIZE];
    ConnectionState State;
} ConnectionContext;

static int serverSocket = -1;
static TaskHandle_t serverTask = NULL;

static Route routes[HTTP_SERVER_MAX_ROUTES];
static uint16_t routeCount = 0;
static Route defaultRoute;
static bool useDefaultRoute = false;

/* -------------------------------------------------------------------------- */
/* HTTP status                                                                 */
/* -------------------------------------------------------------------------- */

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

/* -------------------------------------------------------------------------- */
/* HTTP response                                                               */
/* -------------------------------------------------------------------------- */

static bool SendBytes(int socket, const char *data, size_t length) {
    while (length > 0) {
        size_t chunk = length;

        if (chunk > HTTP_SEND_CHUNK_SIZE) {
            chunk = HTTP_SEND_CHUNK_SIZE;
        }

        int sent = send(socket, data, (int)chunk, 0);

        if (sent <= 0) {
            return false;
        }

        data += sent;
        length -= (size_t)sent;
    }

    return true;
}

static bool SendResponse(int socket, const HttpResponse *response) {
    char header[RESPONSE_HEADER_SIZE];
    const char *customHeader = (response->CustomHeader == NULL || response->CustomHeader[0] == '\0')
        ? "" : response->CustomHeader;

    const char *contentType = response->ContentType != NULL ? response->ContentType : DEFAULT_CONTENT_TYPE;
    unsigned bodyLength = (unsigned)response->BodyLength;
    int headerLength = snprintf(header, sizeof(header),
        "HTTP/1.1 %u %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %u\r\n"
        "Connection: close\r\n"
        "%s"
        "\r\n",
        (unsigned)response->StatusCode, ReasonPhrase(response->StatusCode), contentType, bodyLength, customHeader);

    if (headerLength <= 0 || (size_t)headerLength >= sizeof(header)) {
        return false;
    }

    if (!SendBytes(socket, header, (size_t)headerLength)) {
        return false;
    }

    if (response->Body == NULL || response->BodyLength == 0) {
        return true;
    }

    return SendBytes(socket, response->Body, response->BodyLength);
}

static bool SendStatusOnly(int socket, uint16_t statusCode) {
    const char *body = ReasonPhrase(statusCode);

    HttpResponse response;
    response.StatusCode = statusCode;
    response.ContentType = "text/plain";
    response.Body = body;
    response.BodyLength = (uint32_t)strlen(body);

    return SendResponse(socket, &response);
}

/* -------------------------------------------------------------------------- */
/* HTTP request parsing                                                        */
/* -------------------------------------------------------------------------- */

static size_t ContentLengthOf(const char *headers) {
    const char *line = headers;

    while (line != NULL && *line != '\0') {
        if (strncasecmp(line, "Content-Length:", 15) == 0) {
            size_t value = 0;
            const char *cursor = line + 15;

            while (*cursor == ' ' || *cursor == '\t') {
                cursor++;
            }

            while (*cursor >= '0' && *cursor <= '9') {
                value = (value * 10u) + (size_t)(*cursor - '0');
                cursor++;
            }

            return value;
        }

        line = strstr(line, "\r\n");

        if (line != NULL) {
            line += 2;
        }
    }

    return 0;
}

static bool FindHeaderEnd(ConnectionContext *connection, size_t *headerLength) {
    const char *separator = strstr(connection->RequestBuffer, "\r\n\r\n");

    if (separator == NULL) {
        return false;
    }

    *headerLength = (size_t)(separator - connection->RequestBuffer) + 4;
    return true;
}

static bool CompleteHeaders(ConnectionContext *connection) {
    size_t headerLength;

    if (!FindHeaderEnd(connection, &headerLength)) {
        return false;
    }

    connection->State.HeadersComplete = true;
    connection->State.HeaderLength = headerLength;
    connection->State.ExpectedBodyLength = ContentLengthOf(connection->RequestBuffer);
    return true;
}

static bool RequestIsComplete(ConnectionContext *connection) {
    if (!connection->State.HeadersComplete) {
        return false;
    }

    return connection->State.RequestLength >= connection->State.HeaderLength + connection->State.ExpectedBodyLength;
}

/* -------------------------------------------------------------------------- */
/* Request line                                                               */
/* -------------------------------------------------------------------------- */

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

static bool ParseRequestLine(ConnectionContext *connection, char **method, char **path, char **query) {
    char *methodEnd = strchr(connection->RequestBuffer, ' ');

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

    *method = connection->RequestBuffer;
    *path = pathStart;
    *query = queryStart;
    return true;
}

/* -------------------------------------------------------------------------- */
/* Routing                                                                     */
/* -------------------------------------------------------------------------- */

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

/* -------------------------------------------------------------------------- */
/* Request dispatch                                                            */
/* -------------------------------------------------------------------------- */

static bool DispatchRequest(ConnectionContext *connection) {
    char *methodToken = NULL;
    char *path = NULL;
    char *query = NULL;

    if (!ParseRequestLine(connection, &methodToken, &path, &query)) {
        SendStatusOnly(connection->Socket, 400);
        return false;
    }

    HttpMethod method = HTTP_METHOD_GET;
    bool methodKnown = ParseMethod(methodToken, &method);
    bool pathExists = false;
    const Route *match = MatchRoute(path, method, methodKnown, &pathExists);

    if (match == NULL) {
        SendStatusOnly(connection->Socket, pathExists ? 405 : 404);
        return false;
    }

    size_t bodyLength = connection->State.ExpectedBodyLength;

    if (bodyLength > connection->State.RequestLength - connection->State.HeaderLength) {
        bodyLength = connection->State.RequestLength - connection->State.HeaderLength;
    }

    char *body = connection->RequestBuffer + connection->State.HeaderLength;
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
    SendResponse(connection->Socket, &response);
    return false;
}

/* -------------------------------------------------------------------------- */
/* Connection task                                                             */
/* -------------------------------------------------------------------------- */

static void HttpConnectionTask(void *arg) {
    ConnectionContext *connection = (ConnectionContext *)arg;

    while (true) {
        size_t capacity = sizeof(connection->RequestBuffer) - 1;

        if (connection->State.RequestLength >= capacity) {
            SendStatusOnly(connection->Socket, 413);
            break;
        }

        int remaining = (int)(capacity - connection->State.RequestLength);
        int length =
            recv(connection->Socket, connection->RequestBuffer + connection->State.RequestLength, remaining, 0);

        if (length <= 0) {
            break;
        }

        connection->State.RequestLength += (size_t)length;
        connection->RequestBuffer[connection->State.RequestLength] = '\0';

        if (!connection->State.HeadersComplete) {
            CompleteHeaders(connection);
        }

        if (!connection->State.HeadersComplete) {
            continue;
        }

        if (connection->State.HeaderLength + connection->State.ExpectedBodyLength > capacity) {
            SendStatusOnly(connection->Socket, 413);
            break;
        }

        if (!RequestIsComplete(connection)) {
            continue;
        }

        DispatchRequest(connection);
        break;
    }

    closesocket(connection->Socket);
    vPortFree(connection);
    vTaskDelete(NULL);
}

/* -------------------------------------------------------------------------- */
/* Server task                                                                */
/* -------------------------------------------------------------------------- */

static int CreateServerSocket(uint16_t port) {
    int socket = socket(AF_INET, SOCK_STREAM, 0);

    if (socket < 0) {
        return -1;
    }

    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_port = PP_HTONS(port);
    address.sin_addr.s_addr = PP_HTONL(INADDR_ANY);

    if (bind(socket, (struct sockaddr *)&address, sizeof(address)) < 0) {
        closesocket(socket);
        return -1;
    }

    if (listen(socket, 1) < 0) {
        closesocket(socket);
        return -1;
    }

    printf("HTTP: listening on port %u\n", port);
    return socket;
}

static bool StartConnectionTask(int clientSocket) {
    ConnectionContext *connection = pvPortMalloc(sizeof(ConnectionContext));

    if (connection == NULL) {
        closesocket(clientSocket);
        return false;
    }

    memset(connection, 0, sizeof(ConnectionContext));
    connection->Socket = clientSocket;

    if (xTaskCreate(HttpConnectionTask, "HttpConnection", 1024, connection, 1, NULL) != pdPASS) {
        closesocket(clientSocket);
        vPortFree(connection);
        return false;
    }

    return true;
}

static void HttpServerTask(void *arg) {
    uint16_t port = (uint16_t)(uintptr_t)arg;
    serverSocket = CreateServerSocket(port);

    if (serverSocket < 0) {
        serverTask = NULL;
        vTaskDelete(NULL);
        return;
    }

    while (serverSocket >= 0) {
        struct sockaddr_in clientAddress;
        socklen_t clientAddressLength = sizeof(clientAddress);
        int clientSocket = accept(serverSocket, (struct sockaddr *)&clientAddress, &clientAddressLength);

        if (clientSocket < 0) {
            if (serverSocket < 0) {
                break;
            }

            continue;
        }

        StartConnectionTask(clientSocket);
    }

    if (serverSocket >= 0) {
        closesocket(serverSocket);
        serverSocket = -1;
    }

    serverTask = NULL;
    vTaskDelete(NULL);
}

/* -------------------------------------------------------------------------- */
/* Server lifecycle                                                            */
/* -------------------------------------------------------------------------- */

bool HttpServerStart(uint16_t port) {
    if (serverTask != NULL) {
        return true;
    }

    BaseType_t result = xTaskCreate(HttpServerTask, "HttpServer", 1024, (void *)(uintptr_t)port, 1, &serverTask);

    if (result != pdPASS) {
        serverTask = NULL;
        return false;
    }

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
    (void)timeoutMilliseconds;
    return false;
}

void HttpServerStop(void) {
    if (serverSocket >= 0) {
        int socket = serverSocket;
        serverSocket = -1;
        closesocket(socket);
    }

    routeCount = 0;
}