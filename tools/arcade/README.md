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
```

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

Errors name the file, line and column, and say what to write instead.

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

## Tests

```sh
node --test test/*.test.js
```

The tests build `pbgvm` first. Each program in `test/lang/` runs both in
`pbgvm` and in Node, with Node's arithmetic changed to the cart language's,
and the two must print the same lines. Random expressions are checked the
same way, and mutated carts must be refused or run, never crash or hang.
