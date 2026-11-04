/* pbgvm: runs a cart's script on the host, headless -- the engine's own
 * VM, built for this machine, for the SDK's tests.
 *
 *   pbgvm cart.pbg [--ticks n] [--seed s] [--ceiling units] [--units]
 *                  [--shot file.png] [--dump x,y,w,h] [--menu i]...
 *
 * printh() lines go to stdout. A runtime error goes to stderr as
 * file:line: message (in function), and the exit status is 1. With
 * --units, the units of each call follow on stderr. After the last tick,
 * --shot saves the panel as the player shows it, the 160x120 screen
 * doubled to 320x240, and --dump prints a rectangle of the screen as one
 * hex digit a pixel (the colour's low four bits), a row a line. Each
 * --menu i then chooses pause-menu item i with SELECT, printing the items
 * before and the answer: "menu: <label>|<label>..." and "chose i: r". */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "games/arcade/arcade.h"

struct mem_in
{
    const unsigned char *b;
    uint32_t len, at;
};

static uint32_t mem_read(void *ctx, void *buf, uint32_t n)
{
    struct mem_in *in = ctx;
    uint32_t left = in->len - in->at;
    if (n > left)
        n = left;
    if (buf)
        memcpy(buf, in->b + in->at, n);
    in->at += n;
    return n;
}

static void print(void *ctx, const char *s, int len)
{
    (void)ctx;
    fwrite(s, 1, (size_t)len, stdout);
    fputc('\n', stdout);
}

static void date(void *ctx, int32_t out[7])
{
    (void)ctx;
    static const int32_t fixed[7] = { 2026, 11, 3, 12, 0, 0, 2 };
    memcpy(out, fixed, sizeof(fixed));
}

/* ---- a PNG, uncompressed, so no zlib is needed -------------------------- */

static uint32_t crc_table[256];

static uint32_t crc(uint32_t c, const unsigned char *b, size_t n)
{
    if (!crc_table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t k = i;
            for (int j = 0; j < 8; j++)
                k = k & 1 ? 0xedb88320u ^ (k >> 1) : k >> 1;
            crc_table[i] = k;
        }
    c = ~c;
    while (n--)
        c = crc_table[(c ^ *b++) & 0xff] ^ (c >> 8);
    return ~c;
}

static void be32(unsigned char *b, uint32_t v)
{
    b[0] = (unsigned char)(v >> 24);
    b[1] = (unsigned char)(v >> 16);
    b[2] = (unsigned char)(v >> 8);
    b[3] = (unsigned char)v;
}

static void chunk(FILE *f, const char *tag, const unsigned char *d, size_t n)
{
    unsigned char h[8];
    be32(h, (uint32_t)n);
    memcpy(h + 4, tag, 4);
    fwrite(h, 1, 8, f);
    fwrite(d, 1, n, f);
    uint32_t c = crc(crc(0, (const unsigned char *)tag, 4), d, n);
    be32(h, c);
    fwrite(h, 1, 4, f);
}

static int write_png(const char *path, const unsigned char *rgb, int w, int h)
{
    static const unsigned char sig[8] = { 137, 'P', 'N', 'G', 13, 10, 26, 10 };
    size_t row = (size_t)w * 3 + 1, raw = row * (size_t)h;
    size_t blocks = (raw + 65534) / 65535;
    unsigned char *z = malloc(raw + blocks * 5 + 6);
    unsigned char *line = malloc(raw);
    if (!z || !line)
        return 0;
    for (int y = 0; y < h; y++) {
        line[y * row] = 0;
        memcpy(line + y * row + 1, rgb + (size_t)y * w * 3, (size_t)w * 3);
    }
    size_t n = 0, at = 0;
    uint32_t s1 = 1, s2 = 0;
    z[n++] = 0x78;
    z[n++] = 0x01;
    while (at < raw) {
        size_t k = raw - at < 65535 ? raw - at : 65535;
        z[n++] = at + k == raw;
        z[n++] = (unsigned char)k;
        z[n++] = (unsigned char)(k >> 8);
        z[n++] = (unsigned char)~k;
        z[n++] = (unsigned char)(~k >> 8);
        memcpy(z + n, line + at, k);
        for (size_t i = 0; i < k; i++) {
            s1 = (s1 + line[at + i]) % 65521;
            s2 = (s2 + s1) % 65521;
        }
        n += k;
        at += k;
    }
    be32(z + n, (s2 << 16) | s1);
    n += 4;
    unsigned char ihdr[13];
    be32(ihdr, (uint32_t)w);
    be32(ihdr + 4, (uint32_t)h);
    memcpy(ihdr + 8, "\x08\x02\0\0\0", 5);
    FILE *f = fopen(path, "wb");
    if (!f)
        return 0;
    fwrite(sig, 1, 8, f);
    chunk(f, "IHDR", ihdr, 13);
    chunk(f, "IDAT", z, n);
    chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(z);
    free(line);
    return 1;
}

/* RGB565, as the players' panels take it. */
static uint16_t pack565(uint32_t c)
{
    return (uint16_t)(((c >> 8) & 0xf800) | ((c >> 5) & 0x07e0) |
                      ((c >> 3) & 0x001f));
}

/* The panel as present.c makes it, checked pixel for pixel against the
 * screen doubled the plain way; 0 when they differ. */
static uint16_t panel[320 * 240];

static int present_panel(struct vm *vm)
{
    const struct arcade_screen *s = arcade_screen(vm);
    static uint32_t lut2[256];
    arcade_present_lut(s, lut2, pack565);
    arcade_present_rows(s, lut2, panel, 320, 0, s->height - 1);
    for (int y = 0; y < 240; y++)
        for (int x = 0; x < 320; x++)
            if (panel[y * 320 + x] !=
                pack565(s->rgb[s->px[(y / 2) * s->width + x / 2]]))
                return 0;
    return 1;
}

/* The panel: the screen, each pixel doubled. */
static int shot(struct vm *vm, const char *path)
{
    const struct arcade_screen *s = arcade_screen(vm);
    static unsigned char rgb[320 * 240 * 3];
    if (!present_panel(vm)) {
        fprintf(stderr, "present.c does not match the screen\n");
        return 0;
    }
    for (int y = 0; y < 240; y++)
        for (int x = 0; x < 320; x++) {
            uint32_t c = s->rgb[s->px[(y / 2) * s->width + x / 2]];
            unsigned char *o = rgb + ((size_t)y * 320 + x) * 3;
            o[0] = (unsigned char)(c >> 16);
            o[1] = (unsigned char)(c >> 8);
            o[2] = (unsigned char)c;
        }
    return write_png(path, rgb, 320, 240);
}

static void dump(struct vm *vm, const char *spec)
{
    int x, y, w, h;
    const struct arcade_screen *s = arcade_screen(vm);
    if (sscanf(spec, "%d,%d,%d,%d", &x, &y, &w, &h) != 4)
        return;
    for (int j = y; j < y + h; j++) {
        for (int i = x; i < x + w; i++) {
            int c = i >= 0 && j >= 0 && i < s->width && j < s->height
                ? s->px[j * s->width + i] & 15 : 0;
            putchar("0123456789abcdef"[c]);
        }
        putchar('\n');
    }
}

static int report(struct vm *vm)
{
    const char *file, *fn;
    int line;
    const char *msg = arcade_error(vm, &file, &line, &fn);
    if (file)
        fprintf(stderr, "%s:%d: %s (in %s)\n", file, line, msg, fn);
    else
        fprintf(stderr, "%s\n", msg);
    return 1;
}

int main(int argc, char **argv)
{
    const char *path = NULL;
    long ticks = 0;
    unsigned long long seed = 1;
    int units = 0;
    unsigned long ceiling = 0;
    const char *shot_path = NULL, *dump_spec = NULL;
    int menu_picks[16], npicks = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--ticks") && i + 1 < argc)
            ticks = atol(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc)
            seed = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--ceiling") && i + 1 < argc)
            ceiling = strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc)
            shot_path = argv[++i];
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc)
            dump_spec = argv[++i];
        else if (!strcmp(argv[i], "--menu") && i + 1 < argc && npicks < 16)
            menu_picks[npicks++] = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--units"))
            units = 1;
        else
            path = argv[i];
    }
    if (!path) {
        fprintf(stderr, "usage: pbgvm cart.pbg [--ticks n] [--seed s] "
                "[--ceiling units] [--units] [--shot file.png] "
                "[--dump x,y,w,h]\n");
        return 2;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return 2;
    }
    static unsigned char file[4 << 20];
    struct mem_in in = { file, (uint32_t)fread(file, 1, sizeof(file), f), 0 };
    fclose(f);

    /* The player's limit: the 5G's whole app buffer. */
    static uint64_t arena[512 * 1024 / 8];
    struct arcade_host host = { NULL, print, NULL, date };
    struct vm *vm = arcade_load(arena, sizeof(arena), mem_read, &in, &host,
                                seed);
    if (!vm) {
        fprintf(stderr, "no memory\n");
        return 1;
    }
    if (arcade_error(vm, NULL, NULL, NULL))
        return report(vm);
    if (ceiling)
        arcade_set_ceiling(vm, (uint32_t)ceiling);
    if (!arcade_start(vm))
        return report(vm);
    if (units)
        fprintf(stderr, "start %u\n", (unsigned)arcade_units(vm));
    for (long t = 0; t < ticks; t++) {
        if (!arcade_tick(vm, 1))
            return report(vm);
        if (units)
            fprintf(stderr, "tick %ld %u\n", t + 1, (unsigned)arcade_units(vm));
        arcade_idle(vm);
    }
    if (dump_spec)
        dump(vm, dump_spec);
    for (int k = 0; k < npicks; k++) {
        char label[64];
        printf("menu:");
        for (int i = 1; i <= 5; i++)
            if (arcade_menu_item(vm, i, label, sizeof(label)))
                printf(" %d=%s", i, label);
        printf("\n");
        int r = arcade_menu_choose(vm, menu_picks[k], 4);
        printf("chose %d: %d\n", menu_picks[k], r);
        if (r < 0)
            return report(vm);
    }
    if (shot_path && !shot(vm, shot_path)) {
        fprintf(stderr, "could not write %s\n", shot_path);
        return 2;
    }
    fflush(stdout);
    return 0;
}
