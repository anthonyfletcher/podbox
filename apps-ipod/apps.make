#             __________               __   ___.
#   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
#   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
#   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
#   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
#                     \/            \/     \/    \/            \/
# $Id$
#

# The apps/ include path is owned here, deliberately, rather than by
# tools/configure's APPEXTRA variable. APPEXTRA was set to "recorder:gui:radio"
# for these targets, which is both stale (apps/radio/ was removed long ago) and
# harmful: putting apps/gui and apps/recorder on the search path let any file
# include "splash.h" or "bmp.h" and get a header from a directory it never
# named. Includes are now written relative to apps/ ("gui/splash.h"), so the
# path needs only apps/ itself.
#
# apps/api comes first: it holds forwarding stubs for the handful of apps/
# headers that firmware/ and lib/ include by bare name. Listing it first means
# the build exercises those stubs, so a broken one fails here rather than in a
# later refactor. See api/README.md.
INCLUDES += -I$(APPSDIR)/api -I$(APPSDIR)

# apps/sim holds the simulator shims. apps-ipod/ carries no SIMULATOR
# conditionals, so the declarations a sim build is missing have to arrive
# without editing any include line: sim/include holds shadows of the two
# headers that stop declaring things, and they have to be found FIRST.
#
# Hence the prepend rather than the usual +=. firmware.make is included before
# this file in tools/root.make, so -I$(FIRMDIR)/include and the target include
# path have already claimed both names. Only uisimulator.make is read after
# this, and it appends, so nothing here is lost.
#
# A forced include (-include) would be simpler and is wrong: INCLUDES also
# reaches PPCFLAGS, which preprocesses every SOURCES file, and the header's
# declarations would be emitted into those file lists as bogus source names.
#
# Gated on APP_TYPE so a hardware build sees no new flag at all.
ifeq ($(APP_TYPE),sdl-sim)
INCLUDES := -I$(APPSDIR)/sim/include $(INCLUDES)
endif

SRC += $(call preprocess, $(APPSDIR)/SOURCES)

# apps/features.txt is a file that (is preprocessed and) lists named features
# based on defines in the config-*.h files. The named features will be passed
# to genlang and thus (translated) phrases can be used based on those names.
# button.h is included for the HAS_BUTTON_HOLD define.
#
# Kludge: depends on config.o which only depends on config-*.h to have config.h
# changes trigger a genlang re-run
#

ifneq (,$(USE_LTO))
$(APPSBUILDDIR)/features: PPCFLAGS += -DUSE_LTO
endif

$(APPSBUILDDIR)/features: $(APPSDIR)/features.txt  $(BUILDDIR)/firmware/common/config.o
	$(SILENT)mkdir -p $(APPSBUILDDIR)
	$(SILENT)mkdir -p $(BUILDDIR)/lang
	$(call PRINTS,PP $(<F))
	$(SILENT)$(CC) $(PPCFLAGS) \
                 -E -P -imacros "config.h" -imacros "button.h" -x c $< | \
		grep -v "^#" | grep -v "^ *$$" > $(APPSBUILDDIR)/features; \

$(APPSBUILDDIR)/genlang-features:  $(APPSBUILDDIR)/features
	$(call PRINTS,GEN $(subst $(BUILDDIR)/,,$@))tr \\n : < $< > $@

# The core credits screen (apps/credits.c) #includes this generated name list.
# The rule used to live in apps/plugins/plugins.make, which meant credits.raw
# was only generated when ENABLEDPLUGINS=yes -- a tie to the dead plugin build
# that would have silently stopped generating it if that flag ever flipped.
# Nothing about it is plugin-specific, so it lives here now.
# credits.pl is a prerequisite as well as the tool: without it, editing the
# script leaves a stale credits.raw in place until docs/CREDITS happens to
# change.
$(BUILDDIR)/credits.raw credits.raw: $(DOCSDIR)/CREDITS $(APPSDIR)/plugins/credits.pl
	$(call PRINTS,Create credits.raw)perl $(APPSDIR)/plugins/credits.pl < $< > $(BUILDDIR)/$(@F)

$(APPSBUILDDIR)/credits.o: $(BUILDDIR)/credits.raw

ASMDEFS_SRC += $(APPSDIR)/system/core_asmdefs.c
