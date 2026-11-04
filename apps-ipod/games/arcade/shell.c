/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Arcade's shell: a cart on the player. It owns the screen, the buttons and
 * the app buffer while the cart runs, and gives the engine (arcade.h) its
 * ticks, its input and the panel.
 *
 * The engine lives wholly in the app buffer, capped at the 5G's 512KB so
 * every cart runs on both players, and never in core_alloc, which would
 * stop the music. Nothing in the buffer is pinned, so USB needs nothing
 * given back.
 *
 * Parts, in order:
 *   - the cart file and the save file
 *   - present: the 8-bit screen and borders, doubled onto the panel
 *   - input: the buttons and the wheel, as btn() and wheel() see them
 *   - the pause menu, the hold switch and system events
 *   - arcade_play(), the loop
 ****************************************************************************/

#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include "config.h"
#include "system.h"           /* TIME_BEFORE, cpu_boost */
#include "kernel.h"
#include "lcd.h"
#include "button.h"
#include "backlight.h"
#include "powermgmt.h"        /* reset_poweroff_timer */
#include "file.h"
#include "dir.h"            /* mkdir */
#include "timefuncs.h"        /* get_time */
#include "lang.h"
#include "settings/settings.h"
#include "system/app_buffer.h"
#include "system/activity.h"
#include "system/shutdown.h"  /* default_event_handler */
#include "draw/viewport.h"    /* viewportmanager_theme_enable */
#include "widgets/list.h"     /* simplelist */
#include "widgets/splash.h"
#include "root_menu.h"        /* GO_TO_* */
#include "games/arcade/arcade.h"
#include "games/arcade/shell.h"

/* The engine's memory: the app buffer, no more than the smaller player's. */
#define ARENA_MAX (512 * 1024)

/* Units one call may run before it is stopped as not responding. Generous
 * until phase 1 measures what a frame costs on each player. */
#define CEILING 2000000

#define SAVE_DIR "/.rockbox/games/saves"


/* ---- the cart file and the save file ------------------------------------- */

static uint32_t file_read(void *ctx, void *buf, uint32_t n)
{
    int fd = *(int *)ctx;
    if (!buf) {
        off_t at = lseek(fd, 0, SEEK_CUR);
        off_t end = lseek(fd, 0, SEEK_END);
        if (at < 0 || end < 0)
            return 0;
        if ((off_t)n > end - at)
            n = (uint32_t)(end - at);
        lseek(fd, at + n, SEEK_SET);
        return n;
    }
    ssize_t got = read(fd, buf, n);
    return got < 0 ? 0 : (uint32_t)got;
}

static void save_path(struct vm *vm, char *path, size_t size)
{
    const uint8_t *id = arcade_id(vm);
    snprintf(path, size, SAVE_DIR "/%02x%02x%02x%02x%02x%02x%02x%02x.sav",
             id[0], id[1], id[2], id[3], id[4], id[5], id[6], id[7]);
}

/* The file: "PBGS", 256 little-endian values, then 32 bytes saying which
 * are fractions. */
static void save_load(struct vm *vm)
{
    char path[MAX_PATH];
    unsigned char b[4 + 256 * 4 + 32];
    int32_t values[256];
    save_path(vm, path, sizeof(path));
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return;
    ssize_t got = read(fd, b, sizeof(b));
    close(fd);
    if (got != (ssize_t)sizeof(b) || memcmp(b, "PBGS", 4))
        return;
    for (int i = 0; i < 256; i++) {
        const unsigned char *p = b + 4 + i * 4;
        values[i] = (int32_t)(p[0] | (p[1] << 8) | (p[2] << 16) |
                              ((uint32_t)p[3] << 24));
    }
    arcade_restore(vm, values, b + 4 + 256 * 4);
}

static void save_write(struct vm *vm)
{
    char path[MAX_PATH];
    unsigned char b[4 + 256 * 4 + 32];
    const uint8_t *frac;
    int dirty;
    const int32_t *values = arcade_save(vm, &dirty, &frac);
    if (!dirty)
        return;
    memcpy(b, "PBGS", 4);
    for (int i = 0; i < 256; i++) {
        uint32_t v = (uint32_t)values[i];
        unsigned char *p = b + 4 + i * 4;
        p[0] = (unsigned char)v;
        p[1] = (unsigned char)(v >> 8);
        p[2] = (unsigned char)(v >> 16);
        p[3] = (unsigned char)(v >> 24);
    }
    memcpy(b + 4 + 256 * 4, frac, 32);
    mkdir("/.rockbox/games");
    mkdir(SAVE_DIR);
    save_path(vm, path, sizeof(path));
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return;
    write(fd, b, sizeof(b));
    close(fd);
}

/* ---- the engine's host --------------------------------------------------- */

static void host_print(void *ctx, const char *s, int len)
{
    (void)ctx; (void)s; (void)len;    /* printh is for the IDE's console */
}

static void host_yield(void *ctx)
{
    (void)ctx;
#ifdef HAVE_LCD_UPDATE_ASYNC
    lcd_update_poll();
#endif
    yield();
}

static void host_date(void *ctx, int32_t out[7])
{
    (void)ctx;
    struct tm *tm = get_time();
    out[0] = tm->tm_year + 1900;
    out[1] = tm->tm_mon + 1;
    out[2] = tm->tm_mday;
    out[3] = tm->tm_hour;
    out[4] = tm->tm_min;
    out[5] = tm->tm_sec;
    out[6] = tm->tm_wday;
}

static const struct arcade_host host = {
    NULL, host_print, host_yield, host_date
};

/* ---- present ---------------------------------------------------------------
 * The 160x120 screen fills the panel, each pixel two wide and two high;
 * only rows drawn since the last present are converted, and they go to
 * the panel as one full-width band, the shape the 5G pushes cheapest. On
 * the 5G the band goes by DMA while the next tick runs, moved along by
 * the script's yields; it is read from the framebuffer, so it lands before
 * anything draws there again. */

static uint32_t lut2[256];

static uint16_t pack(uint32_t c)
{
    return LCD_RGBPACK((c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff);
}

static void present_wait(void)
{
#ifdef HAVE_LCD_UPDATE_ASYNC
    lcd_update_finish();
#endif
}

#ifdef CPU_ARM
void present_row_arm(const uint8_t *src, uint32_t *d0, uint32_t *d1,
                     const uint32_t lut2[256], int n);

/* arcade_present_rows(), each row in assembly (present_arm.S) */
static void present_rows(const struct arcade_screen *s, void *fb, int stride,
                         int r0, int r1)
{
    for (int y = r0; y <= r1; y++) {
        uint16_t *row = (uint16_t *)fb + y * 2 * stride;
        present_row_arm(s->px + y * s->width, (uint32_t *)row,
                        (uint32_t *)(row + stride), lut2, s->width / 8);
    }
}
#else
static void present_rows(const struct arcade_screen *s, void *fb, int stride,
                         int r0, int r1)
{
    arcade_present_rows(s, lut2, fb, stride, r0, r1);
}
#endif

static void present(struct vm *vm, bool all)
{
    const struct arcade_screen *s = arcade_screen(vm);

    if (s->palette_changed || all) {
        arcade_present_lut(s, lut2, pack);
        all = true;
    }
    int r0 = all ? 0 : s->dirty_lo, r1 = all ? s->height - 1 : s->dirty_hi;
    if (r0 <= r1) {
        int stride = (int)(FBADDR(0, 1) - FBADDR(0, 0));
        present_wait();
        present_rows(s, FBADDR(0, 0), stride, r0, r1);
#ifdef HAVE_LCD_UPDATE_ASYNC
        lcd_update_start(r0 * 2, (r1 - r0 + 1) * 2);
#else
        lcd_update_rect(0, r0 * 2, LCD_WIDTH, (r1 - r0 + 1) * 2);
#endif
    }
    arcade_presented(vm);
}

/* ---- input --------------------------------------------------------------- */

static int last_wheel = -1;

/* The wheel's absolute position, 0-95, or -1 untouched; a build with no
 * position to read (the simulator) never has a finger on it. */
static int wheel_position(void)
{
#ifdef HAVE_WHEEL_POSITION
    return wheel_status();
#else
    return -1;
#endif
}

/* The wheel's scroll events, off while a game reads the position. */
static void wheel_events(bool on)
{
#ifdef HAVE_WHEEL_POSITION
    wheel_send_events(on);
#else
    (void)on;
#endif
}

/* A cart that claims MENU gets it as it is let go, for one tick, so the
 * hold that opens the pause menu is never also a press in the game. */
static bool menu_down, menu_paused;
static long menu_since;

/* True once a claimed MENU has been held for a second: the pause menu. */
static bool menu_held_long(int status)
{
    if (!(status & BUTTON_MENU))
        return false;
    if (!menu_down) {
        menu_down = true;
        menu_paused = false;
        menu_since = current_tick;
    }
    if (menu_paused || TIME_BEFORE(current_tick, menu_since + HZ))
        return false;
    menu_paused = true;
    return true;
}

static unsigned buttons(struct vm *vm, int status)
{
    unsigned held = 0;
    if (status & BUTTON_LEFT)   held |= ARCADE_PREV;
    if (status & BUTTON_RIGHT)  held |= ARCADE_NEXT;
    if (status & BUTTON_PLAY)   held |= ARCADE_PLAY;
    if (status & BUTTON_SELECT) held |= ARCADE_SELECT;
    if (arcade_menu_is_game(vm) && menu_down && !(status & BUTTON_MENU)) {
        if (!menu_paused)
            held |= ARCADE_MENU;
        menu_down = false;
    }
    return held;
}

/* The clicks since the last call, from the wheel's absolute position:
 * 96 a turn, clockwise positive. */
static int wheel_clicks(int pos)
{
    int d = 0;
    if (pos >= 0 && last_wheel >= 0) {
        d = pos - last_wheel;
        if (d > 48)
            d -= 96;
        else if (d < -48)
            d += 96;
    }
    last_wheel = pos;
    return d;
}

/* ---- pausing and events -------------------------------------------------- */

enum { PLAYING, RESTART, QUIT, USB, FAILED };

static void hold_backlight(bool hold)
{
    if (hold) {
        if (global_settings.backlight_timeout > 0)
            backlight_set_timeout(0);
        if (global_settings.backlight_timeout_plugged > 0)
            backlight_set_timeout_plugged(0);
    } else {
        backlight_set_timeout(global_settings.backlight_timeout);
        backlight_set_timeout_plugged(
            global_settings.backlight_timeout_plugged);
    }
}

/* The screen handed to the game, and handed back: each once, whichever
 * way the game ends, because the boost and the theme are counted. */
static bool screen_held;

static void take_screen(struct viewport *vp)
{
    if (screen_held)
        return;
    screen_held = true;
    viewportmanager_theme_enable(SCREEN_MAIN, false, vp);
    lcd_set_backdrop(NULL);
    lcd_clear_display();
    lcd_update();
    wheel_events(false);
    hold_backlight(true);
    cpu_boost(true);
}

static void give_screen(void)
{
    if (!screen_held)
        return;
    present_wait();
    screen_held = false;
    cpu_boost(false);
    hold_backlight(false);
    wheel_events(true);
    viewportmanager_theme_undo(SCREEN_MAIN, true);
}

/* A system event: saved first, since USB or a shutdown may follow. */
static int event(struct vm *vm, long button)
{
    present_wait();
    save_write(vm);
    if (default_event_handler(button) == SYS_USB_CONNECTED)
        return USB;
    return PLAYING;
}

/* The pause menu's rows: Resume, the cart's own (menuitem), Restart,
 * Quit. */
static char item_label[5][32];
static int item_index[5], item_count;

static const char *menu_row(int row, void *data, char *buf, size_t len)
{
    (void)data; (void)buf; (void)len;
    if (row == 0)
        return str(LANG_CHESSBOX_MENU_RESUME_GAME);
    if (row <= item_count)
        return item_label[row - 1];
    if (row == item_count + 1)
        return str(LANG_CHESSBOX_MENU_RESTART_GAME);
    return str(LANG_MENU_QUIT);
}

/* An item's function returning true keeps the menu up, relabelled, so a
 * cart can make a setting of its own; anything else resumes. */
static int pause_menu(struct vm *vm, struct viewport *vp)
{
    struct simplelist_info info;
    int selection = 0;
    present_wait();
    save_write(vm);
    give_screen();
    for (;;) {
        item_count = 0;
        for (int i = 1; i <= 5; i++)
            if (arcade_menu_item(vm, i, item_label[item_count],
                                 sizeof(item_label[0])))
                item_index[item_count++] = i;
        simplelist_info_init(&info, str(LANG_PLUGIN_GAMES), item_count + 3,
                             NULL);
        info.get_name = menu_row;
        info.selection = selection;
        push_current_activity(ACTIVITY_CONTEXTMENU);
        bool to_root = simplelist_show_list(&info);
        if (get_current_activity() == ACTIVITY_CONTEXTMENU)
            pop_current_activity();
        /* MENU held or USB: out to the root, as everywhere else */
        if (to_root)
            return USB;
        selection = info.selection;
        if (selection == item_count + 1)
            return RESTART;
        if (selection == item_count + 2)
            return QUIT;
        if (selection < 1 || selection > item_count)
            break;
        int r = arcade_menu_choose(vm, item_index[selection - 1], 4);
        if (r < 0)
            return FAILED;
        if (r == 0)
            break;
    }
    take_screen(vp);
    present(vm, true);
    return PLAYING;
}

/* The hold switch: the game waits, the screen shows it, until released. */
static int hold_pause(struct vm *vm)
{
    present_wait();
    save_write(vm);
    while (button_hold()) {
        long b = button_get_w_tmo(HZ / 10);
        if (b & SYS_EVENT) {
            int r = event(vm, b);
            if (r != PLAYING)
                return r;
        }
    }
    present(vm, true);
    return PLAYING;
}

/* Everything queued since the last frame. The buttons are read as held
 * state, so their events are only drained; system events are acted on. */
static int drain(struct vm *vm, bool *menu_pressed)
{
    long b;
    while ((b = button_get(false)) != BUTTON_NONE) {
        if (b & SYS_EVENT) {
            int r = event(vm, b);
            if (r != PLAYING)
                return r;
        } else if (b == BUTTON_MENU && !arcade_menu_is_game(vm))
            *menu_pressed = true;
    }
    return PLAYING;
}

/* ---- arcade_play() ------------------------------------------------------- */

static void show_error(struct vm *vm)
{
    const char *file, *fn;
    int line;
    const char *msg = arcade_error(vm, &file, &line, &fn);
    if (file)
        splashf(HZ * 6, "%s:%d: %s", file, line, msg);
    else
        splashf(HZ * 6, "%s", msg);
}

/* One cart from load to leaving; RESTART asks for it again. */
static int run(const char *path, void *mem, size_t size)
{
    struct viewport vp;
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        splashf(HZ * 3, "%s", path);
        return QUIT;
    }
    struct vm *vm = arcade_load(mem, (uint32_t)size, file_read, &fd, &host,
                                (uint64_t)current_tick * 2654435761u);
    close(fd);
    if (!vm)
        return QUIT;
    if (arcade_error(vm, NULL, NULL, NULL)) {
        show_error(vm);
        return QUIT;
    }
    save_load(vm);
    arcade_set_ceiling(vm, CEILING);

    take_screen(&vp);
    last_wheel = -1;
    menu_down = menu_paused = false;
    int status = PLAYING;
    if (!arcade_start(vm))
        status = FAILED;
    present(vm, true);

    int rate = arcade_rate(vm);
    long start = current_tick;
    unsigned long frame = 0;

    while (status == PLAYING) {
        bool menu_pressed = false;
        long due = start + (long)(((uint64_t)(frame + 1) * HZ) / rate);
        long now = current_tick;

        /* Waiting for the next tick: the last frame lands meanwhile */
        if (TIME_BEFORE(now, due)) {
            present_wait();
            now = current_tick;
        }
        if (TIME_BEFORE(now, due)) {
            long b = button_get_w_tmo(due - now);
            if (b & SYS_EVENT) {
                status = event(vm, b);
                continue;
            }
            if (b == BUTTON_MENU && !arcade_menu_is_game(vm))
                menu_pressed = true;
            if (TIME_BEFORE(current_tick, due) && !menu_pressed)
                continue;
        }
        status = drain(vm, &menu_pressed);
        if (status != PLAYING)
            break;

        if (arcade_menu_is_game(vm) && menu_held_long(button_status()))
            menu_pressed = true;
        if (menu_pressed) {
            status = pause_menu(vm, &vp);
            start = current_tick - (long)((frame * HZ) / rate);
            continue;
        }
        if (button_hold()) {
            status = hold_pause(vm);
            start = current_tick - (long)((frame * HZ) / rate);
            continue;
        }

        unsigned held = buttons(vm, button_status());
        int pos = wheel_position();
        frame++;

        /* Behind by a whole tick: one update without drawing, so the game
         * keeps its speed when a frame is slow; further behind than that,
         * the clock gives up the time rather than racing to catch it. */
        long late = current_tick - (start + (long)((frame * HZ) / rate));
        if (late >= HZ / rate) {
            arcade_input(vm, held, wheel_clicks(pos), pos);
            if (!arcade_tick(vm, 0)) {
                status = FAILED;
                break;
            }
            frame++;
            if (late >= 2 * HZ / rate)
                start = current_tick - (long)((frame * HZ) / rate);
        }
        arcade_input(vm, held, wheel_clicks(pos), pos);
        if (!arcade_tick(vm, 1)) {
            status = FAILED;
            break;
        }
        present(vm, false);
        arcade_idle(vm);
        reset_poweroff_timer();
        /* Once a frame whatever the cart does: a cart always behind its
         * clock never waits above, and one light on script never yields
         * inside a tick, so without this nothing else would run. */
        yield();
        if (arcade_quit_asked(vm))
            status = QUIT;
    }

    if (status == FAILED)
        present(vm, false);
    give_screen();
    if (status == FAILED) {
        show_error(vm);
        status = QUIT;
    }
    save_write(vm);
    return status;
}

int arcade_play(const char *path)
{
    size_t size;
    void *mem = app_try_claim_buffer(&size, "arcade");
    if (!mem) {
        splash(HZ * 2, ID2P(LANG_PLUGIN_GAMES));
        return GO_TO_PREVIOUS;
    }
    /* The engine needs 8-byte alignment. */
    uintptr_t skew = (8 - ((uintptr_t)mem & 7)) & 7;
    mem = (char *)mem + skew;
    size -= skew;
    if (size > ARENA_MAX)
        size = ARENA_MAX;

    int status;
    do
        status = run(path, mem, size);
    while (status == RESTART);

    app_release_buffer("arcade");
    return status == USB ? GO_TO_ROOT : GO_TO_PREVIOUS;
}
