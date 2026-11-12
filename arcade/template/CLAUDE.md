# CLAUDE.md

This repository is one game for the Arcade of PodBox, the firmware for
iPod Video and iPod Classic. The game is the cart folder `mygame/` (or
whatever `CART` in `.github/workflows/build.yml` names): `cart.json`,
`main.js` and the cart's other files. GitHub builds it into a `.pbg` on
every push, with the PodBox build action.

**Load the `podbox-arcade` skill before writing or changing the game.** It
is in `.claude/skills/podbox-arcade/` and covers the cart language, the
API, what things cost on the iPod, and how to build, run and check a cart.

The skill's commands run in the SDK. It is not in this repository; fetch
it, with the manual and the engine its tools build from, into `.podbox/`
(git-ignored) the first time:

```sh
git clone --depth 1 --filter=blob:none --sparse \
    https://github.com/anthonyfletcher/podbox.git .podbox
git -C .podbox sparse-checkout set arcade apps-ipod/games/arcade
(cd .podbox/arcade/sdk && npm ci --omit=dev)
```

and `git -C .podbox pull` before a session, so it matches the firmware's
latest release. Then, from `.podbox/arcade/sdk`:

```sh
node pbgc.js build ../../../mygame -o ../../../mygame.pbg
sh host/build.sh                  # once: pbgvm, needs gcc
host/pbgvm ../../../mygame.pbg --ticks 300 --units --shot out.png
```

Never change a cart's `id` in `cart.json`: its saves and scores are kept
under it. The first build gives a game made from the template its own.
