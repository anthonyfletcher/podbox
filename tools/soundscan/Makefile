# soundscan -- the sound index, built on a desktop against a mounted player.
#
# Not driven by tools/configure. It needs no target and no cross-compiler:
# what it does need is a simulator build, because that is where the host
# codecs come from and where autoconf.h comes from. Build one first:
#
#     ./build-sim.sh 5g win        (or 5g, for a native simulator)
#     make -C tools/soundscan win  (or make, to match)
#
# The include list and the two defines below are the simulator's own. They are
# not decoration: SIMULATOR decides the layout of struct codec_api, and the
# codecs this loads were compiled with it set. Change either and every call a
# codec makes into the tool lands on the wrong function pointer.
#
# The Windows link is -static for the same reason. The simulator links that
# way, so the codecs were built and are tested against a host with a static
# CRT; loading one of them into a dynamically linked host gives "R6031 -
# attempt to initialize the CRT more than once" partway through a library, on
# whichever codec happens to be loaded first.

ROOT   := ../..
APPS   := $(ROOT)/apps-ipod
FIRM   := $(ROOT)/firmware
RBC    := $(ROOT)/lib/rbcodec

SIMDIR ?= $(ROOT)/build-sim-ipodvideo-win32
NATDIR ?= $(ROOT)/build-sim-ipodvideo

CC     ?= gcc
WINCC  ?= x86_64-w64-mingw32-gcc

DEFINES := -DSIMULATOR -DIPOD_VIDEO -DTARGET_ID=15

INCLUDES = -I. -I$(APPS)/sim/include -I$(BUILD) -I$(BUILD)/lang \
           -I$(FIRM)/target/arm/ipod/video -I$(FIRM)/target/hosted/sdl \
           -I$(FIRM)/target/hosted -I$(FIRM)/target/arm/ipod \
           -I$(FIRM)/target/arm/pp -I$(FIRM)/target/arm \
           -I$(FIRM) -I$(FIRM)/export -I$(FIRM)/drivers -I$(FIRM)/include \
           -I$(FIRM)/kernel/include -I$(APPS)/api -I$(APPS) \
           -I$(RBC) -I$(RBC)/dsp -I$(RBC)/codecs -I$(RBC)/metadata \
           -I$(ROOT)/lib/fixedpoint

# The tool's own, the player's own, and the parts of the firmware that a
# hosted build of them needs.
SRC := soundscan.c lc.c stubs.c \
       $(APPS)/audio/codecs.c \
       $(APPS)/audio/track_decode.c \
       $(APPS)/audio/beat_hops.c \
       $(APPS)/audio/beat_track.c \
       $(APPS)/audio/beat_probe.c \
       $(APPS)/audio/chroma.c \
       $(APPS)/audio/spectrum_meter.c \
       $(APPS)/database/sound_index.c \
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

# Warnings that belong to the host's libc rather than to the code: it marks
# write() and ftruncate() warn_unused_result, and the places sound_index.c
# ignores them are ignoring them deliberately -- see the comments there.
WARN := -Wall -Wextra -Wno-unused-result -Wno-unused-parameter

# Overridable so a crash can be looked at: make native OPT="-g -O0"
OPT  ?= -O2

all: native

native: BUILD := $(NATDIR)
native: $(SRC) $(OSFILE_UNIX)
	@test -f $(NATDIR)/autoconf.h || \
	  (echo "No simulator build at $(NATDIR) -- run ./build-sim.sh 5g"; false)
	$(CC) $(OPT) -std=gnu11 $(WARN) $(DEFINES) $(INCLUDES) \
	      -o soundscan $(SRC) $(OSFILE_UNIX) -lm -ldl

win: BUILD := $(SIMDIR)
win: $(SRC) $(OSFILE_WIN)
	@test -f $(SIMDIR)/autoconf.h || \
	  (echo "No simulator build at $(SIMDIR) -- run ./build-sim.sh 5g win"; false)
	$(WINCC) $(OPT) -std=gnu11 $(WARN) $(DEFINES) $(INCLUDES) \
	      -o soundscan.exe $(SRC) $(OSFILE_WIN) -lm -static

# The tool loads the player's own codecs, built for the host by the simulator.
# They travel with it: the ones on the player are ARM objects for the device
# it is plugged into, which is the whole reason lc.c redirects the path.
package: win
	rm -rf dist
	mkdir -p dist/codecs
	cp soundscan.exe dist/
	cp $(SIMDIR)/simdisk/.rockbox/codecs/*.codec dist/codecs/
	@echo "dist/ holds soundscan.exe and $$(ls dist/codecs | wc -l) codecs"

clean:
	rm -rf soundscan soundscan.exe dist

.PHONY: all native win package clean
