# PodBox Theme IDE

Makes themes for PodBox: the skin files in an editor, beside PodBox itself
running them. The player in it is the iPod Video firmware built for the
browser, so every screen it shows -- menus, lists, Quick Settings, Now
Playing -- is drawn by the code that draws it on an iPod.

## Opening a theme

```sh
python serve.py path/to/theme        # the folder holding .rockbox
python serve.py                      # or find one from the current directory
```

On Windows, double-click `ThemeIDE.cmd`, or drag a theme folder onto it.
It opens in a window of its own and closing that window stops the server.
With [pywebview] installed (`pip install pywebview`) the window is a native
one; `--tab` gives an ordinary browser tab and `--no-browser` only prints the
address.

Served this way, the IDE edits the folder in place: **Save** (Ctrl+S) writes
the file, and a change made in another editor arrives in the IDE and on the
player by itself. The server answers only on this computer and only for
that folder.

The page also works without the server -- **File › Open theme folder** --
and in Chrome or Edge it can then save too. Other browsers can only read the
folder. Each release publishes it this way at
<https://anthonyfletcher.github.io/podbox/theme-ide/>, nothing to install.

[pywebview]: https://pywebview.flowrl.com/

## The panels

Every panel can be dragged beside, above or below another, or onto it as a
tab; **View** shows and hides them, and the arrangement is kept.

- **Files** lists the theme by what each file is: its screens (**Now
  Playing** is the `.wps`, **Menus and status bar** the `.sbs`), its theme
  settings and Theme Options, images and fonts. A file with problems says how
  many.
- **Editor**: hover over a tag to have it and each of its arguments
  explained; type `%` for every tag with what it does. Colours show as a
  swatch beside them. Problems are underlined where they are.
- **Player** is PodBox, with the theme on it and a library of invented music
  to play. Open the Now Playing skin and it goes to Now Playing; open the menus
  skin and it goes to the main menu. **Go to** reaches the other screens.
  Click the screen and the keyboard drives it; the wheel under it works as an
  iPod's does.
- **Outline** names each viewport of the open screen by what it shows
  (*Album art*, *Progress bar*, *Title · Artist*) and says which are showing
  on the player now. With **Outlines** on, they are drawn over the player as
  well. Pick one in either place.
- **Inspector** edits the picked viewport: where it sits, its size, its font
  and its colours. Drag it on the player to move it.
- **State** sets what the player is doing: playing or paused, which album,
  shuffle, repeat and volume, so a theme's icons and bars can be seen in each
  state. A theme with Theme Options lists them here, to try each choice.
- **Problems** lists what would go wrong on an iPod, in every screen of the
  theme; click one to go to it.
- **Tag reference** finds a tag by its name or by what it does.
- **Output** is what the firmware prints.

## The player follows the editor

With **Player › Update as you type** on, the player loads the theme again a
moment after each change, saved or not; off, it does so when you save, or
when you choose **Show unsaved changes** (Ctrl+Enter). Each reload keeps the
screen and the track: change Now Playing while a track plays and it plays on
in the new skin.

Each time the page opens, the player builds its music library first, which
takes a few seconds: Now Playing and the State panel's albums wait for it, and
the status bar says so.

The clock reads 9:00 and the battery is full. A font the theme names but does
not carry comes from PodBox's own set, as on an iPod with the full install.

## Keys

| key | does |
| --- | --- |
| Ctrl+S | save the file |
| Ctrl+Enter | show unsaved changes on the player |
| Ctrl+O | open a theme folder |
| Ctrl+F, Ctrl+G | find and replace; go to a line |
| Ctrl+/ | comment a line out, or back in |
| Ctrl+B, Ctrl+J | show or hide Files; Output |

On the player, with its screen clicked: Up and Down turn the wheel, Left and
Right are ◀◀ and ▶▶, Enter is Select, Space is ▶❚❚, Esc is Menu. Hold Esc for
Quick Settings.

## Building it

The page and the player are built on the build server:

```sh
tools/theme-ide/build.sh           # both
tools/theme-ide/build.sh page      # dist/, with Node
tools/theme-ide/build.sh player    # player/, with Emscripten (emsdk in ~/emsdk)
```

`player/` is about 10 MB to load: the firmware, its disk without fonts, and
the demo library's covers. The fonts a theme names are fetched as it is
opened.

`ctl.py` drives a running IDE from a script -- open a file, press a button,
go to a screen, take a picture of the player:

```sh
python ctl.py open scrim.wps
python ctl.py goto wps
python ctl.py shot now-playing.png
```

`python ctl.py --help` lists the rest.
