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

#ifndef __HTTP_SERVER_H_
#define __HTTP_SERVER_H_

#include <stdbool.h>
#include <stdint.h>

/* One request at a time is read into a single static buffer of this size, so it
 * bounds the whole server's RAM footprint on a 264KB device. */
#define HTTP_SERVER_MAX_REQUEST_SIZE 2048
#define HTTP_SERVER_MAX_ROUTES 8

typedef enum {
    HTTP_METHOD_GET,
    HTTP_METHOD_POST
} HttpMethod;

/* Path, Query and Body point into the server's own request buffer and are valid
 * only for the duration of the callback: the next HttpServerPoll overwrites them.
 * A callback that needs the content afterwards copies it. Query is null when the
 * request carried no query string. BodyLength is reported separately so a body
 * carrying arbitrary bytes works. */
typedef struct {
    HttpMethod Method;
    const char *Path;
    const char *Query;
    const char *Body;
    uint16_t BodyLength;
} HttpRequest;

/* The callback owns Body and it must stay valid until HttpServerPoll returns --
 * the body is written out inside that same call, so pointing at a compiled-in
 * constant or at a member of an object that outlives the poll is enough, and no
 * allocation or copy is needed. StatusCode defaults to 200 and ContentType to
 * text/html before the callback runs. */
typedef struct {
    uint16_t StatusCode;
    const char *ContentType;
    const char *Body;
    uint32_t BodyLength;
} HttpResponse;

typedef void (*HttpEndpointCallback)(const HttpRequest *request, HttpResponse *response, void *context);

bool HttpServerStart(uint16_t port);

/* context is carried through to the callback untouched, so a consumer reaches its
 * own state without this library knowing anything about it. */
bool HttpServerRegisterEndpoint(HttpMethod method, const char *path,
                                HttpEndpointCallback callback, void *context);

/*
 * Waits up to timeoutMilliseconds for one connection and, when one arrives, reads
 * the request, calls the matching callback on the caller's thread, writes the
 * response and closes the connection, all before returning. This library creates
 * no thread: the caller lends it one in slices short enough to keep its own loop
 * running.
 *
 * Returns whether a request was served.
 */
bool HttpServerPoll(uint32_t timeoutMilliseconds);

void HttpServerStop(void);

#endif /* __HTTP_SERVER_H_ */
