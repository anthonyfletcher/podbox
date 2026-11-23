# The WAV codec, linked into the simulator built for the browser (Theme IDE).
#
# A browser cannot load a codec as a shared object the way the simulator
# does, so this one is built into the binary, and load_code-emscripten.c
# hands out its header for wav.codec. The names a codec exports -- its
# header, entry points and api pointer, and the codec helpers -- are
# prefixed so none meets the core's. The helpers are wasmcodec_lib.c's, not
# codeclib.c's, which would replace the core's libc.

WASMCODEC_DIR := $(ROOTDIR)/lib/rbcodec/codecs
WASMCODEC_SRC := $(WASMCODEC_DIR)/codec_crt0.c $(WASMCODEC_DIR)/wav.c \
                 $(APPSDIR)/sim/wasmcodec_lib.c \
                 $(call preprocess, $(WASMCODEC_DIR)/libpcm/SOURCES)
WASMCODEC_OBJ := $(addprefix $(BUILDDIR)/wasmcodec/, \
                   $(notdir $(WASMCODEC_SRC:.c=.o)))
WASMCODEC_LIB := $(BUILDDIR)/wasmcodec/libwasmcodec.a

WASMCODEC_NAMES := ci __header codec_start codec_main codec_run _start \
    codec_init codec_set_replaygain
WASMCODEC_FLAGS := $(foreach n,$(WASMCODEC_NAMES),-D$(n)=wavcodec_$(n)) \
    -I$(WASMCODEC_DIR) -I$(WASMCODEC_DIR)/lib -DCODEC

CORE_LIBS += $(WASMCODEC_LIB)

$(WASMCODEC_LIB): $(WASMCODEC_OBJ)
	$(SILENT)$(shell rm -f $@)
	$(call PRINTS,AR $(@F))$(AR) rcs $@ $^ >/dev/null

# make.dep names objects by their source's path, not these; so each
# object writes its own header dependencies.
WASMCODEC_CC = $(call PRINTS,CC $(subst $(ROOTDIR)/,,$<))$(CC) $(CFLAGS) \
    $(RBCODEC_CFLAGS) $(WASMCODEC_FLAGS) -MMD -MP -c $< -o $@

-include $(WASMCODEC_OBJ:.o=.d)

$(BUILDDIR)/wasmcodec/%.o: $(WASMCODEC_DIR)/%.c
	$(SILENT)mkdir -p $(dir $@)
	$(WASMCODEC_CC)

$(BUILDDIR)/wasmcodec/%.o: $(WASMCODEC_DIR)/libpcm/%.c
	$(SILENT)mkdir -p $(dir $@)
	$(WASMCODEC_CC)

$(BUILDDIR)/wasmcodec/%.o: $(APPSDIR)/sim/%.c
	$(SILENT)mkdir -p $(dir $@)
	$(WASMCODEC_CC)
