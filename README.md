## Description

iGame is a frontend to launching WHDLoad games and demos for the Amiga. It is based on Magic User Interface and it works on AmigaOS 3.x Classic Amiga computers.

http://winterland.no-ip.org/igame/

![Alt text](/igame_screen.png?raw=true "iGame screenshot")

## A tribute to iGame, and where this version is going

iGame has been the friendly face of many WHDLoad collections for close to twenty years. Emmanuel Vasilakis
(MrZammler) started it in 2005, and George Sokianos (walkero), the translators and many other contributors
kept it alive and growing ever since. This version stands entirely on their work: the games list, the
repository scanning, the filters, igame.data, the translations and the launching of games are all theirs.
Thank you for building something so useful and keeping it open for everyone.

The plan for this version is to grow iGame into something closer to a **Steam client for the Amiga**: one place
to browse the games that exist, read about them, see what others think of them, install them with a click and
play them. The first steps are in place:

 - The games list includes the WHDLoad games available online, installed with a double click
 - A game details pane shows screenshots, credits, information, categories, scores, reviews and comments for the
   selected game, loaded from the [OpenRetro game database](https://openretro.org)

## Vibe coded with Claude

To be clear up front: **the changes in this version are vibe coded with [Claude](https://claude.ai)**, Anthropic's
AI assistant, working in Claude Code. That covers the online games, the https support, the game details pane,
the screenshot browser, the cache and this README. A human described what was wanted, tried every build on an
emulated Amiga and reported back what worked and what didn't; Claude wrote and fixed the code.

The code has been tested in FS-UAE (A4000, 68060, MUI 5, AmiSSL 5) and not yet on real hardware. Bug reports are
very welcome.

## Builds and releases

The iGame_rel folder should contain builds that are done at some point in time, following any major code changes.

## Installing

Just place the iGame folder anywhere you want on your drive. If a required library is missing, an alert window
will show up.

### Required

 - Kickstart 2.04 or higher
 - Workbench 2.1 or higher
 - MUI 3.8 or higher (MUI 5 recommended)
 - icon.library v37+ (v44+ recommended)
 - TextEditor.mcc
 - NList package: NList.mcc, NListview.mcc and NFloattext.mcc

### For the game details pane

 - AmiSSL 5 or newer. OpenRetro is only reachable over https
 - A TCP/IP stack that provides bsdsocket.library (Roadshow, AmiTCP, Miami, Genesis...)
 - For screenshots, either Guigfx.mcc with guigfx.library and render.library, or Dtpic.mui (part of MUI 4 and
   newer)
 - A PNG datatype, as the screenshots from OpenRetro are PNG files

### Optional

 - Urltext.mcc, for clickable links in the Information window
 - LhA in C:, to install games from the online repository

Make sure your installation contains MUI and the above libraries. You can find the latest versions on Aminet.

**Old MUI classes:** some systems still carry classes from MUI 2 or 3 (version 7.x, from 1994) in
`SYS:Classes/MUI/`, next to a newer MUI. MUI may load those instead of its own, and they fail under MUI 5.
iGame copes where it can, but you get the best result by renaming that drawer, i.e.
`Rename SYS:Classes/MUI SYS:Classes/MUI.old`, and rebooting.

## Online games

The games list comes prefilled with the WHDLoad games of the Retroplay WHDLoad packs, hosted on the Turran FTP
(http://ftp2.grandis.nu/turran/FTP/Retroplay%20WHDLoad%20Packs/). Games that are not installed yet are shown in italics.
When you start one of them, iGame asks for confirmation, downloads the archive, extracts it with LhA into your first
games repository and starts it. The archives are downloaded over plain http, so AmiSSL is not needed for this.

Use **Actions > Update online games list** to refresh the list of available games. The filters "Installed" and
"Not installed" in the list filter show only one kind.

These settings can be changed in igame.prefs:

 - `show_online_games=0` hides the online games (saves memory on small systems)
 - `download_path=DH1:Games` installs downloaded games there instead of the first repository
 - `online_url=http://...` uses another online repository. It must be a plain http folder listing with .lha archives, directly or in sub folders

## Game details

Select a game and the pane on the right shows its details from the [OpenRetro game database](https://openretro.org),
whose text is in the public domain for the use of the Amiga community:

 - **Main**: the title, a screenshot (use the arrows to browse through all of them), the credits, information,
   categories, description, and links to OpenRetro, Hall of Light and Lemon Amiga
 - **Community**: the user score, press reviews and the comments

Nothing is downloaded in advance: a game is looked up when you select it, its comments when you open the
Community tab and each screenshot when it is shown. Progress shows in the status field at the bottom of the window.
Selecting another game stops a lookup that is still running.

Everything downloaded is kept in the `cache` drawer next to iGame, so a game is fetched only once. Nothing
expires: delete the drawer, or single files in it, to get fresh details. Screenshots larger than the screenshot
size in the settings are fetched in a scaled down version.

A game that has its own igame.iff screenshot keeps showing that one. The genres list that used to be on the
right side of the main window is hidden for now; it can be built back in with `GENRES_PANEL_ENABLED` in
`src/iGameExtern.h`.

## Compiling

Please check the [Wiki](https://github.com/MrZammler/iGame/wiki/Compiling-iGame).

This version builds for AmigaOS 3.x (68k) with the `walkero/docker4amigavbcc` Docker image, which has VBCC, the
NDK 3.2 (including the AmiSSL headers), MUI 5 and the Roadshow SDK:

```
docker run --rm --platform linux/amd64 -v "$PWD":/work -w /work \
    walkero/docker4amigavbcc:latest-m68k make -f Makefile.docker CPU=060
```

`CPU` can be 000, 030, 040 or 060.

## Contributing

Please do :-)

iGame is being translated using Weblate, a web tool designed to ease translating for both developers and translators. If you would like to contribute to translation of iGame, you need to register on [https://translate.igame.rocks](https://translate.igame.rocks). Once you have activated your account, proceed to the translation section.

[![Translation status](https://translate.igame.rocks/widget/igame/igame-app/287x66-grey.png)](https://translate.igame.rocks/engage/igame/)

If you would like to contribute with code, please create pull requests to this repository.

## Usage

Docs for the usage of iGame can be found inside the guide directory. The guide will be packed inside iGame releases and
can also be brought up by pressing the Help key when running iGame.
