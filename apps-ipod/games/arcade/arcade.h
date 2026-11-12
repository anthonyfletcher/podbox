/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Arcade: the engine as a host sees it -- the player's shell, the web
 * shim, and the host test tool all drive a cart through these calls.
 *
 * The engine is platform-free C. What it needs from the platform comes
 * through struct arcade_host; it allocates nothing, and keeps everything
 * in the arena it is given.
 ***************************************************************************/
#ifndef ARCADE_H
#define ARCADE_H

#include <stdint.h>

/* The newest API version this engine provides. */
#define ARCADE_API_VERSION 1

/* playback.sound()'s record: SOUND.SIZE in arcade_api.def */
#define ARCADE_SOUND_FIELDS 31

/* A cart's logo, its LOGO chunk: this many pixels a side */
#define ARCADE_LOGO_SIZE 44

/* The logo as 0xRRGGBB pixels, read from the LOGO and PALS chunks without
 * loading the cart (pals NULL for a cart with none); 1 when it fitted in
 * cap pixels, its size in w and h */
int arcade_logo_rgb(const uint8_t *logo, uint32_t logo_len,
                    const uint8_t *pals, uint32_t pals_len,
                    uint32_t *rgb, uint32_t cap, int *w, int *h);

/* tracks.near(): the most it gives for a track, and the most tracks a
 * call takes */
#define ARCADE_NEAR_MAX 64
#define ARCADE_NEAR_IDS 10

struct arcade_host
{
    void *ctx;
    /* printh(): one line of text, not terminated */
    void (*print)(void *ctx, const char *s, int len);
    /* called every YIELD_UNITS units, so the codec keeps running */
    void (*yield)(void *ctx);
    /* year, month, day, hour, minute, second, weekday (0 = Sunday) */
    void (*date)(void *ctx, int32_t out[7]);
    /* click(): the wheel's tick, or with beep its beep; NULL for none */
    void (*click)(void *ctx, int beep);
    /* playback.playing(): whether the owner's music is playing (not
     * paused or stopped); NULL for never */
    int (*playing)(void *ctx);
    /* beat: the music's beat, below; NULL where there is none to follow */
    const struct arcade_beat *beat;
    /* sys.setting(): the owner's setting by its config.cfg name -- 1 with
     * *num for a number, 2 with its text in buf, 0 for no such setting;
     * NULL for none */
    int (*setting)(void *ctx, const char *name, int32_t *num, char *buf,
                   int cap);
    /* sys.translate(): the firmware's phrase whose English is english, in
     * the owner's language, UTF-8; NULL where the firmware has no such
     * phrase. NULL for English only */
    const char *(*translate)(void *ctx, const char *english);
    /* playback.toggle(), next(), prev(): the owner's music paused or
     * played (0), on a track (1) or back one (2), as the earphone remote's
     * keys do; NULL for none */
    void (*control)(void *ctx, int what);
    /* playback and tracks: the playing track, the library and the queue,
     * below; NULL where there is no music to read */
    const struct arcade_tracks *tracks;
    /* ui: one frame of a dialog the engine has drawn on the game's
     * screen -- the screen shown as it stands, the next tick waited for --
     * and the buttons held then (ARCADE_* bits, MENU whenever it is down)
     * and the wheel's clicks since the last. 0 when the cart must stop at
     * once (USB, a shutdown): the dialog is then backed out of. Called
     * with held NULL when the dialog is over and the game's pixels are
     * back. NULL: every dialog is backed out of at once. */
    int (*frame)(void *ctx, unsigned *held, int *wheel);
    /* speak: the player's speech, below; NULL for none */
    const struct arcade_speak *speak;
    /* store: the cart's own files, below; NULL for none */
    const struct arcade_store *store;
    /* FONT.WORLD's glyphs beyond those the engine holds (kana, Hangul,
     * ideographs): codepoint cp's into out -- its advance, then 12 rows a
     * byte each, the high bit on the left -- 1, or 0 when there is none.
     * NULL for none. */
    int (*glyph)(void *ctx, uint32_t cp, uint8_t out[13]);
    /* The IDE's tools, which the player leaves NULL. watch(): a value
     * shown by name, as text. prof(): a section of the profiler begun
     * (name) or ended (NULL); arcade_units_now() then times it. */
    void (*watch)(void *ctx, const char *name, const char *value);
    void (*prof)(void *ctx, const char *name);
    /* n bytes of the cart file from byte at, for the maps and images it
     * reads as it plays: how many it read. NULL where it cannot. */
    uint32_t (*cart_read)(void *ctx, uint32_t at, void *buf, uint32_t n);
};

/* store.*: a cart's own JSON files, each name.json, in an area of its own.
 * Names are 1 to 32 letters, digits, - and _. The host bounds the area:
 * ARCADE_STORE_FILES files and ARCADE_STORE_BYTES in all; the engine
 * writes no file over ARCADE_STORE_FILE. */
#define ARCADE_STORE_FILE  65536
#define ARCADE_STORE_BYTES (256 * 1024)
#define ARCADE_STORE_FILES 64

struct arcade_store
{
    /* name.json into buf, at most cap bytes: how many it holds (cap 0
     * asks only that), or -1 when there is none */
    int32_t (*read)(void *ctx, const char *name, void *buf, int32_t cap);
    /* written whole in place of any before it: 1, or 0 when the area
     * would be over its bounds or the disk refuses */
    int (*write)(void *ctx, const char *name, const void *buf, int32_t n);
    /* 1 when there was one to remove */
    int (*remove)(void *ctx, const char *name);
    /* the names, sorted, at most max of them into names: how many */
    int (*list)(void *ctx, char (*names)[33], int max);
};

/* speak.*: text said by the player's synthesiser, over the music and the
 * cart's sounds. Nothing is said while the owner has it off. */
struct arcade_speak
{
    /* text said, cutting off what is being said, or after it (queue) */
    void (*say)(void *ctx, const char *text, int queue);
    /* whether anything is still being said; stopping it */
    int (*busy)(void *ctx);
    void (*stop)(void *ctx);
    /* whether the owner has the player's menus spoken */
    int (*on)(void *ctx);
};

/* The dialog ui.* has open, for a host that reads or answers it during
 * its frame call: the test tool, and speech. Texts are UTF-8 and ended. */
enum { ARCADE_DIALOG_CHOOSE, ARCADE_DIALOG_CONFIRM, ARCADE_DIALOG_MESSAGE,
       ARCADE_DIALOG_NUMBER, ARCADE_DIALOG_TEXT };

struct arcade_dialog
{
    int kind;
    const char *prompt;
    const char *const *items;           /* choose: n of them */
    int n;
    int at;                             /* choose: the row highlighted */
    int32_t min, max, step, value;      /* number */
    const char *text;                   /* text: the line as it stands */
    int most;                           /* text: the bytes it may hold */
};

/* tracks.pick() and tracks.count()'s filter; 0 or NULL in a field is any */
struct arcade_filter
{
    int32_t minlen, maxlen;     /* ms */
    const char *genre;          /* the genre's name, case aside */
    int32_t decade;             /* 1980 for the 80s */
    int music;                  /* spoken word left out */
    const char *artist;         /* the artist's name, case aside */
};

/* The owner's music and library. A track is its id, the database's number
 * for it; -1 is none. Strings are UTF-8, cut to cap. */
struct arcade_tracks
{
    /* playback: the playing track's id, its elapsed and length (ms), and
     * its title (0), artist (1) or album (2) into buf, cap bytes with its
     * end: the length written */
    int32_t (*current)(void *ctx);
    void (*position)(void *ctx, int32_t out[2]);
    int (*text)(void *ctx, int what, char *buf, int cap);
    /* playback.seek(), and duck(): the music to level / 256 of its
     * volume over ticks ticks, undone when the cart ends */
    void (*seek)(void *ctx, int32_t ms);
    void (*duck)(void *ctx, int32_t level, int32_t ticks);
    /* playback.play(): track id from ms, from a playlist of the cart's own,
     * the owner's set aside until the cart ends; 0 when it cannot.
     * stop(), and started(): whether it is heard yet */
    int (*play)(void *ctx, int32_t id, int32_t ms);
    void (*stop)(void *ctx);
    int (*started)(void *ctx);
    /* tracks.count(), and pick(): up to n ids at random into out, the
     * count */
    int32_t (*count)(void *ctx, const struct arcade_filter *f);
    int32_t (*pick)(void *ctx, int32_t n, const struct arcade_filter *f,
                    int32_t *out);
    /* tracks.field(): 1 with *num for a number, 2 with its text in buf,
     * cap bytes with its end, 0 when there is none (ARCADE_TRACK_* below) */
    int (*field)(void *ctx, int32_t id, int f, int32_t *num, char *buf,
                 int cap);
    /* tracks.near(): for each of count ids, up to n tracks most like it
     * into out, n a track, -1 after the last of each; the count found in
     * all. By how they sound, nearest first, when sound is set. n is at
     * most ARCADE_NEAR_MAX, count ARCADE_NEAR_IDS. */
    int32_t (*near)(void *ctx, const int32_t *ids, int32_t count,
                    int32_t n, int sound, int32_t *out);
    /* tracks.sound(): id's Sound Index record, as playback.sound() */
    int (*sound)(void *ctx, int32_t id, int32_t out[ARCADE_SOUND_FIELDS]);
    /* the queue: its length and the playing place (0 first, -1 none),
     * the id at place i, and a track added after the playing one (next)
     * or at the end; 0 when it cannot be */
    int32_t (*queued)(void *ctx, int32_t *at);
    int32_t (*queue_at)(void *ctx, int32_t i);
    int (*enqueue)(void *ctx, int32_t id, int next);
    /* the tracks the last count(), pick() or near() looked at, which
     * the call is charged for */
    int32_t (*walked)(void *ctx);
    /* playback.colors(): the background, text and most vivid colours
     * the player shows, 0xRRGGBB, into out; 1 when they are the playing
     * album's (dynamic colours), 0 the theme's. NULL for none */
    int (*colours)(void *ctx, uint32_t out[3]);
    /* playback.volume() and adjust(): the owner's volume turned up or down
     * steps of the player's own (0 leaves it), and then where it is, 0 to
     * 100 of its range. NULL for none */
    int32_t (*volume)(void *ctx, int32_t steps);
};

/* tracks.field()'s fields: TRACK.* in arcade_api.def */
#define ARCADE_TRACK_TITLE  0
#define ARCADE_TRACK_ARTIST 1
#define ARCADE_TRACK_ALBUM  2
#define ARCADE_TRACK_GENRE  3
#define ARCADE_TRACK_YEAR   4
#define ARCADE_TRACK_LENGTH 5

/* The beat of the owner's music, for a cart played to it. Times are track
 * ms. The host also hands over each tick's clock (arcade_music()). */
struct arcade_beat
{
    /* beat.listen(): start or stop the analyser and the tempo search */
    void (*listen)(void *ctx, int on);
    /* locked (0/1), bpm, period ms, confidence 0-100, the envelope's
     * fill 0-100, beats since the lock */
    void (*state)(void *ctx, int32_t out[6]);
    /* the nth beat at or after from (0 the first) and its place in the
     * bar, 0-3, from the downbeat once one was found; -1 with no lock */
    int32_t (*next)(void *ctx, int32_t from, int32_t n, int32_t *in_bar);
    /* which of the four beats starts the bar, or -1 (none wins, or under
     * 32 beats heard), and by what margin in tenths */
    int32_t (*downbeat)(void *ctx, int32_t *margin10);
    /* track ms this instant, past the tick */
    int32_t (*now)(void *ctx);
    /* the bar just heard: energy 0-100, flux 0-100, trend (signed) */
    void (*mood)(void *ctx, int32_t out[3]);
    /* playback.bands(): n spectrum bands (1-8), 0-100, as the skins draw
     * them */
    void (*bands)(void *ctx, int32_t *out, int32_t n);
    /* playback.sound(): the playing track's Sound Index record, in the
     * order of SOUND.* (arcade_api.def); 0 where it has none */
    int (*sound)(void *ctx, int32_t out[ARCADE_SOUND_FIELDS]);
    /* beat.ahead(): the hits the analyser has found from from to before
     * to, oldest first, as time, bands (1 low, 2 mid, 4 high) and
     * strength, at most max of them; the count. Only while listening */
    int32_t (*ahead)(void *ctx, int32_t from, int32_t to, int32_t *out,
                     int32_t max);
    /* beat.horizon(): track ms of the newest audio analysed, or -1 when
     * not listening */
    int32_t (*horizon)(void *ctx);
};

/* What the host read for a tick: the clock, steady for the tick; the ms
 * still to play; whether playback moved underneath (a seek, a skip, a new
 * track) since the last, and whether the track is another; and when each
 * button went down, in btn()'s numbering, or -1. */
struct arcade_music
{
    int32_t clock, left, jumped, changed;
    int32_t press[5];
};

/* Reads n bytes into buf, or skips n bytes when buf is NULL; returns the
 * number read or skipped. */
typedef uint32_t (*arcade_read_fn)(void *ctx, void *buf, uint32_t n);

struct vm;

/* Loads and verifies a cart into mem, which must be 8-byte aligned. Returns
 * NULL only when mem is too small to report anything; otherwise check
 * arcade_error(). */
struct vm *arcade_load(void *mem, uint32_t size, arcade_read_fn read,
                       void *rctx, const struct arcade_host *host,
                       uint64_t seed);

/* The cart's settings the host needs: ticks a second, whether MENU is the
 * cart's button 2 rather than the pause menu, and the 8-byte id its save,
 * options and scores are filed under. */
int arcade_rate(const struct vm *vm);
int arcade_menu_is_game(const struct vm *vm);

/* The host fell behind by this many frames and did not draw them: what
 * skipped() reports. The rate can change at any tick (setrate()), so the
 * host reads arcade_rate() after each. */
void arcade_skipped(struct vm *vm, unsigned frames);
/* The game was paused, by the pause menu or the hold switch: what
 * resumed() reports. */
void arcade_paused(struct vm *vm);
/* The owner's mute of the game's own sound (sfx and tone), kept with the
 * options: arcade_audio() then mixes silence, its effects ending on time */
void arcade_mute(struct vm *vm, int on);
int arcade_muted(const struct vm *vm);
const uint8_t *arcade_id(const struct vm *vm);

/* The buttons for btn(): one bit each, in btn()'s numbering. */
#define ARCADE_PREV   0x01
#define ARCADE_NEXT   0x02
#define ARCADE_MENU   0x04  /* only when arcade_menu_is_game() */
#define ARCADE_PLAY   0x08
#define ARCADE_SELECT 0x10

/* Before each tick: the buttons held, the clicks the wheel turned since
 * the last tick (96 a turn, clockwise positive), and where the finger is
 * on it, 0-95 clockwise from the top, or -1 when nothing touches it. */
void arcade_input(struct vm *vm, unsigned held, int wheel, int wheelpos);

/* Before each tick, where the host follows the music: beat.clock() and
 * the rest of the tick's reading. */
void arcade_music(struct vm *vm, const struct arcade_music *m);

/* The cart's own pause-menu rows, made by menuitem(): copies item i's
 * label (1-5) into buf and returns 1, or returns 0 when there is none. */
int arcade_menu_item(const struct vm *vm, int i, char *buf, int cap);

/* Item i chosen: calls its function with the button that chose it, in
 * btn()'s numbering. 1 when it returned a true value -- keep the menu open
 * and redraw it -- 0 to resume, -1 when the cart stopped with an error. */
int arcade_menu_choose(struct vm *vm, int i, int button);

/* Whether the cart has called quit(). */
int arcade_quit_asked(const struct vm *vm);

/* Runs the cart's global setup, then init(). */
int arcade_start(struct vm *vm);
/* The cart closing: its ending(), once, if it has one, before the host
 * writes its saves and any copy kept to carry on. 0 when it failed. */
int arcade_end(struct vm *vm);
/* One tick: update(), then draw() when draw is set and the cart has one. */
int arcade_tick(struct vm *vm, int draw);
/* Between ticks: collect garbage if enough has been allocated. */
void arcade_idle(struct vm *vm);

/* What the arena holds, for a host's memory view (arcade_memory): bytes
 * throughout. The heap's objects are held (reached from the cart's
 * globals or the engine's roots), garbage (unreached, until the next
 * collection frees them) or free; a held object is counted by its kind,
 * a value tag, the storage behind arrays and records as T_BUF. */
#define ARCADE_KINDS 16
struct arcade_memory
{
    uint32_t arena;             /* the whole arena */
    uint32_t tables;            /* the engine's state and the loaded cart */
    uint32_t heap;              /* the heap, the rest */
    uint32_t held, garbage, free;
    uint32_t largest;           /* the largest free block: the biggest
                                   allocation that needs no collection */
    uint32_t constants;         /* of held, the cart's strings, never freed */
    uint32_t engine;            /* of held, reached only from the engine's
                                   own roots: the target, the sheet, the
                                   pause menu's labels */
    uint32_t kind_bytes[ARCADE_KINDS], kind_count[ARCADE_KINDS];
};
/* Fills m, and by_global[i] (up to n of them) with the bytes reached from
 * global i that no earlier global reaches. Between ticks only; nothing is
 * collected or charged, so the cart runs on as though never looked at. */
void arcade_memory(struct vm *vm, struct arcade_memory *m,
                   uint32_t *by_global, uint32_t n);

/* Units charged by the last arcade_start or arcade_tick: the cart's own,
 * and while it listens to playing music, the beat analyser's share of the
 * tick. */
uint32_t arcade_units(const struct vm *vm);
/* Units charged so far in the call running now, from inside it: for
 * prof() sections */
uint32_t arcade_units_now(const struct vm *vm);
/* Units at which one call is stopped as not responding; arcade_start(),
 * init() and the globals before it, may run to four times this. */
void arcade_set_ceiling(struct vm *vm, uint32_t units);

/* For a profiler, from the host's yield, which comes every 2048 units:
 * the frames running, innermost first, as function numbers (LINE's order),
 * up to max of them; the offset in the innermost function's code; and the
 * API slot of the native call it is in, or -1. Returns how deep the frames
 * go, 0 outside any function. */
int arcade_where(const struct vm *vm, uint32_t *fns, int max, uint32_t *pc,
                 int32_t *native);

/* NULL while the cart is running; otherwise what stopped it, and where:
 * the source file, line and function, when known. */
const char *arcade_error(const struct vm *vm, const char **file,
                         int *line, const char **fn);

/* The screen, 8-bit palette indices, for present. Rows dirty_lo..dirty_hi
 * have been drawn since the last arcade_presented() (none when lo > hi);
 * rgb gives each index's colour as 0xRRGGBB, the screen palette already
 * applied, and palette_changed says it has changed, so every row needs
 * converting again. */
struct arcade_screen
{
    const uint8_t *px;
    int width, height;
    int dirty_lo, dirty_hi;
    const uint32_t *rgb;
    int palette_changed;
};

const struct arcade_screen *arcade_screen(struct vm *vm);
void arcade_presented(struct vm *vm);

/* core_sound.c: the cart's next frames of sound at rate Hz, each a stereo pair
 * of 16-bit samples, the two the same. Returns 0, the frames silent, when
 * no voice is sounding. */
int arcade_audio(struct vm *vm, int16_t *out, int frames, int rate);

/* present.c: the conversion for a 16-bit panel at twice the size. The
 * table holds each colour's pixel twice in one word, packed by the host's
 * own pack(); rows r0..r1 of the screen become panel rows 2*r0..2*r1+1 of
 * fb, stride pixels a row. */
void arcade_present_lut(const struct arcade_screen *s, uint32_t lut2[256],
                        uint16_t (*pack)(uint32_t rgb));
void arcade_present_rows(const struct arcade_screen *s,
                         const uint32_t lut2[256], void *fb, int stride,
                         int r0, int r1);

/* The 256 saved values, for the host to write out: their 32 bits, a bit
 * each saying which are fractions, and whether any changed since the last
 * call. */
const int32_t *arcade_save(struct vm *vm, int *dirty,
                           const uint8_t **fractions);
void arcade_restore(struct vm *vm, const int32_t save[256],
                    const uint8_t fractions[32]);

/* Suspending: the arena as it stands between ticks is the whole game, so
 * writing its size bytes from arcade_snapshot() and reading them back to
 * the same address carries on from that tick. Every pointer inside it is
 * an address in this firmware build, so a snapshot holds only for the
 * same build, the same arena address and size, and the same cart -- the
 * host keeps those beside it (arcade_cart_hash names the cart's bytes).
 * A cart that owns the music is not suspended: its playlist is not part
 * of the arena. */
const void *arcade_snapshot(const struct vm *vm, uint32_t *size);
uint32_t arcade_cart_hash(const struct vm *vm);
int arcade_owns_music(const struct vm *vm);
/* whether Quit keeps the game to resume: not where cart.json says "suspend": false */
int arcade_suspends(const struct vm *vm);
/* whether none of the cart's code is running, so a snapshot is one to
 * resume: not within a call, nor after an error */
int arcade_between_calls(const struct vm *vm);
/* mem holds a snapshot read back to where it was taken: the cart, ready
 * for its next tick, or NULL when mem is not one */
struct vm *arcade_resume(void *mem, uint32_t size,
                         const struct arcade_host *host);

/* sys_score.c: a cart's high scores. Its boards come from the cart (the
 * BORD chunk); the host fills them from its file when the cart starts and
 * writes them out when they change. A board's name is matched by key, a
 * hash of the name the cart's code knows it by, and a table is kept only
 * while its version is unchanged. */
#define ARCADE_BOARDS 8
#define ARCADE_RANKS  10
#define ARCADE_SCORE_NUMBER 0   /* a plain number */
#define ARCADE_SCORE_TIME   1   /* ms, shown as a time */
#define ARCADE_SCORE_TICKS  2   /* ticks, shown as a time at the cart's rate */

struct arcade_entry
{
    int32_t value;
    int32_t date;               /* yyyymmdd, 0 unknown */
    char name[16];              /* "" when none was given */
};

struct arcade_board
{
    char title[24];
    uint32_t key;
    uint16_t version;
    uint8_t low, format;        /* low: the lowest value wins */
    uint8_t count;
    struct arcade_entry e[ARCADE_RANKS];
};

/* An achievement, from the cart (ACHV), earned on date (yyyymmdd; 1 when
 * the date was not known, 0 not yet) */
#define ARCADE_ACHIEVEMENTS 32
struct arcade_achievement
{
    char title[32];
    char text[96];
    uint32_t key;
    int32_t date;
};

struct arcade_scores
{
    char cart[32];              /* the cart's name, for a list of every cart */
    uint8_t id[8];
    uint8_t rate;               /* ticks a second, for ARCADE_SCORE_TICKS */
    uint32_t plays, seconds;    /* times started, and time played */
    int count;
    struct arcade_board b[ARCADE_BOARDS];
    int nach;
    struct arcade_achievement ach[ARCADE_ACHIEVEMENTS];
};

/* The running cart's boards; with dirty, it is set when a score was
 * recorded or cleared since the last call that asked, and cleared. */
struct arcade_scores *arcade_scores(struct vm *vm, int *dirty);
/* Takes saved's tables for the boards that match by key and version */
void arcade_scores_restore(struct vm *vm, const struct arcade_scores *saved);
/* The file's bytes: a scores record into buf, its length (0 too small);
 * and back, 1 when buf held a whole one */
int arcade_scores_pack(const struct arcade_scores *s, uint8_t *buf, int cap);
int arcade_scores_unpack(struct arcade_scores *s, const uint8_t *buf, int n);
#define ARCADE_SCORES_MAX (64 + ARCADE_BOARDS * (40 + ARCADE_RANKS * 24) + \
                           1 + ARCADE_ACHIEVEMENTS * (10 + 32 + 96))
/* A value as its board shows it, into buf */
void arcade_score_text(const struct arcade_scores *s, int board,
                       int32_t value, char *buf, int cap);
/* sys_opt.c: a cart's options, the settings the pause menu shows for it.
 * They come from the cart (the OPTN chunk), each with its default; the
 * host puts back the owner's values when the cart starts, matched by key,
 * a hash of the option's name, and keeps them when the owner changes one. */
#define ARCADE_OPTIONS    8
#define ARCADE_CHOICES    8
#define ARCADE_OPT_CHOICE 0     /* one of its choices, by index */
#define ARCADE_OPT_ONOFF  1     /* 0 or 1 */
#define ARCADE_OPT_NUMBER 2     /* min to max, in steps */

struct arcade_option
{
    char title[24];
    char choice[ARCADE_CHOICES][24];
    const char *help;           /* in the cart, not ended; help_len bytes */
    int help_len;
    uint32_t key;
    uint8_t type, nchoices;
    int32_t value, def, min, max, step;
};

struct arcade_options
{
    int count;
    struct arcade_option o[ARCADE_OPTIONS];
};

const struct arcade_options *arcade_options(const struct vm *vm);
/* The owner set option i: value is brought within its range, and
 * opt.changed() is true for the next tick when it differs */
void arcade_option_set(struct vm *vm, int i, int32_t value);
/* The file's bytes: the values with their keys, packed; and back, each
 * value given to the option with its key. pack returns the length, 0 when
 * cap is too small; unpack 1 when buf held a whole record. */
#define ARCADE_OPTIONS_MAX (8 + ARCADE_OPTIONS * 8)
int arcade_options_pack(const struct vm *vm, uint8_t *buf, int cap);
int arcade_options_unpack(struct vm *vm, const uint8_t *buf, int n);
/* Whether opt.set() changed an option since the last call: the host then
 * keeps the values, as it does the pause menu's */
int arcade_options_set(struct vm *vm);
/* The cart's help text, not ended: its length, 0 for none */
int arcade_help(const struct vm *vm, const char **text);

/* score.show(): the board the cart asked to show since the last call, and
 * the rank to mark (0 none), or -1 when none was asked; cleared */
int arcade_score_shown(struct vm *vm, int *rank);
/* An achievement earned since the last call (ach.earn), for the host's
 * alert: its index, or -1; up to eight are kept in turn */
int arcade_achieved(struct vm *vm);

/* The dialog open, during the host's frame call: 1 with it in out, or 0
 * when there is none */
int arcade_dialog(struct vm *vm, struct arcade_dialog *out);
/* The dialog open answered at once, as typed: choose's index (-1 backs
 * out), confirm's 1 or 0, number's number, text's line (~ backs out);
 * message takes anything. NULL backs out of any. */
void arcade_dialog_answer(struct vm *vm, const char *answer);
/* The host's own question over the game, drawn and worked as ui.confirm()
 * is, with buttons labelled yes and no, the cursor on no when no_first:
 * 1 for yes. Translated text the cart's font cannot draw is shown in
 * FONT.WORLD. */
int arcade_confirm(struct vm *vm, const char *prompt, const char *yes,
                   const char *no, int no_first);
/* A banner across the top of the game's screen, head over title, in the
 * dialogs' style: the rows it covers, from 0, to present with it; the
 * game's pixels under them come back with arcade_banner_undo(). Draws
 * nothing, giving 0, while a dialog is open. */
int arcade_banner(struct vm *vm, const char *head, const char *title);
void arcade_banner_undo(struct vm *vm);

#endif
