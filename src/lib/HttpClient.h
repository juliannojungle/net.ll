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

#ifndef __HTTP_CLIENT_H_
#define __HTTP_CLIENT_H_

#include <stdbool.h>

/*
 * Downloads a file from the given URL and saves it to filePath, streaming the
 * body straight to storage rather than buffering it -- the hardware has little
 * RAM. Creates intermediate directories as needed via fs.ll.
 *
 * Synchronous: it returns only once the transfer has finished or failed.
 *
 * Implemented on the Simulator (POSIX sockets + OpenSSL for HTTPS). On RP2040
 * and ESP32 it is still a stub returning false; see AGENTS.md.
 *
 * Returns true on success, false on failure.
 */
bool HttpDownloadFile(const char *url, const char *filePath);

#endif /* __HTTP_CLIENT_H_ */
