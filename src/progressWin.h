/*
  progressWin.h
  Progress window for online actions - header for iGame

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

#ifndef _PROGRESS_WIN_H
#define _PROGRESS_WIN_H

#include <exec/types.h>

/*
 * A small window with a text, a gauge and a Cancel button, shown while
 * iGame works online. Only one is open at a time.
 *
 * modal: the main window sleeps meanwhile and the text also goes to the
 * status bar, i.e. for installing a game. Otherwise the main window stays
 * usable and keeps the keyboard focus, i.e. for loading game details.
 */
void progressOpen(const char *text, BOOL modal);
BOOL progressIsOpen(void);
void progressText(const char *text);
void progressGauge(LONG percent, const char *info);

/* Shows a download's progress: "12 / 40 KB", or "12 KB" without a total */
void progressBytes(ULONG received, ULONG total);

/* Keeps the GUI alive. Returns FALSE once the user pressed Cancel */
BOOL progressPump(void);

void progressClose(void);

#endif
