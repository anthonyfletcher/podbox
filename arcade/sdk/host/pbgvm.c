/* pbgvm: runs a cart's script on the host, headless -- the engine's own
 * VM, built for this machine, for the SDK's tests.
 *
 *   pbgvm cart.pbg [--ticks n] [--seed s] [--ceiling units] [--units]
 *                  [--shot file.png] [--dump x,y,w,h] [--dirty]
 *                  [--wav file.wav] [--buttons t:mask,...] [--skip t:n,...]
 *                  [--wheel t:clicks[:pos],...] [--playing] [--beat bpm]
 *                  [--setting name=value]... [--menu i]...
 *                  [--scores file] [--options file] [--opt t:i:v,...]
 *                  [--ui answer,...] [--speech] [--suspend-at t]
 *                  [--store dir] [--saves file] [--font font.bin]
 *                  [--pause t,...] [--memory]
 *
 * printh() lines go to stdout. A runtime error goes to stderr as
 * file:line: message (in function), and the exit status is 1. With
 * --units, the units of each call follow on stderr. After the last tick,
 * --shot saves the panel as the player shows it, the 160x120 screen
 * doubled to 320x240, and --dump prints a rectangle of the screen as one
 * hex digit a pixel (the colour's low four bits), a row a line, and
 * --dirty prints "dirty lo hi", the rows the player sends next: those
 * drawn since the last tick that drew began, a late tick drawing nothing
 * (all of them when the palette changed, as the player then sends every
 * row). --wav saves
 * the sound, 44.1 kHz stereo, a tick's worth after each tick, and click()
 * prints "click 0" or "click 1". --buttons holds button masks (1 PREV,
 * 2 NEXT, 4 MENU, 8 PLAY, 16 SELECT) from the ticks given, each until the
 * next: "30:16,31:0" presses SELECT for tick 30. --skip makes the ticks
 * given late, as the player handles a tick that runs late: "10:1" reports
 * one skipped frame before tick 10, which runs without drawing.
 * --wheel turns the wheel: "5:12:30" is 12 clicks at tick 5 with the
 * finger at 30 from then on (-1 lifts it).
 * --playing has playback.playing() report music playing, its clock a tick
 * on each tick. --beat bpm plays a steady track at that tempo for beat,
 * or with 0 each track at its own, as the IDE does: its clock follows the
 * ticks, the tracker locks a second after beat.listen(true), and presses
 * are stamped with the tick's time. --setting name=value is what
 * sys.setting(name) gives, a number where the value is one;
 * sys.translate() is English. Each --menu i then chooses pause-menu item i
 * with SELECT, printing the items before and the answer:
 * "menu: <label>|<label>..." and "chose i: r".
 * --scores keeps the cart's high scores in file, read before the start
 * and written after the last tick, in the player's own record. score.show
 * prints "show <title>", then a line for each rank, "<rank> <value>
 * <name> <date>", the marked rank starred, and an achievement earned
 * prints "achieved <title>". --options does the same for
 * the option values the owner set, and --opt sets option i to v before
 * tick t, as the pause menu does. Each ui.* dialog prints a line saying
 * what it showed. With --ui it takes the next answer at once: an index
 * for choose (-1 backs out), 1 or 0 for confirm, a number for number, the
 * text for text (~ backs out); message takes none, and with no
 * answer left each backs out. Without --ui it is worked as on the player:
 * each of its frames is a tick, taking that tick's --buttons and --wheel,
 * and when the ticks run out it is backed out of (--shot then shows it).
 * speak.say and queue print "say <text>" and "queue
 * <text>"; speak.on() is true with --speech, and nothing is ever busy.
 * --suspend-at t suspends the cart before tick t as the player's Quit
 * does, overwrites the arena, and resumes it from the copy, printing
 * "resumed" -- the run's output is then the same as one never suspended.
 * --pause pauses the game before each tick given, as the pause menu or
 * the hold switch does, for resumed(), printing "paused".
 * --store keeps store.* files in dir, as name.json, within the player's
 * bounds; without it nothing is kept. --saves keeps dset()'s numbers in
 * file, read before the start and written after the last tick. quit()
 * ends the run after its tick, printing "quit". watch() prints "watch
 * <name> <value>" each time a name's value changes; prof() sections, with
 * --units, print "prof <name> <units>" to stderr as each ends. --font gives FONT.WORLD the glyphs of
 * a font file (gen_textfont.py --file) beyond the engine's own. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#ifdef _WIN32
#include <direct.h>
#define make_dir(p) _mkdir(p)
#else
#include <sys/stat.h>
#define make_dir(p) mkdir((p), 0777)
#endif
#include "fakemusic.h"
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

/* the cart's maps and images as it plays: from the file read whole */
static const struct mem_in *cart_file;

static uint32_t cart_read(void *ctx, uint32_t at, void *buf, uint32_t n)
{
    (void)ctx;
    if (at > cart_file->len)
        return 0;
    if (n > cart_file->len - at)
        n = cart_file->len - at;
    memcpy(buf, cart_file->b + at, n);
    return n;
}

static void print(void *ctx, const char *s, int len)
{
    (void)ctx;
    fwrite(s, 1, (size_t)len, stdout);
    fputc('\n', stdout);
}

/* watch(): "watch name value" when a name's value changes. prof(): with
 * --units, "prof name units" as each section ends. */
static struct vm *prof_vm;
static int prof_units;

static void watch(void *ctx, const char *name, const char *value)
{
    static char names[32][64], values[32][128];
    static int n;
    int i;
    (void)ctx;
    for (i = 0; i < n && strcmp(names[i], name); i++)
        ;
    if (i < n && !strcmp(values[i], value))
        return;
    if (i == n && n < 32) {
        snprintf(names[n], sizeof(names[0]), "%s", name);
        n++;
    }
    if (i < 32)
        snprintf(values[i], sizeof(values[0]), "%s", value);
    printf("watch %s %s\n", name, value);
}

static void prof(void *ctx, const char *name)
{
    static char open[8][64];
    static uint32_t at[8];
    static int depth;
    (void)ctx;
    if (!prof_vm)
        return;
    uint32_t now = arcade_units_now(prof_vm);
    if (name) {
        if (depth < 8) {
            snprintf(open[depth], sizeof(open[0]), "%s", name);
            at[depth] = now;
        }
        depth++;
    } else if (depth > 0) {
        depth--;
        if (prof_units && depth < 8)
            fprintf(stderr, "prof %s %u\n", open[depth], (unsigned)(now - at[depth]));
    }
}

/* The music, the library and the beat are fakemusic.c's, shared with the
 * IDE; what a cart asks of them is printed. */
static void music_say(const char *line)
{
    printf("%s\n", line);
}

/* --setting name=value: what sys.setting(name) gives, a number where the
 * value is one; none are set otherwise. sys.translate() is English. */
static const char *settings[16];
static int nsettings;

static int fake_setting(void *ctx, const char *name, int32_t *num, char *buf,
                        int cap)
{
    (void)ctx;
    size_t len = strlen(name);
    for (int i = 0; i < nsettings; i++)
        if (!strncmp(settings[i], name, len) && settings[i][len] == '=') {
            const char *v = settings[i] + len + 1;
            char *end;
            long n = strtol(v, &end, 10);
            if (*v && !*end) {
                *num = (int32_t)n;
                return 1;
            }
            snprintf(buf, (size_t)cap, "%s", v);
            return 2;
        }
    return 0;
}

static void click(void *ctx, int beep)
{
    (void)ctx;
    printf("click %d\n", beep);
}

/* --wav: the sound, gathered tick by tick */
#define WAV_RATE 44100
static int16_t *wav;
static size_t wav_frames, wav_cap;

static void wav_tick(struct vm *vm, long tick)
{
    long rate = arcade_rate(vm);
    size_t want = (size_t)((tick + 1) * WAV_RATE / rate) - wav_frames;
    if (wav_frames + want > wav_cap) {
        wav_cap = (wav_frames + want) * 2;
        wav = realloc(wav, wav_cap * 4);
        if (!wav)
            exit(2);
    }
    arcade_audio(vm, wav + wav_frames * 2, (int)want, WAV_RATE);
    wav_frames += want;
}

static void put32(FILE *f, uint32_t v)
{
    fputc((int)(v & 0xff), f); fputc((int)(v >> 8 & 0xff), f);
    fputc((int)(v >> 16 & 0xff), f); fputc((int)(v >> 24), f);
}

static void wav_save(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        return;
    }
    uint32_t bytes = (uint32_t)(wav_frames * 4);
    fwrite("RIFF", 1, 4, f); put32(f, 36 + bytes); fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16); put32(f, 1 | 2 << 16); put32(f, WAV_RATE);
    put32(f, WAV_RATE * 4); put32(f, 4 | 16 << 16);
    fwrite("data", 1, 4, f); put32(f, bytes);
    for (size_t i = 0; i < wav_frames * 2; i++) {
        uint16_t v = (uint16_t)wav[i];
        fputc(v & 0xff, f);
        fputc(v >> 8, f);
    }
    fclose(f);
}

static void date(void *ctx, int32_t out[7])
{
    (void)ctx;
    static const int32_t fixed[7] = { 2026, 11, 3, 12, 0, 0, 2 };
    memcpy(out, fixed, sizeof(fixed));
}

/* ---- high scores --------------------------------------------------------- */

static void scores_read(struct vm *vm, const char *path)
{
    static struct arcade_scores saved;
    unsigned char b[ARCADE_SCORES_MAX];
    FILE *f = fopen(path, "rb");
    if (!f)
        return;
    int n = (int)fread(b, 1, sizeof(b), f);
    fclose(f);
    if (arcade_scores_unpack(&saved, b, n))
        arcade_scores_restore(vm, &saved);
}

static int scores_write(struct vm *vm, const char *path)
{
    unsigned char b[ARCADE_SCORES_MAX];
    int n = arcade_scores_pack(arcade_scores(vm, NULL), b, sizeof(b));
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(b, 1, (size_t)n, f) != (size_t)n) {
        if (f)
            fclose(f);
        return 0;
    }
    return fclose(f) == 0;
}

static void scores_show(struct vm *vm)
{
    int got;
    while ((got = arcade_achieved(vm)) >= 0)
        printf("achieved %s\n", arcade_scores(vm, NULL)->ach[got].title);
    int rank, i = arcade_score_shown(vm, &rank);
    if (i < 0)
        return;
    const struct arcade_scores *s = arcade_scores(vm, NULL);
    const struct arcade_board *b = &s->b[i];
    printf("show %s\n", b->title);
    for (int k = 0; k < b->count; k++) {
        char v[32];
        arcade_score_text(s, i, b->e[k].value, v, sizeof(v));
        printf("%s%d %s %s %ld\n", k + 1 == rank ? "*" : "", k + 1, v,
               b->e[k].name, (long)b->e[k].date);
    }
}

/* ---- saved numbers ------------------------------------------------------- */

/* the 256 values, then a bit each saying which are fractions */
static void saves_read(struct vm *vm, const char *path)
{
    int32_t v[256];
    uint8_t frac[32];
    FILE *f = fopen(path, "rb");
    if (!f)
        return;
    int ok = fread(v, sizeof(v), 1, f) == 1 && fread(frac, sizeof(frac), 1, f) == 1;
    fclose(f);
    if (ok)
        arcade_restore(vm, v, frac);
}

static int saves_write(struct vm *vm, const char *path)
{
    int dirty;
    const uint8_t *frac;
    const int32_t *v = arcade_save(vm, &dirty, &frac);
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(v, 256 * sizeof(int32_t), 1, f) != 1 || fwrite(frac, 32, 1, f) != 1) {
        if (f)
            fclose(f);
        return 0;
    }
    return fclose(f) == 0;
}

/* ---- options ------------------------------------------------------------- */

static void options_read(struct vm *vm, const char *path)
{
    unsigned char b[ARCADE_OPTIONS_MAX];
    FILE *f = fopen(path, "rb");
    if (!f)
        return;
    int n = (int)fread(b, 1, sizeof(b), f);
    fclose(f);
    arcade_options_unpack(vm, b, n);
}

static int options_write(struct vm *vm, const char *path)
{
    unsigned char b[ARCADE_OPTIONS_MAX];
    int n = arcade_options_pack(vm, b, sizeof(b));
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(b, 1, (size_t)n, f) != (size_t)n) {
        if (f)
            fclose(f);
        return 0;
    }
    return fclose(f) == 0;
}

/* each --opt entry for tick t: "t:i:v" */
static void options_set(struct vm *vm, const char *spec, long t)
{
    for (const char *b = spec; b && *b;) {
        long at = strtol(b, (char **)&b, 10), i = -1, v = 0;
        if (*b == ':')
            i = strtol(b + 1, (char **)&b, 10);
        if (*b == ':')
            v = strtol(b + 1, (char **)&b, 10);
        if (at == t)
            arcade_option_set(vm, (int)i, (int32_t)v);
        if (*b == ',')
            b++;
        else
            break;
    }
}

/* ---- dialogs ------------------------------------------------------------- */

static const char *ui_answers;
static struct vm *the_vm;
static long tick_now, tick_last;
static const char *shot_at_end;
static int shot_taken, dialog_said;
static int shot(struct vm *vm, const char *path);
static int input_at(long t, unsigned *held);

/* The next --ui answer into buf; 0 when there are no more */
static int ui_next(char *buf, int cap)
{
    if (!ui_answers || !*ui_answers)
        return 0;
    const char *end = strchr(ui_answers, ',');
    int n = end ? (int)(end - ui_answers) : (int)strlen(ui_answers);
    snprintf(buf, (size_t)cap, "%.*s", n, ui_answers);
    ui_answers = end ? end + 1 : "";
    return 1;
}

/* s on one line: a newline as " / " */
static void print_flat(const char *s)
{
    for (; *s; s++)
        if (*s == '\n')
            fputs(" / ", stdout);
        else
            putchar(*s);
}

static void print_dialog(const struct arcade_dialog *d)
{
    static const char *const kinds[] = { "choose", "confirm", "message",
                                         "number", "text" };
    printf("%s ", kinds[d->kind]);
    print_flat(d->prompt);
    if (d->kind == ARCADE_DIALOG_CHOOSE) {
        printf(":");
        for (int i = 0; i < d->n; i++)
            printf("%s%s", i ? "|" : " ", d->items[i]);
        printf(" at %d", d->at);
    } else if (d->kind == ARCADE_DIALOG_NUMBER)
        printf(": %ld to %ld by %ld from %ld", (long)d->min, (long)d->max,
               (long)d->step, (long)d->value);
    else if (d->kind == ARCADE_DIALOG_TEXT)
        printf(": [%s] up to %d", d->text, d->most);
}

/* A dialog's frame: answered from --ui, or a tick of the input */
static int dialog_frame(void *ctx, unsigned *held, int *wheel)
{
    (void)ctx;
    if (!held) {
        dialog_said = 0;
        return 1;
    }
    struct arcade_dialog d;
    if (!dialog_said && arcade_dialog(the_vm, &d)) {
        dialog_said = 1;
        print_dialog(&d);
        if (ui_answers && d.kind == ARCADE_DIALOG_MESSAGE) {
            printf("\n");
            arcade_dialog_answer(the_vm, "");
            return 1;
        }
        if (ui_answers) {
            char a[128];
            int any = ui_next(a, sizeof(a));
            printf(" -> %s\n", any ? a : "none");
            arcade_dialog_answer(the_vm, any ? a : NULL);
            return 1;
        }
        printf("\n");
    }
    if (tick_now + 1 >= tick_last) {
        if (shot_at_end && !shot_taken) {
            shot(the_vm, shot_at_end);
            shot_taken = 1;
        }
        return 0;
    }
    *wheel = input_at(++tick_now, held);
    return 1;
}

/* ---- speech -------------------------------------------------------------- */

static int speech_flag;

static void sp_say(void *ctx, const char *text, int queue)
{
    (void)ctx;
    printf("%s %s\n", queue ? "queue" : "say", text);
}

static int sp_busy(void *ctx) { (void)ctx; return 0; }
static void sp_stop(void *ctx) { (void)ctx; printf("stop\n"); }
static int sp_on(void *ctx) { (void)ctx; return speech_flag; }

static const struct arcade_speak fake_speak = {
    sp_say, sp_busy, sp_stop, sp_on
};

/* ---- the cart's stored files --------------------------------------------- */

static const char *store_dir;

static void store_path(const char *name, char *path, size_t cap)
{
    snprintf(path, cap, "%s/%s.json", store_dir, name);
}

static int32_t st_read(void *ctx, const char *name, void *buf, int32_t cap)
{
    (void)ctx;
    char path[512];
    if (!store_dir)
        return -1;
    store_path(name, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (cap > 0)
        n = (long)fread(buf, 1, (size_t)(n < cap ? n : cap), f);
    fclose(f);
    return (int32_t)n;
}

static int st_list(void *ctx, char (*names)[33], int max);

static int st_write(void *ctx, const char *name, const void *buf, int32_t n)
{
    char path[512], names[ARCADE_STORE_FILES][33];
    if (!store_dir)
        return 0;
    /* the area's bounds, this file's old bytes aside */
    long total = n;
    int count = st_list(ctx, names, ARCADE_STORE_FILES), there = 0;
    for (int i = 0; i < count; i++) {
        if (!strcmp(names[i], name))
            there = 1;
        else
            total += st_read(ctx, names[i], NULL, 0);
    }
    if (total > ARCADE_STORE_BYTES || (!there && count >= ARCADE_STORE_FILES))
        return 0;
    /* made at the first write, as the player makes its own */
    make_dir(store_dir);
    store_path(name, path, sizeof(path));
    FILE *f = fopen(path, "wb");
    if (!f)
        return 0;
    int ok = fwrite(buf, 1, (size_t)n, f) == (size_t)n;
    return fclose(f) == 0 && ok;
}

static int st_remove(void *ctx, const char *name)
{
    (void)ctx;
    char path[512];
    if (!store_dir)
        return 0;
    store_path(name, path, sizeof(path));
    return remove(path) == 0;
}

static int by_name(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

static int st_list(void *ctx, char (*names)[33], int max)
{
    (void)ctx;
    int n = 0;
    DIR *d = store_dir ? opendir(store_dir) : NULL;
    if (!d)
        return 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && n < max) {
        size_t len = strlen(e->d_name);
        if (len > 5 && len - 5 <= 32 && !strcmp(e->d_name + len - 5, ".json")) {
            memcpy(names[n], e->d_name, len - 5);
            names[n++][len - 5] = 0;
        }
    }
    closedir(d);
    qsort(names, (size_t)n, sizeof(names[0]), by_name);
    return n;
}

static const struct arcade_store fake_store = {
    st_read, st_write, st_remove, st_list
};

/* ---- the font file ------------------------------------------------------- */

static unsigned char *font_data;
static long font_size;

#define FONT_TABLE (8 + 256 * 36)

/* As the player's shell_glyph(): the page, the codepoint's bit, its rank;
 * a glyph running past the file's end is none */
static int font_glyph(void *ctx, uint32_t cp, uint8_t out[13])
{
    (void)ctx;
    if (!font_data || cp >= 0x10000 || cp == 0)
        return 0;
    const unsigned char *page = font_data + 8 + (cp >> 8) * 36;
    uint32_t base = page[0] | page[1] << 8 | page[2] << 16 |
                    (uint32_t)page[3] << 24, low = cp & 0xff, rank = 0;
    if (!base || !(page[4 + (low >> 3)] & (1 << (low & 7))))
        return 0;
    for (uint32_t b = 0; b < low; b++)
        rank += (page[4 + (b >> 3)] >> (b & 7)) & 1;
    if ((unsigned long long)base + rank * 13 + 13 >
        (unsigned long long)font_size)
        return 0;
    memcpy(out, font_data + base + rank * 13, 13);
    return 1;
}

/* As the player's font_open(): PBTF, version 1 and the whole page table,
 * or no font */
static void font_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return;
    fseek(f, 0, SEEK_END);
    font_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    font_data = font_size >= FONT_TABLE ? malloc((size_t)font_size) : NULL;
    if (font_data && (fread(font_data, 1, (size_t)font_size, f) !=
                      (size_t)font_size || memcmp(font_data, "PBTF", 4) ||
                      font_data[4] != 1)) {
        free(font_data);
        font_data = NULL;
    }
    fclose(f);
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

static const char *in_buttons, *in_turns;
static int in_pos = -1;
static unsigned in_held;

/* Tick t's input: the mask of the latest --buttons entry at or before it
 * into *held, and the wheel's clicks, the finger moved as --wheel says */
static int input_at(long t, unsigned *held)
{
    for (const char *b = in_buttons; b && *b;) {
        long at = strtol(b, (char **)&b, 10);
        unsigned mask = 0;
        if (*b == ':')
            mask = (unsigned)strtoul(b + 1, (char **)&b, 10);
        if (at == t)
            in_held = mask;
        if (*b == ',')
            b++;
        else
            break;
    }
    int clicks = 0;
    for (const char *b = in_turns; b && *b;) {
        long at = strtol(b, (char **)&b, 10);
        long n = 0, p = in_pos;
        if (*b == ':')
            n = strtol(b + 1, (char **)&b, 10);
        if (*b == ':')
            p = strtol(b + 1, (char **)&b, 10);
        if (at == t) {
            clicks = (int)n;
            in_pos = (int)p;
        }
        if (*b == ',')
            b++;
        else
            break;
    }
    *held = in_held;
    return clicks;
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

/* --memory: what the arena holds after the last tick, as the IDE's
 * memory view shows it, a global by its slot */
static void memory_show(struct vm *vm)
{
    static const char *const kinds[ARCADE_KINDS] = {
        [2] = "string", [3] = "array", [4] = "Int32Array", [5] = "Uint8Array",
        [6] = "record", [8] = "storage", [10] = "image", [11] = "FixArray",
        [12] = "lazy"
    };
    static uint32_t by_global[65536];
    struct arcade_memory m;
    arcade_memory(vm, &m, by_global, 65536);
    printf("memory arena %u tables %u heap %u held %u garbage %u free %u "
           "largest %u constants %u engine %u\n", m.arena, m.tables, m.heap,
           m.held, m.garbage, m.free, m.largest, m.constants, m.engine);
    for (int k = 0; k < ARCADE_KINDS; k++)
        if (m.kind_count[k])
            printf("memory kind %s %u %u\n", kinds[k] ? kinds[k] : "other",
                   m.kind_count[k], m.kind_bytes[k]);
    for (uint32_t g = 0; g < 65536; g++)
        if (by_global[g])
            printf("memory global %u %u\n", g, by_global[g]);
}

int main(int argc, char **argv)
{
    const char *path = NULL;
    long ticks = 0;
    unsigned long long seed = 1;
    int units = 0, dirty = 0;
    unsigned long ceiling = 0;
    const char *shot_path = NULL, *dump_spec = NULL, *wav_path = NULL;
    const char *buttons = NULL, *skips = NULL, *turns = NULL;
    int pos = -1;
    int playing_flag = 0;
    long bpm = 0;
    int beat_flag = 0;
    int menu_picks[16], npicks = 0;
    const char *scores_path = NULL, *options_path = NULL, *opt_sets = NULL;
    const char *saves_path = NULL;
    long suspend_at = -1;
    const char *pauses = NULL;
    int memory_flag = 0;
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
        else if (!strcmp(argv[i], "--dirty"))
            dirty = 1;
        else if (!strcmp(argv[i], "--memory"))
            memory_flag = 1;
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc)
            wav_path = argv[++i];
        else if (!strcmp(argv[i], "--wheel") && i + 1 < argc)
            turns = argv[++i];
        else if (!strcmp(argv[i], "--setting") && i + 1 < argc &&
                 nsettings < 16)
            settings[nsettings++] = argv[++i];
        else if (!strcmp(argv[i], "--beat") && i + 1 < argc) {
            bpm = atol(argv[++i]);
            beat_flag = 1;
        } else if (!strcmp(argv[i], "--playing"))
            playing_flag = 1;
        else if (!strcmp(argv[i], "--scores") && i + 1 < argc)
            scores_path = argv[++i];
        else if (!strcmp(argv[i], "--saves") && i + 1 < argc)
            saves_path = argv[++i];
        else if (!strcmp(argv[i], "--options") && i + 1 < argc)
            options_path = argv[++i];
        else if (!strcmp(argv[i], "--opt") && i + 1 < argc)
            opt_sets = argv[++i];
        else if (!strcmp(argv[i], "--ui") && i + 1 < argc)
            ui_answers = argv[++i];
        else if (!strcmp(argv[i], "--speech"))
            speech_flag = 1;
        else if (!strcmp(argv[i], "--pause") && i + 1 < argc)
            pauses = argv[++i];
        else if (!strcmp(argv[i], "--suspend-at") && i + 1 < argc)
            suspend_at = atol(argv[++i]);
        else if (!strcmp(argv[i], "--store") && i + 1 < argc)
            store_dir = argv[++i];
        else if (!strcmp(argv[i], "--font") && i + 1 < argc)
            font_load(argv[++i]);
        else if (!strcmp(argv[i], "--buttons") && i + 1 < argc)
            buttons = argv[++i];
        else if (!strcmp(argv[i], "--skip") && i + 1 < argc)
            skips = argv[++i];
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
    struct arcade_host host = { NULL, print, NULL, date, click,
                                playing_flag || beat_flag ? fake_playing
                                                          : NULL,
                                beat_flag ? &fake_beat : NULL, fake_setting,
                                NULL, fake_control, &fake_tracks, dialog_frame,
                                &fake_speak, &fake_store, font_glyph, watch,
                                prof, cart_read };
    cart_file = &in;
    prof_units = units;
    fake_music_init(music_say);
    fake_music_tempo((int32_t)bpm);
    fake_music_play(playing_flag || beat_flag);
    unsigned last_held = 0;
    double track_ms = 0;
    int32_t track_at = 0;
    struct vm *vm = arcade_load(arena, sizeof(arena), mem_read, &in, &host,
                                seed);
    if (!vm) {
        fprintf(stderr, "no memory\n");
        return 1;
    }
    the_vm = vm;
    prof_vm = vm;
    in_buttons = buttons;
    in_turns = turns;
    in_pos = pos;
    tick_last = ticks;
    shot_at_end = shot_path;
    if (arcade_error(vm, NULL, NULL, NULL))
        return report(vm);
    if (ceiling)
        arcade_set_ceiling(vm, (uint32_t)ceiling);
    if (scores_path)
        scores_read(vm, scores_path);
    if (options_path)
        options_read(vm, options_path);
    if (saves_path)
        saves_read(vm, saves_path);
    if (!arcade_start(vm))
        return report(vm);
    if (units)
        fprintf(stderr, "start %u\n", (unsigned)arcade_units(vm));
    unsigned held = 0;
    for (; tick_now < ticks; tick_now++) {
        long t = tick_now;
        int clicks = input_at(t, &held), late = 0;
        for (const char *b = skips; b && *b;) {
            long at = strtol(b, (char **)&b, 10);
            unsigned n = 0;
            if (*b == ':')
                n = (unsigned)strtoul(b + 1, (char **)&b, 10);
            if (at == t) {
                arcade_skipped(vm, n);
                late = 1;
            }
            if (*b == ',')
                b++;
            else
                break;
        }
        for (const char *p = pauses; p && *p;) {
            if (strtol(p, (char **)&p, 10) == t) {
                arcade_paused(vm);
                printf("paused\n");
            }
            if (*p != ',')
                break;
            p++;
        }
        if (t == suspend_at) {
            static uint64_t copy[sizeof(arena) / 8];
            uint32_t n;
            const void *snap = arcade_snapshot(vm, &n);
            memcpy(copy, snap, n);
            memset(arena, 0xa5, sizeof(arena));
            memcpy(arena, copy, n);
            vm = the_vm = arcade_resume(arena, n, &host);
            if (!vm) {
                fprintf(stderr, "not resumable\n");
                return 1;
            }
            printf("resumed\n");
        }
        arcade_input(vm, held, clicks, in_pos);
        options_set(vm, opt_sets, t);
        {
            /* with --beat the clock a tick on; each new press stamped
             * with it */
            struct arcade_music m = { 0, 0, 0, 0, { -1, -1, -1, -1, -1 } };
            if (beat_flag || playing_flag) {
                track_ms += 1000.0 / arcade_rate(vm);
                fake_music_advance((int32_t)track_ms - track_at);
                track_at = (int32_t)track_ms;
            }
            fake_music_tick(&m);
            for (int i = 0; i < 5; i++)
                if ((held & ~last_held) & (1u << i))
                    m.press[i] = m.clock;
            arcade_music(vm, &m);
        }
        last_held = held;
        /* a late tick draws nothing: the next present covers both */
        if (!late)
            arcade_presented(vm);
        if (!arcade_tick(vm, !late))
            return report(vm);
        if (units)
            fprintf(stderr, "tick %ld %u\n", t + 1, (unsigned)arcade_units(vm));
        if (wav_path)
            wav_tick(vm, t);
        scores_show(vm);
        arcade_idle(vm);
        if (arcade_quit_asked(vm)) {
            printf("quit\n");
            tick_now++;
            break;
        }
    }
    if (memory_flag)
        memory_show(vm);
    /* the cart closing: its ending(), before the saves are written */
    if (!arcade_end(vm))
        return report(vm);
    if (saves_path && !saves_write(vm, saves_path)) {
        fprintf(stderr, "could not write %s\n", saves_path);
        return 2;
    }
    if (scores_path && !scores_write(vm, scores_path)) {
        fprintf(stderr, "could not write %s\n", scores_path);
        return 2;
    }
    if (options_path && !options_write(vm, options_path)) {
        fprintf(stderr, "could not write %s\n", options_path);
        return 2;
    }
    if (wav_path)
        wav_save(wav_path);
    if (dump_spec)
        dump(vm, dump_spec);
    if (dirty) {
        const struct arcade_screen *s = arcade_screen(vm);
        /* every row is sent when the palette changed */
        if (s->palette_changed)
            printf("dirty 0 %d\n", s->height - 1);
        else
            printf("dirty %d %d\n", s->dirty_lo, s->dirty_hi);
    }
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
    if (shot_path && !shot_taken && !shot(vm, shot_path)) {
        fprintf(stderr, "could not write %s\n", shot_path);
        return 2;
    }
    fflush(stdout);
    return 0;
}
