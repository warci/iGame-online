/*
  progressWin.c
  Progress window for online actions in iGame

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

/* Prototypes */
#include <clib/alib_protos.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/muimaster.h>

/* ANSI C */
#include <stdio.h>
#include <string.h>

#define iGame_NUMBERS
#include "iGame_strings.h"
#include "iGameGUI.h"
#include "iGameExtern.h"
#include "strfuncs.h"
#include "funcs.h"
#include "progressWin.h"

#ifndef MAKE_ID
#define MAKE_ID(a,b,c,d) ((ULONG) (a)<<24 | (ULONG) (b)<<16 | (ULONG) (c)<<8 | (ULONG) (d))
#endif

extern struct ObjApp *app;

static struct
{
	Object *win;
	Object *text;
	Object *gauge;
	Object *cancel;
	BOOL modal;
	BOOL gaugeIsText;   /* no usable Gauge class: a text line instead */
	ULONG lastShown;
	char info[64];
	char label[MAX_SLAVE_TITLE_SIZE + 64];
} progress;

BOOL progressIsOpen(void)
{
	return progress.win != NULL;
}

void progressText(const char *text)
{
	strlcpy(progress.label, text, sizeof(progress.label));
	if (progress.text)
		set(progress.text, MUIA_Text_Contents, progress.label);
	if (progress.modal)
		setStatusText(progress.label);
}

void progressGauge(LONG percent, const char *info)
{
	if (!progress.gauge)
		return;
	if (percent < 0) percent = 0;
	if (percent > 100) percent = 100;
	if (progress.gaugeIsText)
	{
		snprintf(progress.info, sizeof(progress.info), "%s %ld%%", info ? info : "", (long)percent);
		set(progress.gauge, MUIA_Text_Contents, progress.info);
		return;
	}
	strlcpy(progress.info, info ? info : "", sizeof(progress.info));
	SetAttrs(progress.gauge,
		MUIA_Gauge_Current, percent,
		MUIA_Gauge_InfoText, progress.info,
		TAG_DONE);
}

void progressBytes(ULONG received, ULONG total)
{
	char info[64];
	LONG percent = 0;

	/* Updating the gauge for every packet would slow a 68k down */
	if (!(received < progress.lastShown || received - progress.lastShown >= 8192 ||
		received == 0 || (total && received >= total)))
		return;

	progress.lastShown = received;
	if (total)
	{
		percent = (total >= 100) ? (LONG)(received / (total / 100)) : 100;
		snprintf(info, sizeof(info), "%lu / %lu KB",
			(unsigned long)(received >> 10), (unsigned long)(total >> 10));
	}
	else
	{
		snprintf(info, sizeof(info), "%lu KB", (unsigned long)(received >> 10));
	}
	progressGauge(percent, info);
}

void progressOpen(const char *text, BOOL modal)
{
	if (progress.win)
		progressClose();

	memset(&progress, 0, sizeof(progress));
	strlcpy(progress.label, text, sizeof(progress.label));
	progress.modal = modal;

	if (modal)
		set(app->WI_MainWindow, MUIA_Window_Sleep, TRUE);

	/* An old Gauge.mui from MUI 2 fails under newer MUI: use a text then */
	progress.gauge = GaugeObject,
		GaugeFrame,
		MUIA_Gauge_Horiz, TRUE,
		MUIA_Gauge_Max, 100,
		MUIA_Gauge_InfoText, "",
		MUIA_FixWidthTxt, "0000000000000000000000000000000000000000",
		End;
	if (!progress.gauge)
	{
		progress.gaugeIsText = TRUE;
		progress.gauge = TextObject,
			TextFrame,
			MUIA_Background, MUII_TextBack,
			MUIA_Text_PreParse, "\33c",
			MUIA_Text_Contents, "",
			MUIA_FixWidthTxt, "0000000000000000000000000000000000000000",
			End;
	}

	progress.win = WindowObject,
		MUIA_Window_Title, GetMBString(MSG_OnlineWindowTitle),
		MUIA_Window_ID, MAKE_ID('I', 'G', 'O', 'N'),
		MUIA_Window_CloseGadget, FALSE,
		MUIA_Window_RefWindow, app->WI_MainWindow,
		MUIA_Window_LeftEdge, MUIV_Window_LeftEdge_Centered,
		MUIA_Window_TopEdge, MUIV_Window_TopEdge_Centered,
		/* Leave the keyboard with the games list unless the main window sleeps */
		MUIA_Window_Activate, modal,
		WindowContents, VGroup,
			Child, progress.text = TextObject,
				MUIA_Text_PreParse, "\33c",
				MUIA_Text_Contents, progress.label,
				MUIA_Text_SetMin, FALSE,
				End,
			Child, progress.gauge,
			Child, HGroup,
				Child, HSpace(0),
				Child, progress.cancel = SimpleButton(GetMBString(MSG_OnlineCancel)),
				Child, HSpace(0),
				End,
			End,
		End;

	if (progress.win)
	{
		DoMethod(app->App, OM_ADDMEMBER, progress.win);
		/* No return ID: the flag is polled while working */
		DoMethod(progress.cancel, MUIM_Notify, MUIA_Pressed, FALSE,
			MUIV_Notify_Self, 3, MUIM_Set, MUIA_UserData, 1);
		set(progress.win, MUIA_Window_Open, TRUE);
	}
	else
	{
		progress.text = progress.gauge = progress.cancel = NULL;
	}

	if (modal)
		setStatusText(progress.label);
}

void progressClose(void)
{
	const BOOL modal = progress.modal;

	if (progress.win)
	{
		set(progress.win, MUIA_Window_Open, FALSE);
		DoMethod(app->App, OM_REMMEMBER, progress.win);
		MUI_DisposeObject(progress.win);
	}
	memset(&progress, 0, sizeof(progress));

	if (modal)
		set(app->WI_MainWindow, MUIA_Window_Sleep, FALSE);
}

BOOL progressPump(void)
{
	ULONG cancelled = 0;

	DoMethod(app->App, MUIM_Application_InputBuffered);

	if (progress.cancel)
		get(progress.cancel, MUIA_UserData, &cancelled);
	return cancelled == 0;
}
