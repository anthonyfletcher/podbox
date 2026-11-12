# Arcade API

Every function a cart can call. Arguments in `[brackets]` can be left
out. Colours are palette numbers, 0 to 255; positions are pixels, and a
fraction given as one is rounded down. The IDE shows each function's line
as you type it.

## Drawing

### cls([c])
Clears the drawing target to colour `c` (0 if left out), and puts the cursor and the clip back.

### pset(x, y, [c])
Sets one pixel.

### pget(x, y)
The colour of one pixel of the drawing target; 0 off it.

### line(x0, y0, x1, y1, [c])
A line between two points, both ends drawn.

### rect(x0, y0, x1, y1, [c])
A rectangle's outline, between two corners, inclusive.

### rectfill(x0, y0, x1, y1, [c])
A filled rectangle, between two corners, inclusive.

### circ(x, y, r, [c])
A circle's outline, centre `x, y`, radius `r`.

### circfill(x, y, r, [c])
A filled circle.

### oval(x0, y0, x1, y1, [c])
The outline of the ellipse inside a box.

### ovalfill(x0, y0, x1, y1, [c])
The filled ellipse inside a box.

### tri(x0, y0, x1, y1, x2, y2, [c])
A triangle's outline.

### trifill(x0, y0, x1, y1, x2, y2, [c])
A filled triangle.

### polyfill(xs, ys, n, [c], [ends], [bend], [boxes])
The first n points of xs and ys as a filled outline, concave or crossing itself, by the even-odd rule. ends, an array, cuts it into several outlines, each ending before the index given; one inside another is a hole. Where bend is non-zero, that point bends the line through it into a curve rather than lying on it, as in a TrueType outline. boxes, from `polyboxes` for the same points and ends, passes over the parts of a large outline out of view, drawing the same pixels for less; not with bend. At most 8192 points.

### polyboxes(xs, ys, n, [ends])
The outlines `polyfill` would draw from these points, cut into runs of 16 lines, with the box round each run, as an `Int32Array` to hand to `polyfill`. Made once for an outline that stays still, such as a level's ground; made again if its points change.

### polyline(xs, ys, n, [c], [ends], [closed])
Lines from each of the first n points of xs and ys to the next, in one call. c is a colour, or an array of one for each line (line i from point i). ends, an array, cuts the points into runs, each ending before the index given; closed joins each run's last point back to its first. At most 8192 points.

### fillp([p])
A 4x4 pattern for fills, 16 bits from the top left; set bits draw the colour's high byte (`c + 256 * c2`), or nothing when it is 0. `fillp()` turns it off.

### color([c])
The pen colour, used when a call leaves its colour out.

### cursor([x, y], [c])
Where `print` without a position writes next, and optionally the pen colour.

### pal([c0, c1], [p])
`pal(c0, c1)` draws colour `c0` as `c1` from now on; `pal(c0, c1, 1)` shows colour `c0` on the screen as `c1`'s, until changed. `pal()` puts both back.

### palt([c], [t])
Whether colour `c` is transparent in sprites, the map and images (only 0 at first). `palt()` puts it back.

### setpal(i, rgb)
What palette entry `i` looks like, as `0xRRGGBB`: for fades, flashes and colour cycling.

### getpal(i)
Palette entry `i` as `0xRRGGBB`.

### loadpal(PAL.name)
Loads one of the cart's palettes whole.

### color.mix(a, b, t)
Colour `a` (0xRRGGBB, as `setpal` takes) moved toward `b`, from 0 to 1.0.

### color.tone(c)
How light colour `c` reads, from 0 for black to 100 for white. Two colours read apart by the gap between their tones, whatever their hues.

### color.toned(c, tone)
Colour `c` at that tone, its hue kept: shades of one colour for a palette.

### color.turn(c, t)
Colour `c` with its hue turned by `t` turns, its tone kept: 0.5 is the opposite hue.

### color.contrast(a, b)
How well colours `a` and `b` stand apart, from 1.0 to 21.0. Text wants 4.5 against its background.

### camera([x, y], [turn], [px], [py], [zoom])
Moves everything drawn after by `-x, -y`: scrolling, and screen shake. With `turn` (turns, clockwise) the picture also turns about `px, py`, the middle of the target when left out, and with `zoom` (1 as it is, from 1/16 to 16) it grows about the same point: shapes turn and grow whole, their lines a pixel wide; `rspr` and `rblit` turn and grow too; other pictures, text and particles stay upright and their own size at their moved places; and map and tline are moved but not turned or grown, nor are the 3D systems' pictures, which have a camera of their own. `camera()` resets it.

### clip([x0, y0, x1, y1])
Draws only inside the rectangle between two corners, both inside it, as `rect` takes them. `clip()` resets it to the whole target.

### print(str, [x, y], [c])
Writes text, at the cursor without a position (moving it down a line). Returns the x after the last character. `print(str, c)` takes just a colour.

### textw(str)
How wide `print` would make `str`, in the current font and size.

### textfit(str, w, [more])
`str` cut to fit `w` pixels in the current font, with `more` ("..." when left out) after the cut; `str` itself when it fits. Cuts between characters, never inside one.

### texth(str)
How tall `print` would make `str`: its lines, in the current font and size.

### font(f, [size])
The font for `print`: `FONT.CAPS` (small capitals, the default), `FONT.WORLD` (every language) or one of the cart's `FONT.<name>`. `size` 2 doubles it.

### target(t)
Where drawing goes: `SCREEN`, or an image made by `img`.

## Sprites and images

### sheet([i])
The sheet sprites come from: an image such as `IMG.hero`, or the cart's sheet again with none. An image's sprites are its whole 8x8 cells, numbered along its rows from 0: a 64x16 image is sprites 0 to 7 across the top and 8 to 15 below. `spr`, `sspr`, `rspr`, `sget`, `sset`, `sprites.draw`, `actors.draw` and `plane.billboards` use it. The map, `tline`, the plane's floor and ceiling, and `fget` and `fset` always use the cart's sheet.

### spr(n, x, y, [w, h], [flip])
Sprite `n` of the sheet, or a block `w` by `h` sprites from it, at `x, y`; `flip` 1 mirrors it, 2 turns it upside down, 3 both, as every call that flips takes it.

### sspr(sx, sy, sw, sh, dx, dy, [dw, dh], [flip])
Any rectangle of the sheet, stretched to `dw` by `dh`.

### sget(x, y)
A pixel of the sheet.

### sset(x, y, [c])
Sets a pixel of the sheet.

### fget(n, [f])
Sprite `n`'s flags: all eight as a number, or flag `f` (0 to 7) as true or false. With flags named in `cart.json`, `fget(n) & FLAG.solid` tests one by name.

### fset(n, v, [f])
Sets sprite `n`'s flags: all eight to `v`, or with `f` that one flag to `v`, true or false.

### tline(x0, y0, x1, y1, mx, my, [mdx, mdy])
A line textured from the map, starting at map position `mx, my` (in cells, fractions allowed) and stepping `mdx, mdy` a pixel: floors and roads in the "mode 7" style.

### img(w, h)
A new image, all colour 0, to draw into with `target` and onto the screen with `blit`.

### imgw(i)
An image's width.

### imgh(i)
An image's height.

### img.load(i)
A lazy image (named in cart.json's `lazy_images`) read from the cart file, so that `IMG.name` is then it; again, the same copy. Returns the image.

### img.drop(i)
A lazy image's copy let go: `IMG.name` is not loaded until `img.load` again.

### blit(i, x, y, [flip])
Draws a whole image.

### blitpart(i, sx, sy, w, h, x, y, [flip])
Draws a rectangle of an image.

### rspr(n, x, y, angle, [scale], [w], [h], [flip])
Sprite `n`, or a block `w` by `h`, flipped as `spr` flips, then turned by `angle` (in turns) and scaled about its centre at `x, y`.

### rblit(i, x, y, angle, [scale])
An image turned and scaled about its centre at `x, y`.

## The map

### map([celx, cely, sx, sy, celw, celh], [layers], [cells, w])
Draws map cells from `celx, cely`, `celw` by `celh` of them, at pixel `sx, sy`; with `layers`, only cells whose sprite has all those flags. `map()` draws the whole map. With `cells`, a `Uint8Array` `w` cells a row, draws those instead of the active map: a second layer.

### mget(x, y)
The sprite in map cell `x, y`; 0 off the map.

### mset(x, y, v)
Sets a map cell.

### loadmap(MAP.name, [cells])
Makes one of the cart's maps the active one, read from the cart file as it was built; with `cells`, a `Uint8Array`, unpacks it there instead, the active map untouched. Returns the map's width.

### mapw()
The active map's width in cells.

### maph()
The active map's height in cells.

### mapobj(out, max)
The active map's objects, up to `max` of them into `out`, five numbers each: kind (`OBJ.<kind>`), x, y, w, h. Returns how many the map has.

### mapprop(i, name)
Object `i`'s property `name`: a number or a string, or null.

## Input

### btn([i])
Whether button `i` is held (0 Prev, 1 Next, 2 Menu when the cart takes it, 3 Play, 4 Select); without `i`, all of them as bits.

### btnp([i])
Whether button `i` was pressed this tick, and again while it is held (after 15 ticks, every 4).

### btnr([i])
Whether button `i` was let go this tick: down last tick, up this one.

### wheel()
The clicks the wheel turned this tick, 96 to a turn, clockwise positive.

### wheelsteps()
The rows a list moves for this tick's turn of the wheel, as the player's own lists move: one every 4 clicks, and many at once when it is spun fast. Clockwise positive.

### wheelpos()
Where the finger is on the wheel, 0 to 95 clockwise from the top, or -1 when it is not touched.

### click([beep])
The wheel's own tick, or with `beep` its short beep, from the player's little speaker.

### sfxat(channel)
The effect a channel is playing, `SFX.<name>`'s number; -2 for a `tone`, -1 when it is silent.

## Sound

### sfx(n, [channel])
Plays sound `n` (`SFX.<name>`) on channel 0-3, or the first free one. `sfx(-1, channel)` stops a channel.

### sfxp(n, semitones, [channel])
Plays a sound transposed by `semitones`.

### tone(channel, hz, ticks, vol, wave)
A plain note: `vol` 0-255, `wave` one of `WAVE.SQUARE`, `SAW`, `TRIANGLE`, `SINE` or `NOISE`.

## Maths

### rnd([x])
`rnd()` a fraction below 1; `rnd(10)` a whole number 0 to 9; `rnd(10.0)` a fraction below 10; `rnd(array)` one of its elements.

### srand(x)
Seeds `rnd`, so the same seed gives the same numbers; given the `Int32Array` `rndstate` filled, puts that state back.

### rndstate(out)
`rnd`'s state into an `Int32Array` of 2, to save with the game and give back to `srand`, so a game carries on its random sequence.

### rng.seed(g, seed)
A generator of the cart's own in `g`, an `Int32Array` of 2, seeded as `srand` seeds `rnd`: a stream a daily puzzle can keep apart from the game's other chances. `g` is its state, to save like any numbers.

### rng.rnd(g, [x])
As `rnd(x)`, drawn from generator `g`, which moves on.

### flr(x)
Rounds down to a whole number.

### ceil(x)
Rounds up to a whole number.

### abs(x)
The value without its sign.

### min(a, b)
The smaller.

### max(a, b)
The larger.

### mid(x, y, z)
The middle one of three: `mid(lo, v, hi)` keeps `v` between `lo` and `hi`.

### sgn(x)
1 or -1 by the sign; 1 for 0.

### sqrt(x)
The square root: of a whole number, rounded down; of a fraction, a fraction.

### sin(t)
The sine of `t` turns (1.0 is a full turn), upside down so that it turns the way the screen's y points.

### cos(t)
The cosine of `t` turns.

### atan2(dx, dy)
The angle of a direction, in turns from 0 to just under 1, matching `sin` and `cos`.

### deg(degrees)
An angle in degrees as turns, worked out when the cart is built: `deg(90)` is 0.25. Only a number written in the code.

### rgb(hex)
The colour of the cart's starting palette nearest to `0xRRGGBB`, worked out when the cart is built: `rgb(0xff0000)` is 8 in the standard palette. Only a number written in the code.

## Time, saving and the system

### t()
Seconds since the cart started, a fraction.

### time()
The same as `t()`.

### frame()
Ticks since the cart started.

### date(out)
The date and time into `out`: year, month, day, hour, minute, second, weekday (0 Sunday).

### rate()
Ticks a second now.

### setrate(n)
20, 25, 33 or 50 ticks a second, from the next tick.

### skipped()
Frames that ran late and were not drawn since the last call.

### unitsnow()
The units this tick has spent so far, to leave out an effect before the tick runs late.

### resumed()
Whether the game was paused -- the pause menu, the hold switch, the question a short press of Menu asks -- since the last call.

### dget(i)
Saved number `i`, 0 to 255; kept between plays.

### dset(i, v)
Sets saved number `i`; written when the cart ends or pauses.

### menuitem(i, [label], [fn])
Item `i` (1 to 5) of the pause menu, calling `fn` when chosen; without `label`, removes it.

### quit()
Back to the player's menu once this tick ends; unlike the pause menu's Quit, the game is not kept to carry on.

### printh(str)
A line in the IDE's console; nothing on the player.

### watch(name, v)
A value the IDE shows in its Watch panel as it changes, and `pbgvm` prints when it changes; nothing on the player.

### prof([name])
Starts a section the IDE's profiler times exactly, or with no name ends the latest; `pbgvm --units` prints each section's units. Nothing on the player.

### mem(MEM.name)
The engine's own bytes as a `Uint8Array`: `MEM.SCREEN` (160x120), `MEM.SHEET`, `MEM.MAP` or `MEM.FLAGS`.

### copy(dst, di, src, si, n)
`n` elements from `src` at `si` to `dst` at `di`, overlapping safely; both arrays of one kind.

### fill(a, v, [i], [n])
Sets `n` elements of `a` from `i` to `v`; to the end by default.

## Dialogs

### ui.choose(prompt, items, [at])
A list of 1 to 32 items to choose from, starting at `at`. Returns the index chosen, or -1 if backed out of.

### ui.confirm(text, [title])
Yes or No, No first. Returns true for Yes.

### ui.message(text, [title])
Text to read, and OK.

### ui.number(prompt, min, max, value, [step])
A number turned by the wheel. Returns it, or `value` if backed out of.

### ui.text(prompt, max, [start])
A line of up to `max` bytes (1 to 64; a letter beyond A to Z takes 2 or 3) typed with the wheel. Returns it, or null if backed out of.

### ui.style(box, text, highlight)
The dialogs' three colours.

## Speech

### speak.say(value, ...more)
Says the values, words and numbers, as one phrase, cutting off anything being said.

### speak.queue(value, ...more)
Says the values after what is being said.

### speak.busy()
Whether anything is still being said.

### speak.stop()
Stops speaking.

### speak.on()
Whether the owner has the player's menus spoken.

## High scores, options and achievements

### score.submit(board, value, [name])
Records a score on `BOARD.<name>`. Returns its rank 1 to 10, or 0 when it did not make the ten.

### score.best(board)
The board's top value, or null when it is empty.

### score.get(board, rank)
The value at rank 1 to 10, or null.

### score.name(board, rank)
The name given with the score at `rank`, "" for none, or null.

### score.count(board)
How many scores the board holds, 0 to 10.

### score.show(board)
Shows the player's table for the board after this tick, the latest score marked.

### opt.get(OPT.name)
An option's value: a choice's index, 0 or 1, or the number.

### opt.set(OPT.name, value)
Sets an option from the game -- its own settings page -- fitted to the option's range, and kept as the pause menu keeps it.

### opt.changed()
Whether the owner changed an option since the last tick; true on the first tick, so a cart reads its options where it reads a change.

### ach.earn(ACH.name)
Earns an achievement. Returns true the first time, when the player announces it.

### ach.has(ACH.name)
Whether an achievement has been earned.

## Files

### data.read(DATA.name)
One of the cart's data files as values: arrays, records, strings and numbers, or a `.txt` file as a string. Read it once and keep it.

### store.read(name)
The value stored as `name`, or null.

### store.write(name, value)
Stores a value, kept between plays. Returns false when it is over 64 KB or the cart's space is full.

### store.remove(name)
Removes a stored value. Returns whether there was one.

### store.list()
The names stored, sorted.

## Actors and particles

### actors.pool(n)
A pool of `n` actors: a record of arrays `on`, `x`, `y`, `dx`, `dy`, `w`, `h`, `sp`, `flip`, `frames`, `speed`, `cell`, `zone` and `mat`, with for the whole pool `gravity`, `drag`, `platforms` (another pool, solid, that carries those standing on it), `oneway`, `slope_up`, `slope_down` and `ladders` (flags of one-way cells, ramps and ladders on the map), and `field`, `rules`, `field_x`, `field_y`, `density` and `splash` for a cells field.

### actors.spawn(p)
Turns on the first free actor, its speed cleared. Returns its index, or -1 when all are in use.

### actors.move(p, solid, trigger, out, [oneway])
Moves every actor by its speed, with gravity and drag, stopping against map cells whose flags share a bit with `solid`. Contacts go into `out`, four numbers each: the actor, the side (`HIT.*`) and the cell. Returns how many.

### actors.overlaps(a, b, out)
Pairs of actors, one from each pool, whose boxes overlap, into `out` two numbers each. Returns how many.

### actors.draw(p)
Draws every actor that is on and has a sprite.

### actors.zones(p, objs, n, out)
Which of `n` map objects (from `mapobj`) each actor's centre is in: going in and out are reported as `HIT.ENTER` and `HIT.LEAVE`. Returns how many reports.

### actors.bounce(a, b, out, [give])
Actors as circles, pushed apart and bounced off one another. Returns the pairs that touched.

### actors.bounce_map(p, flags, out, [give])
Actors as circles, moved and bounced off map cells whose flags meet `flags`. Returns the cells struck.

### particles.pool(n)
A pool of `n` particles: arrays `life`, `x`, `y`, `dx`, `dy`, `c`, `sp` and `flip`, with `gravity`, `drag` (negative speeds them up) and `cull` (gone once `particles.draw` finds it off the screen, through the camera it draws by), and for sprite particles `w`, `h` (up to 16), `shade`, `floor`, `bounce` and `spin`.

### particles.emit(p, count, x, y, dx, dy, spread, life, c, [sp])
Up to `count` particles at `x, y`, moving `dx, dy` plus up to `spread` either way, living about `life` ticks; with `sp`, each is that sprite, flipped at random. Returns how many.

### particles.step(p)
A tick of every live particle: gravity, drag and moving; bouncing off the pool's `floor`, and sprites turning every `spin` ticks.

### particles.draw(p)
Every live particle as a pixel of its colour, or as its sprite's top left `w` by `h`, its colours 1 to `shade` taken from the particle's colour onward.

### particles.bounds(p, out)
The box the live particles cover as drawn -- after the camera, a sprite its size -- into an `Int32Array` of 4, `x0, y0, x1, y1` inclusive. Returns how many are live; with none, `out` is left as it was.

### sprites.draw(sp, xs, ys, n, [flips])
`n` sprites at once, from the cart's arrays, each flipped as `spr` flips; `sp` may be one number for all.

## Boards and paths

### grid.fill(b, w, x, y, v)
Flood fill: the region of equal cells at `x, y` on board `b`, `w` wide, set to `v`. Returns the cells changed.

### grid.region(b, w, x, y, out)
The region of equal cells at `x, y`, their indices into `out`. Returns its size.

### grid.runs(b, w, n, out)
Every cell in a row or column run of at least `n` equal cells, into `out`. Returns how many.

### grid.full(b, w, out)
The rows with no empty cell, into `out`. Returns how many.

### grid.collapse(b, w, rows, n)
Takes out the `n` rows listed, those above dropping down. Returns the rows removed.

### grid.fall(b, w)
Drops every cell down through the empty ones below it. Returns the cells that moved.

### grid.slide(b, w, dir, [merged])
Slides every cell toward `DIR.LEFT`, `RIGHT`, `UP` or `DOWN`, two equal cells joining as one, as in 2048. Returns the cells changed.

### grid.fits(b, w, s, sw, x, y)
Whether shape `s`, `sw` wide, at column `x`, row `y`, lands only on empty cells.

### grid.place(b, w, s, sw, x, y)
Writes shape `s` onto the board. Returns the cells written.

### grid.rotate(s, sw, out, turns)
Shape `s` turned clockwise by quarter turns into `out`. Returns its width after.

### path.field(b, w, solid, x, y, out)
Every open cell's distance in steps from `x, y` into `out`, -1 where it cannot be reached. Returns the cells reached.

### path.next(field, w, x, y)
The direction (`DIR.*`) one step nearer the field's start from `x, y`, or -1.

### path.find(b, w, solid, x0, y0, x1, y1, out)
A shortest way from one cell to another, into `out`. Returns its length in steps, or -1 when there is none.

### path.sight(b, w, solid, x0, y0, x1, y1)
Whether one cell can see another across open cells.

### path.view(b, w, solid, x, y, r, out, [mark])
Every cell within `r` of `x, y` that `path.sight` sees, into `out`, a `Uint8Array` the board's size: cleared and each seen cell 1, or with `mark`, only the seen cells set to `mark`. Returns how many are seen.

### path.ray(x, y, dx, dy, solid, out, [len])
The first map cell hit going from `x, y` along `dx, dy`, into `out`: the point, the cell and its face. Returns 1 on a hit.

### noise.at(x, y, seed, [octaves])
Smooth random noise at `x, y`, a fraction from 0 to just under 1.

### noise.fill(dst, w, ox, oy, scale, octaves, seed, ramp)
Fills a board or an image from the noise, each value one of `ramp`'s. Returns the cells filled.

## Cells and links

### cells.rules()
A record of tables for the cart to fill: each material's movement, spread, density, flags, decay and what it becomes, and the reactions between them.

### cells.step(field, w, rules, [actors], [box])
One tick of a field of materials by the rules, flowing round any actors. With `box`, an `Int32Array` of 4: x0, y0, x1, y1 round what changed since the last step, left as it was when nothing did. Returns the cells that moved.

### cells.life(field, w, birth, survive, [born])
One generation of a Life-like rule: birth 8, survive 12 is Life. Returns the cells changed.

### cells.lift(field, rules, x, y, r, pool, [speed])
Throws the loose cells within `r` of `x, y` out as particles: a blast. Returns the cells lifted.

### links.pool(points, links)
A pool of points and the links between them: ropes, chains, bridges and cloth.

### links.point(p, x, y, [pin])
A point at `x, y`, pinned in place if `pin`. Returns its index, or -1 when the pool is full.

### links.link(p, i, j, [len])
Holds points `i` and `j` `len` apart (as far as they are now by default). Returns its index, or -1.

### links.step(p, solid, [out], [which])
A tick of every point and link, kept out of map cells whose flags share a bit with `solid`, and out of the lines of the pool's `segs`: one segment set, or an array of up to 16. With `out`, three numbers a point: the material it touched plus one (0 none), and the way out, `nx`, `ny`; with `which`, one a point: the index in `segs` of the set it met, -1 none.

### links.ring(p, first, n, area, stiffness)
Speeds points `first` to `first + n - 1`, a closed ring, toward holding `area` square pixels: all the way by the next step at `stiffness` 1. Returns the area it holds.

### links.draw(p, colour)
Draws every link as a line.

### links.cloth(p, x, y, cols, rows, gap, [pin])
A sheet of cloth, `cols` by `rows` points `gap` apart; `pin` 1 pins its top row, 2 its top corners, 0 nothing. Returns its first point.

### links.fill(p, ramp)
Draws the cloth filled, each cell shaded from `ramp` by how it faces the light. Returns the cells drawn.

## Segment sets

### segs.make(xs, ys, n, [mats], [ends])
The outlines through the n points, as polyfill takes them, as lines points meet: what polyfill would fill is solid, and points are kept out of it; each line takes the material of the point it starts from. Set it in a links pool's `segs`. At most 4096 points.

### segs.move(s, dx, dy)
Moves the set by `dx, dy`; points resting on it are carried by the `links.step` that follows in the same tick. Called after the step, nothing is carried: a lift rises through what stands on it.

### segs.draw(s, colour)
Draws every line, where the set is now.

### segs.trace(solid, [sharp], [smooth], [cx, cy, cw, ch], [shapes], [outside])
The outlines of the map cells whose flags share a bit with `solid`, as a record of `x`, `y`, `ends` and `n` for polyfill and segs.make: each square corner cut by half a cell, but square where a cell has `sharp`; `smooth` (0 to 3) passes round them. Within cells `cx, cy`, `cw` by `ch`, the whole map when left out. `shapes`, by sprite, makes a cell a slope: 1 full, 2-5 half cut corner to corner (floors rising right and left, ceilings falling right and left), 6-9 shallow floors (rising right low then high half, falling right high then low half), 10-13 those upside down. With `outside` true every cell beyond the area is solid, and the first outline is a rectangle 4096 pixels beyond it; or outside is the sides made solid, added: 2 left, 4 right, 8 above, 16 below, what lies beyond them reaching 4096 pixels out. At most 4096 points.

## The plane, landscapes and walls

### plane.view(x, y, z, angle, horizon, fov)
The camera for the plane: position and height in world pixels, heading in turns, the horizon's screen row and the field of view in turns.

### plane.draw([top], [bottom], [outside], [coarse], [under])
The active map as a floor seen from the camera, on the rows from `top` to `bottom`.

### plane.fog(near, far, colour, [billboards])
Fades the ground to `colour` from `near` to `far` world pixels away; colour -1 turns it off. The billboards fade with it unless `billboards` is false.

### plane.sky(image, [lift])
A panorama behind the view, the image's width a whole turn.

### plane.ceiling(height, [table], [outside], [coarse], [top])
A ceiling `height` world pixels up, from the map's sprites.

### plane.project(x, y, z, out)
A world point on the screen: its x, y, scale and depth into `out`. Returns 0 when it is behind the camera.

### plane.pick(sx, sy, out)
The floor point under a screen pixel, into `out`. Returns 0 above the horizon.

### plane.billboards(xs, ys, zs, sprites, sizes, flips, n, [ws], [hs])
`n` sprites standing on the floor, scaled by distance and drawn far to near. Returns how many were drawn.

### land.draw(heights, colours, [far], [scale], [coarse], [ramp])
A landscape from two images, its heights and colours, seen from the plane's camera. Returns the samples taken.

### land.height(heights, x, y, [scale])
The ground's height at world pixel `x, y`.

### walls.draw(solid, [height], [far], [dark], [doors])
First-person walls from map cells whose flags share a bit with `solid`, seen from the plane's camera. Returns the screen columns that met a wall.

### light.draw(x, y, r, n, ramp)
`n` lights over the scene, each a centre and a reach, darkening what they do not reach by `ramp`'s colour tables.

## The owner's music

### playback.playing()
Whether the owner's music is playing.

### playback.elapsed()
How far into the playing track, in ms.

### playback.length()
The playing track's length in ms.

### playback.changed()
True for one tick after the track changes.

### playback.title()
The playing track's title.

### playback.artist()
The playing track's artist.

### playback.album()
The playing track's album.

### playback.track()
The playing track's id, or -1.

### playback.toggle()
Pauses the music, or plays it again. Needs `"music": "control"`.

### playback.next()
Skips to the next track. Needs `"music": "control"`.

### playback.prev()
Goes back a track. Needs `"music": "control"`.

### playback.seek(ms)
Moves to `ms` into the track. Needs `"music": "control"`.

### playback.duck(level, [ticks])
Turns the music down to `level`/256 of the owner's volume, over `ticks`; undone when the cart ends. Needs `"music": "control"`.

### playback.volume()
The owner's volume, 0 to 100 of the player's range.

### playback.adjust(n)
Turns the owner's volume up `n` of the player's own steps, or down for a negative `n`, as the wheel does on the player's screens; it stays where it is put when the cart ends. Returns the volume then, 0 to 100. Needs `"music": "control"`.

### playback.play(id, [ms])
Plays track `id`, from `ms` in. Returns whether it started. Needs `"music": "own"`.

### playback.stop()
Stops the music. Needs `"music": "own"`.

### playback.started()
Whether the track asked for can be heard yet: a round's timer starts on this.

### playback.bands(out, n)
`n` spectrum bands, 1 to 8, low to high, each 0 to 100, into `out`.

### playback.sound(out)
The playing track's Sound Index (`SOUND.*`) into `out`. Returns 0 when it has none.

### playback.colors(out)
The colours the player is showing, each 0xRRGGBB, into `out`, an Int32Array of 3: the background, the text and the most vivid. Returns 1 when they are the playing album's (dynamic colours), 0 when they are the theme's.

### tracks.count([filter])
How many of the owner's tracks match `filter`: `{ minlen, maxlen, genre, decade, music, artist }`, any field left out.

### tracks.pick(n, filter, out)
`n` tracks at random, their ids into `out`. Returns how many.

### tracks.field(id, f)
One thing about a track: `TRACK.TITLE`, `ARTIST`, `ALBUM` or `GENRE` as a string, `YEAR` or `LENGTH` as a number.

### tracks.near(id, n, out, [sound])
Up to `n` tracks most like `id`, into `out`: the same genre and decade first, or with `sound` true the nearest by Sound Index first. `id` can be a list of up to 10 tracks, `out` then holding `n` for each in turn, -1 after the last of each. Returns how many in all.

### tracks.sound(id, out)
Any track's Sound Index, as `playback.sound` gives it.

### tracks.queued()
How many tracks the queue holds.

### tracks.place()
The playing track's place in the queue, 0 first, or -1.

### tracks.queue(i)
The track at place `i` of the queue, or -1.

### tracks.enqueue(id, [next])
Adds a track after the playing one (`next`) or at the end of the queue. Needs `"music": "control"`.

### sys.setting(name)
One of the owner's settings, by its name in `config.cfg`: a number, or its text; null when there is none.

### sys.translate(phrase)
A phrase the player knows, given in English, in the owner's language.

## The beat

### beat.listen(on)
Starts or stops listening to the music for its beat. Listening costs time, so stop once the tempo is held.

### beat.state(out)
Into `out`: locked, bpm, period in ms, confidence, fill, beats since the lock. Returns whether it is locked.

### beat.next(from, n)
The time of the `n`th beat at or after `from`, or -1 with no lock.

### beat.inbar()
The place in the bar (0 to 3) of the beat `beat.next` last gave.

### beat.downbeat()
Which of the four beats starts the bar, or -1 when it is not yet clear.

### beat.margin()
How clearly `beat.downbeat` won, in tenths.

### beat.clock()
The track's time this tick, in ms: what a frame is drawn from.

### beat.now()
The track's time at this instant, in ms.

### beat.press(i)
When button `i` went down this tick, in the track's ms, or -1: presses are judged by this.

### beat.offset([ms])
The cart's timing calibration, taken off every press `beat.press` reports: a player who presses 40 ms late on what they hear sets 40. -500 to 500; returns the offset in force.

### beat.jumped()
True for the tick after the music jumped: a seek, a skip, a new track.

### beat.left()
The ms of the track still to play.

### beat.mood(out)
The bar just heard, into `out`: energy, flux and trend.

### beat.ahead(out, from, to)
The hits already heard in the music from `from` to `to`, three numbers each: time, bands, strength. Returns how many.

### beat.horizon()
How far ahead `beat.ahead` can see, in the track's ms, or -1 when not listening.
