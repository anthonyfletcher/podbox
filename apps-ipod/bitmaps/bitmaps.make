#             __________               __   ___.
#   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
#   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
#   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
#   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
#                     \/            \/     \/    \/            \/
# $Id$
#

BITMAPDIR = $(COREAPPSDIR)/bitmaps
BMPINCDIR = $(BUILDDIR)/bitmaps

INCLUDES += -I$(BMPINCDIR)

ifneq ($(strip $(BMP2RB_MONO)),)
BMP = $(call preprocess, $(BITMAPDIR)/mono/SOURCES)
endif
ifneq ($(strip $(BMP2RB_NATIVE)),)
BMP += $(call preprocess, $(BITMAPDIR)/native/SOURCES)
endif
# No remote_mono / remote_native stanzas: these targets have no remote LCD, so
# tools/configure leaves BMP2RB_REMOTEMONO and BMP2RB_REMOTENATIVE empty and
# those SOURCES were never read. Both directories have been deleted.

BMPOBJ = $(call full_path_subst,$(ROOTDIR)/%.bmp,$(BUILDDIR)/%.o,$(BMP))

# The generated headers the core build includes. bmp2rb emits these as a side
# effect of compiling BMPOBJ, so each one needs an entry here to give make a
# rule for it. This list must track the .bmp files in mono/SOURCES and
# native/SOURCES exactly: an entry with no bitmap can never be produced, and a
# bitmap with no entry breaks any source that includes its header.
BMPHFILES = $(BMPINCDIR)/default_icons.h \
	$(BMPINCDIR)/podbox_icon_album.h \
	$(BMPINCDIR)/podbox_icon_artist.h \
	$(BMPINCDIR)/podbox_icon_track.h \
	$(BMPINCDIR)/podboxcredits.h \
	$(BMPINCDIR)/podboxnoart.h

$(BMPHFILES): $(BMPOBJ)

# pattern rules to create .c files from .bmp, one for each subdir:
$(APPSBUILDDIR)/bitmaps/mono/%.c: $(COREAPPSDIR)/bitmaps/mono/%.bmp $(TOOLSDIR)/bmp2rb
	$(SILENT)mkdir -p $(dir $@) $(BMPINCDIR)
	$(call PRINTS,BMP2RB $(<F))$(BMP2RB_MONO) -b -h $(BMPINCDIR) $< > $@

$(APPSBUILDDIR)/bitmaps/native/%.c: $(COREAPPSDIR)/bitmaps/native/%.bmp $(TOOLSDIR)/bmp2rb
	$(SILENT)mkdir -p $(dir $@) $(BMPINCDIR)
	$(call PRINTS,BMP2RB $(<F))$(BMP2RB_NATIVE) -b -h $(BMPINCDIR) $< > $@

$(APPSBUILDDIR)/bitmaps/remote_mono/%.c: $(COREAPPSDIR)/bitmaps/remote_mono/%.bmp $(TOOLSDIR)/bmp2rb
	$(SILENT)mkdir -p $(dir $@) $(BMPINCDIR)
	$(call PRINTS,BMP2RB $(<F))$(BMP2RB_REMOTEMONO) -b -h $(BMPINCDIR) $< > $@

$(APPSBUILDDIR)/bitmaps/remote_native/%.c: $(COREAPPSDIR)/bitmaps/remote_native/%.bmp $(TOOLSDIR)/bmp2rb
	$(SILENT)mkdir -p $(dir $@) $(BMPINCDIR)
	$(call PRINTS,BMP2RB $(<F))$(BMP2RB_REMOTENATIVE) -b -h $(BMPINCDIR) $< > $@
