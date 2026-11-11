/*
  gameDetails.c
  Game details pane for iGame, with information from OpenRetro.org

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
 * When a game is selected, its page on the OpenRetro game database is
 * fetched and shown in two tabs:
 *   Main      - screenshot, credits, information, categories, description
 *   Community - user score, press reviews and the discussion comments
 *
 * OpenRetro's text data is public domain and meant to be used by the
 * Amiga community. Pages are only requested for the game the user
 * selects, never in bulk, and every result is cached in PROGDIR:cache so
 * a game is fetched once. Delete that drawer to refresh the details.
 *
 * The downloads run on the main task. While waiting, the GUI is kept
 * alive with MUIM_Application_InputBuffered. Selecting another game in
 * the meantime calls gameDetailsShow() again from inside the running
 * load; that call only records the new game and the running load stops
 * and starts over with it.
 */

/* MUI */
#include <libraries/mui.h>
#include <mui/NListview_mcc.h>
#include <mui/NFloattext_mcc.h>

/* Prototypes */
#include <clib/alib_protos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/muimaster.h>

/* ANSI C */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>

#define iGame_NUMBERS
#include "iGame_strings.h"
#include "iGameGUI.h"
#include "iGameExtern.h"
#include "strfuncs.h"
#include "funcs.h"
#include "fsfuncs.h"
#include <SDI_hook.h>
#include "http.h"
#include "gameDetails.h"

extern struct ObjApp *app;
extern igame_settings *current_settings;

#define DETAILS_MAX_PAGE      (512 * 1024) /* refuse anything bigger */
#define DETAILS_TEXT_SIZE     (24 * 1024)  /* each tab's text */
#define DETAILS_FIELD_SIZE    256
#define DETAILS_SLUG_SIZE     128
#define DETAILS_HASH_SIZE     48
#define DETAILS_MAX_COMMENTS  25
#define DETAILS_MAX_REVIEWS   12
#define DETAILS_MAX_SHOTS     9    /* screenshots per game, one digit in the file name */
#define DETAILS_CACHE_MAGIC   "IGD2"
#define DETAILS_CACHE_MAGIC1  "IGD1"   /* older files: comments always included */

/* Small delay before going online, so scrolling through the list with
   the keyboard doesn't start a download for every entry passed */
#define DETAILS_SETTLE_TICKS  2    /* times 5 ticks = 0.2s */

/* ---------------------------------------------------------------------
 * Growing text buffer
 * ------------------------------------------------------------------- */

typedef struct
{
	char *buf;
	LONG len;
	LONG size;
} textBuf;

static BOOL tbInit(textBuf *t, LONG size)
{
	t->buf = malloc(size);
	t->len = 0;
	t->size = t->buf ? size : 0;
	if (t->buf)
		t->buf[0] = '\0';
	return t->buf != NULL;
}

static void tbFree(textBuf *t)
{
	if (t->buf)
		free(t->buf);
	t->buf = NULL;
	t->len = t->size = 0;
}

/* Appends as much as fits. The text is only ever shown, so cutting a
   very long comments list short is fine */
static void tbAdd(textBuf *t, const char *s)
{
	while (*s && t->len < t->size - 1)
		t->buf[t->len++] = *s++;
	t->buf[t->len] = '\0';
}

static void tbAddf(textBuf *t, const char *fmt, ...)
{
	char line[DETAILS_FIELD_SIZE * 2];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	tbAdd(t, line);
}

/* Download sink: collects a page in memory */
static BOOL pageSink(const char *data, LONG len, void *userData)
{
	textBuf *t = (textBuf *)userData;

	if (t->len + len + 1 > t->size)
	{
		LONG newSize = t->size ? t->size : 32768;
		char *newBuf;

		while (t->len + len + 1 > newSize)
			newSize *= 2;
		if (newSize > DETAILS_MAX_PAGE)
			return FALSE;

		newBuf = realloc(t->buf, newSize);
		if (!newBuf)
			return FALSE;
		t->buf = newBuf;
		t->size = newSize;
	}

	memcpy(t->buf + t->len, data, len);
	t->len += len;
	t->buf[t->len] = '\0';
	return TRUE;
}

/* ---------------------------------------------------------------------
 * HTML to plain Latin-1 text
 * ------------------------------------------------------------------- */

typedef struct
{
	char *out;
	int len;
	int size;
} textOut;

static void toChar(textOut *o, char c)
{
	if (o->len < o->size - 1)
		o->out[o->len++] = c;
}

static void toSpace(textOut *o)
{
	if (o->len > 0 && o->out[o->len - 1] != ' ' && o->out[o->len - 1] != '\n')
		toChar(o, ' ');
}

static void toTrimEnd(textOut *o)
{
	while (o->len > 0 && (o->out[o->len - 1] == ' ' || o->out[o->len - 1] == '\n'))
		o->len--;
}

static void toNewline(textOut *o)
{
	while (o->len > 0 && o->out[o->len - 1] == ' ')
		o->len--;
	if (o->len > 0 && o->out[o->len - 1] != '\n')
		toChar(o, '\n');
}

static void toParagraph(textOut *o)
{
	toNewline(o);
	if (o->len > 1 && o->out[o->len - 2] != '\n')
		toChar(o, '\n');
}

/* Amiga fonts are Latin-1. Anything else gets the nearest ASCII */
static void toUnicode(textOut *o, ULONG cp)
{
	const char *s = NULL;

	if (cp == '\t' || cp == '\r' || cp == '\n' || cp == 0xA0)
	{
		toSpace(o);
		return;
	}
	if (cp < 32 || cp == 127 || (cp >= 0x80 && cp < 0xA0))
		return; /* control codes, including ESC which MUI would interpret */

	if (cp <= 0xFF)
	{
		toChar(o, (char)cp);
		return;
	}

	switch (cp)
	{
		case 0x2018: case 0x2019: case 0x201A: case 0x2032: s = "'"; break;
		case 0x201C: case 0x201D: case 0x201E: case 0x2033: s = "\""; break;
		case 0x2010: case 0x2011: case 0x2012: case 0x2013:
		case 0x2014: case 0x2015: case 0x2212: s = "-"; break;
		case 0x2026: s = "..."; break;
		case 0x2022: case 0x2027: s = "\xB7"; break;
		case 0x20AC: s = "EUR"; break;
		case 0x2122: s = "(TM)"; break;
		default:     s = "?"; break;
	}
	while (*s)
		toChar(o, *s++);
}

static void decodeEntity(textOut *o, const char **pp, const char *end)
{
	const char *p = *pp + 1;  /* after '&' */
	const char *semi = p;
	char name[12];
	int n = 0;
	ULONG cp = 0;

	while (semi < end && *semi != ';' && n < (int)sizeof(name) - 1 &&
		(isalnum((unsigned char)*semi) || *semi == '#'))
		name[n++] = *semi++;
	name[n] = '\0';

	if (semi >= end || *semi != ';' || n == 0)
	{
		toChar(o, '&');
		*pp += 1;
		return;
	}

	if (name[0] == '#')
		cp = (name[1] == 'x' || name[1] == 'X') ? strtoul(name + 2, NULL, 16) : strtoul(name + 1, NULL, 10);
	else if (!strcmp(name, "amp"))   cp = '&';
	else if (!strcmp(name, "lt"))    cp = '<';
	else if (!strcmp(name, "gt"))    cp = '>';
	else if (!strcmp(name, "quot"))  cp = '"';
	else if (!strcmp(name, "apos"))  cp = '\'';
	else if (!strcmp(name, "nbsp"))  cp = ' ';
	else if (!strcmp(name, "copy"))  cp = 0xA9;
	else if (!strcmp(name, "eacute")) cp = 0xE9;
	else if (!strcmp(name, "egrave")) cp = 0xE8;
	else if (!strcmp(name, "auml"))  cp = 0xE4;
	else if (!strcmp(name, "ouml"))  cp = 0xF6;
	else if (!strcmp(name, "uuml"))  cp = 0xFC;
	else if (!strcmp(name, "szlig")) cp = 0xDF;
	else if (!strcmp(name, "hellip")) cp = 0x2026;
	else if (!strcmp(name, "ndash") || !strcmp(name, "mdash")) cp = 0x2013;
	else if (!strcmp(name, "lsquo") || !strcmp(name, "rsquo")) cp = 0x2019;
	else if (!strcmp(name, "ldquo") || !strcmp(name, "rdquo")) cp = 0x201C;
	else cp = '?';

	toUnicode(o, cp);
	*pp = semi + 1;
}

/* Reads one UTF-8 sequence. Invalid bytes are taken as Latin-1 */
static ULONG decodeUtf8(const char **pp, const char *end)
{
	const unsigned char *p = (const unsigned char *)*pp;
	ULONG cp;
	int extra, i;

	if (p[0] < 0x80)      { *pp += 1; return p[0]; }
	else if ((p[0] & 0xE0) == 0xC0) { cp = p[0] & 0x1F; extra = 1; }
	else if ((p[0] & 0xF0) == 0xE0) { cp = p[0] & 0x0F; extra = 2; }
	else if ((p[0] & 0xF8) == 0xF0) { cp = p[0] & 0x07; extra = 3; }
	else { *pp += 1; return p[0]; }

	if ((const char *)p + extra >= end)
	{
		*pp += 1;
		return p[0];
	}
	for (i = 1; i <= extra; i++)
	{
		if ((p[i] & 0xC0) != 0x80)
		{
			*pp += 1;
			return p[0];
		}
		cp = (cp << 6) | (p[i] & 0x3F);
	}
	*pp += extra + 1;
	return cp;
}

static BOOL tagIs(const char *tag, int len, const char *name)
{
	const int n = strlen(name);
	return len >= n && !strncasecmp_ig(tag, name, n) &&
		(len == n || tag[n] == ' ' || tag[n] == '/' || tag[n] == '\t' || tag[n] == '\n');
}

/*
 * Converts the HTML between in and end into plain text: tags removed,
 * paragraphs and line breaks kept, entities and UTF-8 decoded.
 */
static void htmlToText(const char *in, const char *end, char *out, int outSize)
{
	textOut o;
	const char *p = in;

	o.out = out;
	o.len = 0;
	o.size = outSize;

	if (outSize <= 0)
		return;

	while (p < end && *p)
	{
		if (*p == '<')
		{
			const char *close;
			const char *tag = p + 1;
			int tagLen;
			BOOL closing = FALSE;

			if (!strncmp(p, "<!--", 4))
			{
				close = strstr(p + 4, "-->");
				p = (close && close < end) ? close + 3 : end;
				continue;
			}

			close = strchr(p, '>');
			if (!close || close >= end)
				break;

			if (*tag == '/')
			{
				closing = TRUE;
				tag++;
			}
			tagLen = close - tag;

			if (!closing && (tagIs(tag, tagLen, "script") || tagIs(tag, tagLen, "style")))
			{
				const char *skip = strstr(close, tagIs(tag, tagLen, "script") ? "</script" : "</style");
				close = skip ? strchr(skip, '>') : NULL;
				p = (close && close < end) ? close + 1 : end;
				continue;
			}

			if (tagIs(tag, tagLen, "br"))
				toNewline(&o);
			else if (tagIs(tag, tagLen, "p") || tagIs(tag, tagLen, "div") ||
				tagIs(tag, tagLen, "ul") || tagIs(tag, tagLen, "ol") ||
				tagIs(tag, tagLen, "h1") || tagIs(tag, tagLen, "h2") ||
				tagIs(tag, tagLen, "h3"))
				toParagraph(&o);
			else if (tagIs(tag, tagLen, "li"))
			{
				toNewline(&o);
				if (!closing)
					toUnicode(&o, 0x2022);
			}
			else if (!closing && tagIs(tag, tagLen, "img"))
			{
				/* The forums use small smiley images */
				const char *alt = strstr(tag, "alt=");
				if (alt && alt < close && !strncmp(alt + 5, "smiley", 6))
				{
					toSpace(&o);
					toChar(&o, ':');
					toChar(&o, ')');
				}
			}

			p = close + 1;
		}
		else if (*p == '&')
		{
			decodeEntity(&o, &p, end);
		}
		else
		{
			toUnicode(&o, decodeUtf8(&p, end));
		}
	}

	toTrimEnd(&o);
	out[o.len] = '\0';

	/* Leading spaces left over from the markup */
	if (out[0] == ' ' || out[0] == '\n')
	{
		int skip = 0;
		while (out[skip] == ' ' || out[skip] == '\n')
			skip++;
		memmove(out, out + skip, o.len - skip + 1);
	}
}

/* Converts the HTML that follows needle, up to endMark. FALSE if missing */
static BOOL extractText(const char *from, const char *needle, const char *endMark,
	char *out, int outSize)
{
	const char *start = strstr(from, needle);
	const char *end;

	out[0] = '\0';
	if (!start)
		return FALSE;
	start += strlen(needle);
	end = strstr(start, endMark);
	if (!end)
		return FALSE;

	htmlToText(start, end, out, outSize);
	return out[0] != '\0';
}

/* ---------------------------------------------------------------------
 * Matching games with OpenRetro entries
 * ------------------------------------------------------------------- */

/* Lowercase letters and digits only: "1869 [AGA]" -> "1869aga" */
static void normalizeTitle(const char *in, char *out, int outSize, BOOL dropBrackets)
{
	int o = 0;
	int depth = 0;

	for (; *in && o < outSize - 1; in++)
	{
		const unsigned char c = (unsigned char)*in;

		if (dropBrackets && (c == '(' || c == '['))
		{
			depth++;
			continue;
		}
		if (dropBrackets && (c == ')' || c == ']'))
		{
			if (depth > 0)
				depth--;
			continue;
		}
		if (depth > 0)
			continue;

		if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
			out[o++] = c;
		else if (c >= 'A' && c <= 'Z')
			out[o++] = c + 32;
	}
	out[o] = '\0';
}

/* "The Legend of Ragnarok" is listed as "Legend Of Ragnarok" too */
static const char *skipArticle(const char *title)
{
	return strncasecmp_ig(title, "the ", 4) ? title : title + 4;
}

/* Whether a title marks the AGA version, i.e. "Top Gear 2 (AGA 1858)" */
static BOOL isAgaTitle(const char *title)
{
	const char *p = title;

	while ((p = strcasestr(p, "aga")) != NULL)
	{
		const BOOL startOk = (p == title) || !isalnum((unsigned char)p[-1]);
		const BOOL endOk = !isalnum((unsigned char)p[3]);
		if (startOk && endOk)
			return TRUE;
		p += 3;
	}
	return FALSE;
}

/*
 * OpenRetro's address for a title: "Menace" -> "menace",
 * "1869 (AGA)" -> "1869-aga". Other bracketed parts are version notes
 * from the archive names, like "(0109)" or "(ReLINE)", and are left out.
 */
static void slugFromTitle(const char *in, char *out, int outSize)
{
	int o = 0;
	int depth = 0;
	BOOL dash = FALSE;
	const BOOL aga = isAgaTitle(in);

	for (; *in && o < outSize - 1; in++)
	{
		unsigned char c = (unsigned char)*in;

		if (c == '(' || c == '[') { depth++; continue; }
		if (c == ')' || c == ']') { if (depth) depth--; continue; }
		if (depth)
			continue;

		if (c >= 'A' && c <= 'Z')
			c += 32;

		if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
		{
			if (dash && o > 0 && o < outSize - 2)
				out[o++] = '-';
			dash = FALSE;
			out[o++] = c;
		}
		else if (c == '\'')
			continue; /* "Hunter's" -> "hunters" */
		else
			dash = TRUE;
	}
	out[o] = '\0';

	if (aga && o > 0 && o < outSize - 5)
		strcpy(out + o, "-aga");
}

/* The title without bracketed parts, for the search box */
static void searchQuery(const char *in, char *out, int outSize)
{
	int o = 0;
	int depth = 0;

	for (; *in && o < outSize - 1; in++)
	{
		if (*in == '(' || *in == '[') { depth++; continue; }
		if (*in == ')' || *in == ']') { if (depth) depth--; continue; }
		if (depth)
			continue;
		if (*in == ' ' && (o == 0 || out[o - 1] == ' '))
			continue;
		out[o++] = *in;
	}
	while (o > 0 && out[o - 1] == ' ')
		o--;
	out[o] = '\0';
}

/*
 * OpenRetro's search misses some full titles that it finds by a part of
 * them, so there are up to three tries: the whole title, its first two
 * words, and its longest word. The results are checked against the full
 * title either way. Returns FALSE when there is no other query to try.
 */
static BOOL nextSearchQuery(const char *title, int attempt, char *out, int outSize)
{
	char bare[MAX_SLAVE_TITLE_SIZE];
	const char *p;
	int words = 0;

	searchQuery(title, bare, sizeof(bare));
	for (p = bare; *p; p++)
		if (*p == ' ')
			words++;
	words++;

	if (attempt == 0)
	{
		strlcpy(out, bare, outSize);
		return out[0] != '\0';
	}

	if (attempt == 1)
	{
		/* First two words, when there are more than two */
		const char *second;
		if (words <= 2)
			return FALSE;
		second = strchr(bare, ' ');
		second = second ? strchr(second + 1, ' ') : NULL;
		if (!second)
			return FALSE;
		strlcpy(out, bare, outSize);
		if (second - bare < outSize)
			out[second - bare] = '\0';
		return TRUE;
	}

	if (attempt == 2)
	{
		/* The longest word, when it says enough on its own */
		const char *best = NULL;
		int bestLen = 0;
		const char *w = bare;

		if (words <= 1)
			return FALSE;
		while (*w)
		{
			int len = 0;
			while (w[len] && w[len] != ' ')
				len++;
			if (len > bestLen)
			{
				best = w;
				bestLen = len;
			}
			w += len;
			while (*w == ' ')
				w++;
		}
		if (!best || bestLen < 5 || bestLen >= outSize)
			return FALSE;
		memcpy(out, best, bestLen);
		out[bestLen] = '\0';
		return TRUE;
	}

	return FALSE;
}

/*
 * Picks the search result that matches the title. Results look like
 *   <a href="/amiga/1869-aga" ...><img ... alt="Cover for 1869 [AGA]" ...>
 * or, for games without a short name, href="/game/<uuid>".
 * An exact match wins, then a match ignoring the bracketed parts, then
 * the shortest result that starts with the title. Anything looser would
 * show the wrong game too often. Between equal matches, the one that
 * agrees on being the AGA version wins.
 *
 * path receives the page's address without the host, i.e. "amiga/menace".
 */
static BOOL pickSearchResult(const char *page, const char *title, char *path, int pathSize)
{
	char want[DETAILS_SLUG_SIZE], wantBare[DETAILS_SLUG_SIZE];
	char name[DETAILS_FIELD_SIZE], have[DETAILS_SLUG_SIZE];
	char candidate[DETAILS_SLUG_SIZE];
	const BOOL wantAga = isAgaTitle(title);
	int bestScore = 0;
	int bestLen = 0;
	const char *p = page;

	normalizeTitle(skipArticle(title), want, sizeof(want), FALSE);
	normalizeTitle(skipArticle(title), wantBare, sizeof(wantBare), TRUE);
	if (!wantBare[0])
		return FALSE;

	path[0] = '\0';

	while ((p = strstr(p, "alt=\"Cover for ")) != NULL)
	{
		const char *alt = p + 15;
		const char *altEnd = strchr(alt, '"');
		const char *href = NULL;
		const char *s, *q;
		int len, rank = 0, score;

		p = alt;
		if (!altEnd)
			break;

		/* The link around this cover image */
		for (s = p; s > page && s > p - 400; s--)
		{
			if (!strncmp(s, "href=\"/", 7))
			{
				href = s + 7;
				break;
			}
		}
		if (!href)
			continue;
		q = strchr(href, '"');
		if (!q || q == href || q - href >= (int)sizeof(candidate))
			continue;
		len = q - href;
		memcpy(candidate, href, len);
		candidate[len] = '\0';

		/* "amiga/<name>" or "game/<uuid>", nothing deeper */
		if ((strncmp(candidate, "amiga/", 6) && strncmp(candidate, "game/", 5)) ||
			strchr(strchr(candidate, '/') + 1, '/'))
			continue;

		htmlToText(alt, altEnd, name, sizeof(name));

		normalizeTitle(skipArticle(name), have, sizeof(have), FALSE);
		if (!strcmp(have, want))
			rank = 3;
		else
		{
			normalizeTitle(skipArticle(name), have, sizeof(have), TRUE);
			if (!strcmp(have, wantBare))
				rank = 2;
			else if (!strncmp(have, wantBare, strlen(wantBare)))
				rank = 1;
		}
		if (rank == 0)
			continue;

		score = rank * 2 + (isAgaTitle(name) == wantAga ? 1 : 0);
		if (score > bestScore || (score == bestScore && (int)strlen(have) < bestLen))
		{
			bestScore = score;
			bestLen = strlen(have);
			strlcpy(path, candidate, pathSize);
		}
	}

	return bestScore > 0;
}

/* ---------------------------------------------------------------------
 * Parsing the OpenRetro pages
 * ------------------------------------------------------------------- */

typedef struct
{
	char name[DETAILS_FIELD_SIZE];
	char developer[DETAILS_FIELD_SIZE];
	char publisher[DETAILS_FIELD_SIZE];
	char year[16];
	char players[32];
	char languages[DETAILS_FIELD_SIZE];
	char tags[DETAILS_FIELD_SIZE];
	char userScore[16];
	char hol[16];
	char lemon[16];
	char shots[DETAILS_MAX_SHOTS * DETAILS_HASH_SIZE];  /* hashes, comma separated */
} gameInfo;

/* <div class="game-info-row"><span>Publisher:</span>Psyclapse</div> */
static void infoRow(const char *page, const char *label, char *out, int outSize)
{
	char needle[32];
	snprintf(needle, sizeof(needle), ">%s:</span>", label);
	extractText(page, needle, "</div>", out, outSize);
}

static void digitsAfter(const char *page, const char *needle, char *out, int outSize)
{
	const char *p = strstr(page, needle);
	int o = 0;

	if (p)
	{
		p += strlen(needle);
		while (isdigit((unsigned char)*p) && o < outSize - 1)
			out[o++] = *p++;
	}
	out[o] = '\0';
}

/*
 * The header shows the title screen first, then the in-game screenshots:
 *   <img src="/image/<sha1>?w=332&amp;h=208" alt="" class=feature-img ...>
 * Only their names are collected here, as "hash,hash,...". The pictures
 * themselves are downloaded one at a time, when they are shown.
 */
static void findScreenshots(const char *page, char *out, int outSize)
{
	const char *p = page;
	int found = 0;
	int o = 0;

	out[0] = '\0';
	while (found < DETAILS_MAX_SHOTS && (p = strstr(p, "class=feature-img")) != NULL)
	{
		const char *tagStart = p;
		const char *img;

		while (tagStart > page && strncmp(tagStart, "<img", 4))
			tagStart--;
		img = strstr(tagStart, "/image/");
		if (img && img < p && o < outSize - DETAILS_HASH_SIZE - 1)
		{
			img += 7;
			if (found > 0)
				out[o++] = ',';
			while (isxdigit((unsigned char)*img) && o < outSize - 1)
				out[o++] = *img++;
			out[o] = '\0';
			found++;
		}
		p += 17;
	}
}

static void parseGamePage(const char *page, gameInfo *info)
{
	memset(info, 0, sizeof(*info));

	extractText(page, "<h1 id='fs-title'>", "<span", info->name, sizeof(info->name));
	infoRow(page, "Developer", info->developer, sizeof(info->developer));
	infoRow(page, "Publisher", info->publisher, sizeof(info->publisher));
	infoRow(page, "Year", info->year, sizeof(info->year));
	infoRow(page, "Players", info->players, sizeof(info->players));
	infoRow(page, "Languages", info->languages, sizeof(info->languages));
	infoRow(page, "Tags", info->tags, sizeof(info->tags));
	extractText(page, "<div class='score'>Score:", "</div>", info->userScore, sizeof(info->userScore));
	digitsAfter(page, "hol.abime.net/", info->hol, sizeof(info->hol));
	digitsAfter(page, "lemonamiga.com/?game_id=", info->lemon, sizeof(info->lemon));
	findScreenshots(page, info->shots, sizeof(info->shots));
}

static void addHeading(textBuf *t, ULONG msg)
{
	if (t->len > 0)
		tbAdd(t, "\n");
	tbAddf(t, "\33b%s\n", GetMBString(msg));
}

static void addField(textBuf *t, ULONG msg, const char *value)
{
	if (value[0])
		tbAddf(t, "%s: %s\n", GetMBString(msg), value);
}

static void buildMainText(const char *page, const gameInfo *info, const char *title,
	const char *pagePath, textBuf *t)
{
	char *text = malloc(DETAILS_TEXT_SIZE);
	char want[DETAILS_SLUG_SIZE], have[DETAILS_SLUG_SIZE];

	/* Show OpenRetro's name when it differs, so a wrong match is obvious */
	normalizeTitle(title, want, sizeof(want), TRUE);
	normalizeTitle(info->name, have, sizeof(have), TRUE);
	if (info->name[0] && strcmp(want, have))
		tbAddf(t, "\33b%s\n", info->name);

	if (info->developer[0] || info->publisher[0])
	{
		addHeading(t, MSG_DetailsCredits);
		addField(t, MSG_DetailsDeveloper, info->developer);
		addField(t, MSG_DetailsPublisher, info->publisher);
	}

	if (info->year[0] || info->players[0] || info->languages[0])
	{
		addHeading(t, MSG_DetailsInformation);
		addField(t, MSG_DetailsYear, info->year);
		addField(t, MSG_DetailsPlayers, info->players);
		addField(t, MSG_DetailsLanguages, info->languages);
	}

	if (info->tags[0])
	{
		addHeading(t, MSG_DetailsCategories);
		tbAddf(t, "%s\n", info->tags);
	}

	if (text)
	{
		BOOL heading = FALSE;

		/* A short description, then the long one. Both are optional */
		if (extractText(page, "<div class='game-description'>", "</div>", text, DETAILS_TEXT_SIZE))
		{
			addHeading(t, MSG_DetailsDescription);
			heading = TRUE;
			tbAdd(t, text);
			tbAdd(t, "\n");
		}
		if (extractText(page, "<div class='game-about'>", "</div>", text, DETAILS_TEXT_SIZE))
		{
			if (!heading)
				addHeading(t, MSG_DetailsDescription);
			else
				tbAdd(t, "\n");
			tbAdd(t, text);
			tbAdd(t, "\n");
		}
		free(text);
	}

	addHeading(t, MSG_DetailsLinks);
	tbAddf(t, "OpenRetro: openretro.org/%s\n", pagePath);
	if (info->hol[0])
		tbAddf(t, "Hall of Light: amiga.abime.net/%s\n", info->hol);
	if (info->lemon[0])
		tbAddf(t, "Lemon Amiga: lemonamiga.com/games/details.php?id=%s\n", info->lemon);
}

/* <span class='score'>95%</span>\n<span class='publication'>CU Amiga</span> */
static void addReviews(const char *page, textBuf *t)
{
	const char *p = page;
	int count = 0;

	while (count < DETAILS_MAX_REVIEWS && (p = strstr(p, "<span class='score'>")) != NULL)
	{
		char score[16], publication[DETAILS_FIELD_SIZE];

		if (!extractText(p, "<span class='score'>", "</span>", score, sizeof(score)) ||
			!extractText(p, "<span class='publication'>", "</span>", publication, sizeof(publication)))
			break;

		if (count == 0)
			addHeading(t, MSG_DetailsReviews);
		tbAddf(t, "%s: %s\n", publication, score);
		count++;
		p += 20;
	}
}

/*
 * <div id="comment-5145" class="comment"><div class="comment-header">Posted by
 * <span class='comment-user'><a href='/users/x'>x</a></span> at
 * <span class='comment-created'>2022-10-10 02:23:15</span>:</div>
 * <p>text</p><hr></div>
 */
static void addComments(const char *page, textBuf *t)
{
	char *body = malloc(DETAILS_TEXT_SIZE);
	const char *p = page;
	int count = 0;

	if (!body)
		return;

	/* Count first, for the heading */
	while ((p = strstr(p, "class=\"comment\"")) != NULL)
	{
		count++;
		p += 15;
	}

	if (t->len > 0)
		tbAdd(t, "\n");
	tbAdd(t, "\33b");
	tbAddf(t, GetMBString(MSG_DetailsComments), (long)count);
	tbAdd(t, "\n");

	if (count == 0)
	{
		tbAddf(t, "%s\n", GetMBString(MSG_DetailsNoComments));
		free(body);
		return;
	}

	p = page;
	count = 0;
	while (count < DETAILS_MAX_COMMENTS && (p = strstr(p, "class=\"comment\"")) != NULL)
	{
		char user[64], created[32];
		const char *header = strstr(p, ":</div>");
		const char *end;

		p += 15;
		if (!header)
			break;

		extractText(p, "class='comment-user'>", "</span>", user, sizeof(user));
		extractText(p, "class='comment-created'>", "</span>", created, sizeof(created));
		created[16] = '\0'; /* drop the seconds */

		header += 7;
		end = strstr(header, "<hr>");
		if (!end)
			end = strstr(header, "class=\"comment\"");
		if (!end)
			end = header + strlen(header);
		htmlToText(header, end, body, DETAILS_TEXT_SIZE);

		tbAddf(t, "\n\33i%s, %s\n", user[0] ? user : "?", created);
		tbAdd(t, body);
		tbAdd(t, "\n");
		count++;
	}

	free(body);
}

static void buildCommunityText(const char *page, const char *discuss, const gameInfo *info, textBuf *t)
{
	addHeading(t, MSG_DetailsUserScore);
	if (isdigit((unsigned char)info->userScore[0]))  /* "N/A" without votes */
		tbAddf(t, "%s / 10\n", info->userScore);
	else
		tbAddf(t, "%s\n", GetMBString(MSG_DetailsNoScore));

	addReviews(page, t);

	if (discuss)
		addComments(discuss, t);
}

/* ---------------------------------------------------------------------
 * Cache in PROGDIR:cache
 * ------------------------------------------------------------------- */

/*
 * "PROGDIR:cache/1869aga_1a2b3c4d.txt" for the text and
 * "PROGDIR:cache/1869aga_1a2b3c4d.2.png" for the screenshots.
 * Names stay within the 30 characters of the Amiga file systems.
 */
static void cachePath(const char *title, const char *ext, char *out, int outSize)
{
	char norm[DETAILS_SLUG_SIZE];
	ULONG hash = 5381;
	const char *p;

	normalizeTitle(title, norm, sizeof(norm), FALSE);
	for (p = norm; *p; p++)
		hash = hash * 33 + (unsigned char)*p;
	norm[14] = '\0';

	snprintf(out, outSize, "%s/%s_%08lx.%s", DEFAULT_DETAILS_CACHE_DIR,
		norm[0] ? norm : "game", (unsigned long)hash, ext);
}

static BOOL ensureCacheDir(void)
{
	BPTR lock = Lock((STRPTR)DEFAULT_DETAILS_CACHE_DIR, SHARED_LOCK);
	if (!lock)
		lock = CreateDir((STRPTR)DEFAULT_DETAILS_CACHE_DIR);
	if (!lock)
		return FALSE;
	UnLock(lock);
	return TRUE;
}

/*
 * Cache file layout:
 *   IGD2\n<page path>\n<screenshot hashes>\n<flags>\n<main text>\f<community text>
 * flags is "c" when the community text includes the comments, which are
 * only loaded when the Community tab is opened, or "-" otherwise.
 * An empty path means the game isn't on OpenRetro.
 * Older IGD1 files have no flags line and always include the comments.
 */
static void cacheWrite(const char *title, const char *pagePath, const char *shots,
	BOOL withComments, const char *mainText, const char *communityText)
{
	char path[MAX_PATH_SIZE];
	BPTR fh;

	if (!ensureCacheDir())
		return;

	cachePath(title, "txt", path, sizeof(path));
	fh = Open((STRPTR)path, MODE_NEWFILE);
	if (!fh)
		return;

	FPrintf(fh, "%s\n%s\n%s\n%s\n", (ULONG)DETAILS_CACHE_MAGIC, (ULONG)pagePath, (ULONG)shots,
		(ULONG)(withComments ? "c" : "-"));
	Write(fh, (APTR)mainText, strlen(mainText));
	FPutC(fh, '\f');
	Write(fh, (APTR)communityText, strlen(communityText));
	Close(fh);
}

typedef struct
{
	char *data;        /* the whole file, the strings below point into it */
	char *pagePath;
	char *shots;
	BOOL withComments;
	char *mainText;
	char *communityText;
} cacheEntry;

static BOOL cacheRead(const char *title, cacheEntry *entry)
{
	char path[MAX_PATH_SIZE];
	textBuf file = { NULL, 0, 0 };
	char buf[1024];
	char *p, *sep;
	BPTR fh;
	LONG n;

	memset(entry, 0, sizeof(*entry));
	cachePath(title, "txt", path, sizeof(path));

	fh = Open((STRPTR)path, MODE_OLDFILE);
	if (!fh)
		return FALSE;

	while ((n = Read(fh, buf, sizeof(buf))) > 0)
	{
		if (!pageSink(buf, n, &file))
			break;
	}
	Close(fh);

	if (!file.buf ||
		(strncmp(file.buf, DETAILS_CACHE_MAGIC "\n", 5) && strncmp(file.buf, DETAILS_CACHE_MAGIC1 "\n", 5)))
	{
		tbFree(&file);
		return FALSE;
	}

	p = file.buf + 5;
	entry->pagePath = p;
	if (!(p = strchr(p, '\n'))) goto broken;
	*p++ = '\0';
	entry->shots = p;
	if (!(p = strchr(p, '\n'))) goto broken;
	*p++ = '\0';
	entry->withComments = TRUE;
	if (!strncmp(file.buf, DETAILS_CACHE_MAGIC, 4))
	{
		entry->withComments = (*p == 'c');
		if (!(p = strchr(p, '\n'))) goto broken;
		p++;
	}
	entry->mainText = p;
	if (!(sep = strchr(p, '\f'))) goto broken;
	*sep = '\0';
	entry->communityText = sep + 1;
	entry->data = file.buf;
	return TRUE;

broken:
	tbFree(&file);
	memset(entry, 0, sizeof(*entry));
	return FALSE;
}

/* ---------------------------------------------------------------------
 * Loading, one game at a time
 * ------------------------------------------------------------------- */

static struct
{
	char display[MAX_SLAVE_TITLE_SIZE];
	char title[MAX_SLAVE_TITLE_SIZE];
	BOOL screenshot;
	ULONG serial;      /* bumped for every newly selected game */
} wanted;

static ULONG loadingSerial = 0;
static BOOL busy = FALSE;
static char shownTitle[MAX_SLAVE_TITLE_SIZE];  /* fully loaded game */
static BOOL userCancelled = FALSE;              /* Cancel in the progress window */

static BOOL superseded(void)
{
	return loadingSerial != wanted.serial || userCancelled;
}

/*
 * Progress goes to the status field at the bottom of the main window.
 * Looking up a game takes up to three steps: the page, the comments and
 * the screenshot. The bar moves through them, since OpenRetro's pages
 * come without a size to measure them by. Selecting another game stops
 * a lookup; there is no separate Cancel.
 */
static void setTexts(const char *mainText, const char *communityText);

static int netStep = 0;
static int netSteps = 1;
static BOOL netShown = FALSE;
static char netLabel[96];

static BOOL netActive(void)
{
	return netShown;
}

static void netGauge(ULONG received)
{
	char info[sizeof(netLabel) + 24];

	if (received)
		snprintf(info, sizeof(info), "%s (%lu KB)", netLabel, (unsigned long)(received >> 10));
	else
		strlcpy(info, netLabel, sizeof(info));

	setStatusProgress(info, (netStep * 100) / netSteps);
}

static void netProgressf(int step, int steps, ULONG msg, ...)
{
	va_list ap;

	va_start(ap, msg);
	vsnprintf(netLabel, sizeof(netLabel), GetMBString(msg), ap);
	va_end(ap);

	netStep = step;
	netSteps = steps > 0 ? steps : 1;
	netShown = TRUE;
	netGauge(0);
}

static void netDone(void)
{
	if (netShown)
		setStatusText("");
	netShown = FALSE;
}

/* Keeps the GUI alive during downloads and shows how far they are */
static BOOL netPump(ULONG received, ULONG total)
{
	if (netShown)
		netGauge(received);

	DoMethod(app->App, MUIM_Application_InputBuffered);
	return !userCancelled;
}

static BOOL detailsProgress(ULONG received, ULONG total, void *userData)
{
	return netPump(received, total) && !superseded();
}

/* Which class the two text areas are, see newTextArea() */
static BOOL useNFloattext = FALSE;

/* The text each area shows. Kept here because a text class may display
   the string it was given rather than a copy of it */
static char *shownText[2];

static void setTextArea(APTR area, int index, const char *text)
{
	char *copy = strdup(text ? text : "");

	set(area, useNFloattext ? MUIA_NFloattext_Text : MUIA_Floattext_Text, copy ? copy : "");
	if (shownText[index])
		free(shownText[index]);
	shownText[index] = copy;
}

static void setTexts(const char *mainText, const char *communityText)
{
	setTextArea(app->FT_DetailsMain, 0, mainText);
	setTextArea(app->FT_DetailsCommunity, 1, communityText);
}


static void showScreenshotFile(const char *path)
{
	if (current_settings->hide_screenshots || !app->GR_spacedScreenshot)
		return;
	show_screenshot((STRPTR)path);
}

static void showFailure(LONG err)
{
	char *msg = malloc(512);
	if (msg)
	{
		snprintf(msg, 512, GetMBString(MSG_DetailsFailed), httpErrorString(err));
		setTexts(msg, msg);
		free(msg);
	}
}

/* Downloads a page into t. Returns HTTP_OK or an error code */
static LONG fetchPage(const char *url, textBuf *t, LONG *status)
{
	t->len = 0;
	if (t->buf)
		t->buf[0] = '\0';
	return httpGet(url, pageSink, detailsProgress, t, status);
}

/* Waits a moment, giving the user the chance to move on first */
static BOOL settle(void)
{
	int i;
	for (i = 0; i < DETAILS_SETTLE_TICKS; i++)
	{
		Delay(5);
		DoMethod(app->App, MUIM_Application_InputBuffered);
		if (superseded())
			return FALSE;
	}
	return TRUE;
}

/* ---------------------------------------------------------------------
 * Screenshots: the arrows next to the picture browse through them. Each
 * one is downloaded the first time it is shown and then kept in the cache
 * ------------------------------------------------------------------- */

static struct
{
	char hash[DETAILS_MAX_SHOTS][DETAILS_HASH_SIZE];
	int count;
	int index;            /* the one to show */
	int loading;          /* the one being downloaded, or -1 */
	char title[MAX_SLAVE_TITLE_SIZE];
	ULONG serial;         /* the selection they belong to */
	BOOL running;
} shots;

static void showShotButtons(BOOL show)
{
	if (app->GR_DetailsShotPrev)
	{
		set(app->GR_DetailsShotPrev, MUIA_ShowMe, show);
		set(app->GR_DetailsShotNext, MUIA_ShowMe, show);
	}
}

/* Forget the screenshots, i.e. when another game gets selected */
static void shotsForget(void)
{
	shots.count = 0;
	showShotButtons(FALSE);
}

/* Takes the screenshot list of a game: "hash,hash,..." */
static void shotsSet(const char *title, const char *list)
{
	shotsForget();
	while (*list && shots.count < DETAILS_MAX_SHOTS)
	{
		int o = 0;
		while (*list && *list != ',' && o < DETAILS_HASH_SIZE - 1)
			shots.hash[shots.count][o++] = *list++;
		shots.hash[shots.count][o] = '\0';
		if (o > 0)
			shots.count++;
		while (*list && *list != ',')
			list++;
		if (*list == ',')
			list++;
	}

	/* The first one is the title screen. Start with the game itself */
	shots.index = shots.count > 1 ? 1 : 0;
	strlcpy(shots.title, title, sizeof(shots.title));
	shots.serial = wanted.serial;
}

/*
 * Whether a downloaded screenshot is larger than the screenshot size in
 * the settings. If so, fitW and fitH get the largest size that fits with
 * the same proportions. The size is read from the PNG header.
 */
static BOOL shotTooBig(const char *path, LONG *fitW, LONG *fitH)
{
	UBYTE head[24];
	LONG w, h, boxW, boxH;
	BOOL ok = FALSE;
	BPTR fh = Open((STRPTR)path, MODE_OLDFILE);

	if (fh)
	{
		ok = Read(fh, head, sizeof(head)) == sizeof(head);
		Close(fh);
	}
	if (!ok || memcmp(head, "\x89PNG", 4) || memcmp(head + 12, "IHDR", 4))
		return FALSE;

	w = ((LONG)head[16] << 24) | ((LONG)head[17] << 16) | ((LONG)head[18] << 8) | head[19];
	h = ((LONG)head[20] << 24) | ((LONG)head[21] << 16) | ((LONG)head[22] << 8) | head[23];
	boxW = current_settings->screenshot_width > 0 ? current_settings->screenshot_width : 160;
	boxH = current_settings->screenshot_height > 0 ? current_settings->screenshot_height : 128;

	if (w <= 0 || h <= 0 || (w <= boxW && h <= boxH))
		return FALSE;

	if (w * boxH > h * boxW)
	{
		*fitW = boxW;
		*fitH = h * boxW / w;
	}
	else
	{
		*fitH = boxH;
		*fitW = w * boxH / h;
	}
	if (*fitW < 1) *fitW = 1;
	if (*fitH < 1) *fitH = 1;
	return TRUE;
}

static void shotPath(int index, char *out, int outSize)
{
	char ext[8];
	snprintf(ext, sizeof(ext), "%d.png", index + 1);
	cachePath(shots.title, ext, out, outSize);
}

/* Stops a download as soon as the user wants another picture or game */
static BOOL shotProgress(ULONG received, ULONG total, void *userData)
{
	return netPump(received, total) &&
		wanted.serial == shots.serial && shots.index == shots.loading;
}

/*
 * Shows screenshot shots.index, downloading it if it isn't cached yet.
 * The arrows may be pressed again during the download; this keeps going
 * until the picture shown is the one asked for.
 */
static void shotsShow(void)
{
	const ULONG serial = shots.serial;
	const BOOL wasRunning = shots.running;
	BOOL openedProgress = FALSE;

	if (shots.count == 0 || current_settings->hide_screenshots || !app->GR_spacedScreenshot)
		return;

	showShotButtons(shots.count > 1);
	shots.running = TRUE;

	do
	{
		const int index = shots.loading = shots.index;
		char path[MAX_PATH_SIZE];

		LONG fitW = 0, fitH = 0;
		BOOL have, tooBig = FALSE;

		shotPath(index, path, sizeof(path));
		have = check_path_exists(path);
		if (have)
			tooBig = shotTooBig(path, &fitW, &fitH);

		if (!have || tooBig)
		{
			char url[160];
			LONG status;
			LONG result = HTTP_OK;

			if (!ensureCacheDir())
				break;

			/* Part of a game's lookup, or on its own from the arrows */
			if (!netActive())
			{
				openedProgress = TRUE;
				userCancelled = FALSE;
			}
			netProgressf(openedProgress ? 0 : netSteps - 1, openedProgress ? 1 : netSteps,
				MSG_ProgressScreenshot, (long)(index + 1), (long)shots.count);

			/* The original size (s=1x) is usually a small palette PNG */
			if (!have)
			{
				snprintf(url, sizeof(url), "%s/image/%s?s=1x", DEFAULT_OPENRETRO_URL, shots.hash[index]);
				result = httpGetToFile(url, path, shotProgress, NULL, &status);
				if (result == HTTP_OK)
					tooBig = shotTooBig(path, &fitW, &fitH);
			}

			/* Too big for the pane: let OpenRetro scale it, which is much
			   faster than doing it here. It scales to exactly the size
			   asked for, so the proportions were worked out above */
			if (result == HTTP_OK && tooBig)
			{
				snprintf(url, sizeof(url), "%s/image/%s?w=%ld&h=%ld", DEFAULT_OPENRETRO_URL,
					shots.hash[index], (long)fitW, (long)fitH);
				result = httpGetToFile(url, path, shotProgress, NULL, &status);
			}

			if (result != HTTP_OK)
			{
				if (!userCancelled && wanted.serial == serial && shots.index != index)
					continue; /* another one was asked for meanwhile */
				break;
			}
		}

		if (wanted.serial != serial)
			break;
		if (shots.index == index)
			showScreenshotFile(path);
	}
	while (wanted.serial == serial && shots.index != shots.loading);

	shots.loading = -1;
	shots.running = wasRunning;

	if (openedProgress)
		netDone();
}

static void commentsCatchUp(void);

static void shotsStep(int step)
{
	if (shots.count < 2 || wanted.serial != shots.serial)
		return;

	shots.index = (shots.index + step + shots.count) % shots.count;

	/* A download is running already; it picks up the new index */
	if (shots.running)
		return;

	shotsShow();
	userCancelled = FALSE;
	commentsCatchUp();
}

static void shotPrevFunc(void) { shotsStep(-1); }
static void shotNextFunc(void) { shotsStep(1); }
MakeStaticHook(ShotPrevHook, shotPrevFunc);
MakeStaticHook(ShotNextHook, shotNextFunc);

/* ---------------------------------------------------------------------
 * Comments: only fetched when the Community tab is opened, which saves a
 * request for every game that is only looked at on the Main tab
 * ------------------------------------------------------------------- */

static struct
{
	char title[MAX_SLAVE_TITLE_SIZE];
	char pagePath[DETAILS_SLUG_SIZE];
	char *baseText;       /* the community text without the comments */
	BOOL loaded;
	BOOL running;
	ULONG serial;         /* the selection they belong to */
} comments;

static BOOL communityShown(void)
{
	ULONG page = 0;
	if (app->GR_DetailsPages)
		get(app->GR_DetailsPages, MUIA_Group_ActivePage, &page);
	return page == 1;
}

static void commentsForget(void)
{
	if (comments.baseText)
		free(comments.baseText);
	comments.baseText = NULL;
	comments.pagePath[0] = '\0';
	comments.loaded = FALSE;
}

/* The game shown now; loaded tells whether its text has the comments */
static void commentsSet(const char *title, const char *pagePath, const char *communityText, BOOL loaded)
{
	commentsForget();
	strlcpy(comments.title, title, sizeof(comments.title));
	strlcpy(comments.pagePath, pagePath, sizeof(comments.pagePath));
	comments.loaded = loaded;
	comments.serial = wanted.serial;
	if (!loaded)
		comments.baseText = strdup(communityText);
}

static BOOL commentsProgress(ULONG received, ULONG total, void *userData)
{
	return netPump(received, total) && wanted.serial == comments.serial;
}

/* Adds the comments to the cached text, so they are fetched only once */
static void cacheAddComments(const char *title, const char *communityText)
{
	cacheEntry entry;

	if (!cacheRead(title, &entry))
		return;
	cacheWrite(title, entry.pagePath, entry.shots, TRUE, entry.mainText, communityText);
	free(entry.data);
}

/* Fetches and shows the comments of the game shown, if not done yet */
static void commentsFetch(void)
{
	const ULONG serial = comments.serial;
	textBuf discuss = { NULL, 0, 0 };
	textBuf text;
	char url[DETAILS_SLUG_SIZE + 64];
	LONG status;
	BOOL openedProgress = FALSE;

	if (comments.loaded || comments.running || !comments.pagePath[0] ||
		!comments.baseText || wanted.serial != serial)
		return;

	comments.running = TRUE;

	/* Part of a game's lookup, or on its own after opening the tab */
	if (!netActive())
	{
		openedProgress = TRUE;
		netProgressf(0, 1, MSG_ProgressComments);
	}
	else
		netProgressf(1, netSteps, MSG_ProgressComments);

	snprintf(url, sizeof(url), "%s/%s/discuss", DEFAULT_OPENRETRO_URL, comments.pagePath);
	if (httpGet(url, pageSink, commentsProgress, &discuss, &status) == HTTP_OK &&
		wanted.serial == serial && tbInit(&text, DETAILS_TEXT_SIZE))
	{
		tbAdd(&text, comments.baseText);
		addComments(discuss.buf, &text);
		setTextArea(app->FT_DetailsCommunity, 1, text.buf);
		cacheAddComments(comments.title, text.buf);
		comments.loaded = TRUE;
		tbFree(&text);
	}
	tbFree(&discuss);

	if (openedProgress)
		netDone();
	comments.running = FALSE;
}

/* After a load or screenshot download ends: the tab may have been opened
   meanwhile. Fetching right then would mix with the running download */
static void commentsCatchUp(void)
{
	if (!busy && !shots.running && communityShown())
		commentsFetch();
}

static void communityTabFunc(void) { commentsCatchUp(); }
MakeStaticHook(CommunityTabHook, communityTabFunc);

/* Shows a game from the cache. FALSE when it isn't cached yet */
static BOOL showCached(const char *title, BOOL screenshot)
{
	cacheEntry entry;

	if (!cacheRead(title, &entry))
		return FALSE;

	if (entry.pagePath[0])
	{
		setTexts(entry.mainText, entry.communityText);
		commentsSet(title, entry.pagePath, entry.communityText, entry.withComments);
		if (communityShown())
			commentsFetch();
		if (screenshot)
		{
			shotsSet(title, entry.shots);
			shotsShow();
		}
	}
	else
	{
		setTexts(GetMBString(MSG_DetailsNotFound), GetMBString(MSG_DetailsNotFound));
	}

	free(entry.data);
	return TRUE;
}

static void loadDetails(const char *title, BOOL screenshot)
{
	textBuf page = { NULL, 0, 0 };
	textBuf mainText, communityText;
	char path[DETAILS_SLUG_SIZE];  /* "amiga/menace" or "game/<uuid>" */
	char slug[DETAILS_SLUG_SIZE];
	char url[DETAILS_SLUG_SIZE * 3 + 64];
	char query[MAX_SLAVE_TITLE_SIZE];
	char encoded[MAX_SLAVE_TITLE_SIZE * 3];
	gameInfo *info = NULL;
	LONG status = 0;
	LONG err;
	int attempt;

	if (showCached(title, screenshot))
	{
		if (!superseded())
			strlcpy(shownTitle, title, sizeof(shownTitle));
		return;
	}

	if (!settle())
		return;

	/* Steps: the page, the comments when their tab is open, the screenshot */
	netProgressf(0, communityShown() ? 3 : 2, MSG_ProgressLookup);

	/* Most games are found under a name made from their title */
	slugFromTitle(title, slug, sizeof(slug));
	path[0] = '\0';
	err = HTTP_ERR_STATUS;
	if (slug[0])
	{
		snprintf(path, sizeof(path), "amiga/%s", slug);
		snprintf(url, sizeof(url), "%s/%s", DEFAULT_OPENRETRO_URL, path);
		err = fetchPage(url, &page, &status);
	}
	if (superseded())
		goto done;

	/* Otherwise search for it */
	if (err == HTTP_ERR_STATUS && (status == 404 || status == 0))
	{
		BOOL found = FALSE;

		for (attempt = 0; !found && nextSearchQuery(title, attempt, query, sizeof(query)); attempt++)
		{
			httpEncodePath(query, encoded, sizeof(encoded));
			snprintf(url, sizeof(url), "%s/browse/amiga?q=%s", DEFAULT_OPENRETRO_URL, encoded);
			err = fetchPage(url, &page, &status);
			if (superseded())
				goto done;
			if (err != HTTP_OK)
				break;
			found = pickSearchResult(page.buf, title, path, sizeof(path));
		}

		if (err == HTTP_OK && !found)
		{
			/* Remember that it isn't there, to not search again */
			cacheWrite(title, "", "", TRUE, "", "");
			setTexts(GetMBString(MSG_DetailsNotFound), GetMBString(MSG_DetailsNotFound));
			strlcpy(shownTitle, title, sizeof(shownTitle));
			goto done;
		}

		if (found)
		{
			snprintf(url, sizeof(url), "%s/%s", DEFAULT_OPENRETRO_URL, path);
			err = fetchPage(url, &page, &status);
			if (superseded())
				goto done;
		}
	}

	if (err != HTTP_OK)
	{
		if (err != HTTP_ERR_ABORT)
			showFailure(err);
		goto done;
	}

	info = malloc(sizeof(gameInfo));
	if (!info || !tbInit(&mainText, DETAILS_TEXT_SIZE))
		goto done;
	if (!tbInit(&communityText, DETAILS_TEXT_SIZE))
	{
		tbFree(&mainText);
		goto done;
	}

	parseGamePage(page.buf, info);
	buildMainText(page.buf, info, title, path, &mainText);
	buildCommunityText(page.buf, NULL, info, &communityText);  /* comments come later */

	cacheWrite(title, path, info->shots, FALSE, mainText.buf, communityText.buf);
	setTexts(mainText.buf, communityText.buf);
	strlcpy(shownTitle, title, sizeof(shownTitle));
	commentsSet(title, path, communityText.buf, FALSE);

	tbFree(&mainText);
	tbFree(&communityText);

	if (communityShown())
	{
		commentsFetch();
		if (superseded())
			goto done;
	}

	if (screenshot)
	{
		shotsSet(title, info->shots);
		shotsShow();
	}

done:
	if (info)
		free(info);
	tbFree(&page);
}

void gameDetailsShow(const char *displayTitle, const char *title, BOOL fetchShot)
{
	if (!app->GR_Details || !title)
		return;

	/* Clicking the game that is already loading changes nothing */
	if (busy && !strcmp(title, wanted.title))
		return;

	/* Nor does clicking the one that is shown, but the caller may have
	   put the default image back in the meantime */
	if (!busy && !strcmp(title, shownTitle))
	{
		if (fetchShot)
			shotsShow();
		return;
	}

	strlcpy(wanted.display, displayTitle ? displayTitle : title, sizeof(wanted.display));
	strlcpy(wanted.title, title, sizeof(wanted.title));
	wanted.screenshot = fetchShot;
	wanted.serial++;
	shownTitle[0] = '\0';
	userCancelled = FALSE;
	shotsForget();
	commentsForget();

	/* Clear the old game's details right away, even when a load is still
	   running; that load notices the new selection and starts over */
	set(app->TX_DetailsTitle, MUIA_Text_Contents, wanted.display);
	setTexts("", "");  /* the status field says it is loading */

	if (busy)
		return;

	busy = TRUE;
	do
	{
		char current[MAX_SLAVE_TITLE_SIZE];
		BOOL shot = wanted.screenshot;

		loadingSerial = wanted.serial;
		strlcpy(current, wanted.title, sizeof(current));
		loadDetails(current, shot);
	}
	/* Start over with the newly selected game, unless the pane was cleared
	   or the user cancelled */
	while (!userCancelled && superseded() && wanted.title[0]);
	busy = FALSE;

	netDone();
	commentsCatchUp();

	/* Cancelled before the details were there: say so instead of "Loading" */
	if (userCancelled && !shownTitle[0] && wanted.title[0])
		showFailure(HTTP_ERR_ABORT);
	userCancelled = FALSE;
}

void gameDetailsClear(void)
{
	if (!app->GR_Details)
		return;

	/* Stops a running load too */
	wanted.title[0] = '\0';
	wanted.serial++;
	shownTitle[0] = '\0';
	shotsForget();
	commentsForget();

	set(app->TX_DetailsTitle, MUIA_Text_Contents, "");
	setTexts(GetMBString(MSG_DetailsSelectGame), "");
}

/* ---------------------------------------------------------------------
 * GUI
 * ------------------------------------------------------------------- */

static CONST_STRPTR detailsTabs[3];
static const char *paneError = NULL;

/*
 * The tabs are two buttons over a page group rather than a Register
 * group: on some font setups Register draws its titles taller than the
 * row it makes for them, cutting them off.
 */
static APTR newTabButton(CONST_STRPTR label, BOOL selected)
{
	return TextObject,
		ButtonFrame,
		MUIA_Background, MUII_ButtonBack,
		MUIA_Font, MUIV_Font_Button,
		MUIA_Text_Contents, label,
		MUIA_Text_PreParse, "\33c",
		MUIA_InputMode, MUIV_InputMode_Immediate,
		MUIA_Selected, selected,
		MUIA_CycleChain, 1,
		End;
}

const char *gameDetailsPaneError(void)
{
	return paneError;
}

/*
 * A scrolling, word wrapping, read only text area. NFloattext.mcc comes
 * with NList, which iGame needs anyway, and is preferred: some systems
 * still carry an old Floattext.mui from MUI 3 that fails under MUI 5.
 * The other class is the fallback. Both areas use the same class.
 */
static APTR newTextArea(APTR *text)
{
	APTR view = NULL;

	*text = NULL;

	if (useNFloattext)
	{
		/* No frame of its own: inside an NListview that is drawn
		   outside the list and covers whatever is next to it */
		*text = NFloattextObject,
			MUIA_NList_Input, FALSE,
			MUIA_NFloattext_Text, "",
			End;
		if (*text)
		{
			view = NListviewObject,
				MUIA_NListview_NList, *text,
				MUIA_NListview_Horiz_ScrollBar, MUIV_NListview_HSB_None,
				MUIA_NListview_Vert_ScrollBar, MUIV_NListview_VSB_Auto,
				MUIA_CycleChain, 1,
				End;
		}
	}
	else
	{
		*text = FloattextObject,
			MUIA_Frame, MUIV_Frame_ReadList,
			MUIA_Background, MUII_ReadListBack,
			MUIA_Floattext_Text, "",
			End;
		if (*text)
		{
			view = ListviewObject,
				MUIA_Listview_Input, FALSE,
				MUIA_Listview_List, *text,
				MUIA_CycleChain, 1,
				End;
		}
	}

	if (!view)
		*text = NULL; /* disposed together with the failed view */
	return view;
}

static void forgetPane(struct ObjApp *object)
{
	object->GR_Details = object->TX_DetailsTitle = object->GR_DetailsMain = NULL;
	object->LV_DetailsMain = object->FT_DetailsMain = NULL;
	object->LV_DetailsCommunity = object->FT_DetailsCommunity = NULL;
	object->BT_DetailsTabMain = object->BT_DetailsTabCommunity = NULL;
	object->GR_DetailsPages = NULL;
	object->GR_DetailsShotRow = object->GR_DetailsShotPrev = object->GR_DetailsShotNext = NULL;
}

/* An arrow button, vertically centred next to the screenshot. Hidden
   until a game has more than one screenshot */
static APTR newShotArrow(ULONG image, struct Hook *hook)
{
	APTR column;
	APTR button = ImageObject,
		ImageButtonFrame,
		MUIA_Image_Spec, image,
		MUIA_InputMode, MUIV_InputMode_RelVerify,
		MUIA_Background, MUII_ButtonBack,
		MUIA_CycleChain, 1,
		End;

	if (!button)
		return NULL;

	column = VGroup,
		MUIA_ShowMe, FALSE,
		Child, VSpace(0),
		Child, button,
		Child, VSpace(0),
		End;

	if (column)
		DoMethod(button, MUIM_Notify, MUIA_Pressed, FALSE, button, 2, MUIM_CallHook, hook);
	return column;
}

/* Disposes what was made so far. Objects already inside a group go with it */
static APTR paneFailed(struct ObjApp *object, const char *what, APTR *loose, int looseCount)
{
	int i;

	for (i = 0; i < looseCount; i++)
		if (loose[i])
			MUI_DisposeObject(loose[i]);

	paneError = what;
	forgetPane(object);
	return NULL;
}

APTR gameDetailsCreatePane(struct ObjApp *object)
{
	APTR loose[5];

	detailsTabs[0] = (CONST_STRPTR)GetMBString(MSG_DetailsTabMain);
	detailsTabs[1] = (CONST_STRPTR)GetMBString(MSG_DetailsTabCommunity);
	detailsTabs[2] = NULL;
	paneError = NULL;

	/* NFloattext first, then Floattext */
	useNFloattext = TRUE;
	object->LV_DetailsMain = newTextArea(&object->FT_DetailsMain);
	if (!object->LV_DetailsMain)
	{
		useNFloattext = FALSE;
		object->LV_DetailsMain = newTextArea(&object->FT_DetailsMain);
	}
	if (!object->LV_DetailsMain)
	{
		loose[0] = object->GR_spacedScreenshot;
		object->GR_spacedScreenshot = object->IM_GameImage_0 = NULL;
		return paneFailed(object, "NFloattext.mcc and Floattext.mui", loose, 1);
	}

	object->LV_DetailsCommunity = newTextArea(&object->FT_DetailsCommunity);
	if (!object->LV_DetailsCommunity)
	{
		loose[0] = object->GR_spacedScreenshot;
		loose[1] = object->LV_DetailsMain;
		object->GR_spacedScreenshot = object->IM_GameImage_0 = NULL;
		return paneFailed(object, useNFloattext ? "NFloattext.mcc" : "Floattext.mui", loose, 2);
	}

	/* The game's title, at the top of the Main tab */
	object->TX_DetailsTitle = TextObject,
		MUIA_Text_PreParse, "\33c\33b",
		MUIA_Text_Contents, "",
		MUIA_Text_SetMin, FALSE,
		End;
	if (!object->TX_DetailsTitle)
	{
		loose[0] = object->GR_spacedScreenshot;
		loose[1] = object->LV_DetailsMain;
		loose[2] = object->LV_DetailsCommunity;
		object->GR_spacedScreenshot = object->IM_GameImage_0 = NULL;
		return paneFailed(object, "Text.mui", loose, 3);
	}

	/* The screenshot between its arrows. The row is hidden while there
	   is no picture; funcs.c shows and hides it with the screenshot */
	object->GR_DetailsShotPrev = newShotArrow(MUII_ArrowLeft, &ShotPrevHook);
	object->GR_DetailsShotNext = newShotArrow(MUII_ArrowRight, &ShotNextHook);
	if (!object->GR_DetailsShotPrev || !object->GR_DetailsShotNext)
	{
		loose[0] = object->GR_spacedScreenshot;
		loose[1] = object->LV_DetailsMain;
		loose[2] = object->LV_DetailsCommunity;
		loose[3] = object->TX_DetailsTitle;
		loose[4] = object->GR_DetailsShotPrev ? object->GR_DetailsShotPrev : object->GR_DetailsShotNext;
		object->GR_spacedScreenshot = object->IM_GameImage_0 = NULL;
		return paneFailed(object, "Image.mui", loose, 5);
	}

	object->GR_DetailsShotRow = HGroup,
		MUIA_ShowMe, object->GR_spacedScreenshot && check_path_exists(DEFAULT_SCREENSHOT_FILE),
		Child, object->GR_DetailsShotPrev,
		End;
	if (object->GR_DetailsShotRow)
	{
		if (object->GR_spacedScreenshot)
			DoMethod(object->GR_DetailsShotRow, OM_ADDMEMBER, object->GR_spacedScreenshot);
		DoMethod(object->GR_DetailsShotRow, OM_ADDMEMBER, object->GR_DetailsShotNext);
	}
	else
	{
		loose[0] = object->GR_spacedScreenshot;
		loose[1] = object->LV_DetailsMain;
		loose[2] = object->LV_DetailsCommunity;
		loose[3] = object->TX_DetailsTitle;
		loose[4] = object->GR_DetailsShotNext;
		object->GR_spacedScreenshot = object->IM_GameImage_0 = NULL;
		return paneFailed(object, "screenshot row", loose, 5);
	}

	/* The Main tab: title, the screenshot row, then the text */
	object->GR_DetailsMain = VGroup,
		MUIA_HelpNode, "GR_DetailsMain",
		Child, object->TX_DetailsTitle,
		Child, object->GR_DetailsShotRow,
		Child, object->LV_DetailsMain,
		End;
	if (!object->GR_DetailsMain)
	{
		/* Its children were disposed with it */
		object->GR_spacedScreenshot = object->IM_GameImage_0 = NULL;
		loose[0] = object->LV_DetailsCommunity;
		return paneFailed(object, "Main tab group", loose, 1);
	}

	/* The pages, switched by the tab buttons */
	object->GR_DetailsPages = VGroup,
		MUIA_Group_PageMode, TRUE,
		Child, object->GR_DetailsMain,
		Child, object->LV_DetailsCommunity,
		End;
	if (!object->GR_DetailsPages)
	{
		object->GR_spacedScreenshot = object->IM_GameImage_0 = NULL;
		return paneFailed(object, "page group", loose, 0);
	}

	object->BT_DetailsTabMain = newTabButton(detailsTabs[0], TRUE);
	object->BT_DetailsTabCommunity = newTabButton(detailsTabs[1], FALSE);

	/* The pane: tab buttons above the pages. The buttons are as high as
	   the filter cycle gadget on the left, so both sides line up */
	object->GR_Details = VGroup,
		MUIA_HelpNode, "GR_Details",
		MUIA_Weight, 100,
		Child, HGroup,
			MUIA_Group_HorizSpacing, 0,
			Child, object->BT_DetailsTabMain,
			Child, object->BT_DetailsTabCommunity,
			End,
		End;
	if (object->GR_Details)
		DoMethod(object->GR_Details, OM_ADDMEMBER, object->GR_DetailsPages);
	else
	{
		loose[0] = object->GR_DetailsPages;
		object->GR_spacedScreenshot = object->IM_GameImage_0 = NULL;
		return paneFailed(object, "details group", loose, 1);
	}

	/* Each button shows its page and releases the other button */
	DoMethod(object->BT_DetailsTabMain, MUIM_Notify, MUIA_Selected, TRUE,
		object->GR_DetailsPages, 3, MUIM_Set, MUIA_Group_ActivePage, 0);
	DoMethod(object->BT_DetailsTabMain, MUIM_Notify, MUIA_Selected, TRUE,
		object->BT_DetailsTabCommunity, 3, MUIM_NoNotifySet, MUIA_Selected, FALSE);
	DoMethod(object->BT_DetailsTabCommunity, MUIM_Notify, MUIA_Selected, TRUE,
		object->GR_DetailsPages, 3, MUIM_Set, MUIA_Group_ActivePage, 1);
	DoMethod(object->BT_DetailsTabCommunity, MUIM_Notify, MUIA_Selected, TRUE,
		object->BT_DetailsTabMain, 3, MUIM_NoNotifySet, MUIA_Selected, FALSE);
	DoMethod(object->BT_DetailsTabCommunity, MUIM_Notify, MUIA_Selected, TRUE,
		object->BT_DetailsTabCommunity, 2, MUIM_CallHook, &CommunityTabHook);

	set(object->FT_DetailsMain, useNFloattext ? MUIA_NFloattext_Text : MUIA_Floattext_Text,
		GetMBString(MSG_DetailsSelectGame));

	return object->GR_Details;
}
