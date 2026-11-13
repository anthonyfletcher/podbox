# Build a PodBox Arcade cart

A GitHub Action that compiles a [PodBox](https://github.com/anthonyfletcher/podbox)
Arcade cart -- a folder with `cart.json` and `main.js` -- into the `.pbg`
file an iPod running PodBox plays from its Arcade.

```yaml
- uses: actions/checkout@v4
- uses: anthonyfletcher/podbox-arcade-builder@latest
  id: cart
  with:
    cart: mygame            # the folder holding cart.json
- uses: actions/upload-artifact@v4
  with:
    name: cart
    path: ${{ steps.cart.outputs.pbg }}
```

| Input | |
|---|---|
| `cart` | the cart's folder; the repository's top when left out |
| `output` | where to write the `.pbg`; the folder's name with `.pbg` when left out |
| `page` | `true` to write the cart as a web page too, beside the `.pbg` with `.html`: the click wheel on a touch screen, the keys on a computer. A cart that plays the owner's music cannot be one, and fails the step |
| `scale` | the web page's size: `3` (the default) draws the game at 480 by 360, `1` to `4` that many times 160 by 120, each smaller where the window is; `fit` fills the window, and `whole` fills it in whole pixels |

| Output | |
|---|---|
| `pbg` | the `.pbg` written |
| `page` | the web page written, when `page` is `true` |

A mistake in the cart fails the step, with the file, line and message, as
the IDE shows it.

**`@latest` builds for the latest PodBox release**: it moves with each
release, so a cart built with it plays on the newest firmware. To keep a
build fixed, name one of this repository's commits instead.

To start a game of your own, use the
[podbox-game](https://github.com/anthonyfletcher/podbox-game) template,
which builds with this action on every push.

This repository is made by PodBox's release from its `arcade/builder`
folder: change it there, not here. `pbgc.mjs` is the Arcade SDK's
compiler as one file, under the GPL version 2 (see `LICENSE`); the code it
carries from other projects is listed with their licences in
`THIRD-PARTY.txt`.
