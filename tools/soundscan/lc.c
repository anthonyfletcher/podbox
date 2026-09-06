/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Loading a codec, for a tool that is not the player.
 *
 * codecs.c builds a path from CODECS_DIR and hands it here, which on the
 * player is /.rockbox/codecs and is exactly where this tool must not look:
 * those are ARM objects for the device it is plugged into. The name at the
 * end of that path is still the right one, so this keeps the name and
 * supplies its own directory -- the host codecs shipped beside the tool.
 *
 * That interception is the whole reason this file exists rather than the
 * tree's own lc-*.c: everything else here is dlopen under two names.
 ****************************************************************************/
#include <stdio.h>
#include <string.h>
#include "soundscan.h"

#ifdef _WIN32
#include <windows.h>
#define LC_OPEN(p)      ((void *)LoadLibraryA(p))
#define LC_SYM(h, n)    ((void *)GetProcAddress((HMODULE)(h), (n)))
#define LC_CLOSE(h)     FreeLibrary((HMODULE)(h))
#else
#include <dlfcn.h>
#define LC_OPEN(p)      dlopen((p), RTLD_NOW)
#define LC_SYM(h, n)    dlsym((h), (n))
#define LC_CLOSE(h)     dlclose(h)
#endif

void *lc_open(const char *filename, unsigned char *buf, size_t buf_size)
{
    const char *name = strrchr(filename, '/');
    char path[1024];
    void *handle;

    (void)buf; (void)buf_size;   /* Host objects load where the OS puts them */

    name = name != NULL ? name + 1 : filename;
    snprintf(path, sizeof (path), "%s/%s", soundscan_codec_dir(), name);

    handle = LC_OPEN(path);
    if (handle == NULL)
        fprintf(stderr, "  cannot load codec %s\n", path);

    return handle;
}

void *lc_get_header(void *handle)
{
    void *sym = LC_SYM(handle, "__header");

    if (sym == NULL)
        sym = LC_SYM(handle, "___header");

    return sym;
}

void lc_close(void *handle)
{
    if (handle != NULL)
        LC_CLOSE(handle);
}
