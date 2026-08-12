/***************************************************************************
 * Original code from RockBox
 * was: apps/codecs.c
 * Copyright (C) 2002 Björn Stenberg
 * GNU General Public License (version 2+)
 *
 * Loading and linking of .codec modules: locates the file for a format and
 * binds it to the codec API struct.
 *
 * A codec is a separately-linked binary loaded into a fixed region (codecbuf,
 * placed by the linker script). It cannot call into the core directly, so the
 * core hands it a table of function pointers -- ci, below -- and the codec
 * calls through that. Loading is therefore: read the image, sanity-check its
 * header against this build, publish the table, then call its entry point.
 *
 * Parts, in order:
 *   - ci: the API table handed to every codec
 *   - codec buffer helpers: path building, and doling out leftover codec RAM
 *   - load and call: header validation, the api writeback, run and unload
 ****************************************************************************/
#include "config.h"

#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <timefuncs.h>
#include <ctype.h>
#include <stdarg.h>
#include "string-extra.h"
#include "load_code.h"
#include "debug.h"
#include "panic.h"
#include "button.h"
#include "dir.h"
#include "file.h"
#include "kernel.h"
#include "codecs.h"
#include "lang.h"
#include "widgets/keyboard.h"
#include "buffering.h"
#include "backlight.h"
#include "storage.h"
#include "speech/talk.h"
#include "mp3data.h"
#include "powermgmt.h"
#include "system.h"
#include "sound.h"
#include "widgets/splash.h"
#include "general.h"
#include "rbpaths.h"

#define LOGF_ENABLE
#include "logf.h"

#define PREFIX(_x_) _x_

/* For PLATFORM_NATIVE this buffer is defined in *.lds files. */
extern unsigned char codecbuf[];

static size_t codec_size;

/* Initialised by position, not by field name, so the order here must match
 * struct codec_api in codecs.h exactly -- the trailing comments are the only
 * thing tying an entry to its field. Inserting anything mid-list silently
 * shifts every pointer below it, which compiles clean and fails at runtime.
 * Add to the end (and bump CODEC_API_VERSION), as the closing note says. */
struct codec_api ci = {

    0,    /* filesize */
    0,    /* curpos */
    NULL, /* id3 */
    ERR_HANDLE_NOT_FOUND, /* audio_hid */
    NULL, /* struct dsp_config *dsp */
    NULL, /* codec_get_buffer */
    NULL, /* pcmbuf_insert */
    NULL, /* set_elapsed */
    NULL, /* read_filebuf */
    NULL, /* request_buffer */
    NULL, /* advance_buffer */
    NULL, /* seek_buffer */
    NULL, /* seek_complete */
    NULL, /* set_offset */
    NULL, /* configure */
    NULL, /* get_command */
    NULL, /* loop_track */
    NULL, /* strip_filesize */

    /* kernel/ system */
#if defined(ARM_NEED_DIV0)
    __div0,
#endif
    sleep,
    yield,

#if NUM_CORES > 1
    create_thread,
    thread_thaw,
    thread_wait,
    semaphore_init,
    semaphore_wait,
    semaphore_release,
#endif

    commit_dcache,
    commit_discard_dcache,
    commit_discard_idcache,

    /* strings and memory */
    strcpy,
    strlen,
    strcmp,
    strcat,
    memset,
    memcpy,
    memmove,
    memcmp,
    memchr,
/* Must match the struct in lib/rbcodec/codecs/codecs.h, which keys this entry
 * on SIMULATOR as well -- omit it and every later member is off by one. */
#if defined(DEBUG) || defined(SIMULATOR)
    debugf,
#endif
#ifdef ROCKBOX_HAS_LOGF
    logf,
#endif

    (void *)qsort,

#ifdef RB_PROFILE
    profile_thread,
    profstop,
    __cyg_profile_func_enter,
    __cyg_profile_func_exit,
#endif


    /* new stuff at the end, sort into place next time
       the API gets incompatible */
    panicf,

};

/** Codec buffer helpers **/

void codec_get_full_path(char *path, const char *codec_root_fn)
{
    snprintf(path, MAX_PATH-1, CODECS_DIR "/" CODEC_PREFIX "%s."
            CODEC_EXTENSION, codec_root_fn);
}

/* Returns pointer to and size of free codec RAM. Aligns to CACHEALIGN_SIZE. */
void *codec_get_buffer_callback(size_t *size)
{
    void *buf = &codecbuf[codec_size];
    ssize_t s = CODEC_SIZE - codec_size;

    if (s <= 0)
        return NULL;

    *size = s;
    /* MEM_ALIGN_SIZE, not CACHEALIGN_SIZE: the latter exists only on targets
     * with a CPU cache, and cpu.h refuses to let anything else define it. */
    ALIGN_BUFFER(buf, *size, MEM_ALIGN_SIZE);

    return buf;
}

/** Load and call **/
static void *curr_handle = NULL;
static struct codec_header *c_hdr = NULL;

static int codec_load_ram(struct codec_api *api)
{
    struct lc_header *hdr;

    c_hdr = lc_get_header(curr_handle);
    hdr   = c_hdr ? &c_hdr->lc_hdr : NULL;

    if (hdr == NULL
        || (hdr->magic != CODEC_MAGIC
            )
        || hdr->target_id != TARGET_ID
#if (CONFIG_PLATFORM & PLATFORM_NATIVE)
        /* Where the codec landed only means anything when it was loaded into
         * codecbuf. A simulator gets a host shared object at an address of the
         * loader's choosing, and checking it here rejects every codec -- which
         * looks like every track finishing the instant it starts. */
        || hdr->load_addr != codecbuf
        || hdr->end_addr > codecbuf + CODEC_SIZE
#endif
        )
    {
        logf("codec header error");
        lc_close(curr_handle);
        curr_handle = NULL;
        return CODEC_ERROR;
    }

    if (hdr->api_version != CODEC_API_VERSION ||
        c_hdr->api_size > sizeof(struct codec_api))
    {
        logf("codec api version error");
        lc_close(curr_handle);
        curr_handle = NULL;
        return CODEC_ERROR;
    }

#if (CONFIG_PLATFORM & PLATFORM_NATIVE)
    codec_size = hdr->end_addr - codecbuf;
#else
    codec_size = 0;
#endif

    /* This is the link step. A codec is compiled against a `struct codec_api *`
     * of its own and exports its address in the header; writing the core's api
     * table through it is what gives the codec access to core functions. Until
     * this line runs, every core call inside the codec is a null deref. */
    *(c_hdr->api) = api;

    logf("Codec: calling entrypoint");
    return c_hdr->entry_point(CODEC_LOAD);
}

/* Loading a codec straight out of a buffer needs lc_open_from_mem(), which
 * only exists for BINFMT_ROCK -- config.h ties HAVE_CODEC_BUFFERING to that.
 * A simulator loads codecs as host shared objects, so it has no such call and
 * takes codec_load_file() below instead. */
#if defined(HAVE_CODEC_BUFFERING)
int codec_load_buf(int hid, struct codec_api *api)
{
    int rc = bufread(hid, CODEC_SIZE, codecbuf);

    if (rc < 0) {
        logf("Codec: cannot read buf handle");
        return CODEC_ERROR;
    }

    curr_handle = lc_open_from_mem(codecbuf, rc);

    if (curr_handle == NULL) {
        logf("Codec: load error");
        return CODEC_ERROR;
    }

    return codec_load_ram(api);
}
#endif /* HAVE_CODEC_BUFFERING */

int codec_load_file(const char *plugin, struct codec_api *api)
{
    char path[MAX_PATH];

    codec_get_full_path(path, plugin);

    curr_handle = lc_open(path, codecbuf, CODEC_SIZE);

    if (curr_handle == NULL) {
        logf("Codec: cannot read file");
        return CODEC_ERROR;
    }

    return codec_load_ram(api);
}

int codec_run_proc(void)
{
    if (curr_handle == NULL) {
        logf("Codec: no codec to run");
        return CODEC_ERROR;
    }

    logf("Codec: entering run state");
    return c_hdr->run_proc();
}

int codec_close(void)
{
    int status = CODEC_OK;

    if (curr_handle != NULL) {
        logf("Codec: cleaning up");
        status = c_hdr->entry_point(CODEC_UNLOAD);
        lc_close(curr_handle);
        curr_handle = NULL;
    }

    return status;
}

