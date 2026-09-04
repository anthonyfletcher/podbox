/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * The geometry and motion of a scrolling row of cards.
 *
 * Parts: ease()/lerp() -- how anything moves; owner_of()/target_w() -- what
 * folding means; advance()/card_row_card_x() -- the row's one-dimensional
 * layout; then the entry points, which are input (step, toggle), time (tick)
 * and output (plan).
 ****************************************************************************/

#include "card_row.h"

/* The scroll closes three sixteenths of the distance to its target every ten
 * milliseconds, and never less than a pixel -- which settles in about a fifth
 * of a second.
 *
 * A chase rather than a run from one position to another, because the target
 * moves: every step of the wheel moves it, and so does a fold. An ease that
 * runs from where the row was to where it is going has to be told what to do
 * when the target changes under it, and there is no good answer -- restart it
 * and a spin leaves it behind, let it run and a target that changes near the
 * end of its clock is reached almost instantly, which is a jump with no
 * animation in it. A chase has no such moment: what it moves is always a
 * fraction of what is left.
 *
 * The fixed timestep is what keeps it frame-rate independent. Stepping by
 * 'a fraction of dt' does not: it is an Euler integration, and it lands
 * somewhere different for every frame length. Here the number of steps is
 * floor(elapsed / CHASE_MS) however the elapsed time arrived, with the
 * remainder carried, so a device drawing 10 fps ends where one drawing 100
 * fps ends.
 *
 * The pixel floor is what guarantees arrival: an eighth of a small remainder
 * truncates to zero, and without the floor the row stops short of its target
 * for good. */
#define CHASE_MS      10
#define CHASE_NUM      3    /* three sixteenths, per CHASE_MS */
#define CHASE_SHIFT    4

#define FOLD_MS      200

/* Ease-out-quad over 0..1000, for the fold. Most of the distance early,
 * settling gently. */
static int ease(int t, int dur)
{
    int p, inv;

    if (t >= dur)
        return 1000;
    if (t <= 0)
        return 0;

    p = t * 1000 / dur;
    inv = 1000 - p;

    return 1000 - inv * inv / 1000;
}

static int lerp(int from, int to, int e)
{
    return from + (to - from) * e / 1000;
}

/* The parent a sub-card belongs to: the nearest card above it that is not
 * itself a sub-card. A run has no other marker, which is what lets a section
 * be a flat table. */
static int owner_of(const struct card_row *r, int i)
{
    while (i > 0 && (r->flags[i] & CARD_ROW_SUB))
        i--;
    return i;
}

/* Whether this card is currently folded away. Asked rather than inferred from
 * a width of zero, because a folded card may be showing a spine. */
static bool is_folded(const struct card_row *r, int i)
{
    return (r->flags[i] & CARD_ROW_SUB) && owner_of(r, i) != r->open;
}

/* The width a card is heading for: its spine unless it is showing. */
static int target_w(const struct card_row *r, int i)
{
    if (is_folded(r, i))
        return r->fold_w;
    return r->full_w[i];
}

/* How far the next card starts beyond this one's left edge.
 *
 * The gap fades in with the card rather than appearing whole the moment a
 * fold begins, so an unfolding run pushes the row smoothly from zero. */
static int advance(const struct card_row *r, int i)
{
    int cw = r->cur_w[i];
    int fw = r->fold_w;

    if (cw <= 0)
        return 0;
    if (cw >= r->full_w[i])
        return cw + r->gap;
    if (cw <= fw || r->full_w[i] <= fw)
        return cw;      /* spines stand against each other, like books */

    return cw + r->gap * (cw - fw) / (r->full_w[i] - fw);
}

int card_row_card_x(const struct card_row *r, int idx)
{
    int x = 0;

    for (int i = 0; i < idx && i < r->n; i++)
        x += advance(r, i);

    return x;
}

int card_row_total_w(const struct card_row *r)
{
    int x = 0, tail = 0;

    for (int i = 0; i < r->n; i++)
    {
        int a = advance(r, i);

        x += a;
        if (a)
            tail = r->gap;
    }

    /* The trailing gap belongs to no card. */
    return x - tail;
}

/* Where the row has to sit for the focused card to be at the focus point.
 *
 * Held back at the front only. The end is NOT clamped: the last card comes to
 * the focus point like any other and leaves blank space to its right, which
 * is what lets position alone say which card is current. Clamping the end
 * instead leaves the last few cards short of the focus point, and then
 * something else has to mark them.
 *
 * Re-read every frame rather than latched at the input, because a fold moves
 * every card to the right of it and the chase simply follows. */
static int scroll_for_focus(const struct card_row *r)
{
    int want = card_row_card_x(r, r->focus) - r->focus_x;

    return want < 0 ? 0 : want;
}

void card_row_init(struct card_row *r, const short *full_w,
                   const unsigned char *flags, short *cur_w, short *from_w,
                   int n, int view_w, int focus_x, int gap)
{
    r->full_w = full_w;
    r->flags  = flags;
    r->cur_w  = cur_w;
    r->from_w = from_w;
    r->n      = n;
    r->focus  = 0;
    r->open   = -1;
    r->view_w = view_w;
    r->focus_x = focus_x;
    r->gap    = gap;
    r->fold_t = FOLD_MS;
    r->accum  = 0;
    r->fold_w = 0;

    for (int i = 0; i < n; i++)
        cur_w[i] = 0;
    for (int i = 0; i < n; i++)
        cur_w[i] = from_w[i] = (short)target_w(r, i);

    r->scroll = r->scroll_to = scroll_for_focus(r);
}

void card_row_set_fold_w(struct card_row *r, int w)
{
    r->fold_w = w;
    for (int i = 0; i < r->n; i++)
        r->cur_w[i] = r->from_w[i] = (short)target_w(r, i);
    r->scroll = r->scroll_to = scroll_for_focus(r);
}

bool card_row_step(struct card_row *r, int dir)
{
    int d = (dir > 0) ? 1 : -1;

    if (dir == 0)
        return false;

    /* A folded card is not a stop, so a run that is closing is stepped over
     * even while it still has width on screen -- and a spine is not a stop
     * either, whatever it is showing. */
    for (int i = r->focus + d; i >= 0 && i < r->n; i += d)
    {
        if (!is_folded(r, i))
        {
            r->focus = i;
            return true;
        }
    }

    return false;
}

void card_row_set_focus(struct card_row *r, int idx)
{
    if (idx < 0)
        idx = 0;
    if (idx >= r->n)
        idx = r->n - 1;

    while (idx > 0 && is_folded(r, idx))
        idx--;

    r->focus = idx;
    r->scroll = r->scroll_to = scroll_for_focus(r);
}

bool card_row_toggle(struct card_row *r)
{
    int p = owner_of(r, r->focus);

    if (!(r->flags[p] & CARD_ROW_PARENT))
        return false;

    /* Whatever every card's width is at this instant is where its next
     * animation starts, so opening a second run while a first is still
     * closing needs no special case. */
    for (int i = 0; i < r->n; i++)
        r->from_w[i] = r->cur_w[i];
    r->fold_t = 0;

    if (r->open == p)
    {
        r->open = -1;
        r->focus = p;
    }
    else
    {
        /* Opening a run moves into it. What was just asked for is the run,
         * and leaving the focus on the parent means the first thing the user
         * does next is step past a card they have already read. */
        r->open = p;
        if (p + 1 < r->n && (r->flags[p + 1] & CARD_ROW_SUB))
            r->focus = p + 1;
    }

    return true;
}

bool card_row_tick(struct card_row *r, int dt)
{
    int e;

    if (dt < 0)
        dt = 0;

    r->fold_t += dt;
    if (r->fold_t > FOLD_MS)
        r->fold_t = FOLD_MS;

    e = ease(r->fold_t, FOLD_MS);
    for (int i = 0; i < r->n; i++)
        r->cur_w[i] = (short)lerp(r->from_w[i], target_w(r, i), e);

    r->scroll_to = scroll_for_focus(r);

    for (r->accum += dt; r->accum >= CHASE_MS; r->accum -= CHASE_MS)
    {
        int d = r->scroll_to - r->scroll;
        int step;

        /* Still drains the accumulator, so a run of still frames cannot
         * leave time banked up for the next move to spend at once. */
        if (d == 0)
            continue;

        step = (d * CHASE_NUM) >> CHASE_SHIFT;
        if (step == 0)
            step = (d > 0) ? 1 : -1;
        r->scroll += step;
    }

    return r->fold_t < FOLD_MS || r->scroll != r->scroll_to;
}

int card_row_plan(const struct card_row *r, struct card_row_item *out,
                  int max)
{
    int x = 0, n = 0;

    for (int i = 0; i < r->n && n < max; i++)
    {
        int cw = r->cur_w[i];

        if (cw > 0)
        {
            int dst = x - r->scroll;
            int src = 0;
            int w = cw;

            if (dst < 0)
            {
                src = -dst;
                w -= src;
                dst = 0;
            }
            if (dst + w > r->view_w)
                w = r->view_w - dst;

            if (w > 0)
            {
                out[n].index = (short)i;
                out[n].src_x = (short)src;
                out[n].dst_x = (short)dst;
                out[n].w     = (short)w;
                n++;
            }
        }

        x += advance(r, i);
        if (x - r->scroll >= r->view_w)
            break;
    }

    return n;
}
