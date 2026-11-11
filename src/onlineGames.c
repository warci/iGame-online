/*
  onlineGames.c
  Online games repository functions for iGame

  Lists the WHDLoad archives available in an online repository (by default
  the Retroplay WHDLoad packs on the Turran FTP), shows them in the games
  list, and downloads and installs them when the user starts one that is
  not installed yet.

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

/* MUI */
#include <libraries/mui.h>
#include <mui/NListview_mcc.h>

/* Prototypes */
#include <clib/alib_protos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/muimaster.h>

#include <dos/dostags.h>

/* ANSI C */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#define iGame_NUMBERS
#include "iGame_strings.h"

#include "iGameGUI.h"
#include "iGameExtern.h"
#include "strfuncs.h"
#include "fsfuncs.h"
#include "funcs.h"
#include "slavesList.h"
#include "genresList.h"
#include "chipsetList.h"
#include "http.h"
#include "onlineGames.h"
#include "progressWin.h"

extern struct ObjApp *app;
extern igame_settings *current_settings;

#define ONLINE_MAX_DIRS       64
#define ONLINE_MAX_DIRNAME    64
#define ONLINE_MAX_HREF       256
#define ONLINE_LINE_SIZE      1024
#define ONLINE_MAX_STAGED     32
#define ONLINE_URL_SIZE       640
#define ONLINE_ARCHIVE_NAME   "iGame_download.lha"
#define ONLINE_STAGING_NAME   "iGame_download_tmp"

/*****************************************************************************
 * A small case insensitive string set. The catalogue has a few thousand
 * entries, so the merge needs hashing to stay fast on a 68000.
 * Keys are not copied; they point into the list nodes.
 *****************************************************************************/

#define SET_BUCKETS 1024

typedef struct setEntry
{
	struct setEntry *next;
	const char *key;
	int len;
	slavesList *node;
	BOOL seen;
} setEntry;

typedef struct
{
	setEntry *buckets[SET_BUCKETS];
} strSet;

static ULONG hashStr(const char *s, int len)
{
	ULONG h = 5381;
	while (len-- > 0)
		h = (h * 33) ^ (ULONG)tolower((unsigned char)*s++);
	return h & (SET_BUCKETS - 1);
}

static strSet *setNew(void)
{
	return (strSet *)calloc(1, sizeof(strSet));
}

static void setFree(strSet *set)
{
	int i;
	if (!set)
		return;
	for (i = 0; i < SET_BUCKETS; i++)
	{
		setEntry *e = set->buckets[i];
		while (e)
		{
			setEntry *next = e->next;
			free(e);
			e = next;
		}
	}
	free(set);
}

static setEntry *setFind(strSet *set, const char *key, int len)
{
	setEntry *e;
	if (!set || !key || len <= 0)
		return NULL;
	for (e = set->buckets[hashStr(key, len)]; e; e = e->next)
	{
		if (e->len == len && !strncasecmp_ig(e->key, key, len))
			return e;
	}
	return NULL;
}

static setEntry *setAdd(strSet *set, const char *key, int len, slavesList *node)
{
	setEntry *e;
	ULONG h;
	if (!set || !key || len <= 0)
		return NULL;
	if ((e = setFind(set, key, len)))
		return e;
	e = (setEntry *)malloc(sizeof(setEntry));
	if (!e)
		return NULL;
	h = hashStr(key, len);
	e->key = key;
	e->len = len;
	e->node = node;
	e->seen = FALSE;
	e->next = set->buckets[h];
	set->buckets[h] = e;
	return e;
}

/*****************************************************************************
 * Name helpers
 *****************************************************************************/

/* "A/Foo_v1.2_AGA.lha" -> "Foo_v1.2_AGA.lha" */
static const char *archiveFileName(const char *remote)
{
	const char *slash = strrchr(remote, '/');
	return slash ? slash + 1 : remote;
}

/* Position where the version part ("_v1.2") starts, or -1 */
static int findVersionPos(const char *name)
{
	int i;
	for (i = 1; name[i] && name[i + 1] && name[i + 2]; i++)
	{
		if (name[i] == '_' && (name[i + 1] == 'v' || name[i + 1] == 'V') && isdigit((unsigned char)name[i + 2]))
			return i;
	}
	return -1;
}

/*
 * The base name of an archive is the name of the folder it contains in the
 * Retroplay packs: "A/A10TankKiller_v2.0_2Disk_1396.lha" -> "A10TankKiller"
 * Returns the length of the base name, which starts at archiveFileName().
 */
static int archiveBaseLen(const char *remote)
{
	const char *name = archiveFileName(remote);
	int pos = findVersionPos(name);
	if (pos < 0)
	{
		const char *p = strchr(name, '_');
		if (!p)
			p = strrchr(name, '.');
		pos = p ? (int)(p - name) : (int)strlen(name);
	}
	return pos;
}

/*
 * Make a readable title from an archive name:
 * "A10TankKiller_v2.0_2Disk_1396.lha" -> "A10 Tank Killer (2Disk 1396)"
 * The extra parts are kept because they tell the variants apart.
 */
static void titleFromArchive(const char *remote, char *out, int outSize)
{
	char name[MAX_SLAVE_TITLE_SIZE];
	char base[MAX_SLAVE_TITLE_SIZE];
	char *extras = NULL;
	char *dot;
	int baseLen;

	strlcpy(name, archiveFileName(remote), sizeof(name));
	dot = strrchr(name, '.');
	if (dot)
		*dot = '\0';

	baseLen = archiveBaseLen(name);
	if (baseLen <= 0 || baseLen >= (int)sizeof(base))
		baseLen = strlen(name);
	memcpy(base, name, baseLen);
	base[baseLen] = '\0';

	if (name[baseLen] == '_')
	{
		/* Skip the version token, keep what follows */
		extras = name + baseLen + 1;
		if (findVersionPos(name) == baseLen)
		{
			extras = strchr(extras, '_');
			if (extras)
				extras++;
		}
	}

	if (current_settings->no_smart_spaces)
		strlcpy(out, base, outSize);
	else
		add_spaces_to_string(base, out, outSize);

	if (extras && *extras)
	{
		char *p;
		for (p = extras; *p; p++)
			if (*p == '_')
				*p = ' ';
		strlcat(out, " (", outSize);
		strlcat(out, extras, outSize);
		strlcat(out, ")", outSize);
	}
}

/* Name of the folder that holds the item: "DH1:Games/Foo/Foo.slave" -> "Foo" */
static int parentDirName(const char *path, const char **start)
{
	const char *end = strrchr(path, '/');
	const char *p;
	if (!end || end == path)
		return 0;
	p = end - 1;
	while (p > path && *p != '/' && *p != ':')
		p--;
	if (*p == '/' || *p == ':')
		p++;
	*start = p;
	return end - p;
}

static BOOL hasSuffix(const char *s, const char *suffix)
{
	const int ls = strlen(s);
	const int lx = strlen(suffix);
	return ls >= lx && !strncasecmp_ig(s + ls - lx, suffix, lx);
}

static void onlineBaseUrl(char *out, int outSize)
{
	const char *url = DEFAULT_ONLINE_URL;
	if (current_settings && !isStringEmpty(current_settings->online_url))
		url = current_settings->online_url;
	strlcpy(out, url, outSize);
	if (out[0] && out[strlen(out) - 1] != '/')
		strlcat(out, "/", outSize);
}

/*****************************************************************************
 * Merging the online games list into the games list
 *****************************************************************************/

/*
 * Adds the games of the online games list that are not in the games list
 * yet. Items that are installed locally (matched by their folder name) are
 * not added, and not installed items that disappeared from the online list
 * get hidden.
 * Returns the number of online games, or -1 when there is no list file.
 */
LONG onlineMergeList(void)
{
	strSet *remotes, *localDirs, *titles;
	slavesList *node;
	char *line;
	BPTR fh;
	LONG count = 0;

	if (!current_settings->show_online_games)
		return 0;

	fh = Open((STRPTR)DEFAULT_ONLINE_LIST_FILE, MODE_OLDFILE);
	if (!fh)
		return -1;

	line = malloc(ONLINE_LINE_SIZE);
	remotes = setNew();
	localDirs = setNew();
	titles = setNew();
	if (!line || !remotes || !localDirs || !titles)
	{
		msg_box((const char *)GetMBString(MSG_NotEnoughMemory));
		goto cleanup;
	}

	for (node = getSlavesListHead(); node; node = node->next)
	{
		if (node->remote)
			setAdd(remotes, node->remote, strlen(node->remote), node);

		if (!isStringEmpty(node->path))
		{
			const char *dir;
			const int len = parentDirName(node->path, &dir);
			if (len > 0)
				setAdd(localDirs, dir, len, node);
		}

		setAdd(titles, node->title, strlen(node->title), node);
	}

	while (FGets(fh, line, ONLINE_LINE_SIZE - 1))
	{
		int len = strlen(line);
		setEntry *e;

		while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
			line[--len] = '\0';
		if (len == 0 || line[0] == '#')
			continue;

		count++;

		if ((e = setFind(remotes, line, len)))
		{
			e->seen = TRUE;
			continue;
		}

		/* Installed locally already, from an earlier scan */
		if (setFind(localDirs, archiveFileName(line), archiveBaseLen(line)))
			continue;

		node = (slavesList *)calloc(1, sizeof(slavesList));
		if (node)
			node->remote = strdup(line);
		if (!node || !node->remote)
		{
			if (node)
				free(node);
			msg_box((const char *)GetMBString(MSG_NotEnoughMemory));
			break;
		}

		strlcpy(node->genre, "Unknown", sizeof(node->genre));
		titleFromArchive(line, node->title, sizeof(node->title));

		/* Titles are used to find the items, so they must be unique */
		if (setFind(titles, node->title, strlen(node->title)))
		{
			const int baseLen = strlen(node->title);
			int n;
			for (n = 2; n < 100; n++)
			{
				snprintf(node->title + baseLen, sizeof(node->title) - baseLen, " [%d]", n);
				if (!setFind(titles, node->title, strlen(node->title)))
					break;
			}
		}

		slavesListAddTail(node);
		setAdd(titles, node->title, strlen(node->title), node);
		e = setAdd(remotes, node->remote, strlen(node->remote), node);
		if (e)
			e->seen = TRUE;
	}

	/*
	 * Not installed items: hide the ones that are gone from the online
	 * list or that got installed manually since they were added.
	 */
	if (count > 0)
	{
		for (node = getSlavesListHead(); node; node = node->next)
		{
			if (ITEM_NOT_INSTALLED(node))
			{
				const setEntry *e = setFind(remotes, node->remote, strlen(node->remote));
				node->deleted = (e == NULL || !e->seen ||
					setFind(localDirs, archiveFileName(node->remote), archiveBaseLen(node->remote)) != NULL);
			}
		}
	}

cleanup:
	Close(fh);
	if (line)
		free(line);
	setFree(remotes);
	setFree(localDirs);
	setFree(titles);
	return count;
}

static BOOL downloadProgress(ULONG received, ULONG total, void *userData)
{
	progressBytes(received, total);
	return progressPump();
}

/*****************************************************************************
 * Updating the online games list
 *****************************************************************************/

typedef struct
{
	BPTR out;
	const char *prefix;
	char line[ONLINE_LINE_SIZE];
	int lineLen;
	char lastName[ONLINE_MAX_HREF];
	char dirs[ONLINE_MAX_DIRS][ONLINE_MAX_DIRNAME];
	int dirCount;
	BOOL collectDirs;
	LONG files;
	BOOL writeError;
	LONG step;
	LONG steps;
} listingCtx;

static int hexValue(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

/* Undo the HTML entities and URL encoding of an href value */
static void decodeHref(const char *in, char *out, int outSize)
{
	int o = 0;
	while (*in && o < outSize - 1)
	{
		if (*in == '&')
		{
			if (!strncmp(in, "&amp;", 5))       { out[o++] = '&';  in += 5; continue; }
			if (!strncmp(in, "&lt;", 4))        { out[o++] = '<';  in += 4; continue; }
			if (!strncmp(in, "&gt;", 4))        { out[o++] = '>';  in += 4; continue; }
			if (!strncmp(in, "&quot;", 6))      { out[o++] = '"';  in += 6; continue; }
			if (!strncmp(in, "&#39;", 5))       { out[o++] = '\''; in += 5; continue; }
		}
		if (*in == '%' && hexValue(in[1]) >= 0 && hexValue(in[2]) >= 0)
		{
			out[o++] = (char)(hexValue(in[1]) * 16 + hexValue(in[2]));
			in += 3;
			continue;
		}
		out[o++] = *in++;
	}
	out[o] = '\0';
}

static void handleHref(listingCtx *ctx, const char *href)
{
	char name[ONLINE_MAX_HREF];
	int len;
	char *slash;

	/* Sorting links, absolute links, parent folder and external links */
	if (href[0] == '?' || href[0] == '/' || href[0] == '#' || href[0] == '.' ||
		strstr(href, "://") || !strncasecmp_ig(href, "mailto:", 7))
		return;

	decodeHref(href, name, sizeof(name));
	len = strlen(name);
	if (len == 0)
		return;

	/* Every entry is linked twice, from the icon and from the name */
	if (!strcmp(name, ctx->lastName))
		return;
	strlcpy(ctx->lastName, name, sizeof(ctx->lastName));

	slash = strchr(name, '/');
	if (slash)
	{
		/* A sub folder: only one level deep */
		if (ctx->collectDirs && slash == name + len - 1 &&
			len < ONLINE_MAX_DIRNAME && ctx->dirCount < ONLINE_MAX_DIRS)
		{
			strlcpy(ctx->dirs[ctx->dirCount++], name, ONLINE_MAX_DIRNAME);
		}
		return;
	}

	if (hasSuffix(name, ".lha") || hasSuffix(name, ".lzh"))
	{
		if (ctx->out && !ctx->writeError)
		{
			if (FPuts(ctx->out, (STRPTR)ctx->prefix) || FPuts(ctx->out, (STRPTR)name) || FPutC(ctx->out, '\n') < 0)
				ctx->writeError = TRUE;
		}
		ctx->files++;
	}
}

static void processListingLine(listingCtx *ctx)
{
	char href[ONLINE_MAX_HREF];
	const char *p = ctx->line;

	ctx->line[ctx->lineLen] = '\0';
	while ((p = strstr(p, "href=\"")))
	{
		const char *q;
		int len;

		p += 6;
		q = strchr(p, '"');
		if (!q)
			break;

		len = q - p;
		if (len > 0 && len < (int)sizeof(href))
		{
			memcpy(href, p, len);
			href[len] = '\0';
			handleHref(ctx, href);
		}
		p = q + 1;
	}
	ctx->lineLen = 0;
}

/* The listing is processed line by line while it arrives */
static BOOL listingSink(const char *data, LONG len, void *userData)
{
	listingCtx *ctx = (listingCtx *)userData;

	while (len-- > 0)
	{
		const char c = *data++;
		if (c == '\n')
			processListingLine(ctx);
		else if (ctx->lineLen < ONLINE_LINE_SIZE - 1)
			ctx->line[ctx->lineLen++] = c;
	}

	return !ctx->writeError;
}

static BOOL listingProgress(ULONG received, ULONG total, void *userData)
{
	listingCtx *ctx = (listingCtx *)userData;
	char info[32];

	snprintf(info, sizeof(info), "%ld", (long)ctx->files);
	progressGauge(ctx->steps ? (ctx->step * 100) / ctx->steps : 0, info);

	return progressPump();
}

static LONG fetchListing(listingCtx *ctx, const char *baseUrl, const char *dir)
{
	char *url = malloc(ONLINE_URL_SIZE);
	char label[128];
	LONG result;

	if (!url)
		return HTTP_ERR_MEMORY;

	strlcpy(url, baseUrl, ONLINE_URL_SIZE);
	if (dir)
	{
		const int baseLen = strlen(url);
		httpEncodePath(dir, url + baseLen, ONLINE_URL_SIZE - baseLen);
	}

	snprintf(label, sizeof(label), (const char *)GetMBString(MSG_OnlineFetchingList),
		(long)(ctx->step + 1), (long)(ctx->steps ? ctx->steps : 1));
	progressText(label);

	ctx->prefix = dir ? dir : "";
	ctx->lineLen = 0;
	ctx->lastName[0] = '\0';

	result = httpGet(url, listingSink, listingProgress, ctx, NULL);
	if (ctx->lineLen > 0)
		processListingLine(ctx);
	if (result == HTTP_OK && ctx->writeError)
		result = HTTP_ERR_WRITE;

	free(url);
	return result;
}

void onlineUpdateList(void)
{
	char tmpFile[64];
	char baseUrl[MAX_PATH_SIZE];
	char msg[512];
	listingCtx *ctx;
	LONG result;
	int i;

	if (!httpAvailable())
	{
		msg_box((const char *)GetMBString(MSG_OnlineNoBsdsocket));
		return;
	}

	ctx = (listingCtx *)calloc(1, sizeof(listingCtx));
	if (!ctx)
	{
		msg_box((const char *)GetMBString(MSG_NotEnoughMemory));
		return;
	}

	onlineBaseUrl(baseUrl, sizeof(baseUrl));
	snprintf(tmpFile, sizeof(tmpFile), "%s.new", DEFAULT_ONLINE_LIST_FILE);

	ctx->out = Open((STRPTR)tmpFile, MODE_NEWFILE);
	if (!ctx->out)
	{
		snprintf(msg, sizeof(msg), (const char *)GetMBString(MSG_OnlineUpdateFailed),
			httpErrorString(HTTP_ERR_WRITE));
		msg_box(msg);
		free(ctx);
		return;
	}
	FPuts(ctx->out, (STRPTR)"# iGame online games list\n# ");
	FPuts(ctx->out, (STRPTR)baseUrl);
	FPutC(ctx->out, '\n');

	progressOpen("", TRUE);

	/* The top folder, which holds either the archives or a folder per letter */
	ctx->collectDirs = TRUE;
	ctx->step = 0;
	ctx->steps = 1;
	result = fetchListing(ctx, baseUrl, NULL);

	ctx->collectDirs = FALSE;
	ctx->steps = 1 + ctx->dirCount;
	for (i = 0; result == HTTP_OK && i < ctx->dirCount; i++)
	{
		/* Be gentle with the server: it rate limits heavy users */
		Delay(TICKS_PER_SECOND / 2);
		if (!progressPump())
		{
			result = HTTP_ERR_ABORT;
			break;
		}

		ctx->step = i + 1;
		result = fetchListing(ctx, baseUrl, ctx->dirs[i]);
	}

	Close(ctx->out);
	progressClose();

	if (result == HTTP_OK && ctx->files > 0)
	{
		DeleteFile((STRPTR)DEFAULT_ONLINE_LIST_FILE);
		Rename((STRPTR)tmpFile, (STRPTR)DEFAULT_ONLINE_LIST_FILE);

		set(app->WI_MainWindow, MUIA_Window_Sleep, TRUE);
		onlineMergeList();
		save_list();
		filter_change();
		set(app->WI_MainWindow, MUIA_Window_Sleep, FALSE);

		snprintf(msg, sizeof(msg), (const char *)GetMBString(MSG_OnlineUpdateDone), (long)ctx->files);
		setStatusText(msg);
	}
	else
	{
		DeleteFile((STRPTR)tmpFile);
		if (result != HTTP_ERR_ABORT)
		{
			snprintf(msg, sizeof(msg), (const char *)GetMBString(MSG_OnlineUpdateFailed),
				httpErrorString(result == HTTP_OK ? HTTP_ERR_STATUS : result));
			msg_box(msg);
		}
		filter_change();
	}

	free(ctx);
}

/*****************************************************************************
 * Downloading and installing a game
 *****************************************************************************/

/* Where downloaded games get installed: the setting, or the first repository */
static BOOL getInstallRoot(char *out, int outSize)
{
	STRPTR repo = NULL;

	if (!isStringEmpty(current_settings->download_path))
	{
		strlcpy(out, current_settings->download_path, outSize);
		return TRUE;
	}

	DoMethod(app->LV_GameRepositories, MUIM_List_GetEntry, 0, &repo);
	if (repo && !isStringEmpty((const char *)repo))
	{
		strlcpy(out, (const char *)repo, outSize);
		return TRUE;
	}

	return FALSE;
}

static LONG runCommand(const char *cmd)
{
	struct TagItem tags[3];
	BPTR nilIn = Open((STRPTR)"NIL:", MODE_OLDFILE);
	BPTR nilOut = Open((STRPTR)"NIL:", MODE_NEWFILE);
	LONG rc;

	tags[0].ti_Tag = SYS_Input;  tags[0].ti_Data = (ULONG)nilIn;
	tags[1].ti_Tag = SYS_Output; tags[1].ti_Data = (ULONG)nilOut;
	tags[2].ti_Tag = TAG_DONE;   tags[2].ti_Data = 0;

	rc = SystemTagList((STRPTR)cmd, tags);

	if (nilIn) Close(nilIn);
	if (nilOut) Close(nilOut);
	return rc;
}

static void deleteAll(const char *path)
{
	char cmd[MAX_PATH_SIZE + 40];
	snprintf(cmd, sizeof(cmd), "Delete \"%s\" ALL QUIET FORCE", path);
	runCommand(cmd);
}

/* Look for a .slave file in a folder and its sub folders */
static BOOL findSlave(const char *dir, char *result, int resultSize, int depth)
{
	struct FileInfoBlock *fib;
	BOOL found = FALSE;
	BPTR lock = Lock((STRPTR)dir, SHARED_LOCK);

	if (!lock)
		return FALSE;

	fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
	if (fib && Examine(lock, fib))
	{
		/* Files first, so the slave in the top folder wins */
		while (!found && ExNext(lock, fib))
		{
			if (fib->fib_DirEntryType < 0 && hasSuffix(fib->fib_FileName, ".slave"))
			{
				strlcpy(result, dir, resultSize);
				AddPart(result, fib->fib_FileName, resultSize);
				found = TRUE;
			}
		}

		if (!found && depth > 0 && Examine(lock, fib))
		{
			char *sub = malloc(MAX_PATH_SIZE);
			while (sub && !found && ExNext(lock, fib))
			{
				if (fib->fib_DirEntryType > 0)
				{
					strlcpy(sub, dir, MAX_PATH_SIZE);
					AddPart(sub, fib->fib_FileName, MAX_PATH_SIZE);
					found = findSlave(sub, result, resultSize, depth - 1);
				}
			}
			if (sub)
				free(sub);
		}
	}

	if (fib)
		FreeDosObject(DOS_FIB, fib);
	UnLock(lock);
	return found;
}

typedef struct
{
	char name[108];
	BOOL isDir;
} stagedEntry;

/*
 * Move what was extracted into the staging folder to the install folder.
 * A folder that exists already (an older version of the game) is updated
 * in place, so saved games and settings in it are kept.
 * The first folder that was moved is returned in gameDir.
 */
static BOOL moveStaged(const char *staging, const char *root, char *gameDir, int gameDirSize)
{
	stagedEntry *entries;
	struct FileInfoBlock *fib;
	char *src, *dst, *cmd;
	int count = 0, i;
	BOOL ok = TRUE;
	BPTR lock;

	gameDir[0] = '\0';

	entries = (stagedEntry *)calloc(ONLINE_MAX_STAGED, sizeof(stagedEntry));
	src = malloc(MAX_PATH_SIZE);
	dst = malloc(MAX_PATH_SIZE);
	cmd = malloc(MAX_PATH_SIZE * 2 + 40);
	fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
	lock = Lock((STRPTR)staging, SHARED_LOCK);

	if (!entries || !src || !dst || !cmd || !fib || !lock)
	{
		ok = FALSE;
		goto cleanup;
	}

	/* Read the names first; renaming while ExNext() runs isn't safe */
	if (Examine(lock, fib))
	{
		while (count < ONLINE_MAX_STAGED && ExNext(lock, fib))
		{
			strlcpy(entries[count].name, fib->fib_FileName, sizeof(entries[count].name));
			entries[count].isDir = fib->fib_DirEntryType > 0;
			count++;
		}
	}
	UnLock(lock);
	lock = 0;

	for (i = 0; i < count; i++)
	{
		strlcpy(src, staging, MAX_PATH_SIZE);
		AddPart(src, entries[i].name, MAX_PATH_SIZE);
		strlcpy(dst, root, MAX_PATH_SIZE);
		AddPart(dst, entries[i].name, MAX_PATH_SIZE);

		if (!Rename((STRPTR)src, (STRPTR)dst))
		{
			snprintf(cmd, MAX_PATH_SIZE * 2 + 40, "Copy \"%s\" TO \"%s\" %sCLONE QUIET",
				src, dst, entries[i].isDir ? "ALL " : "");
			if (runCommand(cmd) != 0)
				ok = FALSE;
		}

		if (entries[i].isDir && gameDir[0] == '\0')
			strlcpy(gameDir, dst, gameDirSize);
	}

cleanup:
	if (lock) UnLock(lock);
	if (fib) FreeDosObject(DOS_FIB, fib);
	if (entries) free(entries);
	if (src) free(src);
	if (dst) free(dst);
	if (cmd) free(cmd);
	return ok;
}

/* Take the details from the igame.data file next to the slave, if any */
static void readIGameData(slavesList *node)
{
	char *path = malloc(MAX_PATH_SIZE);
	slavesList *info = (slavesList *)calloc(1, sizeof(slavesList));

	if (path && info)
	{
		getParentPath(node->path, path, MAX_PATH_SIZE);
		AddPart(path, DEFAULT_IGAMEDATA_FILE, MAX_PATH_SIZE);
		if (check_path_exists(path))
		{
			getIGameDataInfo(path, info);
			if (!isStringEmpty(info->genre))
			{
				strlcpy(node->genre, info->genre, sizeof(node->genre));
				addGenreInList(node->genre);
			}
			if (!isStringEmpty(info->chipset))
			{
				strlcpy(node->chipset, info->chipset, sizeof(node->chipset));
				addChipsetInList(node->chipset);
			}
			if (info->year)
				node->year = info->year;
			if (info->players)
				node->players = info->players;
		}
	}

	if (path) free(path);
	if (info) free(info);
}

/*
 * Download and install a game that is not installed yet.
 * Asks the user first. Returns TRUE when the game is ready to be started.
 */
BOOL onlineInstallGame(slavesList *node)
{
	char root[MAX_PATH_SIZE];
	char *url = NULL, *archive = NULL, *staging = NULL, *gameDir = NULL;
	char *slave = NULL, *msg = NULL;
	const char *title = isStringEmpty(node->user_title) ? node->title : node->user_title;
	BOOL success = FALSE;
	LONG result;
	BPTR lock;

	if (!node->remote)
		return FALSE;

	if (!getInstallRoot(root, sizeof(root)) || !check_path_exists(root))
	{
		msg_box((const char *)GetMBString(MSG_OnlineNoInstallPath));
		return FALSE;
	}

	msg = malloc(1024);
	url = malloc(ONLINE_URL_SIZE);
	archive = malloc(MAX_PATH_SIZE);
	staging = malloc(MAX_PATH_SIZE);
	gameDir = malloc(MAX_PATH_SIZE);
	slave = malloc(MAX_PATH_SIZE);
	if (!msg || !url || !archive || !staging || !gameDir || !slave)
	{
		msg_box((const char *)GetMBString(MSG_NotEnoughMemory));
		goto cleanup;
	}

	snprintf(msg, 1024, (const char *)GetMBString(MSG_OnlineConfirmDownload), title, root);
	if (MUI_Request(app->App, app->WI_MainWindow, 0,
			(char *)GetMBString(MSG_OnlineWindowTitle),
			(char *)GetMBString(MSG_OnlineConfirmButtons),
			"%s", msg) != 1)
		goto cleanup;

	if (!httpAvailable())
	{
		msg_box((const char *)GetMBString(MSG_OnlineNoBsdsocket));
		goto cleanup;
	}

	onlineBaseUrl(url, ONLINE_URL_SIZE);
	httpEncodePath(node->remote, url + strlen(url), ONLINE_URL_SIZE - strlen(url));

	strlcpy(archive, root, MAX_PATH_SIZE);
	AddPart(archive, ONLINE_ARCHIVE_NAME, MAX_PATH_SIZE);
	strlcpy(staging, root, MAX_PATH_SIZE);
	AddPart(staging, ONLINE_STAGING_NAME, MAX_PATH_SIZE);

	/* Download */
	snprintf(msg, 1024, (const char *)GetMBString(MSG_OnlineDownloading), title);
	progressOpen(msg, TRUE);
	progressGauge(0, "");

	result = httpGetToFile(url, archive, downloadProgress, NULL, NULL);
	if (result != HTTP_OK)
	{
		progressClose();
		if (result != HTTP_ERR_ABORT)
		{
			snprintf(msg, 1024, (const char *)GetMBString(MSG_OnlineDownloadFailed),
				archiveFileName(node->remote), httpErrorString(result));
			msg_box(msg);
		}
		goto cleanup;
	}

	/* Extract into an empty staging folder, then move it in place */
	snprintf(msg, 1024, (const char *)GetMBString(MSG_OnlineExtracting), title);
	progressText(msg);
	progressGauge(100, "");
	progressPump();

	if (check_path_exists(staging))
		deleteAll(staging);
	lock = CreateDir((STRPTR)staging);
	if (lock)
		UnLock(lock);

	snprintf(msg, 1024, "LhA x \"%s\" \"%s/\"", archive, staging);
	if (!lock || runCommand(msg) != 0)
	{
		progressClose();
		deleteAll(staging);
		DeleteFile((STRPTR)archive);
		msg_box((const char *)GetMBString(MSG_OnlineExtractFailed));
		goto cleanup;
	}

	moveStaged(staging, root, gameDir, MAX_PATH_SIZE);
	deleteAll(staging);
	DeleteFile((STRPTR)archive);
	progressClose();

	if (isStringEmpty(gameDir) || !findSlave(gameDir, slave, MAX_PATH_SIZE, 2))
	{
		snprintf(msg, 1024, (const char *)GetMBString(MSG_OnlineNoSlaveFound),
			isStringEmpty(gameDir) ? root : gameDir);
		msg_box(msg);
		goto cleanup;
	}

	/* The item is installed now */
	getFullPath(slave, node->path);
	if (isStringEmpty(node->path))
		strlcpy(node->path, slave, sizeof(node->path));
	node->exists = 1;
	node->deleted = 0;
	readIGameData(node);
	save_list();

	/* Redraw the entry, so it no longer shows as not installed */
	{
		LONG active = MUIV_NList_Active_Off;
		get(app->LV_GamesList, MUIA_NList_Active, &active);
		if (active >= 0)
			DoMethod(app->LV_GamesList, MUIM_NList_ReplaceSingle, node, active, NOWRAP, ALIGN_LEFT);
	}

	success = TRUE;

cleanup:
	if (msg) free(msg);
	if (url) free(url);
	if (archive) free(archive);
	if (staging) free(staging);
	if (gameDir) free(gameDir);
	if (slave) free(slave);
	return success;
}

void onlineCleanup(void)
{
	httpCleanup();
}
