/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Arcade: the screen's rows converted for a 16-bit panel at twice the size.
 * Platform-free, so the host tool checks it; the player's shell owns the
 * framebuffer and the push.
 *
 * Each colour is one 32-bit word holding its 16-bit pixel twice, so a
 * source pixel is one load from the table and one store to each of the two
 * panel rows it becomes. Source pixels are read four at a time, and the
 * framebuffer is only ever written, never read back.
 ***************************************************************************/
#include "games/arcade/arcade.h"

void arcade_present_lut(const struct arcade_screen *s, uint32_t lut2[256],
                        uint16_t (*pack)(uint32_t rgb))
{
    for (int i = 0; i < 256; i++) {
        uint32_t v = pack(s->rgb[i]);
        lut2[i] = v | (v << 16);
    }
}

void arcade_present_rows(const struct arcade_screen *s,
                         const uint32_t lut2[256], void *fb, int stride,
                         int r0, int r1)
{
    int quads = s->width / 4;
    for (int y = r0; y <= r1; y++) {
        const uint32_t *src = (const uint32_t *)(s->px + y * s->width);
        uint32_t *d0 = (uint32_t *)((uint16_t *)fb + (y * 2) * stride);
        uint32_t *d1 = (uint32_t *)((uint16_t *)fb + (y * 2 + 1) * stride);
        for (int q = 0; q < quads; q++) {
            uint32_t p = *src++;
            uint32_t a = lut2[p & 0xff], b = lut2[(p >> 8) & 0xff];
            uint32_t c = lut2[(p >> 16) & 0xff], e = lut2[p >> 24];
            d0[0] = a; d0[1] = b; d0[2] = c; d0[3] = e;
            d1[0] = a; d1[1] = b; d1[2] = c; d1[3] = e;
            d0 += 4;
            d1 += 4;
        }
    }
}
