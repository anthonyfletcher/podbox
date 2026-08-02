/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to dialog.c: the dialog_style descriptor and the frame/inset
 * helpers.
 ****************************************************************************/

/* Modal box primitive shared by the popup / yes-no / text-input dialogs.
 *
 * Two entry points, deliberately separate (see docs/design/dialog-widget.md):
 *
 *   dialog_frame_box() - pure drawing: fill + border a centred box and return
 *                        its content viewport. No theme, no input loop. Used by
 *                        the self-closing popup (was splash) and, internally,
 *                        by dialog_run().
 *
 *   dialog_run()       - the interactive path: theme enable/undo, the
 *                        skin-flush guard, and the get_action loop. Used by the
 *                        yes-no and text-input dialogs. The flush guard lives
 *                        inside the loop so no caller can forget it.
 *
 * Theming is by properties (struct dialog_style), not a render engine: colours,
 * border width/radius and fonts. Every colour defaults to
 * DIALOG_COLOR_INHERIT, i.e. "whatever the screen already has" - the theme's
 * colours, or an override a caller installed before drawing (the popup's
 * broken-theme fallback). So the default style reproduces the untheme
 * behaviour exactly, and a caller only names the fields it wants to change.
 */

#ifndef _GUI_DIALOG_H_
#define _GUI_DIALOG_H_

#include <stdbool.h>
#include "draw/screen_access.h"
#include "draw/viewport.h"

struct dialog;

/* ---------------------------------------------------------------------- *
 * Style                                                                  *
 * ---------------------------------------------------------------------- */

/* A colour that resolves, at draw time, to the one the screen already carries
 * (foreground for fg/border fields, background for bg fields). */
#define DIALOG_COLOR_INHERIT ((unsigned)-1)

/* Colours that resolve, also at draw time, to the theme's own foreground and
 * background -- album-derived while dynamic colours are running, so a dialog
 * follows the music the way the rest of the UI does.
 *
 * Not the same as INHERIT, which takes whatever the screen happens to carry:
 * inside a button that is the *box's* fill, so INHERIT there would give the box
 * background rather than the theme's. These five are absolute.
 *
 * BG2 is the background lifted toward the foreground, so a box reads as raised
 * off the screen behind it.
 *
 * ACCENT is the selection colour, and is derived from the background's hue
 * rather than mixed from the pair. Mixing spends contrast -- a muted foreground
 * is by definition nearer the background than the foreground was -- while
 * turning the hue keeps the distance and changes the colour instead.
 *
 * ON_ACCENT is whichever of the foreground and the background reads better on
 * ACCENT, and means nothing apart from it. A single fixed choice cannot work:
 * the accent's lightness is not tied to the pair it came from, so with a light
 * album the background-coloured text it was designed for falls to 1.4:1. */
#define DIALOG_COLOR_FG        ((unsigned)-2)
#define DIALOG_COLOR_BG        ((unsigned)-3)
#define DIALOG_COLOR_ACCENT    ((unsigned)-4)
#define DIALOG_COLOR_BG2       ((unsigned)-5)
#define DIALOG_COLOR_ON_ACCENT ((unsigned)-6)

/* A font that resolves to the one the parent (theme) viewport already carries. */
#define DIALOG_FONT_INHERIT  (-1)

struct dialog_style
{
    /* the box itself */
    unsigned box_fg;                /* interior text and graphics           */
    unsigned box_bg;                /* interior fill                        */
    unsigned box_border_color;
    int      box_border_width;      /* 0 == borderless                      */
    int      box_margin;            /* content inset inside the box         */
    int      box_font;

    /* the button row (the yes/no and text-input dialogs share one renderer) */
    unsigned button_fg;
    unsigned button_bg;
    unsigned button_border_color;
    unsigned button_fg_selected;
    unsigned button_bg_selected;
    unsigned button_border_color_selected;
    int      button_border_width;
    int      button_border_radius;
    int      button_font;
};

/* Every colour DIALOG_COLOR_INHERIT, every font DIALOG_FONT_INHERIT, 1px square
 * borders, 10px margin: the Stage 1-3 look, driven entirely by the theme. */
void dialog_style_default(struct dialog_style *s);

/* The style used by dialogs that pass NULL to dialog_init() and by the popup -
 * i.e. the one place to restyle every dialog at once. `s` is copied; NULL
 * restores dialog_style_default(). */
void dialog_set_default_style(const struct dialog_style *s);
const struct dialog_style *dialog_get_default_style(void);

/* How much `style` takes out of the box on each side (the margin). Sizing code
 * (a measure() callback) uses this to convert a content size into a box size. */
struct dialog_insets { int left, top, right, bottom; };
void dialog_get_insets(const struct dialog_style *style,
                       struct dialog_insets *out);

/* ---------------------------------------------------------------------- *
 * Frame primitive                                                        *
 * ---------------------------------------------------------------------- */

/* Draw an opaque, bordered box into `box`, which MUST already be the active
 * viewport on `screen` (carrying the final screen-relative geometry). It is
 * left active. `box` is not re-set here because set_viewport() reloads fg/bg
 * from the viewport and would undo a caller's colour override, which is how the
 * popup's broken-theme fallback reaches DIALOG_COLOR_INHERIT.
 *
 * `content_out` is filled with the content sub-viewport: `box` inset per
 * dialog_get_insets(), i.e. by the style's margin on every side. It is NOT made
 * active and NOT flushed - the caller draws the interior into it and owns the
 * update_viewport()/flush.
 *
 * `box`'s own colour patterns are updated to the style's resolved colours (and
 * inherited by content_out), so re-setting either viewport active keeps the
 * style rather than reloading the theme's colours.
 */
void dialog_frame_box(struct screen *screen, struct viewport *box,
                      const struct dialog_style *style,
                      struct viewport *content_out);

/* Draw a labelled button at (x, y, w, h) in the active viewport's coordinates,
 * in the style's normal or selected colours. Shared by the yes/no and
 * text-input dialogs. The screen's drawmode, foreground and font are restored
 * afterwards. */
void dialog_draw_button(struct screen *screen,
                        const struct dialog_style *style,
                        int x, int y, int w, int h,
                        const char *label, bool selected);

/* Where the label sits. Centred is right for a Cancel/OK pair; left is right
 * for a button that is really a row of a list. */
enum dialog_button_align {
    DIALOG_BTN_CENTRE = 0,
    DIALOG_BTN_LEFT,
};

/* As above, with the three things a list row needs that a Cancel/OK pair does
 * not:
 *
 *   `align`  - see above.
 *   `icon`   - optional 1bpp bitmap drawn at the label's left, in the label's
 *              colour, with the text indented past it. NULL for none.
 *   `scroll_px` - how far the label is scrolled left, in pixels. Clamped to
 *              what actually overflows, so 0 is always "start of the text".
 *   `overflow_out` - if non-NULL, receives how many pixels the label overflows
 *              the space available to it (0 when it fits). That is the range
 *              scroll_px is clamped to, so a caller animating the text has the
 *              one number it needs.
 *
 * Scrolling is the caller's to drive, deliberately: the shared scroll engine
 * redraws asynchronously using whatever colours are current when its thread
 * runs, which on a selected row is the wrong background and text in the
 * background's colour. A dialog repaints on every pass anyway, so stepping an
 * offset there costs nothing and is the same every frame.
 *
 * The label is always clipped to the button's interior, so it can never spill
 * over the border the way a bare putsxy() does. */
void dialog_draw_button_ex(struct screen *screen,
                           const struct dialog_style *style,
                           int x, int y, int w, int h,
                           const char *label, bool selected,
                           enum dialog_button_align align,
                           const struct bitmap *icon,
                           int scroll_px, int *overflow_out);

/* A wrapped display line: a (non-terminated) slice of the source text. */
struct dialog_text_line
{
    const char *str;
    int len;
};

/* Greedy word-wrap `text` into display lines no wider than `max_width` pixels
 * in `font`, writing up to `max_lines` slices into `out` (each pointing into
 * `text`). A single word wider than max_width is emitted on its own line (it
 * will clip). Returns the number of lines written. */
int dialog_wrap_text(const char *text, int max_width, int font,
                     struct dialog_text_line *out, int max_lines);

/* ---------------------------------------------------------------------- *
 * Interactive dialog                                                     *
 * ---------------------------------------------------------------------- */

enum dialog_disposition
{
    DIALOG_CONTINUE = 0,   /* keep looping                                 */
    DIALOG_ACCEPT,         /* close: OK / yes outcome                      */
    DIALOG_CANCEL,         /* close: cancel / no outcome                   */
    DIALOG_ABORT,          /* close: system event (e.g. USB); the callback *
                            * has stashed specifics in its own `data`      */
};

struct dialog_callbacks
{
    /* Fill `box` with the desired frame geometry (screen-relative x/y/w/h) for
     * `screen`. Called before each (re)draw with the theme-enabled parent
     * viewport active, so its font is set and text metrics are valid here. */
    void (*measure)(struct dialog *d, struct screen *s,
                    struct viewport *box, void *data);

    /* Draw the interior into `content` - the framed box's content viewport,
     * already active and already inset past the style's margin. The callback
     * owns its own sub-viewports (message block, button row, edit line, ...)
     * but not the outer padding. */
    void (*draw)(struct dialog *d, struct screen *s,
                 struct viewport *content, void *data);

    /* Handle one action - ACTION_NONE is delivered on each idle/poll timeout,
     * so timeout countdowns and expiry are handled here. Return a
     * dialog_disposition. The loop repaints the box on every pass (the
     * status-bar skin dirties the framebuffer inside get_action), so callbacks
     * just mutate state; the next redraw reflects it. */
    int  (*on_action)(struct dialog *d, int action, void *data);

    /* Optional (may be NULL): called once after the loop but before the theme
     * is undone, so a closing message can be drawn in the same theme scope
     * (e.g. the yes/no result message). */
    void (*on_close)(struct dialog *d, void *data);
};

struct dialog
{
    /* --- set by dialog_init(), read-only to callbacks --- */
    const struct dialog_callbacks *cb;
    void *data;
    int   context;              /* CONTEXT_* passed to get_action()         */
    const char *title;          /* SBS persistent title, or NULL for none   */
    struct dialog_style style;  /* resolved copy; measure/draw may read it  */

    /* --- maintained by dialog_run(), readable by callbacks --- */
    struct viewport parent[NB_SCREENS]; /* theme-enabled parent per screen  */
    bool  backlight_on;         /* backlight state sampled before get_action */

    /* The get_action timeout for the NEXT pass, seeded from dialog_run()'s
     * argument. A callback may write it to change how often it is woken with
     * ACTION_NONE -- a dialog that animates something can ask for a fast tick
     * only while the animation is running, and go back to idling after. Every
     * pass costs a full repaint of the box (see dialog_run), so this is the
     * difference between animating and burning CPU. */
    int   poll_ticks;
};

/* Initialise a dialog. `title` and `style` may be NULL (NULL style == the
 * default style, see dialog_set_default_style()); `style` is copied. The struct
 * is caller-allocated (typically on the stack); treat the fields as opaque. */
void dialog_init(struct dialog *d, int context, const char *title,
                 const struct dialog_style *style,
                 const struct dialog_callbacks *cb, void *data);

/* Enable the theme, then loop: repaint the box, inhibit the skin flush, get one
 * action (with `poll_ticks` timeout - HZ/2 for a countdown, TIMEOUT_BLOCK for
 * none), and dispatch it to on_action. Undo the theme on exit. Returns the
 * final dialog_disposition. */
int dialog_run(struct dialog *d, int poll_ticks);

#endif /* _GUI_DIALOG_H_ */
