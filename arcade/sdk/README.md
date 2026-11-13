# Arcade SDK

The compiler for Arcade carts: games written in a small subset of
JavaScript and played on the iPod as `.pbg` files.

To learn to write one, read [the manual](../manual/README.md):
a tutorial that builds a first game, and a reference for every part of
the Arcade. The IDE opens it from **Help**.

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
node pbgc.js copy game newgame "New Game"  # a new game made from game (a folder or a .pbg)
node pbgc.js pack game               # the same as build
node pbgc.js html mygame             # mygame.html: the cart as a web page
```

A cart carries its own sources, so `unpack` gives back every file as it
was written. Pictures come back as indexed PNGs in the cart's first
palette, with colour 0 transparent: the same palette numbers, though not
the same file if it was saved with other colours. `pack` then makes the
same cart, byte for byte.

A game made from another needs an id of its own, or the two share saves
and high scores on the player. `copy` gives it one, and the name given;
the IDE's **Save as a new game** does the same.

A cart is a folder:

- `main.js` holds `init()`, run once, `update()`, run every tick, and
  optionally `draw()`.
- Any other `.js` file is a namespace named after the file: `hit()` in
  `brick.js` is called as `brick.hit()`.
- `cart.json` holds the settings:

| Key | Value |
|---|---|
| `id` | 16 hex digits, made by `pbgc new`. Saves and scores are kept under it: never change it by hand, but make a game from another with `pbgc copy` or **Save as a new game** |
| `name` | shown in the Games menu |
| `author`, `version`, `help` | text, optional |
| `rate` | ticks a second: 20, 25, 33 (the default) or 50 |
| `menu_button` | `pause` (the default) or `game`, to have MENU as button 2 |
| `music` | `listen` (the default), `control` or `own` |
| `suspend` | `true` (the default): Quit from the pause menu keeps the game, and the player offers to carry on next time; `false` ends it, for a game played against the music, which cannot be picked up again |
| `boards` | the high-score tables, up to 8, below |
| `options` | the settings the pause menu shows, up to 8, below |
| `achievements` | up to 32 records: `name`, used in code as `ACH.<name>`; `title`, 1 to 31 bytes (the name when left out); and `text`, up to 95 bytes |
| `lazy_images` | images left in the cart file until `img.load(IMG.name)` reads them, and `img.drop` lets go: an image needed only at times, such as one course's sky |
| `flags` | names for the sprite flags, flag 0 first: `["solid", null, "ladder"]` gives `FLAG.solid` and `FLAG.ladder`, each the flag as a set of flags (1 and 4), for `actors.move()` and `map()`; `fget(n) & FLAG.solid` tests one |

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
| `logo.png` | the cart's logo, 44x44 in its palette's colours: what a list of games shows for it, and an exported page's icon |
| `palettes/<name>.gpl` | a palette in GIMP's format; the first, by name, is loaded when the cart starts |
| `maps/<name>.json` | a map: `width` and `height` (up to 256), `rows` of two hex digits a cell, and `objects`, each with a `kind`, `x`, `y`, `w`, `h` in pixels and `props` of whole numbers or strings |
| `objects.json` | the kinds of object the maps may hold, and each kind's properties with their defaults and help: see [Kinds of object](../manual/reference-maps.md#kinds-of-object) |
| `images/<name>.png` | any other picture, up to 1024 pixels a side; after `sheet(IMG.<name>)` the sprite calls draw its 8x8 cells as sprites |
| `data/<name>.json` | data the code reads: levels, waves, dialogue |
| `data/<name>.txt` | text the code reads as one string |
| `editors/<name>.html` | an editor of the cart's own for `data/<name>.json` or `.txt`, run in the IDE: see [A cart's own editor](#a-carts-own-editor) |
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
| `menu.js` | an in-game menu on the wheel and SELECT |
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
- **`rgb(0xRRGGBB)` is a palette number, found when compiling.** It gives
  the colour nearest to the one written in the palette the cart starts
  with -- its first palette by name, or the engine's sixteen -- the first
  of any equally near: `cls(rgb(0x1a2244))` is `cls(1)`. Its argument must
  be a number known when compiling; at run time, use the colour's number.
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
- **Run as** (in the Run menu) runs the cart as an iPod Classic, an iPod
  Video, or an iPod Video playing music: each tick takes as long as it
  would on that player, so a cart doing more than the player manages
  drops frames there, and the frame rate under the screen shows it. The
  iPod Video with music playing is the slowest a cart meets.
- **The Game menu** holds what the player's pause menu holds, while the
  cart runs: its own items from `menuitem()`, its options, its high
  scores and achievements, its help, and Restart. Menu on the player's
  wheel pauses. Scores, option values and saved numbers (`dset`) are kept
  in the browser for the cart, as the player keeps them, so the next run
  starts where the last left off; `score.show()` shows the board, an
  achievement is announced in the output, and `quit()` stops the cart.
- **Watch** (View > Watch) shows each `watch(name, value)` as it changes;
  it opens by itself the first time a cart calls it.
- **The profiler** (Run > Run and profile, or View > Profiler and its
  record button) shows where the time goes on the player you run as:
  each frame's cost as a bar against the time a frame has, red where it
  runs over, and a table of the functions and API calls that took it --
  each one's own share, its share with everything it called, and its
  milliseconds a frame -- or of the lines, with sending the screen as a
  row of its own. Click a row to go to its code. It samples every 2048
  units, so something cheaper than that a frame shows only now and then:
  record a few seconds.
- **Memory** (View > Memory) shows what the cart's 512 KB holds as it
  runs: the heap held, waiting to be cleared up and free, the largest
  free block, and the bytes held by kind and by each global.
- **Replays**: every run is recorded -- its random seed, the store as it
  began, the music, and every tick's buttons and wheel -- and Run >
  Replay the last run plays it again exactly, frame for frame, whatever
  player you run it as, and then the cart carries on with your input. Run
  it again after changing the code to see the change on the same play,
  or with the profiler recording to compare; if the screen stops matching
  the recording, the output says from which frame. Save the last run as
  a replay to keep it, and Open a replay to play it again: on the same
  cart, and with the store as it was, so the browser's own is left alone.
- **Panels**: the files, the editor, the output, the player, and the open
  editor's own -- the sheet's sprites, a map's tiles and objects, a font's
  glyphs, a sound's generators -- can each be dragged by its tab beside,
  above or below another, or onto it to share its place as a tab, and are
  kept where you leave them. The View menu shows or hides each (Ctrl+B the
  files, Ctrl+Shift+B the player, Ctrl+J the output) and puts them all back
  as they started. The output opens when there is something to read. A
  file with an editor of its own -- a picture, map, font, sound, palette
  or cart.json -- switches between it and its text from the button at the
  right of its tab bar, as an image does between one picture and sprites.
  The status bar shows whether the cart builds and the frame rate it gets
  as the chosen player; click the rate to choose another.
- **The music** under the screen stands in for the owner's: a made-up
  library of five albums and a spoken chapter, its queue playing through
  the first ten tracks. `playback` and `tracks` read it and `beat`
  hears each track's steady tempo; play, pause and skip it there, and
  tick Metronome to hear the beat. `pbgvm --beat` and `pbgrun --beat` use the
  same library.
- **Pictures**: open an image to draw on it in palette colours. The
  sprite sheet opens as a picker: click a sprite, or drag across several,
  to draw on them, and set the chosen sprites' flags from the Flags list on
  the toolbar. Tile mode shows a
  picture repeated round itself, drawing wrapping at its edges, for tiles
  that join; invert turns each colour to the one nearest its opposite, and
  Replace (C) makes every pixel of the colour clicked the drawing colour, in
  the selection or the whole picture. An
  image changes size with Canvas size, which keeps the picture at the corner,
  edge or middle you choose, or with Crop to cut it to the selection; a drag
  past the edge of the view scrolls it. Choose
  several sprites in a row and press Animate to make them an animation: it
  plays under the picker at the speed you set, with the line of code that
  plays it. The onion button shows the sprites before the ones you are
  drawing -- in an animation, the frame before -- faintly over them, and
  right-clicking a sprite in the picker shows that one instead. Paste a picture
  from any program, or drop one on the editor: where its colours are not
  in the palette, choose the nearest, the nearest dithered, or to add them
  to the palette (as many as it has room for, chosen to stand for the
  rest). Copy puts the selection on the clipboard for other programs too,
  and File > Export picture saves the open one. An image can be edited as
  sprites too, with the grid button on its tab bar: it then opens as the
  sheet does, its cells numbered for `sheet(IMG.<name>)`, without flags.
- **Fonts**: open a `.bdf` to draw its characters: each in a cell the line
  tall with the baseline marked, its advance set by number; add a character
  by typing it, delete one, and change the ascent and descent. A line of
  text shows in the font as you work. It is saved as BDF, so any BDF
  editor still opens it.
- **Maps**: open one to paint it with the sheet's sprites. Choose a
  sprite, or drag across a block of them, in the picker, or from the tile
  bar under the map, which holds the ones in use (drag from the picker to
  add one, right-click to take it off). Paint, draw a line, a rectangle or
  an oval (Shift fills it), fill an area, and erase (or paint with the
  right button). Select cells to clear them or fill all of them at once. Mark
  the cells whose sprites have a flag to check the solid ones line up.
  With the Objects tool, drag on the map to place an object, drag one to
  move it or its corner to size it. Its kind is on the toolbar -- set it
  before placing, and the objects placed take it -- and its place, size
  and properties are beside the picker; objects sit on whole cells unless
  Shift is held. The map's
  size is set from the toolbar. A trace image -- a picture from disk, a
  pixel to a map pixel -- shows faintly under the map to paint over; it is
  kept in the browser for that map, never in the cart.
- **Sounds**: open one to shape it with sfxr's settings -- the wave, its
  envelope, pitch and slide, vibrato, filters and the rest -- or have a
  generator beside it make a coin, a laser, an explosion and so on, again
  for another; Mutate changes the one you have a little. Letting go of a
  slider plays it (Space plays it again; double-click a slider for its
  default), and what you hear and see is what the player stores, 8-bit at
  22,050 Hz, with its length and size.
- **Settings** (Cart > Settings, or open `cart.json`): the cart's name
  and text, its rate, what MENU and the music do for it, the sprite flags'
  names, and its high-score boards, pause-menu options and achievements,
  as a form. A name code could not use is marked as you type; the build
  checks the rest.
- **Palettes**: open one to set its colours, shown as the player shows
  them; colours no picture uses are marked, and two that look the same on
  the player are pointed out. Moving and sorting colours rewrites the
  pictures so they look the same -- but code naming a colour by number
  draws whichever colour now has that number.
- **Errors** are marked in the code as you type. A runtime error stops the
  cart, marks its line and opens the file at it.
- **Hints**: inside a call, the function's arguments show with the one
  you are at in bold, and what it does; hovering over a name shows the
  same. Your own functions show their arguments and the comment above
  them. The API's are from `api.md`, the reference to every function.
- **Files**: the File menu makes a new file of any kind a cart holds,
  adds files from disk (each goes where its kind belongs, and a picture is
  matched to the palette), and renames or deletes the one open.
- **Open** (Cart menu) takes a `.pbg`, and dropping one on the page
  opens it too. **Save as .pbg** writes the cart, and **Save as a new game**
  writes it as a game of its own, with a new id and name. In Chrome and Edge, Save
  writes back to the file you opened, and **Open folder** edits a cart
  folder in place, Save writing its files back. Where the browser will not
  write to files -- other browsers, or one whose owner has turned it off --
  use `pbgide` instead, below.
- **Export as web page** (Cart menu) saves the cart as a web page to play
  it in: see below.
- The cart open when you close the page is there when you come back.
  `store` files are kept in the browser.
- **About ArcadeIDE** (Help menu) credits the open-source projects the IDE
  is built with.

### The IDE on a cart folder: pbgide

```sh
host/pbgide mygame          # opens the IDE in the browser, on the folder mygame
```

`pbgide` serves the IDE from your own computer and opens it in your
default browser, already editing the cart folder you give it: Save writes
its files straight back there, and a file changed in the folder by another
program is taken into the IDE within a couple of seconds -- or, where you
have changed it in the IDE too, you are asked which to keep. The browser never writes a file itself, so
it works in any browser, including one set never to let pages write files.

- It answers this computer only (`127.0.0.1`), and only the page it opened
  can read or write the folder: the address carries a key made each time
  it starts. It reads and writes the cart's own files and nothing else.
- It runs until you stop it (Ctrl+C). Close the page and open the address
  it printed to come back -- each start prints a new one, and a page from an
  earlier start says so; a new IDE built with `node ide/build.js` is
  served at the next reload.
- `--port n` picks the port (8484, or the next free one, by default),
  `--no-browser` only prints the address, and `--ide file.html` serves an
  IDE page from elsewhere. Without a folder it serves the IDE alone.
- `sh host/build.sh` builds it, beside `pbgvm`.

## A cart's own editor

A cart can carry a page that edits its data -- a level editor, a phrase
table, a dialogue tree -- and the IDE runs it in place of the text when
that file is opened. `editors/levels.html` edits `data/levels.json` (or
`data/levels.txt`); **Edit as text** switches back, and opening the
`.html` itself shows its HTML, with **Run this editor**. File > New file
makes one to start from: the data shown as text, saved back by a button.

The page is ordinary HTML with its scripts inline, and reaches the cart
through `window.cart`:

| | |
|---|---|
| `cart.file` | the file it was opened for, or `null` |
| `cart.read(name)` | a Promise of a file's text, or a `Uint8Array` for a picture; `undefined` when there is none |
| `cart.write(name, value)` | a Promise, kept once the file is written: any file a cart holds, but never an editor |
| `cart.list()` | a Promise of every file's name |
| `cart.run(files, ticks)` | a Promise of `{ lines, error }`: the cart built and run unseen, for up to `ticks` ticks or until it prints `done`, with every line it printed. Its code is the `.js` files named in `files` and no others -- each the text to build with, or `true` for the cart's own -- and any other file named is used in place of the cart's |

```js
cart.read(cart.file).then(function (text) { show(JSON.parse(text)); });
// ... and when the owner changes something:
cart.write(cart.file, JSON.stringify(levels, null, 1));
```

- The editor travels inside the `.pbg` with the cart's sources, so anyone
  who opens the cart has it. The player never runs it.
- It runs walled off: it cannot reach the network, the IDE's storage or
  the IDE's page, only the cart's files through `cart`. Fetch nothing:
  put fonts and pictures in the page as `data:` URLs.
- The first time a cart's editor would run, the IDE asks. A page changed
  since asks again, except one you changed in the IDE yourself.
- The cart reads what it writes with `data.read(DATA.levels)`, as any
  data file.
- **Checking data by the cart's own rules**: put the check in a code file
  of the cart's that the game never calls -- it is left out of the game's
  build -- and have the editor run it with `cart.run`, a `main.js` of its
  own and the data as the editor has it, saved or not:

```js
cart.run({ "main.js": "function init() { check.all(data.read(DATA.levels)); }\n" +
                      "function update() {}\n",
           "rules.js": true, "check.js": true, "data/levels.json": text }, 10)
    .then(function (r) { show(r.error || r.lines.join("\n")); });
```

  A run nobody watches may take far longer in one call than a frame
  could, so a check can work through all the data in `init()`.

## A cart as a web page

```sh
node pbgc.js html mygame --wheel touch --scale fit   # writes mygame.html
```

`pbgc html` (or **Export as web page** in the IDE) writes one HTML file
that plays the cart in a browser, from a website or opened from disk: the
engine, the cart and its runner all inside it, about 300 KB with a small
cart. It runs the cart as an iPod Classic would, each tick taking what it
takes there, so a cart too slow for the player drops frames here too.
Scores, option values, saved numbers and `store` files are kept in the
browser. Click the screen, or press Enter, to start; Esc or Menu opens the
pause menu: the cart's menu items, its options, high scores and help, and
Restart. A cart that takes Menu as a button pauses when it is held a
second, as on the player.

| Option | Values |
|---|---|
| `--wheel` | where the click wheel is drawn under the screen: `touch` (the default) on touch screens only, `always`, or `never`. The keys are as in the IDE either way |
| `--scale` | `fit` (the default) fills the window; `whole` fills it in whole pixels; `1` to `4` a fixed size, 160 by 120 times that, or smaller where the window is |
| `--font` | `font.bin` (the file `bundle-font.sh` makes), for Japanese, Chinese and Korean text; without it they show as `?`, and `pbgc` warns when the cart has them |
| `-o` | the file to write: the cart's name with `.html` by default |

A cart that uses the owner's music -- `playback`, `beat` or `tracks` --
cannot be exported: a web page has no music library to play from. The
page links to the engine's source and licences, and carries in full the
licences of the text font (`fonts/LICENSE-world.txt`) and of the C
library routines the engine links (`wasm/LICENSE-libc.txt`); leave both
in when you share it.

## Running a cart's script

`host/pbgvm` is the player's engine built for this computer. It runs a
cart without a screen, printing what `printh()` prints:

```sh
sh host/build.sh                      # needs gcc (MinGW on Windows)
host/pbgvm mygame.pbg --ticks 100 --units
```

`--units` reports the cost of `init()` and of each tick. A unit is about
0.55 µs on an iPod Video with no music playing, and about 1.9 µs playing
FLAC; the iPod Classic is three to four times faster. A cart that redraws
the whole screen at 33 ticks a second has about 12,500 units a tick on an
iPod Video playing FLAC: see the manual's *Making it fast*.
A runtime error names the file, the line and the function.

These options show what a cart drew, after the last tick:

- `--shot file.png` saves the panel as the player shows it: the 160x120
  screen doubled to 320x240.
- `--dump x,y,w,h` prints that part of the screen, one hex digit a pixel
  and a row a line.
- `--dirty` prints `dirty lo hi`: the rows drawn since the last tick that
  drew began, a late tick drawing nothing, which are the rows the player
  sends to the screen next.
- `--memory` prints what the memory view shows after the last tick: the
  totals, each kind's count and bytes, and each global's bytes by its
  number in declaration order.
- `--menu i` chooses the cart's pause-menu item `i` (from `menuitem()`), as
  SELECT would, printing the items first and what the item's function
  returned. Give it more than once to choose several in turn.

And these drive or keep more of a run (the comment at the top of
`host/pbgvm.c` lists every option):

- `--buttons 30:16,31:0` holds button masks from those ticks (1 Prev,
  2 Next, 4 Menu, 8 Play, 16 Select); `--wheel 5:12:30` turns the wheel
  12 clicks at tick 5 with the finger at 30; `--skip 10:1` makes tick 10
  late, run without drawing.
- `--saves file`, `--scores file`, `--options file` and `--store dir`
  keep saved numbers, scores, option values and stored files between
  runs; `--opt t:i:v` sets option `i` before tick `t`.
- `--ui answers` answers dialogs; `--playing` and `--beat bpm` play
  the made-up music, `--beat 0` with each track at its own tempo, as the
  IDE plays it.
- `watch()` prints `watch name value` as a value changes; with
  `--units`, `prof()` sections print their units. `quit()` ends the run.

`pbgrun.js` runs a cart the same way on `wasm/arcade.wasm`, the engine
built as WebAssembly, which the IDE runs too, and prints the same lines,
units and pixels -- `watch` and `prof` lines among them. It takes
`--ticks`, `--seed`, `--ceiling`, `--units`, `--shot`, `--dump`,
`--dirty`, `--buttons`, `--wheel`, `--skip`, `--setting`, `--speech`,
`--font`, `--playing`, `--beat` and `--store`, and refuses any other. So
it has no `--menu`, `--wav`, `--ui`, `--scores`, `--saves`, `--options`,
`--opt` or `--suspend-at`: its dialogs are worked from `--buttons` and
`--wheel`, only the store is kept between runs, and it prints no `show`
or `achieved` lines for `score.show()` and achievements:

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
