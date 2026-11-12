# cart.json

Every setting a cart's `cart.json` can hold, in one place. The IDE's
**Cart > Settings** writes the file; it is plain JSON, and a key it does
not know is an error when the cart is built.

- [The settings](#the-settings)
- [Keeping a game to carry on](#keeping-a-game-to-carry-on)

## The settings

| Key | Value |
|---|---|
| `id` | 16 hex digits, made when the cart is made. Saves and scores are kept under it: never change it by hand; **Save as a new game** makes another game from this one |
| `name` | required: shown on the Arcade shelf |
| `author`, `version` | text, optional: the cart's own record of them |
| `help` | text, optional: the pause menu's **Help** ([Scores, options and the pause menu](reference-scores.md#the-pause-menu)) |
| `rate` | ticks a second: 20, 25, 33 (the default) or 50 ([Time and speed](reference-time.md)) |
| `menu_button` | `"pause"` (the default), or `"game"` to have Menu as a button of the game's own ([Input](reference-input.md)) |
| `music` | `"listen"` (the default), `"control"` or `"own"`: how much of the owner's music it may use ([The owner's music](reference-music.md#how-it-works)) |
| `suspend` | `true` (the default) or `false`: whether Quit keeps the game to carry on, below |
| `boards` | up to 8 high-score boards ([Declaring boards](reference-scores.md#declaring-boards)) |
| `options` | up to 8 settings for the pause menu ([Declaring options](reference-scores.md#declaring-options)) |
| `achievements` | up to 32 ([Declaring achievements](reference-scores.md#declaring-achievements)) |
| `lazy_images` | images left in the cart file until `img.load` reads them: `["sky_desert", "sky_snow"]` ([Lazy images](reference-images.md#lazy-images)) |
| `flags` | names for the sprite flags, flag 0 first: `["solid", null, "ladder"]` gives `FLAG.solid` and `FLAG.ladder` ([Maps](reference-maps.md#walls-floors-and-ladders-flags)) |

```json
{
    "id": "3f9a0c1d2e4b5a67",
    "name": "Cave Run",
    "author": "Me",
    "rate": 25,
    "flags": ["solid", "water", "ladder"],
    "boards": [{ "name": "points", "title": "Points" }]
}
```

## Keeping a game to carry on

When the owner quits -- **Quit** in the pause menu, or **Yes** to the
question a tap of Menu asks -- the iPod keeps the whole game as it
stands: every variable, array, image and pool, after
[`ending()`](reference-language.md#a-carts-files) has run. The next time the cart
starts it asks, over the game, whether to carry on; either answer
discards the kept game, so it is picked up at most once.

- The cart's own [`quit()`](reference-scores.md#quit) keeps nothing: the
  next start is a fresh one.
- The music is not part of it. A cart that times a round against a track
  sees [`resumed()`](reference-time.md#resumed) true on its first tick
  back, the same as after a pause.
- A cart with `"music": "own"` is never kept, and neither is one with
  `"suspend": false` -- the setting for any game that cannot be picked
  up again later.
- A kept game belongs to the PodBox it was made under: after an update,
  or a new build of the cart, it is not offered. **Delete Saved Game** in
  the cart's menu on the shelf discards one by hand.
- In the IDE and `pbgvm`, Quit ends the run; nothing is kept.
