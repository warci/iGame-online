##########################################################################
# Makefile for iGame on Windows using VBCC. Assumed VBCC installation directory is D:\vbcc.
#-------------------------------------------------------------------------
# To compile an iGame flat executable  using this makefile, run:
#  make -f Makefile.Windows.mak
#-------------------------------------------------------------------------
##########################################################################

##########################################################################
# Default: Build iGame with standard optimizations and 000 support
##########################################################################
all: iGame

##########################################################################
# Set up version and date properties
##########################################################################

DATE = $(shell date --iso=date)

##########################################################################
# Compiler settings
##########################################################################
CC			= vc
LINK		= vc

INCLUDES	= -I$(NDK_INC) -I$(MUI38_INC)

CFLAGS		= -c +aos68k -dontwarn=-1 -O2 -c99 -DCPU_VERS=68000 -DRELEASE_DATE=$(DATE)
CFLAGS_030	= -c +aos68k -cpu=68030 -dontwarn=-1 -O2 -c99 -DCPU_VERS=68030 -DRELEASE_DATE=$(DATE)
CFLAGS_040	= -c +aos68k -cpu=68040 -dontwarn=-1 -O2 -c99 -DCPU_VERS=68040 -DRELEASE_DATE=$(DATE)
CFLAGS_060	= -c +aos68k -cpu=68060 -dontwarn=-1 -O2 -c99 -DCPU_VERS=68060 -DRELEASE_DATE=$(DATE)

##########################################################################
# Builder settings
##########################################################################
#MKLIB			= join
LIBFLAGS		= +aos68k -lamiga -lauto -o

##########################################################################
# Object files which are part of iGame
##########################################################################

include make_includes/obj_files.inc

##########################################################################
# Rule for building
##########################################################################

include make_includes/rules.inc

##########################################################################
# catalog files
##########################################################################

include make_includes/catalogs.inc

catalogs/%/iGame.catalog: catalogs/%/iGame.ct catalogs/iGame.cd
	flexcat catalogs/iGame.cd $< CATALOG $@ FILL QUIET

##########################################################################
# object files (generic 000)
##########################################################################

include make_includes/obj_000.inc

##########################################################################
# object files (030)
##########################################################################

include make_includes/obj_030.inc

##########################################################################
# object files (040)
##########################################################################

include make_includes/obj_040.inc

##########################################################################
# object files (060)
##########################################################################

include make_includes/obj_060.inc

##########################################################################
# generic build options
##########################################################################

clean:
	del iGame iGame.* src\funcs*.o src\iGameGUI*.o src\iGameMain*.o src/strfuncs*.o src\iGame_cat*.o $(catalog_files)

# pack everything in a nice lha file
release:
	cp required_files iGame_rel/iGame-$(DATE) -r
	cp alt_icons iGame_rel/iGame-$(DATE)/Icons -r
	cp iGame_rel/iGame-$(DATE)/igame_drawer_3.0.info iGame_rel/iGame-$(DATE).info
	mv iGame_rel/iGame-$(DATE)/igame_drawer_3.0.info iGame_rel/iGame-$(DATE)/Icons/
	mv iGame_rel/iGame-$(DATE)/igame_drawer.info iGame_rel/iGame-$(DATE)/Icons/
	if [ -f "iGame" ]; then cp iGame iGame_rel/iGame-$(DATE)/; fi
	if [ -f "iGame.030" ]; then cp iGame.030 iGame_rel/iGame-$(DATE)/; fi
	if [ -f "iGame.040" ]; then cp iGame.040 iGame_rel/iGame-$(DATE)/; fi
	if [ -f "iGame.060" ]; then cp iGame.060 iGame_rel/iGame-$(DATE)/; fi
	cd iGame_rel && lha -aq2o6 iGame-$(DATE).lha iGame-$(DATE)/ iGame-$(DATE).info

clean-release:
	rm -rf iGame_rel/iGame-$(DATE)
	rm iGame_rel/iGame-$(DATE).lha
	rm iGame_rel/iGame-$(DATE).info
