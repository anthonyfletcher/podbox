# Arcade SDK

The compiler for Arcade carts: games written in a small subset of
JavaScript and played on the iPod as `.pbg` files.

Needs Node 20 or later. Once, in this folder:

```sh
npm install
```

## Making a cart

```sh
node pbgc.js new mygame "My Game"    # a folder with cart.json and main.js
node pbgc.js build mygame            # writes mygame.pbg
node pbgc.js dis mygame.pbg          # checks a cart and lists its bytecode
node pbgc.js unpack game.pbg game    # the cart's folder, from the cart alone
node pbgc.js pack game               # the same as build
```

A cart carries its own sources, so `unpack` gives back every file as it
was written. Pictures come back as indexed PNGs in the cart's first
palette, with colour 0 transparent: the same palette numbers, though not
the same file if it was saved with other colours. `pack` then makes the
same cart, byte for byte.

A cart is a folder:

- `main.js` holds `init()`, run once, `update()`, run every tick, and
  optionally `draw()`.
- Any other `.js` file is a namespace named after the file: `hit()` in
  `brick.js` is called as `brick.hit()`.
- `cart.json` holds the settings:

| Key | Value |
|---|---|
| `id` | 16 hex digits, made by `pbgc new`. Saves and scores are kept under it, so never change it |
| `name` | shown in the Games menu |
| `author`, `version`, `help` | text, optional |
| `rate` | ticks a second: 20, 25, 33 (the default) or 50 |
| `menu_button` | `pause` (the default) or `game`, to have MENU as button 2 |
| `music` | `listen` (the default), `control` or `own` |
| `boards` | the high-score tables, up to 8, below |
| `options` | the settings the pause menu shows, up to 8, below |

Errors name the file, line and column, and say what to write instead.

Each board is a record: `name`, which the code uses as `BOARD.<name>`;
`title`, shown in High Scores (the name when left out, at most 23 bytes);
`wins`, `high` (the default) or `low`; `format`, `number` (the
default), `time` (milliseconds, shown as 1:23.45) or `ticks` (shown as a
time at the cart's rate); and `version`, 1 by default. A board keeps its ten
best, each with its date and any name given to `score.submit()`. Its table
is found again by its name, so boards can be reordered; raise its version
to start it afresh.

```json
"boards": [
  { "name": "points", "title": "Points" },
  { "name": "lap", "title": "Best Lap", "wins": "low", "format": "time" }
]
```

Each option is a record: `name`, which the code uses as `OPT.<name>`;
`title`, its row in the pause menu (the name when left out, at most 23
bytes); `type`; `default`; and `help`, up to 300 bytes, shown under
its title by the pause menu's Help, after the cart's own `help`. The
types:

| `type` | Also takes | `opt.get()` gives | `default` |
|---|---|---|---|
| `choice` | `choices`, 2 to 8 texts of up to 23 bytes | the index of the one chosen | an index, 0 when left out |
| `onoff` | | 0 or 1 | `true` or `false`, `false` when left out |
| `number` | `min`, `max`, and `step` (1 when left out) | the number | `min` when left out |

The owner's values are kept, each found again by its option's name.
`opt.changed()` is true for the one tick after the owner changes one: the
time to rebuild anything that depends on them.

```json
"options": [
  { "name": "level", "title": "Difficulty", "type": "choice",
    "choices": ["Easy", "Normal", "Hard"], "default": 1 },
  { "name": "trails", "title": "Trails", "type": "onoff", "default": true },
  { "name": "speed", "title": "Speed", "type": "number", "min": 1, "max": 10 }
]
```

A cart's art goes in the same folder:

| File | Holds |
|---|---|
| `sheet.png` | the 128x128 sprite sheet |
| `flags.json` | the 256 sprite flags: an array, or `{ "12": 3 }` for just some |
| `palettes/<name>.gpl` | a palette in GIMP's format; the first, by name, is loaded when the cart starts |
| `maps/<name>.json` | a map: `width` and `height` (up to 256), `rows` of two hex digits a cell, and `objects`, each with a `kind`, `x`, `y`, `w`, `h` in pixels and `props` of whole numbers or strings |
| `images/<name>.png` | any other picture, up to 1024 pixels a side |
| `data/<name>.json` | data the code reads: levels, waves, dialogue |
| `data/<name>.txt` | text the code reads as one string |
| `fonts/<name>.bdf` | a font in BDF, the bitmap font format FontForge and other font editors save: up to 16 a cart, glyphs up to 32 pixels a side, 64 KB of glyphs in all |

Save every PNG as an **indexed** (palette) PNG: each pixel's palette number
is the colour number the cart draws with, whatever colours the PNG shows.

In code, `MAP.<name>` and `PAL.<name>` are for `loadmap()` and `loadpal()`,
`OBJ.<kind>` is the number `mapobj()` gives an object of that kind, and
`IMG.<name>` is the picture itself, for `blit()`, `target()` and the rest.
`font(FONT.<name>)` draws text in a cart font, and `font(FONT.<name>, 2)`
doubles it, as for the engine's `FONT.CAPS` and `FONT.WORLD`. A character
the font lacks draws as its `?`, or not at all if it has none; a `ui`
dialog whose text the font lacks is drawn in `FONT.WORLD`. Text in a cart
font or WORLD costs more than in CAPS: about 20 units a character and
three quarters of a unit a pixel drawn. This SDK's own `fonts/caps.bdf`
is CAPS itself, a starting point to draw from.
`data.read(DATA.<name>)` gives a data file as the cart's own values --
arrays, records, strings, numbers, with `true` and `false` as 1 and 0 --
or a `.txt` file as a string, made afresh at each call, so read it once
in `init()`. A number a cart cannot hold (a whole number past 32 bits, a
fraction past -32768 to 32767) is an error when the cart is built.
The first map is the active one when the cart starts; without any, the map
is an empty 128x64.

### Script libraries

`lib/` holds libraries written in the cart language. Copy one into the
cart's folder and it is a namespace there, read and changed as the cart's
own code; a function the cart never calls is dropped from the build. Each
file's opening comment lists what it gives.

| File | Gives |
|---|---|
| `dial.js` | the wheel as a control: detents with the rest carried, the angle turned, a smoothed speed, the finger's place on an arc |
| `tween.js` | easing curves, and any array slot moved along one over a number of ticks |
| `text.js` | padded numbers, a clock, centred and right-aligned text, outlined text, words wrapped in a box |
| `palfx.js` | fades and flashes of the whole palette, and colour cycling |
| `menu.js` | an in-game menu on the wheel and SELECT; needs `dial.js` |
| `gui.js` | the pieces the `ui` dialogs are made of, drawn the same way: a panel, text broken into lines, buttons, a scroll bar, a scrolling list and a text field typed with the wheel |

## How the language differs from JavaScript

- **A number is an integer or a fraction.**
  - A number written with a point is a fraction: `1.5`, `0.25`, and also
    `2.0`. Without one it is an integer: `2`.
  - Integers are 32 bits, so scores and counters can be large.
  - Fractions are PICO-8's: steps of 1/65536, from -32768 to just under
    32768.
  - Anything involving a fraction gives a fraction: `x * 0.5` and
    `speed / 4.0` work as written.
  - **Two integers divide to an integer**: `7 / 2` is 3, truncated toward
    zero. Write `7 / 2.0` for 3.5.
  - Arithmetic wraps when it runs out of range, as PICO-8's does. An
    integer above 32767 mixed with a fraction wraps.
  - `Math.floor`, `ceil`, `round` and `trunc` turn a fraction into an
    integer. So do `|`, `&` and the other bitwise operators, which truncate
    as JavaScript's do: `x | 0`.
  - A fraction used as an array index is rounded down.
  - `Math.sqrt(2)` is 1, because the root of an integer is an integer.
    `Math.sqrt(2.0)` is 1.4142.
  - Fractions print with up to four decimal places: `1 / 3.0` prints
    `0.3333`.
- **`==` is `===`.** Nothing is converted for a comparison, so `1 == "1"` is
  false.
- **Strings are bytes.** Text is stored as UTF-8. `length`, `charCodeAt` and
  `substring` count bytes, which matches JavaScript only for plain ASCII.
- **Functions cannot capture local variables.** A function expression may
  use globals and its own parameters only.
- **`undefined` is `null`.** `typeof null` is `"object"`. `true` and `false`
  are 1 and 0, so `typeof true` is `"number"`.
- **The top level of a file holds only declarations.** Use `var`,
  `function`, and `const` with a value known when compiling. Code that runs
  once goes in `init()`. Globals that need code to set them (`new
  Uint8Array(n)`, an array or record literal) are set before `init()`: the
  other files first, in alphabetical order, then `main.js`.
- **Records can't have methods named like array or string methods.** On a
  record, `o.push()`, `o.sort()` and the rest of those names are always the
  built-in methods, never a field of the record.
- **A sort's comparator can't sort.** Calling `.sort()` from inside a
  comparator stops the cart with an error.

## The IDE

```sh
node ide/build.js --font font.bin     # writes ide/dist/arcade-ide.html
```

The IDE is one HTML file: open it in a browser, from the web or from
disk, with no server. It edits a cart's files and runs the cart beside
them on the same engine as the player. `--font` builds in the Japanese,
Chinese and Korean glyphs from the player's `font.bin`; without it they
show as `?`.

- **Run** (Ctrl+Enter) builds the cart and plays it at its rate. Click
  the screen to give it the keys: Left and Right are Prev and Next, Up and
  Down turn the wheel, Enter is Select, Space is Play and Esc is Menu,
  which pauses unless the cart takes Menu as a button. Or use the drawn
  wheel: drag round the ring to turn it, click the ring for Menu, Prev,
  Next or Play, hold it for a held button, and click the middle for
  Select.
- **Errors** are marked in the code as you type. A runtime error stops the
  cart, marks its line and opens the file at it.
- **The meter** shows the units each tick costs against what the iPod
  Video has with music playing: keep it green.
- **Open** takes a `.pbg`, and dropping one on the page opens it too.
  **Save .pbg** writes the cart. In Chrome and Edge, Save writes back to
  the file you opened, and **Open folder** edits a cart folder in place,
  Save writing its files back.
- The cart open when you close the page is there when you come back.
  `store` files are kept in the browser.

## Running a cart's script

`host/pbgvm` is the player's engine built for this computer. It runs a
cart without a screen, printing what `printh()` prints:

```sh
sh host/build.sh                      # needs gcc, or CLion's MinGW on Windows
host/pbgvm mygame.pbg --ticks 100 --units
```

`--units` reports the cost of `init()` and of each tick. A unit is about
0.55 µs on an iPod Video with no music playing, and about twice that with
music playing; the iPod Classic is three to four times faster. A cart
that redraws the whole screen at 30 ticks a second has about 45,000 units
a tick on the iPod Video with no music, and about half that with music.
A runtime error names the file, the line and the function.

These options show what a cart drew, after the last tick:

- `--shot file.png` saves the panel as the player shows it: the 160x120
  screen doubled to 320x240.
- `--dump x,y,w,h` prints that part of the screen, one hex digit a pixel
  and a row a line.
- `--dirty` prints `dirty lo hi`: the rows the last tick drew, which are
  the rows the player sends to the screen.
- `--menu i` chooses the cart's pause-menu item `i` (from `menuitem()`), as
  SELECT would, printing the items first and what the item's function
  returned. Give it more than once to choose several in turn.

`pbgrun.js` runs a cart the same way on `wasm/arcade.wasm`, the engine
built as WebAssembly, which the IDE runs too. It takes the same options
but `--menu`, `--wav` and the music, scores and store options, and
prints the same lines, units and pixels:

```sh
sh wasm/build.sh                      # needs wasi-sdk and binaryen
node pbgrun.js mygame.pbg --ticks 100 --units
```

## Tests

```sh
node --test test/*.test.js
```

The tests build `pbgvm` first. Each program in `test/lang/` runs both in
`pbgvm` and in Node, with Node's arithmetic changed to the cart language's,
and the two must print the same lines. Random expressions are checked the
same way, and mutated carts must be refused or run, never crash or hang.
`pbgrun` must match `pbgvm` line for line, unit for unit and pixel for
pixel, so the tests need a `wasm/arcade.wasm` built from the current
sources.
