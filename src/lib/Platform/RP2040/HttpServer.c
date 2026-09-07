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

bool HttpServerStart(uint16_t port) {
    (void)port;
    printf("HttpServerStart: not implemented on RP2040 yet (needs a radio and lwIP enabled)\n");
    return false;
}

bool HttpServerRegisterEndpoint(HttpMethod method, const char *path,
                                HttpEndpointCallback callback, void *context) {
    (void)method;
    (void)path;
    (void)callback;
    (void)context;
    return false;
}

bool HttpServerPoll(uint32_t timeoutMilliseconds) {
    (void)timeoutMilliseconds;
    return false;
}

void HttpServerStop(void) {
}
