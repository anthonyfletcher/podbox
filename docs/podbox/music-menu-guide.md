# Your own Music menus

Everything under **Music** is described by a text file, and you can add your
own views to it: a menu of one genre, an artist's live albums kept apart from
the studio ones, a list of every album from the 1990s. The views you add appear
under `Music > Custom menu`.

---

## Getting started

Create a plain text file called `tagnavi_custom.config` in the `.rockbox`
folder of the player, and put this in it:

```
#! rockbox/tagbrowser/2.0

%menu_start "custom" "Custom menu"
"Jazz" -> albumartist ? genre == "Jazz" -> album -> title = "fmt_title"
```

Restart the player. `Music > Custom menu` now appears near the end of the
Music menu, holding one row, **Jazz**, which lists the artists with jazz albums, then
their albums, then the tracks.

Three things stop a file from working, and none of them shows an error:

- **The first line must be `#! rockbox/tagbrowser/2.0`**, exactly. Without it
  the whole file is ignored and Custom menu does not appear.
- **A blank line ends a menu.** Rows after it are ignored. Comment lines,
  starting with `#`, are fine anywhere.
- **A row with a mistake in it is left out.** If a row is missing from the
  menu, check its quotes and spelling against the examples here.

The file is read when the player starts, so restart it after each edit.

---

## How a row reads

```
"Jazz" -> albumartist ? genre == "Jazz" -> album -> title = "fmt_title"
```

| Part | Means |
|---|---|
| `"Jazz"` | The row's name in the menu |
| `-> albumartist` | Choosing it lists album artists... |
| `? genre == "Jazz"` | ...counting only the tracks whose genre is Jazz |
| `-> album` | Choosing an artist lists their albums |
| `-> title = "fmt_title"` | Choosing an album lists its tracks, in disc and track order |

Each `->` is one list you step into. End with `-> title = "fmt_title"` and the
tracks show the way they do everywhere else in Music.

A condition after `?` applies to that list and every list below it, so here
the albums and tracks are only the jazz ones too.

### Tags

| Tag | Holds |
|---|---|
| `album` | Album |
| `albumartist` | Album artist, or the track artist where a track has none |
| `artist` | Track artist |
| `canonicalartist` | Track artist, or the album artist where a track has none. The Artist row uses this |
| `genre` | Genre |
| `year` | Year |
| `composer` | Composer |
| `grouping` | Grouping (iTunes's Grouping field) |
| `comment` | Comment |
| `title` | Track title |
| `playcount` | Times played |
| `rating` | Your rating |

### Conditions

| Write | Keeps tracks whose tag... |
|---|---|
| `genre == "Jazz"` | is Jazz |
| `genre != "Jazz"` | is not Jazz |
| `genre ~ "Jazz"` | contains Jazz, so Acid Jazz and Jazz Fusion as well |
| `genre !~ "Christmas"` | does not contain Christmas |
| `album ^ "Live"` | begins with Live |
| `year >= "1990"` | is 1990 or later. Also `>`, `<` and `<=` |

Capitals make no difference. Join conditions with `&` to need both, or `|`
to need either:

```
"1990s" -> albumartist ? year >= "1990" & year <= "1999" -> album -> title = "fmt_title"
```

A condition with nothing between the quotes asks you to type a value each
time you open the row:

```
"Comment contains..." -> album ? comment ~ "" -> title = "fmt_title"
```

---

## Header rows: groupings above the albums

Mark a level `%header_all` or `%header_untagged` and it stops being a list of
its own: its values become rows at the top of the list below it.

```
"Album Artist (groupings)" -> albumartist -> grouping %header_all -> album -> title = "fmt_title"
```

Open an artist and their groupings, such as **Live** or **Remixes**, sit above
their albums. Choosing a grouping lists only the albums in it.

The two differ in what comes below the header rows:

| Mark | Lists below the header rows |
|---|---|
| `%header_all` | Every album |
| `%header_untagged` | Only the albums with no grouping, so each grouped album is listed once, under its grouping |

It works with any tag, not only grouping. This puts an artist's genres above
the albums that have no genre:

```
"Album Artist (genres)" -> albumartist -> genre %header_untagged -> album -> title = "fmt_title"
```

- **The list opens on the first album.** Scroll up for the header rows, which
  sit with `[All Tracks]` and `[Random]`.
- **A header needs two more levels below it**, as `-> album -> title` above.
  A row that ends straight after the header's level is left out.
- **Up to 32 header rows show.** An artist with more groupings than that shows
  32 of them, and the rest cannot be reached from that artist's list. With
  `%header_all` their albums are still listed below the header rows.
- **An album where only some tracks have a grouping** is listed under that
  grouping and, with `%header_untagged`, below the header rows as well.

---

## Common edits

**One genre, by composer.** Change the genre after `==` to use another:

```
"Classical" -> composer ? genre == "Classical" -> album -> title = "fmt_title"
```

**Everything but one genre:**

```
"Albums without Christmas" -> album ? genre !~ "Christmas" -> title = "fmt_title"
```

**Genres straight to albums**, skipping the artist list:

```
"Genre (albums)" -> genre -> album -> title = "fmt_title"
```

**Albums with a track played more than five times:**

```
"Played often" -> albumartist ? playcount > "5" -> album -> title = "fmt_title"
```

**A menu of menus.** `==>` opens another menu, which you start with its own
`%menu_start` after a blank line:

```
#! rockbox/tagbrowser/2.0

%menu_start "custom" "Custom menu"
"Decades" ==> "decades"
"Album Artist (groupings)" -> albumartist -> grouping %header_untagged -> album -> title = "fmt_title"

%menu_start "decades" "Decades"
"1970s" -> albumartist ? year >= "1970" & year <= "1979" -> album -> title = "fmt_title"
"1980s" -> albumartist ? year >= "1980" & year <= "1989" -> album -> title = "fmt_title"
"1990s" -> albumartist ? year >= "1990" & year <= "1999" -> album -> title = "fmt_title"
```

**Built-in rows.** Some of Music's rows are not described by conditions and
are written by name instead. Any of them can go in your menus:

| Row | Write |
|---|---|
| Search | `"Search" ==> "db_search"` |
| Random Album | `"Random Album" ==> "random_album"` |
| Featured Artists | `"Featured Artists" ==> "featured_artists"` |
| Most Played Albums | `"Most Played Albums" ==> "chart_albums_most_played"` |
| Most Played Artists | `"Most Played Artists" ==> "chart_artists_most_played"` |
| Recently Played Albums | `"Recently Played Albums" ==> "chart_albums_recent"` |
| Recently Played Artists | `"Recently Played Artists" ==> "chart_artists_recent"` |
| Forgotten Albums | `"Forgotten Albums" ==> "chart_albums_forgotten"` |
| Forgotten Artists | `"Forgotten Artists" ==> "chart_artists_forgotten"` |

These show their own translated name whatever you write before `==>`.

---

## Putting a custom row on the main menu

`Settings > Appearance > Edit Main Menu` lists the rows of Custom menu after
the Music menu's own, and any of them can be switched on. Choosing one opens
that view straight away, and **BACK** returns to the main menu.

- **The main menu has room for 20 rows from Music, and the Music menu uses
  most of them**, so only the first few rows of Custom menu are offered: 5
  with the Music menu as it ships.
- **A row on the main menu is remembered by its place in Custom menu.** Move
  or delete a row above it in the file and the main menu opens whichever row
  now sits there. Add new rows at the end to leave it alone.

---

## Changing the Music menu itself

Custom menu is the place for new views. To change one of Music's own rows,
such as adding groupings to **Album Artist**, copy `.rockbox/tagnavi.config`
to `.rockbox/tagnavi_user.config` and edit the copy. The player then reads
your copy instead of its own, so the original stays untouched and an update
does not overwrite your changes.

Two things to know first:

- **Your copy does not change with updates.** A new row in a later version of
  PodBox does not appear until you copy the new `tagnavi.config` and make your
  edits again.
- **Edit the Music menu's rows in place; do not add or remove them.**
  `Settings > Library > Music > Edit Music Menu` and the main menu remember
  rows by their position, so adding or removing one moves your choices onto
  different rows. Put new rows in Custom menu instead.

To hide a row or change the order, use `Edit Music Menu` rather than either
file.

Row names are shown as you write them. Only the names the player already uses
appear translated in other languages.
