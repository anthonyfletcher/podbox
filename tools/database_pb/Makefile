# database_pb -- the player's database, brought up to date from a desktop.
#
# Not driven by tools/configure, and not upstream's tools/database: that one
# builds upstream's tagcache.c, whose files this firmware does not read. This
# one compiles apps-ipod's, as tools/soundscan does its analysis, against a
# simulator build's autoconf.h:
#
#     ./build-sim.sh 5g win          (or 5g, for a native simulator)
#     make -C tools/database_pb win  (or make, to match)
#
# Only the .exe is for a player. The native build is for testing: its file
# times come out an hour off in summer, so every track looks changed.
#
# The defines are the simulator's own, so the structures it writes are laid
# out as the player's are. tagcache.c is built with DBTOOL, which gives it the
# one entry point the tool calls in place of the database thread.

ROOT   := ../..
APPS   := $(ROOT)/apps-ipod
FIRM   := $(ROOT)/firmware
RBC    := $(ROOT)/lib/rbcodec

SIMDIR ?= $(ROOT)/build-sim-ipodvideo-win32
NATDIR ?= $(ROOT)/build-sim-ipodvideo

CC     ?= gcc
WINCC  ?= x86_64-w64-mingw32-gcc

DEFINES := -DSIMULATOR -DIPOD_VIDEO -DTARGET_ID=15 -DDBTOOL

INCLUDES = -I. -I$(APPS)/sim/include -I$(BUILD) -I$(BUILD)/lang \
           -I$(FIRM)/target/arm/ipod/video -I$(FIRM)/target/hosted/sdl \
           -I$(FIRM)/target/hosted -I$(FIRM)/target/arm/ipod \
           -I$(FIRM)/target/arm/pp -I$(FIRM)/target/arm \
           -I$(FIRM) -I$(FIRM)/export -I$(FIRM)/drivers -I$(FIRM)/include \
           -I$(FIRM)/kernel/include -I$(APPS)/api -I$(APPS) \
           -I$(RBC) -I$(RBC)/dsp -I$(RBC)/codecs -I$(RBC)/metadata \
           -I$(ROOT)/lib/fixedpoint

# The tool's own, the player's database, and the parts of the firmware that a
# hosted build of it needs.
SRC := database_pb.c stubs.c \
       $(APPS)/database/tagcache.c \
       $(APPS)/database/libfile.c \
       $(APPS)/database/path_key.c \
       $(APPS)/database/db_spoken.c \
       $(APPS)/system/strutil.c \
       $(FIRM)/buflib_mempool.c \
       $(ROOT)/lib/fixedpoint/fixedpoint.c \
       $(wildcard $(RBC)/metadata/*.c) \
       $(ROOT)/uisimulator/common/filesystem-sim.c \
       $(FIRM)/common/pathfuncs.c \
       $(FIRM)/common/strlcpy.c \
       $(FIRM)/common/strmemccpy.c \
       $(FIRM)/common/strcasestr.c \
       $(FIRM)/common/unicode.c \
       $(FIRM)/common/crc32.c \
       $(FIRM)/common/itoa_buf.c \

OSFILE_UNIX := $(FIRM)/target/hosted/filesystem-unix.c
OSFILE_WIN  := $(FIRM)/target/hosted/filesystem-win32.c

WARN := -Wall -Wextra -Wno-unused-result -Wno-unused-parameter

OPT  ?= -O2

# strutil.c and the metadata readers each carry the same string_option(); on
# the player the readers are an archive, here they are linked whole.
LDFLAGS := -Wl,--allow-multiple-definition

DEPS := $(wildcard *.h $(APPS)/*.h $(APPS)/*/*.h $(APPS)/sim/include/*.h \
                   $(RBC)/*.h $(RBC)/*/*.h $(FIRM)/export/*.h \
                   $(FIRM)/include/*.h) Makefile

all: native

native: database_pb
win: database_pb.exe

database_pb: BUILD := $(NATDIR)
database_pb: $(SRC) $(OSFILE_UNIX) $(DEPS) $(wildcard $(NATDIR)/autoconf.h)
	@test -f $(NATDIR)/autoconf.h || \
	  (echo "No simulator build at $(NATDIR) -- run ./build-sim.sh 5g"; false)
	$(CC) $(OPT) -std=gnu11 $(WARN) $(DEFINES) $(INCLUDES) \
	      -o database_pb $(SRC) $(OSFILE_UNIX) -lm $(LDFLAGS)

database_pb.exe: BUILD := $(SIMDIR)
database_pb.exe: $(SRC) $(OSFILE_WIN) $(DEPS) $(wildcard $(SIMDIR)/autoconf.h)
	@test -f $(SIMDIR)/autoconf.h || \
	  (echo "No simulator build at $(SIMDIR) -- run ./build-sim.sh 5g win"; false)
	$(WINCC) $(OPT) -std=gnu11 $(WARN) $(DEFINES) $(INCLUDES) \
	      -o database_pb.exe $(SRC) $(OSFILE_WIN) -lm -static $(LDFLAGS)

clean:
	rm -f database_pb database_pb.exe

.PHONY: all native win clean
