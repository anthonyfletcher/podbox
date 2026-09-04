/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The line art. Black ink on white, primitives only: no bitmaps, no
 * greyscale, no asset pipeline. White ground reads better than the inverse
 * on the transflective panel in daylight, and it is the hand-drawn language
 * the design is written in.
 *
 * Geometry is static. Every line is drawn at exact integer coordinates and
 * stays there, because the game asks the player to judge two cells ahead
 * and in peripheral vision whether a gap is jumpable -- and any per-frame
 * movement that is not the scroll competes with that judgement. Precision
 * reads as precision.
 *
 * Parts, in order:
 *   - the world
 *   - the player
 *   - the chrome
 *   - the frame
 ****************************************************************************/

#include <stdio.h>
#include <string.h>
#include "config.h"
#include "lcd.h"
#include "draw/viewport.h"
#include "font.h"
#include "settings/settings.h"          /* global_settings fg/bg */
#include "skin/skin_albumart_color.h"    /* dynamic_colors_resolve */
#include "games/spike/spike_draw.h"
#include "games/spike/spike_pose.h"
#include "draw/screen_access.h"
#include "draw/scrollbar.h"

/* Cells drawn either side of the ten on screen, so a cell entering or
 * leaving is complete before it is clipped. */
#define SPK_MARGIN       2

/* The pose tables' resolution, which the stomp counts its frames in so the
 * two agree about what a frame is. */
#define SPK_ROWS_PER_BEAT 16

/* A diamond: half-height fixed, half-width flipped between two on the beat
 * so it reads as turning. Two frames rather than four, because two is what
 * the hatch and the creature's legs do and one pulse across the whole field
 * is worth more than any of them being smoother alone. */
#define SPK_DIAMOND_H    6
#define SPK_DIAMOND_WIDE 6
#define SPK_DIAMOND_THIN 2

/* A creature: a solid slab of a body carried on two legs.
 *
 * The legs are where the life is. They snap straight on the beat -- knees
 * together, body up -- and relax back out over the rest of it, so the body
 * bobs because the legs did something rather than because the whole shape
 * was moved. It never leaves its cell, so this is all it has.
 *
 * The body is the only filled shape on the field. Everything else is
 * outline, so a mass of solid black is unmistakable at two cells' distance,
 * which is what the one thing that kills on contact has to be. */
#define SPK_CR_TOP       12      /* half-width at the top of the slab */
#define SPK_CR_BOT       9       /* ...and at the bottom */
#define SPK_CR_BODY      11      /* the head's depth, hollow inside */
#define SPK_CR_UP        12      /* leg length, straightened */
#define SPK_CR_DOWN      4       /* ...and relaxed */
#define SPK_CR_FOOT_IN   2       /* half the stance, straightened */
#define SPK_CR_FOOT_OUT  14      /* ...and relaxed */
#define SPK_CR_HIP       3       /* half the gap between the legs at the top */

/* How high the head stands, as a level in 8.8 -- which is what the arc of a
 * jump coming down on one has to end at. */
#define SPK_CR_HEAD8     (((SPK_CR_UP + SPK_CR_BODY + 3) * 256) / SPK_LEVEL_PX)

/* A rising block. Small: a box the size of the player rather than a slab
 * filling its cell, because a cell-wide obstacle cannot be jumped *over* in
 * any way the eye believes, and because one that spills into the neighbouring
 * cell looks as though it has landed on a player who is still safely in it.
 *
 * Down it has to fill the space a walking body takes; up it has to reach
 * into the arc of a jump and leave a walker clear underneath. */
#define SPK_BLOCK_W      7
#define SPK_BLOCK_H      14

/* How far it rises, which has to be SPK_BLOCK_LEVEL levels: the rules say a
 * raised block fills that level, and this is where that is drawn. */
#define SPK_BLOCK_LIFT   (SPK_LEVEL_PX * SPK_BLOCK_LEVEL)

/* A switch: a pad on a stem, standing proud until it is landed on and
 * pressed flat. And the height a waiting platform's ghost line sits at. */
#define SPK_SW_W         9
#define SPK_SW_UP        6

/* A spring: a cap on a zigzag.
 *
 * The cap keeps still -- one whose height moves on the beat is one a landing
 * cannot meet, and standing on it is the whole point of it. The coil keeps
 * the beat instead, by mirroring rather than by moving: the same trick the
 * platforms' hatching plays, and for the same reason. A thing can be on the
 * beat without going anywhere. */
#define SPK_SPR_W        7       /* half-width of the cap */
#define SPK_SPR_CAP      3       /* ...and its thickness */
#define SPK_SPR_TALL     14      /* the cap above the ground, at rest */
#define SPK_SPR_LOW      4       /* ...squashed under the arrival */
#define SPK_SPR_KICK     19      /* ...and thrown past rest, launching */
#define SPK_SPR_ZIG      3       /* diagonals under the cap */

/* The spike on a creature's head: tall enough to read at a glance, because
 * mistaking it for an ordinary creature costs the run. */
#define SPK_SPK_W        5
#define SPK_SPK_H        8

/* How far above its landing the body starts, and the shape of the fall.
 *
 * Held inside the field: the drop begins under the rule rather than off the
 * top of the panel, because the soft edges are clipped to the field and a
 * body that starts above it arrives as a line of severed pixels. Squared, so
 * it accelerates -- a fall at a constant speed reads as being lowered. */
#define SPK_DROP_H      110

/* The extra flattening it lands with, on top of the landing pose's own. That
 * pose is written for an ordinary hop and this is a fall out of the sky, so
 * it wants more of the same thing rather than a different shape -- and only
 * over the first rows, while the body is still absorbing it. */
#define SPK_DROP_SQUASH   3

/* The flyer: the same animal on its side, head into the wind.
 *
 * Turned a quarter turn, the legs trail behind it and kick it along instead
 * of carrying it, and the spike on its head leads -- which is the whole of
 * why it kills what it runs into and is killed by what comes down on it.
 * Nothing about the shape changes; only which way is up for it.
 *
 * SPK_FLY_MID is the centre line, above the surface it flies over. Set so
 * the body fills the band a standing player's own body occupies -- they meet
 * head on, and it has to look like it. The relaxed kick splays a leg a pixel
 * past the floor line, which reads as flying low. */
#define SPK_FLY_MID     13
#define SPK_FLY_LONG    (SPK_SPK_H + SPK_CR_BODY + SPK_CR_UP)

/* ...and the beats either side of its arrival that it is drawn for. It and
 * the player close at two cells a beat and the field shows seven ahead, so
 * three and a half beats is the whole of the warning there can be -- this
 * only has to be past that. */
#define SPK_FLY_LEAD     4

/* The hook that turns down at each side of a gap, and the shorter tick at
 * every other cell boundary. The hook is what makes a gap countable rather
 * than merely visible; the tick is what makes a *cell* countable, which is
 * the unit every decision in the game is made in. At half the hook's length
 * the two do not read alike. */
#define SPK_HOOK_PX      8
#define SPK_TICK_PX      4

/* How far into a cell the body has travelled when it meets what is standing
 * in the next one: about twelve pixels of the thirty-two, in eighths. */
#define SPK_TOUCH8       96

/* One weight for the whole field. Most strokes are drawn as a pair of passes
 * an offset apart rather than by any cleverness: they are horizontal or
 * vertical, and an axis-aligned line has nothing to smooth. The three shapes
 * that are mostly diagonal -- the player, the diamonds, the creature's
 * legs -- use the anti-aliased stroke below instead. */
#define SPK_INK          2

/* Space between the strokes of a platform's hatch. Every stroke is a
 * drawline against the three hlines a flat hatch cost, so this is the one
 * number that decides whether the field is cheap to draw: at 4 a platform
 * is about ten of them. */
#define SPK_HATCH_STEP   8

/* The tail a jump leaves: five dashes along the arc just travelled, and the
 * gap between them as a fraction of the two-beat jump. 24 of 256 is six
 * pixels, which is the coarsest the arc still reads as a curve and the
 * finest the dashes stay separate.
 *
 * While the body is in the air the tail is a pure function of where it is,
 * and it is held in the world rather than behind the player so it stays put
 * as the world scrolls. At the end of the jump it is simply let go, and
 * falls. Shedding a mote every few frames instead reads as the triangle
 * coming apart, which is not what a tail is. */
#define SPK_TRAIL        8
#define SPK_TRAIL_JUMP   5
#define SPK_TRAIL_STEP   24
#define SPK_TRAIL_MS     900
#define SPK_TRAIL_FALL   1400

/* One pose row back per dash while falling, so the tail is a picture of the
 * fall's own acceleration: evenly spaced in time and so spreading out as it
 * goes. It grows with the level fallen from, because a drop from up there
 * has further to be seen falling. */
#define SPK_FALL_STEP    (SPK_PHASE / 16)


/** The soft stroke **/

/* Anti-aliasing, for the three shapes that are diagonal enough to need it:
 * the player, the diamonds and the creature's legs. Everything else on the
 * field is horizontal or vertical, and an axis-aligned line has nothing to
 * smooth.
 *
 * The trick that makes it nearly free is that the field is one flat colour
 * behind every line, so there is nothing to blend *with*: a seventeen-entry
 * ramp from the background to the ink, built once a frame, turns a coverage
 * into a colour with an index and a store. A read-modify-write blend per
 * pixel would cost about five times as much and produce the same picture.
 *
 * The one read is to stop a stroke's soft edge eating the solid core of the
 * stroke it meets at a corner. The ramp is a straight line between two
 * colours, so the sum of a pixel's components moves monotonically along it
 * and answers "is this already at least this covered?" without having to
 * work out which entry it came from. Where the two ends of the ramp happen
 * to have the same sum there is no order to read and the test is skipped. */
#define SPK_AA_STEPS     16

static fb_data aa_ink[SPK_AA_STEPS + 1];
static fb_data aa_accent[SPK_AA_STEPS + 1];

static const fb_data *aa_ramp = aa_ink;
static bool    full_flush = true;   /* the palette moved: send the lot once */

/* The caption's room and pace. The gap is what keeps it from reading as one
 * line with the score; the wrap is the clear space before it comes round
 * again, and without it a long title looks like it stutters. */
#define SPK_CAP_EDGE     4      /* in from the left edge */
#define SPK_CAP_GAP     14      /* ...and clear of the score's block */
#define SPK_CAP_MIN     48      /* narrower than this and there is no room */

/* The crown and the multiplier, which are both notes about the score and so
 * stand beside it.
 *
 * In the score's own face, not the block glyphs: the multiplier is a second
 * number in a line of numbers, and a different typeface for it reads as a
 * different piece of software. That is also what makes it legible: the block
 * glyphs are unreadable at the seven pixels there is room for beside the
 * score, and the twenty-one they need puts the multiplier out on the field,
 * legible and belonging to nothing. The crown takes the scale that matches
 * the face.
 *
 * Fixed slots rather than a row that reflows. Each comes and goes with the
 * run, and a pair that shuffled along as the other appeared would read as
 * the band twitching. The multiplier is two characters at every value it can
 * take and the face is monospaced, so its slot is a constant width -- what
 * the title gives up for them it gives up once. */
#define SPK_TAG_GAP      6

/* The crown, against a line of type: shorter than the face's own height,
 * which includes the room a descender wants and a crown does not. */
#define SPK_CROWN_H(th)  ((th) * 5 / 8)

/* The volume read-out, in the caption's room. */
#define SPK_VOL_H       14      /* the horn's mouth, top to bottom */
#define SPK_VOL_BOX      5      /* the driver's width */
#define SPK_VOL_FLARE    5      /* ...and the horn's */
#define SPK_VOL_CONE    (SPK_VOL_BOX + SPK_VOL_FLARE)
#define SPK_VOL_LIP      4      /* the throat, in from top and bottom */
#define SPK_VOL_GAP      6      /* between the cone and the bar */
#define SPK_VOL_INSET    2      /* the fill sits inside the trough */
#define SPK_VOL_MIN     24      /* narrower than this and there is no bar */

/* The pair the whole screen is drawn in, kept because a viewport installed
 * afterwards comes up in FG_FALLBACK/BG_FALLBACK and has to be told. */
static unsigned was_ink, was_paper;
static bool     colors_known;

/* The complement of a colour, at the same lightness: subtract each channel
 * from the sum of the largest and smallest, which is a half turn of the hue
 * wheel with the saturation and the lightness left where they were. No
 * contrast target is applied on top, because forcing one whitens every
 * accent -- see the dynamic-colour work.
 *
 * A grey has no hue to turn, and comes back as itself. The theme's selection
 * colour stands in there: a theme picks it to be seen against its own
 * background, which is exactly the job. */
static unsigned spk_complement(unsigned c)
{
    int r = RGB_UNPACK_RED(c), g = RGB_UNPACK_GREEN(c);
    int b = RGB_UNPACK_BLUE(c);
    int hi = r > g ? (r > b ? r : b) : (g > b ? g : b);
    int lo = r < g ? (r < b ? r : b) : (g < b ? g : b);

    if (hi - lo < 24)
        return dynamic_colors_resolve(global_settings.lss_color);

    return LCD_RGBPACK(hi + lo - r, hi + lo - g, hi + lo - b);
}



static short   aa_key[SPK_AA_STEPS + 1];
static bool    aa_ordered;      /* the ramp's ends differ, so it has an order */
static bool    aa_rising;       /* ...and which way it runs */

static int spk_key(unsigned c)
{
    return RGB_UNPACK_RED(c) + RGB_UNPACK_GREEN(c) + RGB_UNPACK_BLUE(c);
}

static void spk_ramp(fb_data *out, unsigned paper, unsigned ink)
{
    int pr = RGB_UNPACK_RED(paper), pg = RGB_UNPACK_GREEN(paper);
    int pb = RGB_UNPACK_BLUE(paper);
    int dr = RGB_UNPACK_RED(ink) - pr, dg = RGB_UNPACK_GREEN(ink) - pg;
    int db = RGB_UNPACK_BLUE(ink) - pb;
    int i;

    for (i = 0; i <= SPK_AA_STEPS; i++)
        out[i] = LCD_RGBPACK(pr + dr * i / SPK_AA_STEPS,
                             pg + dg * i / SPK_AA_STEPS,
                             pb + db * i / SPK_AA_STEPS);
}

static void spk_aa_use(const fb_data *ramp)
{
    int i;

    aa_ramp = ramp;

    for (i = 0; i <= SPK_AA_STEPS; i++)
        aa_key[i] = (short)spk_key(ramp[i]);

    aa_rising = aa_key[SPK_AA_STEPS] > aa_key[0];
    aa_ordered = aa_key[SPK_AA_STEPS] != aa_key[0];
}

/* The rows a soft edge may land on. The field's own drawing is held to the
 * field, so an anti-aliased line cannot spill into the band the score sits
 * in or the strip below the floor; the summary screen owns the whole panel
 * and says so. Each screen states its own bounds on the way in, so neither
 * depends on the other having tidied up. */
static int aa_top = SPK_HUD_H;
static int aa_bot = SPK_UPDATE_H;

static void spk_aa_put(int x, int y, int cover)
{
    fb_data *p;
    int k = cover >> 4;

    if (k <= 0 || x < 0 || y < aa_top || x >= LCD_WIDTH || y >= aa_bot)
        return;

    if (k > SPK_AA_STEPS)
        k = SPK_AA_STEPS;

    p = FBADDR(x, y);

    if (aa_ordered)
    {
        int had = spk_key(*p);

        if (aa_rising ? had >= aa_key[k] : had <= aa_key[k])
            return;
    }

    *p = aa_ramp[k];
}

/* A run of pixels from lo to hi in eighths of a pixel, with whatever falls
 * inside each one as its coverage. Both spans are the same idea and neither
 * is worth writing twice, but they walk the framebuffer differently enough
 * that a single one taking an axis would cost more than it saved. */
static void spk_aa_vspan(int x, int lo, int hi)
{
    int p = lo >> 8, last = (hi - 1) >> 8;

    for (; p <= last; p++)
    {
        int a = p << 8, b = a + 256;

        spk_aa_put(x, p, (hi < b ? hi : b) - (lo > a ? lo : a));
    }
}

static void spk_aa_hspan(int y, int lo, int hi)
{
    int p = lo >> 8, last = (hi - 1) >> 8;

    for (; p <= last; p++)
    {
        int a = p << 8, b = a + 256;

        spk_aa_put(p, y, (hi < b ? hi : b) - (lo > a ? lo : a));
    }
}

/* A stroke SPK_INK pixels wide with soft edges.
 *
 * Wu's algorithm draws one pixel and two fringes, and a second pass beside it
 * does not make a two-pixel line -- it makes two one-pixel lines with a seam
 * down the middle. The width has to come from the geometry instead: at every
 * step along the major axis the stroke covers a span, and each pixel the span
 * touches takes the part of itself that falls inside it.
 *
 * The span is longer than the stroke is wide wherever the line is not
 * axis-aligned, by the ratio of its length to its major axis; without that a
 * diagonal reads thinner than the horizontal beside it. The length is the
 * usual max-plus-three-eighths-min approximation, which is within a few per
 * cent and costs no square root. */
static void spk_aa_line(int x0, int y0, int x1, int y1)
{
    int dx = x1 - x0, dy = y1 - y0;
    int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
    int major = ax > ay ? ax : ay, minor = ax > ay ? ay : ax;
    int half, i;

    if (major == 0)
    {
        spk_aa_put(x0, y0, 256);
        return;
    }

    half = (SPK_INK * 128 * (major + (minor * 3) / 8)) / major;

    if (ax >= ay)
    {
        int sx = dx < 0 ? -1 : 1;

        for (i = 0; i <= ax; i++)
        {
            int c = (y0 << 8) + (dy * (i << 8)) / ax;

            spk_aa_vspan(x0 + sx * i, c - half, c + half);
        }
    }
    else
    {
        int sy = dy < 0 ? -1 : 1;

        for (i = 0; i <= ay; i++)
        {
            int c = (x0 << 8) + (dx * (i << 8)) / ay;

            spk_aa_hspan(y0 + sy * i, c - half, c + half);
        }
    }
}


/** The world **/

static int spk_level_y(int level8)
{
    return SPK_GROUND_Y - ((SPK_LEVEL_PX * level8) >> 8);
}

static struct
{
    long  world8;           /* where the dash sits, in 8.8 cells */
    short y;
} trail[SPK_TRAIL];

static int           trail_n;      /* dashes held; 0 when there is no tail */
static bool          trail_falling;
static unsigned long trail_at;     /* grid time it was let go */

void spk_draw_reset(void)
{
    trail_n = 0;
    trail_falling = false;
}

void spk_draw_full_flush(void)
{
    full_flush = true;
}

/* How far into the cell the world has scrolled.
 *
 * A death holds the field exactly where it was on the frame the death began,
 * and that number is carried in rather than assumed. It cannot be assumed: a
 * contact happens part-way through a beat, and the frame that notices it is
 * however far past that the frame rate put it. Freezing at a nominal
 * position instead steps the whole screen backwards by the difference --
 * which is small, and is visible precisely because everything else has
 * stopped. */
static int spk_scroll8(const struct spk_frame *f)
{
    if (f->death_phase >= 0)
        return f->death_scroll;

    if (f->frozen)
        return 0;

    return spk_ease(f->phase);
}

static int spk_scroll(const struct spk_frame *f)
{
    return (spk_scroll8(f) * SPK_CELL_PX) >> 8;
}


static void spk_hline(int x1, int x2, int y)
{
    lcd_hline(x1, x2, y);
    lcd_hline(x1, x2, y + 1);
}

static void spk_vline(int x, int y1, int y2)
{
    lcd_vline(x, y1, y2);
    lcd_vline(x + 1, y1, y2);
}

static void spk_line(int x1, int y1, int x2, int y2)
{
    lcd_drawline(x1, y1, x2, y2);
    lcd_drawline(x1 + 1, y1, x2 + 1, y2);
}

/* The floor does not pulse. It used to thicken for the head of each beat,
 * which was the one weight in the line art that changed -- and once the
 * hatch, the diamonds and the creature's legs all keep the beat, a floor
 * that also flickers is a fourth thing saying the same word underneath the
 * three that say it in place. Let the floor be the floor. */
static void spk_draw_ground(int cell, int x)
{
    int right = x + SPK_CELL_PX - 1;

    spk_hline(x, right, SPK_GROUND_Y);

    if (!(spk_world_live(cell - 1) & 1u))
        spk_vline(x, SPK_GROUND_Y, SPK_GROUND_Y + SPK_HOOK_PX - 1);
    else
        spk_vline(x, SPK_GROUND_Y, SPK_GROUND_Y + SPK_TICK_PX - 1);

    if (!(spk_world_live(cell + 1) & 1u))
        spk_vline(right - 1, SPK_GROUND_Y, SPK_GROUND_Y + SPK_HOOK_PX - 1);
}

/* Diagonals at 45 degrees, clipped to a box. Every stroke is a separate
 * line rather than a fill because the box is six pixels tall: a stroke
 * crossing it is six pixels long, and what carries the direction is having
 * several of them rather than any one being long. */
static void spk_hatch(int x0, int y0, int x1, int y1, bool lean)
{
    int h = y1 - y0;
    int b;

    for (b = x0 - h; b <= x1; b += SPK_HATCH_STEP)
    {
        int sx = b < x0 ? x0 : b;
        int ex = b + h > x1 ? x1 : b + h;

        if (sx > ex)
            continue;

        if (lean)
            spk_line(sx, y1 - (sx - b), ex, y1 - (ex - b));
        else
            spk_line(sx, y0 + (sx - b), ex, y0 + (ex - b));
    }
}

/* A cell with no floor says so at its own edges, not only at the two ends
 * of the gap it belongs to. Marking the ends alone leaves the middle of a
 * long void blank, and blank is what safe ground with the line scrolled off
 * also looks like -- so a four-cell drop reads as nothing at all until the
 * player is in it. Hooks turn downward, so a run of them is a comb rather
 * than a dashed floor: nothing here to stand on. */
static void spk_draw_void(int x)
{
    spk_vline(x, SPK_GROUND_Y, SPK_GROUND_Y + SPK_HOOK_PX - 1);
    spk_vline(x + SPK_CELL_PX - 2, SPK_GROUND_Y,
             SPK_GROUND_Y + SPK_HOOK_PX - 1);
}

/* Rotated square, four lines, floating clear of the surface it sits over. */
/* The one thing on the field drawn in a colour of its own: four facets, all
 * of them diagonal, in the complement of the ink. It is the only thing the
 * player goes out of their way for, and a shape that is only worth a press
 * has to be findable in the corner of the eye. */
static void spk_draw_diamond(const struct spk_frame *f, int x, int level)
{
    int cx = x + SPK_CELL_PX / 2;
    int cy = SPK_GROUND_Y - SPK_LEVEL_PX * level - SPK_DIAMOND_H - 5;
    int w = f->strong ? SPK_DIAMOND_WIDE : SPK_DIAMOND_THIN;

    spk_aa_use(aa_accent);
    spk_aa_line(cx, cy - SPK_DIAMOND_H, cx + w, cy);
    spk_aa_line(cx + w, cy, cx, cy + SPK_DIAMOND_H);
    spk_aa_line(cx, cy + SPK_DIAMOND_H, cx - w, cy);
    spk_aa_line(cx - w, cy, cx, cy - SPK_DIAMOND_H);
}

/* The head: a hollow trapezoid, wider at the top. Drawn as an outline and
 * not a fill, so the creature is a shell on solid legs -- the one shape on
 * the field that is empty where everything else of its size is either line
 * or body. */
static void spk_draw_shell(int cx, int top, int depth, int half_top,
                          int half_bot)
{
    int bot = top + depth - SPK_INK;

    spk_hline(cx - half_top, cx + half_top, top);
    spk_hline(cx - half_bot, cx + half_bot, bot);
    spk_line(cx - half_top, top, cx - half_bot, bot);
    spk_line(cx + half_top - SPK_INK, top, cx + half_bot - SPK_INK, bot);
}

/* A leg: a wedge from a span at the hip down to a point at the foot. Filled
 * by rows for the same reason the slab is -- the shape is not a rectangle
 * and there is no cheaper honest way to say so. */
/* A filled wedge, and its two long edges are what the eye reads as the leg
 * straightening. Drawn a row at a time with the ends kept in eighths of a
 * pixel, so the edges arrive where the arithmetic put them rather than where
 * the rounding did. */
static void spk_draw_leg(int xl, int xr, int ytop, int xtip, int ybot)
{
    int h = ybot - ytop;
    int y;

    if (h <= 0)
        return;

    spk_aa_use(aa_ink);

    for (y = 0; y <= h; y++)
    {
        int a = (xl << 8) + ((xtip - xl) * (y << 8)) / h;
        int b = (xr << 8) + ((xtip - xr) * (y << 8)) / h;

        spk_aa_hspan(ytop + y, a < b ? a : b, a < b ? b : a);
    }
}

static void spk_draw_creature(const struct spk_frame *f, int x, int level,
                              bool spiked)
{
    int cx = x + SPK_CELL_PX / 2;
    int base = SPK_GROUND_Y - SPK_LEVEL_PX * level;

    /* Up on one beat and down on the next, which is a step rather than a
     * sweep -- the same two-frame pulse the hatch and the diamonds keep, so
     * the whole field moves on one beat instead of four things each being
     * smooth on their own. The body bobs *because the legs did something*,
     * which is why the slab is never simply translated. */
    int lift = f->strong ? SPK_CR_UP : SPK_CR_DOWN;
    int foot = f->strong ? SPK_CR_FOOT_IN : SPK_CR_FOOT_OUT;
    int hip = base - lift;

    spk_draw_shell(cx, hip - SPK_CR_BODY, SPK_CR_BODY, SPK_CR_TOP, SPK_CR_BOT);

    spk_draw_leg(cx - SPK_CR_BOT, cx - SPK_CR_HIP, hip, cx - foot, base);
    spk_draw_leg(cx + SPK_CR_HIP, cx + SPK_CR_BOT, hip, cx + foot, base);

    /* Landing on this one is fatal, so it is drawn on the surface a landing
     * would touch: the one place the player is already looking when the
     * press is decided. */
    if (spiked)
    {
        int top = hip - SPK_CR_BODY;

        spk_line(cx - SPK_SPK_W, top, cx, top - SPK_SPK_H);
        spk_line(cx + SPK_SPK_W, top, cx, top - SPK_SPK_H);
    }
}

/* The shell and a leg, turned a quarter turn: the same two drawings about
 * swapped axes, spans down the screen instead of across. Written out rather
 * than folded into the standing pair behind an axis flag -- the flag would
 * be read on every span of every leg to say something that is decided once,
 * and the shapes are five lines each. */
static void spk_draw_shell_side(int nose, int mid, int depth, int half_lead,
                                int half_tail)
{
    int tail = nose + depth - SPK_INK;

    spk_vline(nose, mid - half_lead, mid + half_lead);
    spk_vline(tail, mid - half_tail, mid + half_tail);
    spk_line(nose, mid - half_lead, tail, mid - half_tail);
    spk_line(nose, mid + half_lead - SPK_INK, tail, mid + half_tail - SPK_INK);
}

static void spk_draw_leg_side(int ytop, int ybot, int xhip, int ytip,
                              int xtip)
{
    int w = xtip - xhip;
    int x;

    if (w <= 0)
        return;

    spk_aa_use(aa_ink);

    for (x = 0; x <= w; x++)
    {
        int a = (ytop << 8) + ((ytip - ytop) * (x << 8)) / w;
        int b = (ybot << 8) + ((ytip - ybot) * (x << 8)) / w;

        spk_aa_vspan(xhip + x, a < b ? a : b, a < b ? b : a);
    }
}

/* ...and the animal it makes, kicking twice as often because it is covering
 * twice the ground: it closes a cell a beat against a world already
 * scrolling one, so on the panel it comes at the player at two. The
 * half-beat flip carries straight on across the beat, so the kicks are even
 * rather than paired.
 *
 * Straight legs are the thrust and splayed ones the recovery, which is the
 * standing creature's own two frames read sideways -- the legs snap back
 * together and long, then fold and spread. Nothing was written for it. */
static void spk_draw_flyer(const struct spk_frame *f, int x, int level)
{
    int cx = x + SPK_CELL_PX / 2;
    int mid = SPK_GROUND_Y - SPK_LEVEL_PX * level - SPK_FLY_MID;
    bool kick = f->strong != (f->phase >= SPK_PHASE / 2);
    int lift = kick ? SPK_CR_UP : SPK_CR_DOWN;
    int foot = kick ? SPK_CR_FOOT_IN : SPK_CR_FOOT_OUT;
    int nose = cx - SPK_FLY_LONG / 2 + SPK_SPK_H;
    int hip = nose + SPK_CR_BODY;

    spk_draw_shell_side(nose, mid, SPK_CR_BODY, SPK_CR_TOP, SPK_CR_BOT);

    spk_draw_leg_side(mid - SPK_CR_BOT, mid - SPK_CR_HIP, hip, mid - foot,
                      hip + lift);
    spk_draw_leg_side(mid + SPK_CR_HIP, mid + SPK_CR_BOT, hip, mid + foot,
                      hip + lift);

    /* The spike, out in front, which is the thing a walk runs into. */
    spk_line(nose, mid - SPK_SPK_W, nose - SPK_SPK_H, mid);
    spk_line(nose, mid + SPK_SPK_W, nose - SPK_SPK_H, mid);
}

/* Where the flyer is now, read backwards from the column being drawn.
 *
 * It meets the player in the cell it is authored in, on that cell's own
 * beat; before then it is one cell further right for every beat still to
 * come. So the cell at column c holds it when M = (c + b) / 2 -- a whole
 * number only every other column, and always one for the flyer itself,
 * since c + b = 2M by construction.
 *
 * Bounded either side so it flies in rather than appearing, and carries on
 * past rather than vanishing where it missed. */
static bool spk_flyer_at(const struct spk_frame *f, int cell, int *level)
{
    int b = f->st->beat;
    int m;

    if (((cell + b) & 1) != 0)
        return false;

    m = (cell + b) / 2;

    if (m - b > SPK_FLY_LEAD || b - m > SPK_FLY_LEAD)
        return false;

    return spk_world_flyer(m, level);
}

/* How high the cap is, for the instant being drawn.
 *
 * The body reads this too, so the two cannot disagree about where the plate
 * is -- which is the whole of standing on one. Squashed by the arrival, then
 * thrown past its own rest height: the kick is what launches the body rather
 * than the arc simply beginning, and three rows is what makes it visible at
 * thirty frames a second. */
static int spk_spring_top(const struct spk_frame *f, int cell)
{
    if (f->st->motion != SPK_ARC_SPRING || cell != f->st->beat)
        return SPK_SPR_TALL;

    switch ((f->phase * SPK_ROWS_PER_BEAT) / SPK_PHASE)
    {
    case 0:  return SPK_SPR_LOW;
    case 1:  return (SPK_SPR_LOW + SPK_SPR_TALL) / 2;
    case 2:  return SPK_SPR_KICK;
    default: return SPK_SPR_TALL;
    }
}

/* A spring, always on the ground. The zigzag is a run of segments rather
 * than a coil, for the reason everything else here is straight lines: a
 * curve at this size is a smudge. */
static void spk_draw_spring(const struct spk_frame *f, int x, int cell)
{
    int cx = x + SPK_CELL_PX / 2;
    int h = spk_spring_top(f, cell);
    int top = SPK_GROUND_Y - h;
    int lean = f->strong ? 0 : 1;
    int i;

    /* The cap carries the weight, so it is drawn heavier than the coil under
     * it: at the same thickness it reads as one more turn of the zigzag
     * rather than as the top of it. */
    lcd_fillrect(cx - SPK_SPR_W, top, 2 * SPK_SPR_W, SPK_SPR_CAP);

    /* Anti-aliased, like every other diagonal here. A stepped zigzag beside
     * a smooth triangle is the one shape that says the drawing is unfinished
     * -- the aliasing is read as the object, not as the renderer. */
    spk_aa_use(aa_ink);

    for (i = 0; i < SPK_SPR_ZIG; i++)
    {
        int span = h - SPK_SPR_CAP;
        int y0 = top + SPK_SPR_CAP + (span * i) / SPK_SPR_ZIG;
        int y1 = top + SPK_SPR_CAP + (span * (i + 1)) / SPK_SPR_ZIG;
        int x0 = ((i ^ lean) & 1) ? cx + SPK_SPR_W : cx - SPK_SPR_W;

        spk_aa_line(x0, y0, 2 * cx - x0, y1);
    }
}

/* What is left of a diamond that has been eaten: the four edges coming
 * apart and going the way each of them faced, shrinking as they go. It
 * needs no state -- the beat says it happened, the cell says where. */
static void spk_draw_eaten(const struct spk_frame *f, int x, int level)
{
    static const signed char away[4][2] = {
        { 1, -1 }, { 1, 1 }, { -1, 1 }, { -1, -1 }
    };
    int cx = x + SPK_CELL_PX / 2;
    int cy = SPK_GROUND_Y - SPK_LEVEL_PX * level - SPK_DIAMOND_H - 5;
    int row = (f->phase * SPK_ROWS_PER_BEAT) / SPK_PHASE;
    int gone, len, i;

    if (row >= 5)
        return;

    gone = 3 + row * 3;
    len = 4 - row;

    spk_aa_use(aa_accent);

    for (i = 0; i < 4; i++)
    {
        int fx = cx + away[i][0] * gone;
        int fy = cy + away[i][1] * gone;

        spk_aa_line(fx - away[i][0] * len, fy - away[i][1] * len,
                   fx + away[i][0] * len, fy + away[i][1] * len);
    }
}

/* What a stomped creature leaves behind, for the first fraction of the beat
 * it was stomped on: a wedge flattening, then its base flying apart, and a
 * burst off the impact. It needs no state of its own -- the landing beat
 * says it happened and the player's own cell says where. */
/* Three things this has to survive, and the first version survived none.
 *
 * It **starts as a creature**. The player is on it from the first frame of
 * the landing beat -- the engine resolved the stomp at the boundary -- so a
 * wreck that is already flat when the beat opens was never seen to be
 * squashed at all. It opens at the height the thing was standing at and is
 * driven down over the first quarter of the beat, under a body that is
 * squashing at the same time.
 *
 * It is **wider than the player**, which lands squashed to fourteen either
 * side and is drawn over the top of this: anything narrower is behind a body
 * and not on screen.
 *
 * And it **outlasts the impact**. The player pops clear over rows two to
 * five, which is exactly when there is something to look at. */
/* Drawn at the player's own column rather than at the cell's.
 *
 * This is what kept the crush off screen through three attempts at it. The
 * wreck belongs to a cell, and a cell scrolls: over the landing beat it
 * slides the full thirty-two pixels left while the player holds its column,
 * so by the third frame the creature being squashed is most of a cell away
 * from the body squashing it. It is an impact, not a corpse -- it lasts a
 * sixth of a beat and it belongs where the impact was. */
/* The two animals are crushed from where each of them was, and arrive at the
 * same thing: a slab four deep lying on the surface, 44 across, which then
 * comes apart. That is the whole of what they share and it is the part that
 * matters -- by the burst neither of them is a shape any more.
 *
 * SPK_STOMP_SPAN and SPK_STOMP_FLAT are that slab. The walker starts standing
 * on the surface and is driven down through its own legs; the flyer is on its
 * side a body's height above it, and is driven *out of the air* first -- it
 * has nothing under it to be crushed against until it reaches the floor. */
#define SPK_STOMP_SPAN   22      /* half the slab, across */
#define SPK_STOMP_FLAT    4      /* ...and how deep it ends up */
#define SPK_STOMP_SPLAY  26      /* how far the walker's feet are thrown */

static void spk_draw_stomp_side(int base, int row)
{
    int cx = SPK_PLAYER_X;

    /* Down, and flat, and spread the way it was already going. The spike
     * stays out in front the whole way: it is the last part of the shape
     * that is still recognisable, and it is pointing where it was headed. */
    int mid = base - SPK_FLY_MID
              + ((SPK_FLY_MID - SPK_STOMP_FLAT / 2 - 1) * row) / 3;
    int half = SPK_CR_TOP - ((SPK_CR_TOP - SPK_STOMP_FLAT / 2) * row) / 3;
    int reach = SPK_CR_BODY / 2
                + ((SPK_STOMP_SPAN - SPK_CR_BODY / 2) * row) / 3;
    int lift = SPK_CR_UP - ((SPK_CR_UP - 3) * row) / 3;
    int foot = SPK_CR_FOOT_IN + ((half - SPK_CR_FOOT_IN) * row) / 3;

    spk_draw_shell_side(cx - reach, mid, 2 * reach, half, half);

    spk_draw_leg_side(mid - half, mid - half / 2, cx + reach, mid - foot,
                      cx + reach + lift);
    spk_draw_leg_side(mid + half / 2, mid + half, cx + reach, mid + foot,
                      cx + reach + lift);

    spk_line(cx - reach, mid - half, cx - reach - SPK_SPK_H, mid);
    spk_line(cx - reach, mid + half, cx - reach - SPK_SPK_H, mid);
}

static void spk_draw_stomp(const struct spk_frame *f, int level, bool flying)
{
    int cx = SPK_PLAYER_X;
    int base = SPK_GROUND_Y - SPK_LEVEL_PX * level;
    int row = (f->phase * SPK_ROWS_PER_BEAT) / SPK_PHASE;
    int stand = SPK_CR_UP + SPK_CR_BODY;

    if (row >= 6)
        return;

    if (row < 4)
    {
        if (flying)
            spk_draw_stomp_side(base, row);
        else
        {
            int top = base - stand + ((stand - 5) * row) / 3;
            int depth = SPK_CR_BODY
                        - ((SPK_CR_BODY - SPK_STOMP_FLAT) * row) / 3;
            int half = SPK_CR_TOP
                       + ((SPK_STOMP_SPAN - SPK_CR_TOP) * row) / 3;
            int foot = SPK_CR_FOOT_IN
                       + ((SPK_STOMP_SPLAY - SPK_CR_FOOT_IN) * row) / 3;
            int hip = top + depth;

            spk_draw_shell(cx, top, depth, half, half - 2);
            spk_draw_leg(cx - SPK_CR_BOT, cx - SPK_CR_HIP, hip,
                         cx - foot, base);
            spk_draw_leg(cx + SPK_CR_HIP, cx + SPK_CR_BOT, hip,
                         cx + foot, base);
        }

        /* Out past the body, or it is drawn under the player and is not a
         * burst at all. Both animals get it: by here neither of them is a
         * shape any more, and what is being said is the same thing. */
        if (row >= 2)
        {
            spk_line(cx - 20, base - 8, cx - 31, base - 18);
            spk_line(cx + 20, base - 8, cx + 31, base - 18);
        }
    }
    else
    {
        /* And then it comes apart, the two halves going outward. */
        int fly = (row - 4) * 9;
        int len = 8 - (row - 4) * 3;

        spk_line(cx - 20 - fly, base - 3, cx - 20 - fly - len, base);
        spk_line(cx + 20 + fly, base - 3, cx + 20 + fly + len, base);
    }
}

/* A block that rides up and down. Down it fills the cell a walker needs; up
 * it fills the air a jump goes through.
 *
 * It is *seen* to travel: the position is eased between the beat it came
 * from and the beat it is in, which is the same easing the world scrolls on
 * and freezes with it. That matters more than any marking would -- what the
 * player has to read is not where the box is but which way it is going, and
 * a box that teleports says nothing about that. It also puts the movement on
 * the beat the player arrives on, so being caught under a dropping box looks
 * like exactly what it was. */
static void spk_draw_block(const struct spk_frame *f, int x, int cell,
                          int level)
{
    int cx = x + SPK_CELL_PX / 2;
    int base = SPK_GROUND_Y - SPK_LEVEL_PX * level;
    int down = base - SPK_BLOCK_H;
    int up = down - SPK_BLOCK_LIFT;
    /* It travels over the beat *before* the one it matters on, so that it
     * is already in place when the player arrives and the player watched it
     * get there. Moving it on arrival instead is what made a box kill from
     * a position it had not reached: crushing from the air on its way down,
     * or stopping a jump against a gap it had not yet risen into. */
    int was = spk_world_block_up(cell, f->st->beat) ? up : down;
    int now = spk_world_block_up(cell, f->st->beat + 1) ? up : down;
    int top = was + ((now - was) * spk_scroll8(f)) / SPK_PHASE;

    lcd_drawrect(cx - SPK_BLOCK_W, top, SPK_BLOCK_W * 2, SPK_BLOCK_H);
    lcd_drawrect(cx - SPK_BLOCK_W + 1, top + 1,
                 SPK_BLOCK_W * 2 - 2, SPK_BLOCK_H - 2);
}

/* A pad on two short legs, standing proud of the surface. 'press' is how far
 * it has been pushed down, nought to SPK_SW_UP -- so the same call draws it
 * standing, going down under the body that landed on it, and flat
 * afterwards. It stays flat, which makes the field itself the record of
 * whether the switch has been thrown. */
static void spk_draw_switch(int x, int level, int press)
{
    int cx = x + SPK_CELL_PX / 2;
    int base = SPK_GROUND_Y - SPK_LEVEL_PX * level;
    int top = base - SPK_SW_UP + press;

    spk_hline(cx - SPK_SW_W, cx + SPK_SW_W, top);

    if (press < SPK_SW_UP - SPK_INK)
    {
        spk_vline(cx - SPK_SW_W, top, base - 1);
        spk_vline(cx + SPK_SW_W - SPK_INK, top, base - 1);
    }
}

/* A platform waiting on a switch, drawn as the line it would stand on and
 * nothing else. It has to say "there could be something here" without
 * saying "there is". */
static void spk_draw_promise(int x, int level)
{
    int y = SPK_GROUND_Y - SPK_LEVEL_PX * level;
    int i;

    for (i = x + 2; i < x + SPK_CELL_PX - 2; i += 6)
        spk_hline(i, i + 1, y);
}

/* A platform with nothing under it needs no mark of its own: the missing
 * ground line beneath it already says the only thing the player has to
 * know, which is that stepping there is a fall.
 *
 * The hatch changes hand on every beat and does nothing in between. That is
 * the platforms' share of the pulse, and it is deliberately a jump rather
 * than a sweep: no per-frame movement may compete with the scroll for the
 * player's judgement of a gap two cells ahead, and a fill that merely swaps
 * direction on the beat adds a pulse without adding motion.
 *
 * It leans with the foot the player lands on, so the whole screen keeps one
 * beat rather than two that happen to agree. */
static void spk_draw_platform(int x, int level, bool lean)
{
    int y = SPK_GROUND_Y - SPK_LEVEL_PX * level;

    lcd_drawrect(x + 1, y, SPK_CELL_PX - 2, SPK_PLATFORM_H);
    lcd_drawrect(x + 2, y + 1, SPK_CELL_PX - 4, SPK_PLATFORM_H - 2);

    spk_hatch(x + 3, y + 2, x + SPK_CELL_PX - 4, y + SPK_PLATFORM_H - 3, lean);
}

static void spk_draw_world(const struct spk_frame *f)
{
    int scroll = spk_scroll(f);
    int first = f->st->beat - SPK_PLAYER_COL - SPK_MARGIN;
    int cells = LCD_WIDTH / SPK_CELL_PX + 2 * SPK_MARGIN;
    int i;

    for (i = 0; i < cells; i++)
    {
        int cell = first + i;
        int x = (i - SPK_MARGIN) * SPK_CELL_PX - scroll;
        unsigned int live = spk_world_live(cell);
        unsigned int soon = spk_world_promised(cell);
        int level;

        if (live & 1u)
            spk_draw_ground(cell, x);
        else
        {
            spk_draw_void(x);

            /* A floor that is only waiting on a switch is still a hole, and
             * has to read as one -- the promise says what throwing the
             * switch would buy, and the hooks say what is there now. */
            if (soon & 1u)
                spk_draw_promise(x, 0);
        }

        for (level = 1; level < SPK_LEVELS; level++)
        {
            if (live & (1u << level))
                spk_draw_platform(x, level, f->strong);
            else if (soon & (1u << level))
                spk_draw_promise(x, level);
        }

        if (spk_world_has_switch(cell, &level))
        {
            int press = SPK_SW_UP;

            if (spk_world_switch(cell, &level))
                press = 0;                      /* still standing */
            else if (f->st->threw && cell == f->st->beat)
            {
                /* Going down under the body that landed on it, over the
                 * same rows the body rides down. */
                int row = (f->phase * SPK_ROWS_PER_BEAT) / SPK_PHASE;

                press = row < 4 ? (SPK_SW_UP * row) / 4 : SPK_SW_UP;
            }

            spk_draw_switch(x, level, press);
        }

        if (spk_world_diamond(cell, &level))
            spk_draw_diamond(f, x, level);
        else if (f->st->got && cell == f->st->beat)
            spk_draw_eaten(f, x, f->st->got_level);

        if (spk_world_blocks(cell))
            spk_draw_block(f, x, cell, 0);

        if (spk_world_spring(cell))
            spk_draw_spring(f, x, cell);

        if (spk_world_creature(cell, &level))
            spk_draw_creature(f, x, level, spk_world_spiked(cell));
        else if (f->st->stomped && cell == f->st->beat)
            spk_draw_stomp(f, f->st->level < 0 ? 0 : f->st->level,
                           spk_world_flew(cell));

        /* One more cell's worth of scroll under it, which is the whole of
         * its movement: the column it is drawn in steps left a cell a beat
         * on its own, and this carries it the second one. Both are the same
         * eased scroll, so it closes on the beat rather than near it. */
        if (spk_flyer_at(f, cell, &level))
            spk_draw_flyer(f, x - scroll, level);
    }
}


/** The player **/

/* Where the body is, and how it is held, for the instant being drawn. The
 * jump reads its arc at the true phase rather than from wherever the press
 * was noticed: a press late inside its window starts the arc part-way
 * through, so the take-off is clipped and the landing still falls exactly
 * on the beat. Under no circumstances does the landing slip. */
static int spk_player_pose(const struct spk_frame *f, struct spk_pose *p)
{
    const struct spk_state *st = f->st;
    int level = st->level < 0 ? 0 : st->level;

    if (f->death_phase >= 0)
    {
        if (f->death_kind == SPK_DEATH_OUCH)
        {
            spk_pose_ouch(p, f->death_phase);

            /* A jump that met the underside of a raised block happened up
             * where the block is, not on the floor it took off from and
             * never reached. Merging the three contact deaths into one lost
             * this, and the body then played the recoil on the ground with
             * nothing near it. */
            if (f->st->motion == SPK_ARC_RISE)   /* never a spring: it clears */
                p->y_offset = (int8_t)(p->y_offset - SPK_BLOCK_LIFT);

            /* And a descent that came down on a spike happened on the head
             * it came down on, not on the floor it never reached. st->level
             * is -1 all the way through an arc, so without this the recoil
             * snaps to the ground with nothing near it -- the same fault as
             * the block above, from the other end of the arc. */
            else if (st->motion == SPK_ARC_FALL && st->to >= 0)
            {
                int cl;

                if (spk_world_creature(st->beat + 1, &cl) && cl == st->to)
                    return (st->to << 8) + SPK_CR_HEAD8;

                return st->to << 8;
            }
        }
        else
            spk_pose_fall(p, f->death_phase, f->death_kind == SPK_DEATH_AIR,
                         f->strong);

        return level << 8;
    }

    switch (st->motion)
    {
    case SPK_ARC_RISE:
    case SPK_ARC_SPRING:
    case SPK_ARC_FALL:
    {
        /* Two clocks run through a jump, and they are not the same one.
         *
         * The pose table keeps wall time, so the crouch, the launch and the
         * turn last what they were written to last. The height keeps the
         * eased phase, so the body rises and falls in the same lurch and
         * hold that the world scrolls in. Given a smooth arc instead, the
         * world pulses twice underneath a character doing nothing in
         * particular, and the jump stops belonging to the beat -- which is
         * the one thing a two-beat move in this game cannot afford. */
        int u_time = f->phase >> 1;
        int u_world = spk_ease(f->phase) >> 1;
        bool land_strong = f->strong;

        if (st->motion == SPK_ARC_FALL)
        {
            u_time += SPK_PHASE / 2;
            u_world += SPK_PHASE / 2;

            /* The landing is the beat after this one, and the foot
             * alternates, so it is the other one. */
            land_strong = !f->strong;
        }

        spk_pose_jump(p, u_time, land_strong);

        /* Coming down on a creature means coming down on its *head*, so the
         * arc has to finish there and not on the surface under it.
         *
         * Landing on the surface and then lifting the body onto the head is
         * what the ride-down pose did on its own, and it reads as bouncing
         * up off something the player has not touched yet -- the impact
         * arrives before the contact does. Ending the descent on the head
         * puts the two in the right order, and the ride carries on from
         * exactly where the arc left off. */
        if (st->motion == SPK_ARC_FALL)
        {
            int cl, over = 0;

            /* The arc has to finish on top of whatever is being landed on,
             * or the landing pose lifts the body onto it afterwards and the
             * impact arrives before the contact. */
            /* A flyer is stomped on its side, whose top edge is a pixel
             * under a standing creature's head -- near enough that the two
             * ride down on the same number. */
            if ((spk_world_creature(st->beat + 1, &cl) && cl == st->to)
                || (spk_world_flyer(st->beat + 1, &cl) && cl == st->to))
                over = SPK_CR_HEAD8;
            else if (spk_world_switch(st->beat + 1, &cl) && cl == st->to)
                over = (SPK_SW_UP * 256) / SPK_LEVEL_PX;
            else if (st->to == 0 && spk_world_spring(st->beat + 1))
                over = (SPK_SPR_TALL * 256) / SPK_LEVEL_PX;

            if (over)
                return spk_arc_level(st->from, st->to, u_world, st->apex)
                       + (over * f->phase) / SPK_PHASE;
        }

        /* Off a spring he rides the cap down and up until the arc lifts him
         * clear of it, which is what makes a launch read as being thrown
         * rather than as a jump that happens to start on a spring. Taking
         * the higher of the two needs no blend and cannot step: the moment
         * the arc passes the cap it simply wins. */
        if (st->motion == SPK_ARC_SPRING)
        {
            int arc = spk_arc_level(st->from, st->to, u_world, st->apex);
            int cap = (spk_spring_top(f, st->beat) * 256) / SPK_LEVEL_PX;

            return arc > cap ? arc : cap;
        }

        return spk_arc_level(st->from, st->to, u_world, st->apex);
    }

    case SPK_WALK:
    default:
        if (st->landed)
            spk_pose_land(p, f->phase, f->strong,
                         st->stomped ? SPK_CR_UP + SPK_CR_BODY
                                     : st->threw ? SPK_SW_UP : 0);
        else
            spk_pose_hop(p, f->phase, f->strong);
        return spk_walk_level(st->from, st->to, f->phase);
    }
}

/* The drop's two numbers, shared because the body falls on this curve and
 * its tail is drawn from the same one: a ladder that does not match what the
 * body did is the thing that makes a fall look drawn rather than fallen.
 *
 * 'high' is how far up it starts: the room over that level, capped. */
static int spk_drop_high(int level)
{
    int room = spk_level_y(level << 8) - SPK_FIELD_TOP - SPK_BODY_PX;

    return room < 0 ? 0 : room > SPK_DROP_H ? SPK_DROP_H : room;
}

static int spk_drop_lift(int high, int t)
{
    if (t <= 0)
        return high;
    if (t >= SPK_PHASE)
        return 0;

    /* Already moving when it comes into view. The body is a beat further up
     * than the field is tall when the fall starts, so what gets drawn is the
     * fast end of a longer one -- which is the curve below, a squared fall
     * read from its second beat rather than its first.
     *
     * Trap: starting it from rest instead puts a ninth of the distance in
     * the first third of the time, and on a two-beat drop that is a third of
     * a second of a triangle hanging in the air. */
    return high - (high * t * (t + 2 * SPK_PHASE))
                  / (3 * SPK_PHASE * SPK_PHASE);
}

/* Where the tail is, for the instant being drawn: five points back along
 * the arc, each turned into a world position so it stays where it was left
 * rather than following the body. */
static void spk_trail_hold(const struct spk_frame *f, long world8, int u)
{
    const struct spk_state *st = f->st;
    int i;

    trail_n = 0;
    trail_falling = false;

    for (i = 1; i <= SPK_TRAIL_JUMP; i++)
    {
        int back = u - i * SPK_TRAIL_STEP;

        if (back < 0)
            break;

        /* u measures how far through the two cells the body is, so the gap
         * between dashes is a world distance directly. */
        trail[trail_n].world8 = world8 - (long)(i * SPK_TRAIL_STEP * 2);
        trail[trail_n].y =
            (short)(spk_level_y(spk_arc_level(st->from, st->to, back, st->apex)) - 6);
        trail_n++;
    }
}

/* The same tail, drawn up the line the body has just dropped down. The
 * world is frozen for the death, so every dash is at the body's own column
 * and the tail is a vertical streak back to the edge it left -- which is the
 * one thing a fall has to say. */
static void spk_trail_drop(const struct spk_frame *f, int level)
{
    long world8 = ((long)f->st->beat << 8) + spk_scroll8(f);
    int base = spk_level_y(level << 8);
    int n = SPK_TRAIL_JUMP + level;
    int i;

    trail_n = 0;
    trail_falling = false;

    if (n > SPK_TRAIL)
        n = SPK_TRAIL;

    for (i = 1; i <= n; i++)
    {
        struct spk_pose p;
        int back = f->death_phase - i * SPK_FALL_STEP;

        if (back < 0)
            break;

        spk_pose_fall(&p, back, f->death_kind == SPK_DEATH_AIR, f->strong);

        trail[trail_n].world8 = world8;
        trail[trail_n].y = (short)(base + p.y_offset - 6);
        trail_n++;
    }
}

/* The streak a drop-in comes down in: the tail the jump and the death
 * already leave, taken off the drop's own curve. Every dash is a place the
 * body genuinely was, which is why they spread out towards the bottom --
 * a ladder of even rungs says nothing about how fast it is going.
 *
 * The column is fixed, at the cell being returned to, so the streak stays in
 * the air it fell through while the field scrolls past it. */
static void spk_trail_plunge(const struct spk_frame *f)
{
    long world8 = ((long)f->st->beat + f->drop_cells) << 8;
    int high = spk_drop_high(f->drop_level);
    int base = spk_level_y(f->drop_level << 8);
    struct spk_pose now;
    int top, i;

    trail_n = 0;
    trail_falling = false;

    /* Dashes below the apex are inside the body rather than behind it: near
     * the top of the fall it covers less in one step than it is tall, and a
     * mark there reads as drawn on the triangle. Dropping them thins the
     * streak while the fall is slow and lets it out as it speeds up, which
     * is the right way round for a body picking up speed. */
    spk_pose_drop(&now, f->drop_fall, f->strong);
    top = base - spk_drop_lift(high, f->drop_fall) - now.height;

    for (i = 1; i <= SPK_TRAIL; i++)
    {
        int back = f->drop_fall - i * SPK_FALL_STEP;
        int y;

        if (back < 0)
            break;

        y = base - spk_drop_lift(high, back) - 6;

        if (y > top)
            continue;

        trail[trail_n].world8 = world8;
        trail[trail_n].y = (short)y;
        trail_n++;
    }
}

static void spk_trail_step(const struct spk_frame *f)
{
    const struct spk_state *st = f->st;

    /* The only tail there is while the run is skipping. Once it lands the
     * streak is let go with everything else, and hangs in the air a moment
     * before it drifts down. */
    if (f->drop_cells >= 0 && f->drop_fall < SPK_PHASE)
    {
        spk_trail_plunge(f);
        return;
    }

    if (!f->skipping)
    {
        if (f->death_phase >= 0)
        {
            /* A body that never fell has no fall to trail. */
            /* Only a body that fell has a fall to trail. */
            if (f->death_kind != SPK_DEATH_OUCH)
                spk_trail_drop(f, st->level < 0 ? 0 : st->level);
            return;
        }

        if (st->motion == SPK_ARC_RISE || st->motion == SPK_ARC_FALL
            || st->motion == SPK_ARC_SPRING)
        {
            int u = spk_ease(f->phase) >> 1;

            if (st->motion == SPK_ARC_FALL)
                u += SPK_PHASE / 2;

            spk_trail_hold(f, ((long)st->beat << 8) + spk_scroll8(f), u);
            return;
        }
    }

    /* Whatever it was following is over. Let go of it once, and let it drop
     * from where it was. */
    if (trail_n > 0 && !trail_falling)
    {
        trail_falling = true;
        trail_at = f->now_ms;
    }
}

/* Falling, it stops on whatever is under its own cell -- and goes down a
 * hole where there is nothing, which is worth having: the tail of a jump
 * that cleared a gap follows it in. */
static void spk_trail_draw(const struct spk_frame *f)
{
    long camera8 = ((long)f->st->beat << 8) + spk_scroll8(f);
    int drop = 0, i;

    if (trail_n == 0)
        return;

    if (trail_falling)
    {
        long age = (long)f->now_ms - (long)trail_at;

        if (age < 0 || age > SPK_TRAIL_MS)
        {
            trail_n = 0;
            return;
        }

        drop = (int)((age * age) / SPK_TRAIL_FALL);
    }

    for (i = 0; i < trail_n; i++)
    {
        int y = trail[i].y + drop;
        int surf = spk_world_surface((int)(trail[i].world8 >> 8),
                                    SPK_LEVELS - 1);
        int x;

        if (surf >= 0)
        {
            int floor_y = SPK_GROUND_Y - SPK_LEVEL_PX * surf;

            if (y > floor_y)
                y = floor_y;
        }
        else if (y > SPK_UPDATE_H)
            continue;

        x = SPK_PLAYER_X
            + (int)(((trail[i].world8 - camera8) * SPK_CELL_PX) >> 8);

        if (x > -4 && x <= LCD_WIDTH + 4)
            spk_hline(x - 2, x, y);
    }
}

/* Coming back after a death: falling onto the cell the run restarts from,
 * then standing on it while the world carries it back to its own column.
 *
 * Drawn against that cell and not against the player's column, which is what
 * makes it drift: a cell's x is its distance from the player plus the scroll,
 * and both of those close as the field moves. By the beat it arrives the two
 * are the same place, and the walk takes over with nothing to catch up. */
static void spk_draw_drop(const struct spk_frame *f)
{
    struct spk_pose p;
    int surface = spk_level_y(f->drop_level << 8);
    int high = spk_drop_high(f->drop_level);
    int t = f->drop_fall;
    int x = SPK_PLAYER_X + f->drop_cells * SPK_CELL_PX - spk_scroll(f);
    int pt[3][2];
    int i;

    if (t < SPK_PHASE)
    {
        /* The pose carries no height of its own -- spk_pose_fall() does, in
         * its later rows, and it is written for a body leaving the screen,
         * so the two together put this one through the floor. Here the
         * height is the caller's and the pose is the shape it falls in. */
        spk_pose_drop(&p, t, f->strong);
        surface -= spk_drop_lift(high, t);
    }
    else
    {
        spk_pose_land(&p, f->phase, f->strong, 0);

        /* Flatter than a hop lands, and only while it is still absorbing
         * it: it has come down the height of the field. */
        if (f->phase < SPK_PHASE / 4)
        {
            int give = SPK_DROP_SQUASH
                       - (SPK_DROP_SQUASH * f->phase * 4) / SPK_PHASE;

            p.half_width = (int8_t)(p.half_width + give);
            p.height = (int8_t)(p.height - give);
        }
    }

    spk_pose_points(&p, x, surface, pt);

    spk_aa_use(aa_ink);
    for (i = 0; i < 3; i++)
    {
        int j = (i + 1) % 3;

        spk_aa_line(pt[i][0], pt[i][1], pt[j][0], pt[j][1]);
    }
}

static void spk_draw_player(const struct spk_frame *f)
{
    struct spk_pose p;
    int level8 = spk_player_pose(f, &p);
    int y = spk_level_y(level8);
    int pt[3][2];
    int i;

    spk_pose_points(&p, SPK_PLAYER_X, y, pt);

    /* Two px, and only here. The density difference against the 1 px world
     * is what makes the player findable at a glance, so nothing else may
     * have it. Soft-edged, because two of the three edges are steep and
     * turning: a staircase that changes shape every frame is the one place
     * on the field where aliasing is watched rather than glanced at. */
    spk_aa_use(aa_ink);

    for (i = 0; i < 3; i++)
    {
        int j = (i + 1) % 3;

        spk_aa_line(pt[i][0], pt[i][1], pt[j][0], pt[j][1]);
    }

    /* Ouch. Three short lines thrown off the point of contact, for the
     * moment of it and no longer: what they say is "that hurt", and a mark
     * that outstays the impact stops saying it. */
    if (f->death_phase >= 0 && f->death_kind == SPK_DEATH_OUCH
        && f->death_phase < SPK_PHASE / 5)
    {
        int hx = SPK_PLAYER_X + 13;
        int hy = y - 14;

        spk_line(hx, hy, hx + 9, hy - 7);
        spk_line(hx + 1, hy + 7, hx + 11, hy + 5);
        spk_line(hx - 3, hy - 6, hx + 1, hy - 15);
    }

    /* Impact ticks, flicking outward from the base corners. */
    if (f->st->landed && f->phase < SPK_PHASE / 8 && f->death_phase < 0)
    {
        spk_line(pt[0][0], pt[0][1], pt[0][0] - 4, pt[0][1] - 3);
        spk_line(pt[1][0], pt[1][1], pt[1][0] + 4, pt[1][1] - 3);
    }
}


/** The chrome **/

/* The crown, drawn rather than blitted, and square: h is its side.
 *
 * The block-glyph crown is a seven-pixel bitmap scaled up, and both places
 * it appears now set their score in the caption face -- beside which it
 * reads as a different era of the same screen. This one is lines, taken to
 * the height of the digits it stands next to.
 *
 * Three peaks and a band, which is the least a crown can be and still not be
 * a chess piece. The bitmap needs a gap and a bar under it to say as much,
 * because at seven pixels the peaks alone are ambiguous; at this size they
 * are not.
 *
 * The caller sets the ramp, since the two callers use different ones. */
static void spk_crown_drawn(int x, int y, int h)
{
    /* Everything is an offset either side of a centre, so the two halves are
     * the same by construction. Placing the points across a span instead
     * puts the apex half a pixel off it -- the width is odd as often as not
     * -- and half a pixel is a whole one on a peak four high. */
    int half = h / 2;
    int cx = x + half;
    int dip = y + h * 2 / 5;         /* where the notches bottom out */
    int pt[7][2];
    int i;

    pt[0][0] = cx - half;     pt[0][1] = y + h / 4;
    pt[1][0] = cx - half / 2; pt[1][1] = dip;
    pt[2][0] = cx;            pt[2][1] = y;
    pt[3][0] = cx + half / 2; pt[3][1] = dip;
    pt[4][0] = cx + half;     pt[4][1] = y + h / 4;
    pt[5][0] = cx + half;     pt[5][1] = y + h;
    pt[6][0] = cx - half;     pt[6][1] = y + h;

    for (i = 0; i < 7; i++)
    {
        int j = (i + 1) % 7;

        spk_aa_line(pt[i][0], pt[i][1], pt[j][0], pt[j][1]);
    }

    spk_aa_line(cx - half, y + h * 3 / 4, cx + half, y + h * 3 / 4);
}

/* What the wheel just did, in the room the track's name usually has.
 *
 * A cone and a bar. The trough is white on a dark paper and black on a light
 * one -- not the ink, which is the album's and can be any lightness, so a
 * trough in it can vanish against the ground it sits on. The fill is the
 * complement, which is the one colour on this screen guaranteed to be seen
 * against both. */
static void spk_draw_volume(const struct spk_frame *f, int room)
{
    int h = SPK_VOL_H;
    int y = SPK_BAND_Y(h);
    int cx = SPK_CAP_EDGE;
    int bx, bw, fill, i;
    unsigned trough, level;
    bool light;

    if (room < SPK_VOL_MIN)
        return;

    /* The driver, and the horn flaring out of it -- filled column by column
     * so the two are one solid shape. Drawn as an outline it is a block with
     * a bracket floating beside it, and the hole in the middle is what stops
     * it reading as a speaker at all. */
    lcd_fillrect(cx, y + SPK_VOL_LIP, SPK_VOL_BOX, h - 2 * SPK_VOL_LIP);

    for (i = 0; i <= SPK_VOL_FLARE; i++)
    {
        int top = SPK_VOL_LIP - SPK_VOL_LIP * i / SPK_VOL_FLARE;

        lcd_fillrect(cx + SPK_VOL_BOX + i, y + top, 1, h - 2 * top);
    }

    bx = cx + SPK_VOL_CONE + SPK_VOL_GAP;
    bw = room - (bx - SPK_CAP_EDGE);
    if (bw < SPK_VOL_MIN)
        return;

    /* Black and white, and nothing from the palette.
     *
     * The trough takes whichever of the two stands against the paper, and
     * the level takes the other -- so the level is guaranteed to be seen
     * against the trough, and the trough against the ground it sits on. Two
     * colours chosen by one decision, and no way for them to collide.
     *
     * The complement was the obvious thing to reach for and is the wrong
     * one: it is a rotation of the ink's hue at the ink's own lightness, so
     * against a trough picked for contrast it can land anywhere, including
     * on top of it.
     *
     * Lightness, not hue -- RGB_UNPACK_* weighted the way the eye weights
     * them. */
    light = ((RGB_UNPACK_RED(was_paper) * 77
              + RGB_UNPACK_GREEN(was_paper) * 150
              + RGB_UNPACK_BLUE(was_paper) * 29) >> 8) >= 128;

    trough = light ? LCD_BLACK : LCD_WHITE;
    level  = light ? LCD_WHITE : LCD_BLACK;

    lcd_set_foreground(trough);
    lcd_fillrect(bx, y + h / 4, bw, h - h / 2);

    fill = f->volume <= 0 ? 0 : (bw - 2 * SPK_VOL_INSET) * f->volume / 100;
    if (fill > 0)
    {
        lcd_set_foreground(level);
        lcd_fillrect(bx + SPK_VOL_INSET, y + h / 4 + SPK_VOL_INSET,
                     fill, h - h / 2 - 2 * SPK_VOL_INSET);
    }

    lcd_set_foreground(was_ink);
}

/* The track, along the left of the score's band.
 *
 * Scrolled by hand rather than through lcd_puts_scroll(). That engine
 * repaints on its own thread at its own cadence, into a viewport it keeps a
 * pointer to -- and this band is cleared and redrawn thirty times a second
 * over a viewport that lives on spk_draw_frame()'s stack. The two would
 * fight over the same rows, and the pointer would dangle the moment the
 * frame returned.
 *
 * Done here it costs one line of text a frame and nothing at all to send:
 * these rows are already in every flush. It runs on grid time, so it holds
 * still when the field does. */
static void spk_draw_caption(const struct spk_frame *f, int room)
{
    struct viewport cv, *was;
    int tw, th, chars, period, n, off, span, i;

    if (f->caption == NULL || !*f->caption || f->caption_chars < 1)
        return;

    /* The width was measured where the text is built: lcd_getstringsize()
     * walks every glyph, and a cache miss on any of them is an lseek and a
     * read off the disk. One glyph is enough for the height. */
    lcd_setfont(f->font);
    tw = f->caption_w;
    lcd_getstringsize("0", NULL, &th);

    chars = f->caption_chars;
    period = f->caption_at[chars];

    /* Where the box may end: the last character boundary that still fits.
     * The face is proportional, so this is a walk over the measured grid
     * rather than a division -- and it is the whole of "never slice a
     * glyph", because the box's edge is the only thing that ever would. */
    if (tw <= room || !f->caption_scroll)
    {
        n = 0;
        off = 0;
    }
    else
    {
        n = f->caption_step % chars;
        off = f->caption_at[n];
    }

    span = 0;
    for (i = 0; i < chars; i++)
    {
        int k = (n + i) % chars;
        int wide = f->caption_at[k + 1] - f->caption_at[k];

        if (span + wide > room)
            break;
        span += wide;
    }

    if (span < SPK_CAP_MIN)
        return;

    viewport_set_defaults(&cv, SCREEN_MAIN);
    cv.x = SPK_CAP_EDGE;
    cv.width = span;
    cv.y = SPK_BAND_Y(th);
    cv.height = th;

    /* The field's ink and paper, not the fallbacks a fresh viewport carries:
     * the caption is part of the same picture and follows the album's
     * palette with it. */
    if (colors_known)
    {
        cv.fg_pattern = was_ink;
        cv.bg_pattern = was_paper;
    }

    cv.font = f->font;
    was = lcd_set_viewport(&cv);

    /* Drawn from off the left edge: clip_viewport_rect() shifts the source
     * across for a negative x. Both edges now fall between characters, so
     * nothing is sliced -- the clipping only ever removes whole glyphs.
     * lcd_putsxy_style_offset() would say this more directly and is declared
     * in lcd.h, but nothing in the tree defines it. */
    lcd_putsxy(-off, 0, f->caption);

    /* ...and the copy coming round behind it, on the same grid, so the join
     * is a character boundary like every other. */
    if (period - off < span)
        lcd_putsxy(period - off, 0, f->caption);

    lcd_set_viewport(was);
    lcd_setfont(FONT_SYSFIXED);
}

/* Centred in the real font, thickened on the beat by drawing it over itself
 * -- the same widening the block glyphs do, one pixel each side so the word
 * grows about its own centre rather than sliding.
 *
 * Trap: this has to be DRMODE_FG. The alpha blitter's SOLID case is
 * `blend_two_colors(bg, fg, alpha)` -- it blends between the two *patterns*
 * and not with the framebuffer, so every glyph paints its whole box and the
 * copy at x wipes the copies at x-1 and x+1. FG blends against the
 * destination and leaves a transparent pixel alone, which is the only mode
 * under which a word can be drawn over itself at all. */
static void spk_font_centred(int font, int y, const char *str, bool bold)
{
    int w, h, x;

    lcd_setfont(font);
    lcd_getstringsize(str, &w, &h);
    x = (LCD_WIDTH - w) / 2;

    lcd_set_drawmode(DRMODE_FG);

    if (bold)
    {
        lcd_putsxy(x - 1, y, str);
        lcd_putsxy(x + 1, y, str);
    }

    lcd_putsxy(x, y, str);
    lcd_set_drawmode(DRMODE_SOLID);
}

static void spk_draw_hud(const struct spk_frame *f)
{
    char line[12];
    int x, sw, sh, mw, ch, room;

    /* Leading zeros, so the number is the same width whatever it says and
     * the eye can read it without leaving the field. Seven digits: six was
     * a ceiling a good Run could actually meet, and a score that stops
     * counting is worse than one that is a digit wider.
     *
     * In the caption's face rather than the block glyphs. The face is
     * monospaced, so leading zeros still hold the number still, and one
     * typeface across the band beats two. */
    snprintf(line, sizeof (line), "%07d",
             (int)(f->score > 9999999 ? 9999999 : f->score));

    lcd_setfont(f->font);
    lcd_getstringsize(line, &sw, &sh);
    lcd_getstringsize("X8", &mw, NULL);

    ch = SPK_CROWN_H(sh);

    x = LCD_WIDTH - 4 - sw;
    lcd_putsxy(x, SPK_BAND_Y(sh), line);

    /* Past the best, said while it is happening rather than at the end, and
     * hard against the score because that is what it is about. */
    x -= SPK_TAG_GAP + ch;
    if (f->crowned)
    {
        int was_top = aa_top;

        /* The plotter is set to the field's rows for the whole frame, and
         * the band is above them: without this the crown is clipped away to
         * the couple of rows that reach into the field, which is a mark the
         * player cannot read and cannot place. */
        aa_top = 0;
        spk_aa_use(aa_ink);
        spk_crown_drawn(x, SPK_BAND_Y(ch), ch);
        aa_top = was_top;
    }

    /* ...and the multiplier outside it, on the score's own baseline so the
     * two read as one line rather than as two things that happen to be near
     * each other. */
    x -= SPK_TAG_GAP + mw;
    if (f->multiplier > 1)
    {
        snprintf(line, sizeof (line), "X%d", f->multiplier);
        lcd_putsxy(x, SPK_BAND_Y(sh), line);
    }

    lcd_setfont(FONT_SYSFIXED);

    /* What is left is the title's, and it is left over rather than reserved
     * -- x has already walked past both slots whether or not they were
     * filled. */
    room = x - SPK_CAP_EDGE - SPK_CAP_GAP;

    if (f->volume >= 0)
        spk_draw_volume(f, room);
    else
        spk_draw_caption(f, room);

    /* And the rule that closes the band. It is the top of the block the
     * layout centres, so it is drawn where the field begins rather than at
     * some distance chosen to look right -- and edge to edge, because the
     * floor at the bottom of the same block is, and two rules bounding one
     * picture have to agree about where the picture stops. */
    spk_hline(0, LCD_WIDTH - 1, SPK_HUD_H);

    if (f->waiting)
    {
        /* The word the player is waiting for, pulsing on the provisional
         * grid: the tempo is not known yet, so what this says is that the
         * game is listening rather than stuck.
         *
         * And nothing else. What the tracker is making of it -- beats
         * waited, confidence, windows analysed -- is on the menu, which is
         * where a number a player cannot act on belongs. */
        spk_font_centred(f->font, SPK_HUD_H + 30,
                         "Waiting for the", false);
        spk_font_centred(f->font, SPK_HUD_H + 56,
                         "beat", f->phase < SPK_PHASE / 2);
        lcd_setfont(FONT_SYSFIXED);
    }
}


/** The frame **/

/* The field takes the theme's two colours, and takes them through the
 * dynamic-colour resolver -- so with the setting on it is the album's
 * palette and with it off it is the theme's, and neither case is special
 * here. Asked every frame because the palette moves with the track, and
 * extraction is a no-op until it does. */
/* Work out the pair, and say nothing to the LCD about it: the caller decides
 * which viewport they land on, and a fresh viewport comes up in the
 * fallbacks whatever was set before it. */
static void spk_resolve_colors(void)
{
    unsigned ink, paper;

    dynamic_colors_check_extraction(-1);
    ink = dynamic_colors_resolve(global_settings.fg_color);
    paper = dynamic_colors_resolve(global_settings.bg_color);

    if (colors_known && ink == was_ink && paper == was_paper)
        return;

    colors_known = true;
    was_ink = ink;
    was_paper = paper;
    spk_ramp(aa_ink, paper, ink);
    spk_ramp(aa_accent, paper, spk_complement(ink));

    /* The whole panel is in these two colours, and the strips above and
     * below the field are only ever reached by a full clear -- so a palette
     * that moved has to repaint all of it, not just the rows the field is
     * flushed in. */
    full_flush = true;
}

/* ...and for a screen that draws straight into the default viewport rather
 * than installing one of its own. */
static void spk_draw_colors(void)
{
    spk_resolve_colors();
    lcd_set_foreground(was_ink);
    lcd_set_background(was_paper);
}

void spk_draw_frame(const struct spk_frame *f)
{
    struct viewport vp;

    aa_top = SPK_HUD_H;
    aa_bot = SPK_UPDATE_H;

    spk_resolve_colors();

    /* Ordinarily the field's own rows and no others: the caption is drawn on
     * the frames that change it and has to survive the ones that do not, and
     * clearing the whole panel every frame is what would stop it.
     *
     * On a frame that will be sent in full -- the first one, a palette that
     * moved, a menu that drew over everything -- the clear covers the whole
     * panel instead. The strips above and below the field are in the paper
     * colour too, and nothing else ever reaches them.
     *
     * The colours go on the viewport rather than through lcd_set_foreground:
     * a fresh viewport comes up in FG_FALLBACK/BG_FALLBACK, so anything set
     * on the one it replaces is thrown away with it. */
    viewport_set_defaults(&vp, SCREEN_MAIN);
    vp.y = 0;
    vp.height = full_flush ? LCD_HEIGHT : SPK_UPDATE_H;
    vp.fg_pattern = was_ink;
    vp.bg_pattern = was_paper;
    lcd_set_viewport(&vp);

    lcd_set_drawmode(DRMODE_SOLID);
    lcd_clear_viewport();

    spk_draw_world(f);

    spk_trail_step(f);
    spk_trail_draw(f);

    /* Nothing to draw for most of the skip: the run is not the player's for
     * those beats, and an absent triangle says so more plainly than any
     * treatment of a present one. The end of it is the body coming back,
     * which is drawn against the cell it is coming back to. */
    if (f->drop_cells >= 0)
        spk_draw_drop(f);
    else if (!f->skipping)
        spk_draw_player(f);

    spk_draw_hud(f);

    lcd_set_viewport(NULL);
}

void spk_draw_flush(void)
{
    if (full_flush)
    {
        full_flush = false;
        lcd_update();
        return;
    }

    lcd_update_rect(0, SPK_UPDATE_TOP, LCD_WIDTH,
                    SPK_UPDATE_H - SPK_UPDATE_TOP);
}


/** The summary **/

/* Where the parts of a finished run sit. The label, the score and the two
 * number lines are a fixed block; the list takes whatever is left between
 * them and the floor the body turns somersaults on, which is why the number
 * of rows is answered rather than declared. */
#define SPK_SUM_EDGE      8     /* the margin everything is set against */
#define SPK_SUM_GAP       4     /* between the rows of the top part */
#define SPK_SUM_LIST_Y   10     /* the rule, to the first name under it */
#define SPK_SUM_GENRE   104     /* the genre's column, on the right */
#define SPK_SUM_LIST_BOT (LCD_HEIGHT - SPK_SUM_EDGE)

/* The column the body somersaults in: against the right edge, and as wide as
 * the turn can get so the words beside it never have to allow for it.
 *
 * It stands on the rule that divides the screen. There is already a line
 * under it, and a second one would read as a shelf. */
#define SPK_SUM_BODY_W   (SPK_BODY_PX + 4)
#define SPK_SUM_BODY_X   (LCD_WIDTH - SPK_SUM_EDGE - SPK_SUM_BODY_W / 2)
#define SPK_SUM_WORDS_R  (LCD_WIDTH - SPK_SUM_EDGE - SPK_SUM_BODY_W)

/* The list's scrollbar, in the right margin. It is drawn whether or not
 * there is anything to scroll: a column that comes and goes moves every name
 * beside it, which is a worse thing to look at than a full bar. */
#define SPK_SUM_BAR_W     4
#define SPK_SUM_BAR_X    (LCD_WIDTH - SPK_SUM_EDGE - SPK_SUM_BAR_W)
#define SPK_SUM_LIST_R   (SPK_SUM_BAR_X - 6)

/* One row of the top part, clipped to the words' half of the screen so a
 * long one runs out rather than into the body. */
static void spk_sum_left(int x, int y, int th, int font, const char *s)
{
    struct viewport cv, *was;

    viewport_set_defaults(&cv, SCREEN_MAIN);
    cv.x = x;
    cv.width = SPK_SUM_WORDS_R - x;
    cv.y = y;
    cv.height = th;
    cv.fg_pattern = was_ink;
    cv.bg_pattern = was_paper;
    cv.font = font;

    was = lcd_set_viewport(&cv);
    lcd_putsxy(0, 0, s);
    lcd_set_viewport(was);
    lcd_setfont(FONT_SYSFIXED);
}

/* One track: its name, and its genre after it in a muted ink.
 *
 * Muted rather than the accent, which is a hue away from the ink and shouts
 * for a label that is only there to place the track. Two thirds of the way
 * up the ink's own ramp reads as secondary at any pair of colours a theme
 * can set, which a second hue does not.
 *
 * Each column is a viewport, because a name is as long as it likes and the
 * only exact way to stop one is to give it a box. */
static void spk_sum_track(int y, int th, int font, const char *name,
                          const char *genre)
{
    struct viewport cv, *was;
    int gw = 0;

    if (genre[0] != '\0')
    {
        lcd_setfont(font);
        lcd_getstringsize(genre, &gw, NULL);
        if (gw > SPK_SUM_GENRE)
            gw = SPK_SUM_GENRE;
    }

    viewport_set_defaults(&cv, SCREEN_MAIN);
    cv.x = SPK_SUM_EDGE;
    cv.width = SPK_SUM_LIST_R - SPK_SUM_EDGE - (gw ? gw + SPK_SUM_EDGE : 0);
    cv.y = y;
    cv.height = th;
    cv.fg_pattern = was_ink;
    cv.bg_pattern = was_paper;
    cv.font = font;

    was = lcd_set_viewport(&cv);
    lcd_putsxy(0, 0, name);

    if (gw > 0)
    {
        cv.x = SPK_SUM_LIST_R - gw;
        cv.width = gw;
        cv.fg_pattern = aa_ink[SPK_AA_STEPS * 5 / 8];
        lcd_set_viewport(&cv);
        lcd_putsxy(0, 0, genre);
    }

    lcd_set_viewport(was);
    lcd_setfont(FONT_SYSFIXED);
}

/* The end of a run, and the record, which are the same screen: what the run
 * came to across the top, the tracks it was played over under a rule, and
 * the body turning somersaults on the rule between them -- the gymnastics
 * that were cut from the wait, kept for the one moment there is something to
 * celebrate and nothing to time.
 *
 * The words are set in the caption face rather than the block glyphs, which
 * are the field's voice and belong to a screen that is moving. Nothing here
 * is a fixed row height as a result: the face is loaded at run time, so
 * every row is measured and stepped from the one above it. */
void spk_draw_summary(struct spk_summary *s, int phase, int move)
{
    struct spk_pose p;
    char line[48];
    char name[SPK_NAME_MAX], genre[SPK_GENRE_MAX];
    int pt[3][2];
    int x, y, i, th, ch, top, rows, rule;

    aa_top = 0;
    aa_bot = LCD_HEIGHT;

    spk_draw_colors();
    lcd_set_drawmode(DRMODE_SOLID);
    lcd_clear_display();

    lcd_setfont(s->font);
    lcd_getstringsize("0", NULL, &th);
    lcd_setfont(FONT_SYSFIXED);

    y = SPK_SUM_EDGE;

    spk_sum_left(SPK_SUM_EDGE, y, th, s->font,
                 s->record ? "BEST RUN" : "RUN OVER");
    y += th + SPK_SUM_GAP;

    /* The crown leads the score and is part of the same line, so a run that
     * beat the best is a wider thing on the screen and not merely a
     * differently annotated one. */
    snprintf(line, sizeof (line), "%ld",
             s->run.score > 9999999 ? 9999999 : s->run.score);

    x = SPK_SUM_EDGE;
    ch = SPK_CROWN_H(th);

    if (s->crowned)
    {
        spk_aa_use(aa_ink);
        spk_crown_drawn(x, y + (th - ch) / 2, ch);
        x += ch + SPK_TAG_GAP;
    }

    spk_sum_left(x, y, th, s->font, line);
    y += th + SPK_SUM_GAP;

    /* What it came to. Minutes are the player's measure of a run and beats
     * are the game's, and neither answers for the other -- a slow track is
     * fewer beats for the same evening. */
    snprintf(line, sizeof (line), "%ldM%02ld  %ld BEATS", s->run.secs / 60,
             s->run.secs % 60, s->run.beats);
    spk_sum_left(SPK_SUM_EDGE, y, th, s->font, line);
    y += th;

    snprintf(line, sizeof (line), "%d TRACKS  %d BPM", s->run.tracks,
             (s->run.bpm10 + 5) / 10);
    spk_sum_left(SPK_SUM_EDGE, y, th, s->font, line);
    y += th;

    /* Only where there is still something to beat. A run that beat it wears
     * the crown, and the record has nothing above it. */
    if (!s->crowned && s->best > 0)
    {
        snprintf(line, sizeof (line), "BEST %ld", s->best);
        spk_sum_left(SPK_SUM_EDGE, y, th, s->font, line);
        y += th;
    }

    rule = y + SPK_SUM_GAP;
    spk_hline(0, LCD_WIDTH - 1, rule);

    /* Standing on it, with nothing left to dodge. */
    spk_pose_idle(&p, phase, move);
    spk_pose_points(&p, SPK_SUM_BODY_X, rule, pt);

    spk_aa_use(aa_ink);
    for (i = 0; i < 3; i++)
    {
        int j = (i + 1) % 3;

        spk_aa_line(pt[i][0], pt[i][1], pt[j][0], pt[j][1]);
    }

    /* And the tracks under it. How many fit is a property of the face they
     * are set in, so it is measured here and handed back to the caller that
     * does the scrolling. */
    top = rule + SPK_SUM_LIST_Y;
    rows = th > 0 ? (SPK_SUM_LIST_BOT - top) / th : 0;
    if (rows < 0)
        rows = 0;

    s->shown = rows;

    if (s->first > s->rows - rows)
        s->first = s->rows - rows;
    if (s->first < 0)
        s->first = 0;

    for (i = 0; i < rows && s->first + i < s->rows; i++)
    {
        spk_score_track(s->log, s->first + i, name, sizeof (name),
                        genre, sizeof (genre));
        spk_sum_track(top + i * th, th, s->font, name, genre);
    }

    if (rows > 0)
        gui_scrollbar_draw(&screens[SCREEN_MAIN], SPK_SUM_BAR_X, top,
                           SPK_SUM_BAR_W, rows * th,
                           s->rows < rows ? rows : s->rows,
                           s->first, s->first + rows, VERTICAL);

    lcd_update();
}
