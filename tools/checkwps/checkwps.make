#             __________               __   ___.
#   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
#   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
#   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
#   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
#                     \/            \/     \/    \/            \/

ENGLISH := english

# Use global GCCOPTS
# __PCTOOL__ makes font.h skip the generated sysfont.h, which this build has
# no rule for; the height is the 08-Schumacher-Clean system font's.
GCCOPTS += -D__PCTOOL__ -DCHECKWPS -DSYSFONT_HEIGHT=8

CHECKWPS_SRC = $(call preprocess, $(TOOLSDIR)/checkwps/SOURCES)
CHECKWPS_OBJ = $(call c2obj,$(CHECKWPS_SRC)) $(BUILDDIR)/lang/lang_core.o

OTHER_SRC += $(CHECKWPS_SRC)

# The skin parser is the fork's, not upstream's, so the application-layer
# include path has to be the one apps-ipod/apps.make sets: api/ first, then
# the directory itself. APPSDIR is this tool's own directory here, so it is
# COREAPPSDIR that names the application layer.
# checkwps/include holds shadows of the three firmware headers that stop
# declaring things under __PCTOOL__, and has to be found FIRST for their
# #include_next to work.
INCLUDES = -I$(TOOLSDIR)/checkwps/include \
           -I$(COREAPPSDIR)/api \
           -I$(COREAPPSDIR) \
           -I$(FIRMDIR)/kernel/include \
           -I$(ROOTDIR)/firmware/export \
           -I$(ROOTDIR)/firmware/include \
           -I$(ROOTDIR)/firmware/target/hosted \
           -I$(ROOTDIR)/firmware/target/hosted/sdl \
           -I$(ROOTDIR)/lib/fixedpoint \
           -I$(ROOTDIR)/lib/rbcodec \
           -I$(ROOTDIR)/lib/rbcodec/metadata \
           -I$(ROOTDIR)/lib/rbcodec/dsp \
           -I$(APPSDIR) \
           -I$(BUILDDIR) \
           -I$(BUILDDIR)/lang \
           $(TARGET_INC)

.SECONDEXPANSION: # $$(OBJ) is not populated until after this

$(BUILDDIR)/$(BINARY): $$(CHECKWPS_OBJ) $$(CORE_LIBS)
	@echo LD $(BINARY)
	$(SILENT)$(HOSTCC) -o $@ $+ $(INCLUDE) $(GCCOPTS)  \
	-L$(BUILDDIR)/lib $(call a2lnk,$(CORE_LIBS))

$(BUILDDIR)/fontbundle.h: $(ROOTDIR)/fonts/*bdf
	@echo FONTBUNDLE
	$(SILENT)echo "static unsigned char* bundledfonts[] = {" > $@
	$(SILENT)ls $(ROOTDIR)/fonts/*bdf | perl -pne 's|.*/(\d+-.*)\.bdf|  "$$1",|;' >> $@
	$(SILENT)echo "  NULL, " >> $@
	$(SILENT)echo "};" >> $@

#### Everything below is hacked in from apps.make and lang.make

$(BUILDDIR)/apps/features: $(COREAPPSDIR)/features.txt
	$(SILENT)mkdir -p $(BUILDDIR)/apps
	$(SILENT)mkdir -p $(BUILDDIR)/lang
	$(call PRINTS,PP $(<F))
	$(SILENT)$(CC) $(PPCFLAGS) \
		-E -P -imacros "config.h" -imacros "button.h" -x c $< | \
	grep -v "^#" | grep -v "^ *$$" > $(BUILDDIR)/apps/features; \

$(BUILDDIR)/apps/genlang-features:  $(BUILDDIR)/apps/features
	$(call PRINTS,GEN $(subst $(BUILDDIR)/,,$@))tr \\n : < $< > $@

$(BUILDDIR)/lang_enum.h: $(BUILDDIR)/lang/lang.h $(TOOLSDIR)/genlang

$(BUILDDIR)/lang/lang.h: $(COREAPPSDIR)/lang/$(ENGLISH).lang $(BUILDDIR)/apps/features $(TOOLSDIR)/genlang $(BUILDDIR)/apps/genlang-features
	$(call PRINTS,GEN lang.h)
	$(SILENT)$(TOOLSDIR)/genlang -e=$(COREAPPSDIR)/lang/$(ENGLISH).lang -p=$(BUILDDIR)/lang -t=$(MODELNAME):`cat $(BUILDDIR)/apps/genlang-features` $<

$(BUILDDIR)/lang/lang_core.c: $(BUILDDIR)/lang/lang.h $(TOOLSDIR)/genlang

$(BUILDDIR)/lang/lang_core.o: $(BUILDDIR)/lang/lang.h $(BUILDDIR)/lang/lang_core.c
	$(call PRINTS,CC lang_core.c)$(CC) $(CFLAGS) -c $(BUILDDIR)/lang/lang_core.c -o $@

$(BUILDDIR)/lang/max_language_size.h: $(BUILDDIR)/lang/lang.h
	$(call PRINTS,GEN $(subst $(BUILDDIR)/,,$@))
	$(SILENT)echo "#define MAX_LANGUAGE_SIZE 131072" > $@
