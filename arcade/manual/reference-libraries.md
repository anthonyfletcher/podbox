# Script libraries

Ready-made code for what many games write for themselves: text laid out,
the wheel as a control, easing, palette effects, a menu, panels and
lists, screen shake and timers, chance beyond `rnd()`.

- [Using one](#using-one)
- [text](#text), [fx](#fx), [rand](#rand), [tween](#tween),
  [palfx](#palfx), [dial and menu](#dial-and-menu), [gui](#gui)

## Using one

A library is a file in the SDK's `arcade/sdk/lib/` folder. Copy it into
the cart -- in the IDE, **File > Add files from disk** -- and it is a
namespace there: `text.js` gives `text.pad()`, `text.center()` and the
rest. Functions a cart never calls are left out when it is built, so a
library costs nothing for what goes unused.

Each file's opening comment lists all it does; this page shows what each
is for.

| File | Gives |
|---|---|
| `text.js` | padded numbers, clocks and times, centred, right-aligned, outlined, shadowed and wrapped text |
| `fx.js` | screen shake, hit-stop, and timers that count ticks down |
| `rand.js` | shuffles, weighted picks, ranges, from `rnd()` or a generator of the cart's own |
| `tween.js` | easing curves; any array slot moved along one over ticks |
| `palfx.js` | palette fades, flashes and colour cycling |
| `dial.js` | the wheel as a control: detents, angle, smoothed speed, a paddle |
| `menu.js` | an in-game menu on the wheel |
| `gui.js` | panels, buttons, lists and text fields in the dialogs' style |

## text

| Function | |
|---|---|
| `text.pad(n, width)` | `n` with noughts in front: `text.pad(42, 6)` is `000042` |
| `text.clock(seconds, [whole])` | `1:23.4`, or `1:23` with `whole` true |
| `text.lap(ms)` | `1:23.45`: milliseconds, as a time board shows them |
| `text.center(s, x, y, c)` | `s` centred on `x`; on the screen with `x` null |
| `text.right(s, x, y, c)` | `s` ending at `x` |
| `text.outline(s, x, y, c, edge)`, `text.shadow(s, x, y, c, under)` | `s` with an edge round it, or over its own shadow |
| `text.box(s, x, y, w, c)` | `s` wrapped at spaces to lines `w` wide; returns the height |
| `text.width(s)` | `s`'s width in pixels |

Cutting a line short to fit, with "...", is the engine's
[`textfit`](reference-text.md#textfittext-w-more).

```js
text.shadow("SCORE " + text.pad(points, 6), 4, 4, 7, 1);
text.center("GAME OVER", null, 56, 8);
```

## fx

The feel of a hit: the screen shaken, the game stood still for a moment,
and things that happen a few ticks later.

| Function | |
|---|---|
| `fx.shake(pixels, ticks)` | shakes the screen up to `pixels` either way for `ticks` |
| `fx.stop(ticks)` | a hit-stop: `fx.stopped()` is true for `ticks` |
| `fx.step()` | once a tick, first: counts both down, and the timers |
| `fx.view(x, y)` | `camera(x, y)` with the shake added |
| `fx.x`, `fx.y` | the shake this tick, to add when drawing by hand |
| `fx.after(ticks, id)` | `id` due in `ticks`; up to 32 wait at once, and false when all 32 are taken |
| `fx.due()` | the next `id` due this tick, -1 when none: call it until -1 |

```js
function update() {
    fx.step();
    if (fx.stopped()) return;            // the hit reads
    if (bossHit) { fx.shake(3, 8); fx.stop(2); fx.after(40, BOSS_NEXT); }
    for (var id = fx.due(); id >= 0; id = fx.due()) happen(id);
}

function draw() {
    fx.view(scrollX, 0);
    ...
}
```

## rand

Each takes a generator from [`rng.seed`](reference-language.md#rngseedg-seed-and-rngrndg-x)
last, to draw from it instead of `rnd()`'s own stream.

| Function | |
|---|---|
| `rand.int(lo, hi, [g])` | a whole number from `lo` to `hi`, both included |
| `rand.shuffle(a, [g])` | `a` in a random order, in place |
| `rand.weighted(w, [g])` | an index into `w`, each as likely as its weight |
| `rand.chance(n, [g])` | true one time in `n` |

```js
var bag = [0, 1, 2, 3, 4, 5, 6];
rand.shuffle(bag);                       // the next seven pieces
var drop = rand.weighted([70, 25, 5]);   // common, rare, very rare
```

## tween

Easing curves -- `tween.linear`, `in_quad`, `out_quad`, `in_out_quad`,
`out_cubic`, `out_back`, `out_bounce` -- and values moved along them.
`tween.start(arr, i, target, ticks, curve)` moves `arr[i]`, any slot of
an array, to `target` over `ticks`; `tween.step()` advances every one
once a tick and returns how many still run; `tween.running(arr, i)` says
whether that one does. Up to 32 at once: `tween.start` returns -1 when
all are taken.

```js
var pos = new FixArray(2);
tween.start(pos, 0, 120, 30, tween.out_back);   // a panel slides in
```

## palfx

Effects on the palette: everything on the screen changes at once,
nothing redrawn. `palfx.fade(t, rgb)` takes the colours `t` of the way
to `rgb`, `palfx.flash(rgb)` turns them all to it, and
`palfx.cycle(first, n, ticks)` turns a run of entries round for water,
lava and lights. The first fade or flash keeps the palette as it then
is, to go back to: after a `loadpal` or `setpal`, call `palfx.keep()`,
or the effects return to the old colours. See
[Colour and palettes](reference-colour.md) for what a change of the
palette costs.

## dial and menu

The wheel as a control, and an in-game menu worked by it: see
[The dial library](reference-input.md#the-dial-library) and
[The menu library](reference-input.md#the-menu-library).

## gui

The pieces the iPod's dialogs are made of, for a cart's own screens,
in the same style: `gui.box`, `gui.button`, `gui.buttons`, `gui.bar`,
`gui.lines` for text broken into lines, `gui.list` for a list moved by
the wheel, and `gui.field` for text typed with it. Its opening comment
lists every argument.
