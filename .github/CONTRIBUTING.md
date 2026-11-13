# Contributing to PodBox

PodBox runs on two players: the **iPod Video** (5G and 5.5G, `ipodvideo`)
and the **iPod Classic** (6G and 7G, `ipod6g`). They have different
processors, USB controllers and audio codecs, and every owner of either
expects the same firmware.

## What is accepted

1. **It works on both players.** A change designed for only one of them is
   declined, however good it is on that one, so design everything around
   the most limited of them, the iPod Video with 32 MB.
   Build both (`./build-hw.sh` and `./build-hw.sh 5g`), and test on both, or
   say plainly in the pull request which one you could not test.
   The one exception is hardware only one player has, such as the 5G's TV
   output; say so in the pull request.
2. **It is based on current `master`** and merges without conflicts.
3. **One feature a pull request.** Independent changes go in separate pull
   requests, so each can be taken on its own.
4. **It's not niche.** The change benefits the many, not the one person who
   likes things a particular way.

## How it is written

- **The application layer is `apps-ipod/`**; read `apps-ipod/README.md`
  before adding a file there. Outside it the tree follows upstream Rockbox,
  so put a feature in a file of its own and call it from upstream's code
  with a small hook, rather than editing upstream's functions. List every
  file you change outside `apps-ipod/` in `docs/podbox/upstream-divergence.md`.
- **Text on screen** goes in `apps-ipod/lang/english.lang`, never as a
  string in the code. A new setting takes the five places `CLAUDE.md`
  lists, and `sh tools/check-settings-docs.sh` must print nothing.
- **Code style** is in `CLAUDE.md`: C (gnu11), 4 spaces, 80 columns,
  `/* */` comments that say what the code does rather than how it came to
  be.
- **Tests** that ship go in a folder of their own under `tools/`, with one
  script that runs them, as `tools/bitperfect/` does.

## In the pull request

Say what it changes for an owner, how you tested it on each player, and
what you could not test.
