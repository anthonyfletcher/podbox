# Scrim and Scrim_p

## Details
- Original theme for PodBox.
- Created by: Anthony Fletcher
- License: CC BY-SA 3.0 (https://creativecommons.org/licenses/by-sa/3.0/deed.en)

## Fonts

Scrim ships with the firmware, so its fonts land in `/.rockbox/fonts/` on the
player, each with its full licence text beside it.

- Material Design Icons (`24x24-icons`)
  - Created by Google (https://fonts.google.com/icons)
  - Apache License Version 2.0 — `LICENSE-Material-Design-Icons.txt`
- Noto Sans and Noto Serif (`12`/`18`/`22`/`26-noto-sans*`, `22-noto-serif`)
  - Copyright 2022 The Noto Project Authors (https://github.com/notofonts/latin-greek-cyrillic)
  - Noto Sans built from MicroNotoSans - a fork of Noto Sans by Evan Kenny aka [Dook](https://d00k.net/)
  - Copyright 2026 Micro Noto Sans Authors (https://github.com/D0-0K/MicroNotoSans)
  - SIL Open Font License, Version 1.1 — `LICENSE-Noto.txt`
- Seven Fifteen (`24-seven-fifteen`)
  - Copyright Douglas Vautour (https://burpyfresh.itch.io/seven-fifteen-font)
  - Bundled with UnifontEX, a fork of GNU Unifont maintained by stgiga
    (https://github.com/stgiga/UnifontEX), which draws the scripts Seven
    Fifteen does not cover
  - CC BY-SA 4.0; UnifontEX under the GPL v2 or later with the GNU font
    embedding exception, or the SIL Open Font License 1.1 —
    `LICENSE-Seven-Fifteen.txt`

## Screenshots

<img src="ss_2.png"/>
<img src="ss_3.png"/>
<img src="ss_1.png"/>

## Installation

Scrim is shipped with PodBox.  Switch between `scrim` and `scrim_p` via `Settings > Appearance > Load Theme`.

## Preferences

The line under the track title can show the artist, the album, both, or the
playlist name.  Edit `sub-line mode:` in `/.rockbox/themes/scrim.preferences`
to `artist`, `album`, `artist-album`, `album-artist` or `playlist_name`, then
load the theme again to pick the change up.  Both `scrim` and `scrim_p` read it.

`scrim` can also clear the track title, sub-line, progress bar, times and
status icons away once the player has been left alone.  Set `auto-hide:` to
`5`, `10` or `30` seconds, or `off`; a button press or wheel turn brings them
back.  `scrim_p` ignores it.

The play/pause icon shows what is happening by default.  Set `play-pause icon:`
to `next` to have it show what pressing Play will do instead -- pause while a
track plays, play while it is paused.  `scrim_p` has no play/pause icon; it
fades the cover and draws large pause bars over it while paused.

`scrim`'s spectrum analyser has falling peak caps.  Set `spectrum peaks:` to
`off` to remove them.
