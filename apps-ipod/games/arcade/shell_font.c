/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * FONT.WORLD's kana, Hangul and ideographs on the player: the host's glyph
 * call, from /.rockbox/games/font.bin (arcade/sdk/gen_textfont.py; Fusion
 * Pixel 8 px, SIL Open Font License 1.1). The file opens the first time a
 * cart asks for a glyph the engine does not hold -- on the 5G that may wait
 * for the disk -- and only its page table is read in: 256 pages, an offset
 * and a bit for each codepoint present. A glyph is then one 13-byte read,
 * kept in a small cache. The file closes when the cart ends.
 ****************************************************************************/
#include <stdbool.h>
#include <string.h>
#include "config.h"
#include "system.h"
#include "file.h"
#include "games/arcade/arcade.h"
#include "games/arcade/shell_font.h"

#define FONT_FILE   "/.rockbox/games/font.bin"
#define FONT_HEAD   8
#define PAGE_BYTES  36
#define CACHE       128

static int fd = -1;
static bool tried;                  /* opened, or failed to, this cart */
static uint8_t pages[256 * PAGE_BYTES];
static struct
{
    uint32_t cp;                    /* 0: empty */
    uint8_t glyph[13];
} cache[CACHE];

static bool font_open(void)
{
    uint8_t head[FONT_HEAD];
    tried = true;
    fd = open(FONT_FILE, O_RDONLY);
    if (fd < 0)
        return false;
    if (read(fd, head, FONT_HEAD) != FONT_HEAD || memcmp(head, "PBTF", 4) ||
        head[4] != 1 ||
        read(fd, pages, sizeof(pages)) != (ssize_t)sizeof(pages)) {
        close(fd);
        fd = -1;
        return false;
    }
    return true;
}

int shell_glyph(void *ctx, uint32_t cp, uint8_t out[13])
{
    (void)ctx;
    if (cp >= 0x10000 || cp == 0)
        return 0;
    uint32_t slot = (cp ^ (cp >> 7)) & (CACHE - 1);
    if (cache[slot].cp == cp) {
        memcpy(out, cache[slot].glyph, 13);
        return 1;
    }
    if (fd < 0 && (tried || !font_open()))
        return 0;
    const uint8_t *page = pages + (cp >> 8) * PAGE_BYTES;
    uint32_t base = load_le32(page), low = cp & 0xff;
    const uint8_t *bits = page + 4;
    if (!base || !(bits[low >> 3] & (1 << (low & 7))))
        return 0;
    /* its place among the page's glyphs: those present below it */
    uint32_t rank = 0;
    for (uint32_t b = 0; b < (low >> 3); b++) {
        uint8_t v = bits[b];
        while (v) {
            rank++;
            v &= (uint8_t)(v - 1);
        }
    }
    for (uint32_t b = low & ~7u; b < low; b++)
        rank += (bits[b >> 3] >> (b & 7)) & 1;
    if (lseek(fd, (off_t)(base + rank * 13), SEEK_SET) < 0 ||
        read(fd, out, 13) != 13)
        return 0;
    cache[slot].cp = cp;
    memcpy(cache[slot].glyph, out, 13);
    return 1;
}

void shell_font_end(void)
{
    if (fd >= 0)
        close(fd);
    fd = -1;
    tried = false;
    memset(cache, 0, sizeof(cache));
}
