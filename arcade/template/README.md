# My Game

A game for the Arcade on an iPod running
[PodBox](https://github.com/anthonyfletcher/podbox).

## Making it

1. Open the [Arcade IDE](https://anthonyfletcher.github.io/podbox/ide/) in
   Chrome or Edge, choose **Cart > Open folder**, and pick this
   repository's `mygame` folder. Edit, run and test it there; the IDE saves
   back into the folder.
2. Commit and push. GitHub builds the game: open the **Actions** tab, the
   latest run, and download the game under **Artifacts**: it comes as a
   zip holding the `.pbg`. A mistake in the code shows there with its file
   and line.
3. Copy the `.pbg` into the iPod's `Games` folder, and choose **Arcade** on
   its main menu.

The first build gives the game an id of its own (its saves and scores are
kept under it) and commits that change, so pull before your next push.

To publish a version, push a tag -- `git tag v1 && git push --tags` -- and
the `.pbg` is attached to a release of that name.

Each push to the main branch also puts the game on the web, playable in a
browser with the keys or a phone's touch wheel: turn on **Settings > Pages**
with the `gh-pages` branch as its source, once, and it is at
`https://<you>.github.io/<this repository>/`, 480 by 360 pixels, or smaller
in a smaller window. `SCALE` in `.github/workflows/build.yml` sets the size:
1 to 4 times 160 by 120, or `fit` to fill the window. GitHub serves Pages
free for a public repository; a private one needs a paid plan. A game that plays the
owner's music cannot be a web page: set `PAGE` to `false` in
`.github/workflows/build.yml` for one.

To rename the game, change `name` in `mygame/cart.json`. To rename the
folder, change `CART` in `.github/workflows/build.yml` to match.

## Learning

- [The manual](https://github.com/anthonyfletcher/podbox/blob/master/arcade/manual/README.md):
  a tutorial that builds a whole game, then a chapter for each part of the
  Arcade.
- [Every function](https://github.com/anthonyfletcher/podbox/blob/master/arcade/sdk/api.md),
  one line each.

With [Claude Code](https://claude.com/claude-code), the Arcade skill in
`.claude/skills/podbox-arcade` is loaded when you ask for help with the
game.

## Licence

The template is under the 0BSD licence (`LICENSE`): your game is yours,
under whatever licence you choose. The skill in `.claude/skills` is text
from the PodBox manual, under the GPL version 2.
