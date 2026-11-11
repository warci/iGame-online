/*
  http.h
  Minimal HTTP/HTTPS client over bsdsocket.library - header for iGame

  This file is part of iGame.

  iGame is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  iGame is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with iGame. If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef _HTTP_H
#define _HTTP_H

#include <exec/types.h>

#define HTTP_OK              0
#define HTTP_ERR_NOLIB      -1  /* bsdsocket.library could not be opened */
#define HTTP_ERR_URL        -2  /* malformed or unsupported URL */
#define HTTP_ERR_HTTPS      -3  /* https needs AmiSSL, which is not available */
#define HTTP_ERR_RESOLVE    -4  /* host name lookup failed */
#define HTTP_ERR_CONNECT    -5  /* could not connect */
#define HTTP_ERR_SEND       -6  /* sending the request failed */
#define HTTP_ERR_RECV       -7  /* receiving failed or connection dropped */
#define HTTP_ERR_TIMEOUT    -8  /* server stopped responding */
#define HTTP_ERR_STATUS     -9  /* server answered with a non 200 status */
#define HTTP_ERR_ABORT      -10 /* cancelled by the user */
#define HTTP_ERR_WRITE      -11 /* the data could not be stored */
#define HTTP_ERR_REDIRECT   -12 /* too many redirects */
#define HTTP_ERR_MEMORY     -13 /* out of memory */
#define HTTP_ERR_TLS        -14 /* TLS handshake or certificate check failed */

/*
 * Receives body data. Return FALSE to stop the transfer (HTTP_ERR_WRITE).
 */
typedef BOOL (*httpSinkFunc)(const char *data, LONG len, void *userData);

/*
 * Called regularly during a transfer, also while waiting for data.
 * total is 0 when the server didn't send a Content-Length.
 * Return FALSE to cancel the transfer (HTTP_ERR_ABORT).
 */
typedef BOOL (*httpProgressFunc)(ULONG received, ULONG total, void *userData);

BOOL httpAvailable(void);
void httpCleanup(void);
LONG httpGet(const char *url, httpSinkFunc sink, httpProgressFunc progress,
	void *userData, LONG *httpStatus);
LONG httpGetToFile(const char *url, const char *path,
	httpProgressFunc progress, void *userData, LONG *httpStatus);
const char *httpErrorString(LONG err);
void httpEncodePath(const char *in, char *out, int outSize);

#endif
