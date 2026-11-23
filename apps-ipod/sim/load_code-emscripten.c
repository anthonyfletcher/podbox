/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Codec loading for the simulator built for the browser (Theme IDE), in
 * place of firmware/target/hosted/sdl/load_code-sdl.c. A page cannot open a
 * shared object, so the codecs it plays are linked into the binary
 * (codecs-wasm.make) and "opening" one is finding its header by name.
 ****************************************************************************/
#include "config.h"

#ifdef __EMSCRIPTEN__

#include <string.h>
#include "system.h"
#include "load_code.h"

/* CODEC_HEADER in wav.c, renamed by codecs-wasm.make */
extern const char wavcodec___header[];

void *os_lc_open(const char *ospath)
{
    const char *name = strrchr(ospath, '/');
    name = name ? name + 1 : ospath;
    if (!strcmp(name, "wav.codec"))
        return (void *)wavcodec___header;
    return NULL;
}

void *lc_get_header(void *handle)
{
    return handle;
}

void lc_close(void *handle)
{
    (void)handle;
}

#endif /* __EMSCRIPTEN__ */
