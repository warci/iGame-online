/*
  http.c
  Minimal HTTP/1.0 client over bsdsocket.library for iGame

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

/*
 * Plain http:// works with any bsdsocket.library compatible stack
 * (Roadshow, AmiTCP, Miami, Genesis). https:// additionally needs
 * AmiSSL 5 or newer, which is opened on demand the first time a secure
 * address is requested, so iGame still runs on systems without it.
 *
 * Requests are sent as HTTP/1.0 with "Connection: close" so the server
 * never uses chunked transfer encoding and the end of the body is simply
 * the end of the connection.
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <proto/bsdsocket.h>
#include <libraries/bsdsocket.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>

/* AmiSSL */
#include <proto/amisslmaster.h>
#include <proto/amissl.h>
#include <libraries/amisslmaster.h>
#include <libraries/amissl.h>
#include <amissl/amissl.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

/* ANSI C */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

#include "version.h"
#include "strfuncs.h"
#include "http.h"

#define HTTP_BUFFER_SIZE   8192
#define HTTP_MAX_URL       512
#define HTTP_MAX_HOST      128
#define HTTP_MAX_REDIRECTS 4
#define HTTP_TIMEOUT_SECS  45   /* give up after this long without data */
#define HTTP_POLL_PER_SEC  5    /* progress callbacks per second while waiting */

struct Library *SocketBase = NULL;
struct Library *AmiSSLMasterBase = NULL;
struct Library *AmiSSLBase = NULL;
struct Library *AmiSSLExtBase = NULL;

static SSL_CTX *sslCtx = NULL;
static BOOL sslTried = FALSE;  /* don't retry opening AmiSSL on every request */

/* The last TLS session, reused for the next connection to the same host.
   A resumed session skips most of the handshake, which is slow on 68k */
static SSL_SESSION *sslSession = NULL;
static char sslSessionHost[HTTP_MAX_HOST];

/* An open connection, plain or TLS */
typedef struct
{
	LONG s;          /* -1 when closed */
	SSL *ssl;        /* NULL for plain http */
	char host[HTTP_MAX_HOST];
	UWORD port;
	BOOL secure;
} httpConn;

/*
 * Our own fd set and timeval. They are layout compatible with what every
 * bsdsocket.library expects (an array of 32-bit masks, and two ULONGs for
 * seconds and microseconds). Using our own types avoids the differences
 * between the SDK headers (fd_set missing, tv_secs vs tv_sec etc).
 */
struct httpFdSet { ULONG bits[8]; };
struct httpTimeVal { ULONG secs; ULONG micro; };

BOOL httpAvailable(void)
{
	if (SocketBase)
		return TRUE;

	/* Opened on demand, so iGame still works without a TCP/IP stack and
	   picks it up when the user starts the stack later on */
	SocketBase = OpenLibrary("bsdsocket.library", 4);
	if (!SocketBase)
		return FALSE;

	/*
	 * Have the library put its error codes into errno. AmiSSL reads them
	 * from there (AmiSSL_ErrNoPtr) to tell "no data yet" on a
	 * non-blocking socket from a real error; without this every such
	 * read looks like a broken connection.
	 */
	SocketBaseTags(
		SBTM_SETVAL(SBTC_ERRNOPTR(sizeof(errno))), (ULONG)&errno,
		SBTM_SETVAL(SBTC_LOGTAGPTR), (ULONG)"iGame",
		TAG_DONE);

	return TRUE;
}

/*
 * Open AmiSSL the first time a https:// address is used. It needs the
 * socket library base, so this must run after httpAvailable().
 */
static BOOL sslAvailable(void)
{
	if (sslCtx)
		return TRUE;

	if (sslTried)
		return FALSE;
	sslTried = TRUE;

	AmiSSLMasterBase = OpenLibrary("amisslmaster.library", AMISSLMASTER_MIN_VERSION);
	if (!AmiSSLMasterBase)
		return FALSE;

	if (OpenAmiSSLTags(AMISSL_CURRENT_VERSION,
			AmiSSL_UsesOpenSSLStructs, FALSE,
			AmiSSL_GetAmiSSLBase, (ULONG)&AmiSSLBase,
			AmiSSL_GetAmiSSLExtBase, (ULONG)&AmiSSLExtBase,
			AmiSSL_SocketBase, (ULONG)SocketBase,
			AmiSSL_ErrNoPtr, (ULONG)&errno,
			TAG_DONE) != 0)
	{
		CloseLibrary(AmiSSLMasterBase);
		AmiSSLMasterBase = NULL;
		AmiSSLBase = AmiSSLExtBase = NULL;
		return FALSE;
	}

	sslCtx = SSL_CTX_new(TLS_client_method());
	if (!sslCtx)
	{
		CloseAmiSSL();
		CloseLibrary(AmiSSLMasterBase);
		AmiSSLMasterBase = NULL;
		AmiSSLBase = AmiSSLExtBase = NULL;
		return FALSE;
	}

	/* Check the server certificates against the AmiSSL:Certs store */
	SSL_CTX_set_default_verify_paths(sslCtx);
	SSL_CTX_set_verify(sslCtx, SSL_VERIFY_PEER, NULL);
	SSL_CTX_set_min_proto_version(sslCtx, TLS1_2_VERSION);

	return TRUE;
}

static void sslCleanup(void)
{
	if (sslSession)
	{
		SSL_SESSION_free(sslSession);
		sslSession = NULL;
	}
	if (sslCtx)
	{
		SSL_CTX_free(sslCtx);
		sslCtx = NULL;
	}
	if (AmiSSLBase)
	{
		CloseAmiSSL();
		AmiSSLBase = AmiSSLExtBase = NULL;
	}
	if (AmiSSLMasterBase)
	{
		CloseLibrary(AmiSSLMasterBase);
		AmiSSLMasterBase = NULL;
	}
	sslTried = FALSE;
}

void httpCleanup(void)
{
	/* AmiSSL uses the socket library, so it goes first */
	sslCleanup();

	if (SocketBase)
	{
		CloseLibrary(SocketBase);
		SocketBase = NULL;
	}
}

const char *httpErrorString(LONG err)
{
	switch (err)
	{
		case HTTP_OK:            return "OK";
		case HTTP_ERR_NOLIB:     return "bsdsocket.library is not available. Is your TCP/IP stack running?";
		case HTTP_ERR_URL:       return "The address is not a valid http:// or https:// URL.";
		case HTTP_ERR_HTTPS:     return "Secure (https) connections need AmiSSL 5 or newer.";
		case HTTP_ERR_TLS:       return "The secure connection failed. Is AmiSSL up to date?";
		case HTTP_ERR_RESOLVE:   return "Could not find the server. Please check your network connection.";
		case HTTP_ERR_CONNECT:   return "Could not connect to the server.";
		case HTTP_ERR_SEND:      return "Sending the request failed.";
		case HTTP_ERR_RECV:      return "The connection was lost.";
		case HTTP_ERR_TIMEOUT:   return "The server stopped responding.";
		case HTTP_ERR_STATUS:    return "The server returned an error.";
		case HTTP_ERR_ABORT:     return "Cancelled.";
		case HTTP_ERR_WRITE:     return "Could not write the downloaded data. Is the disk full?";
		case HTTP_ERR_REDIRECT:  return "Too many redirects.";
		case HTTP_ERR_MEMORY:    return "Not enough memory.";
	}
	return "Unknown error.";
}

/*
 * Percent-encode a path for use in a URL. Slashes are kept, so
 * "A/Foo & Bar.lha" becomes "A/Foo%20%26%20Bar.lha".
 */
void httpEncodePath(const char *in, char *out, int outSize)
{
	static const char hex[] = "0123456789ABCDEF";
	int o = 0;

	if (outSize <= 0)
		return;

	for (; *in && o < outSize - 1; in++)
	{
		const unsigned char c = (unsigned char)*in;
		if (
			(c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
			(c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
			c == '~' || c == '/' || c == '!' || c == '\'' || c == '(' ||
			c == ')' || c == '*' || c == ',' || c == '='
		) {
			out[o++] = c;
		}
		else
		{
			if (o + 3 >= outSize)
				break;
			out[o++] = '%';
			out[o++] = hex[c >> 4];
			out[o++] = hex[c & 15];
		}
	}
	out[o] = '\0';
}

/*
 * Split "http[s]://host[:port]/path" into its parts.
 */
static LONG parseUrl(const char *url, char *host, int hostSize, UWORD *port,
	const char **path, BOOL *secure)
{
	const char *p;
	int len;

	if (!strncasecmp_ig(url, "https://", 8))
	{
		*secure = TRUE;
		url += 8;
	}
	else if (!strncasecmp_ig(url, "http://", 7))
	{
		*secure = FALSE;
		url += 7;
	}
	else
		return HTTP_ERR_URL;

	p = url;
	while (*p && *p != ':' && *p != '/')
		p++;

	len = p - url;
	if (len == 0 || len >= hostSize)
		return HTTP_ERR_URL;

	memcpy(host, url, len);
	host[len] = '\0';

	*port = *secure ? 443 : 80;
	if (*p == ':')
	{
		LONG portNum = atol(p + 1);
		if (portNum <= 0 || portNum > 65535)
			return HTTP_ERR_URL;
		*port = (UWORD)portNum;
		while (*p && *p != '/')
			p++;
	}

	*path = (*p == '/') ? p : "/";
	return HTTP_OK;
}

static LONG connectTo(const char *host, UWORD port, LONG *sockOut)
{
	struct hostent *he;
	struct sockaddr_in addr;
	LONG s;

	he = gethostbyname((STRPTR)host);
	if (!he || !he->h_addr_list || !he->h_addr_list[0])
		return HTTP_ERR_RESOLVE;

	s = socket(AF_INET, SOCK_STREAM, 0);
	if (s < 0)
		return HTTP_ERR_CONNECT;

	/* Our fd set only covers 256 descriptors; a fresh library base starts at 0 */
	if (s >= 256)
	{
		CloseSocket(s);
		return HTTP_ERR_CONNECT;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sin_len = sizeof(addr);
	addr.sin_family = AF_INET;
	addr.sin_port = port; /* Amiga is big endian: host order is network order */
	memcpy(&addr.sin_addr, he->h_addr_list[0], 4);

	if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) < 0)
	{
		CloseSocket(s);
		return HTTP_ERR_CONNECT;
	}

	*sockOut = s;
	return HTTP_OK;
}

static void connClose(httpConn *c)
{
	if (c->ssl)
	{
		/* Keep the session for the next request. With TLS 1.3 the ticket
		   arrives after the handshake, so it is taken here, not on connect */
		SSL_SESSION *session = SSL_get1_session(c->ssl);
		if (session)
		{
			if (SSL_SESSION_is_resumable(session))
			{
				if (sslSession)
					SSL_SESSION_free(sslSession);
				sslSession = session;
				strlcpy(sslSessionHost, c->host, sizeof(sslSessionHost));
			}
			else
				SSL_SESSION_free(session);
		}
		SSL_free(c->ssl);
		c->ssl = NULL;
	}
	if (c->s >= 0)
	{
		CloseSocket(c->s);
		c->s = -1;
	}
}

/*
 * Connect and, for https, do the TLS handshake. The certificate must be
 * valid for the host name.
 */
static LONG connOpen(httpConn *c, const char *host, UWORD port, BOOL secure)
{
	LONG result;

	c->s = -1;
	c->ssl = NULL;
	strlcpy(c->host, host, sizeof(c->host));
	c->port = port;
	c->secure = secure;

	if (secure && !sslAvailable())
		return HTTP_ERR_HTTPS;

	result = connectTo(host, port, &c->s);
	if (result != HTTP_OK)
		return result;

	if (!secure)
		return HTTP_OK;

	c->ssl = SSL_new(sslCtx);
	if (!c->ssl)
	{
		connClose(c);
		return HTTP_ERR_MEMORY;
	}

	SSL_set_tlsext_host_name(c->ssl, (char *)host);  /* SNI */
	SSL_set1_host(c->ssl, host);                      /* name check */
	SSL_set_fd(c->ssl, c->s);

	if (sslSession && !strcmp(sslSessionHost, host))
		SSL_set_session(c->ssl, sslSession);

	if (SSL_connect(c->ssl) <= 0)
	{
		connClose(c);
		return HTTP_ERR_TLS;
	}

	return HTTP_OK;
}

static LONG sendAll(httpConn *c, const char *data, LONG len)
{
	while (len > 0)
	{
		LONG sent = c->ssl
			? SSL_write(c->ssl, (void *)data, len)
			: send(c->s, (APTR)data, len, 0);
		if (sent <= 0)
			return HTTP_ERR_SEND;
		data += sent;
		len -= sent;
	}
	return HTTP_OK;
}

/*
 * Receive with a timeout. While waiting, the progress callback is called
 * a few times a second so the GUI stays alive and the user can cancel.
 * Returns the number of bytes read, 0 when the server closed the
 * connection, or a negative HTTP_ERR_ code.
 */
static LONG recvWait(httpConn *c, char *buf, LONG len,
	httpProgressFunc progress, void *userData, ULONG received, ULONG total)
{
	const LONG s = c->s;
	int waited = 0;

	/* TLS may already hold decrypted data the socket doesn't know about */
	if (c->ssl && SSL_pending(c->ssl) > 0)
	{
		LONG n = SSL_read(c->ssl, buf, len);
		return (n > 0) ? n : HTTP_ERR_RECV;
	}

	while (1)
	{
		struct httpFdSet readSet;
		struct httpTimeVal tv;
		ULONG sigs = SIGBREAKF_CTRL_C;
		LONG n;

		memset(&readSet, 0, sizeof(readSet));
		readSet.bits[s >> 5] |= 1UL << (s & 31);
		tv.secs = 0;
		tv.micro = 1000000 / HTTP_POLL_PER_SEC;

		n = WaitSelect(s + 1, (void *)&readSet, NULL, NULL, (void *)&tv, &sigs);
		if (n < 0)
			return HTTP_ERR_RECV;

		if (n > 0)
		{
			if (c->ssl)
			{
				/* The socket is blocking, so this waits for a full record */
				n = SSL_read(c->ssl, buf, len);
				if (n > 0)
					return n;
				/* A clean close_notify, or a server that just hung up */
				switch (SSL_get_error(c->ssl, n))
				{
					case SSL_ERROR_ZERO_RETURN:
					case SSL_ERROR_SYSCALL:
						return 0;
				}
				return HTTP_ERR_RECV;
			}

			n = recv(s, (APTR)buf, len, 0);
			return (n < 0) ? HTTP_ERR_RECV : n;
		}

		if (sigs & SIGBREAKF_CTRL_C)
			return HTTP_ERR_ABORT;

		if (progress && !progress(received, total, userData))
			return HTTP_ERR_ABORT;

		if (++waited >= HTTP_TIMEOUT_SECS * HTTP_POLL_PER_SEC)
			return HTTP_ERR_TIMEOUT;
	}
}

/*
 * Find a header value in the response header block. The block is
 * NUL terminated and uses \r\n line endings. Returns the length copied.
 */
static int getHeader(const char *headers, const char *name, char *out, int outSize)
{
	const int nameLen = strlen(name);
	const char *line = headers;

	while (line && *line)
	{
		if (!strncasecmp_ig(line, name, nameLen) && line[nameLen] == ':')
		{
			const char *v = line + nameLen + 1;
			int o = 0;
			while (*v == ' ' || *v == '\t')
				v++;
			while (*v && *v != '\r' && *v != '\n' && o < outSize - 1)
				out[o++] = *v++;
			out[o] = '\0';
			return o;
		}
		line = strchr(line, '\n');
		if (line)
			line++;
	}
	return 0;
}

/*
 * Every request uses its own connection, as HTTP/1.0 with
 * "Connection: close": the end of the body is the end of the connection.
 * Keeping connections open (HTTP/1.1) saves TLS handshakes, but stalled
 * downloads under FS-UAE's emulated bsdsocket.library with AmiSSL.
 */
LONG httpGet(const char *url, httpSinkFunc sink, httpProgressFunc progress,
	void *userData, LONG *httpStatus)
{
	char *curUrl = NULL;
	char *buf = NULL;
	char host[HTTP_MAX_HOST];
	LONG result = HTTP_ERR_REDIRECT;
	int redirects;

	if (httpStatus)
		*httpStatus = 0;

	if (!httpAvailable())
		return HTTP_ERR_NOLIB;

	curUrl = AllocVec(HTTP_MAX_URL, MEMF_CLEAR);
	buf = AllocVec(HTTP_BUFFER_SIZE + 1, MEMF_CLEAR);
	if (!curUrl || !buf)
	{
		if (curUrl) FreeVec(curUrl);
		if (buf) FreeVec(buf);
		return HTTP_ERR_MEMORY;
	}
	strlcpy(curUrl, url, HTTP_MAX_URL);

	for (redirects = 0; redirects <= HTTP_MAX_REDIRECTS; redirects++)
	{
		const char *path;
		UWORD port;
		BOOL secure = FALSE;
		httpConn conn;
		LONG headerLen = 0;
		LONG got;
		char *bodyStart = NULL;
		LONG status = 0;
		ULONG contentLength = 0;
		ULONG received = 0;
		char value[HTTP_MAX_URL];

		result = parseUrl(curUrl, host, sizeof(host), &port, &path, &secure);
		if (result != HTTP_OK)
			break;

		if (progress && !progress(0, 0, userData))
		{
			result = HTTP_ERR_ABORT;
			break;
		}

		result = connOpen(&conn, host, port, secure);
		if (result != HTTP_OK)
			break;

		snprintf(buf, HTTP_BUFFER_SIZE,
			"GET %s HTTP/1.0\r\n"
			"Host: %s\r\n"
			"User-Agent: iGame/%d.%d.%d (Amiga)\r\n"
			"Accept: */*\r\n"
			"Connection: close\r\n"
			"\r\n",
			path, host, MAJOR_VERS, MINOR_VERS, PATCH_VERS);

		result = sendAll(&conn, buf, strlen(buf));
		if (result != HTTP_OK)
		{
			connClose(&conn);
			break;
		}

		/* Read until the end of the response header */
		while (bodyStart == NULL)
		{
			if (headerLen >= HTTP_BUFFER_SIZE)
			{
				result = HTTP_ERR_RECV;
				break;
			}

			got = recvWait(&conn, buf + headerLen, HTTP_BUFFER_SIZE - headerLen,
				progress, userData, 0, 0);
			if (got <= 0)
			{
				result = (got == 0) ? HTTP_ERR_RECV : got;
				break;
			}
			headerLen += got;
			buf[headerLen] = '\0';

			bodyStart = strstr(buf, "\r\n\r\n");
			if (bodyStart)
			{
				bodyStart[2] = '\0'; /* terminate the header block */
				bodyStart += 4;
			}
		}

		if (bodyStart == NULL)
		{
			connClose(&conn);
			break;
		}

		/* Status line: HTTP/1.x NNN Reason */
		{
			const char *sp = strchr(buf, ' ');
			status = sp ? atol(sp + 1) : 0;
		}
		if (httpStatus)
			*httpStatus = status;

		if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308)
		{
			connClose(&conn);

			if (!getHeader(buf, "Location", value, sizeof(value)))
			{
				result = HTTP_ERR_STATUS;
				break;
			}

			if (!strncasecmp_ig(value, "http://", 7) || !strncasecmp_ig(value, "https://", 8))
			{
				strlcpy(curUrl, value, HTTP_MAX_URL);
			}
			else if (value[0] == '/')
			{
				const char *scheme = secure ? "https" : "http";
				if (port == (secure ? 443 : 80))
					snprintf(curUrl, HTTP_MAX_URL, "%s://%s%s", scheme, host, value);
				else
					snprintf(curUrl, HTTP_MAX_URL, "%s://%s:%u%s", scheme, host, (unsigned int)port, value);
			}
			else
			{
				result = HTTP_ERR_URL;
				break;
			}

			result = HTTP_ERR_REDIRECT;
			continue;
		}

		if (status != 200)
		{
			connClose(&conn);
			result = HTTP_ERR_STATUS;
			break;
		}

		if (getHeader(buf, "Content-Length", value, sizeof(value)))
			contentLength = (ULONG)atol(value);

		/* Body bytes that arrived together with the header */
		got = headerLen - (bodyStart - buf);
		result = HTTP_OK;
		if (got > 0)
		{
			if (!sink(bodyStart, got, userData))
				result = HTTP_ERR_WRITE;
			received += got;
		}

		while (result == HTTP_OK)
		{
			if (progress && !progress(received, contentLength, userData))
			{
				result = HTTP_ERR_ABORT;
				break;
			}

			if (contentLength && received >= contentLength)
				break;

			got = recvWait(&conn, buf, HTTP_BUFFER_SIZE, progress, userData, received, contentLength);
			if (got < 0)
			{
				result = got;
				break;
			}
			if (got == 0)
			{
				/* Connection closed: fine, unless we know more was due */
				if (contentLength && received < contentLength)
					result = HTTP_ERR_RECV;
				break;
			}

			if (!sink(buf, got, userData))
				result = HTTP_ERR_WRITE;
			received += got;
		}

		connClose(&conn);
		break;
	}

	FreeVec(buf);
	FreeVec(curUrl);
	return result;
}

static BOOL fileSink(const char *data, LONG len, void *userData)
{
	BPTR fh = *(BPTR *)userData;
	return Write(fh, (APTR)data, len) == len;
}

struct fileSinkData
{
	BPTR fh;  /* must be first, fileSink reads it through userData */
	httpProgressFunc progress;
	void *userData;
};

static BOOL fileProgress(ULONG received, ULONG total, void *userData)
{
	struct fileSinkData *d = (struct fileSinkData *)userData;
	return d->progress ? d->progress(received, total, d->userData) : TRUE;
}

LONG httpGetToFile(const char *url, const char *path,
	httpProgressFunc progress, void *userData, LONG *httpStatus)
{
	struct fileSinkData d;
	LONG result;

	d.fh = Open((STRPTR)path, MODE_NEWFILE);
	if (!d.fh)
		return HTTP_ERR_WRITE;

	d.progress = progress;
	d.userData = userData;

	result = httpGet(url, fileSink, fileProgress, &d, httpStatus);
	Close(d.fh);

	if (result != HTTP_OK)
		DeleteFile((STRPTR)path);

	return result;
}
