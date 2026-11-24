# fetch_pb notes

Writes liner notes and biographies for your music into the iPod's `/Notes` folder, as pages
for PodBox's Liner Notes and Biographies: each album's story and credits, and each artist's
biography, from [Wikipedia](https://www.wikipedia.org) and
[MusicBrainz](https://musicbrainz.org).

```bash
.\fetch_pb notes E:\ --contact you@example.com    # the first time
.\fetch_pb notes E:\                              # after that
.\fetch_pb notes E:\ --depth 1                    # producers, songwriters and band members too
.\fetch_pb notes E:\ --tracks                     # and a page about each album's tracks
```

On a Mac, `./fetch_pb notes /Volumes/IPOD`. From source, `python notes_fetch.py` takes
the same options.

It looks everything up first and writes the pages at the end, so that each page can link to
the others. What it has looked up is kept as it goes, so a run you stop loses nothing: the next
one picks up where it was.

Wikipedia asks tools like this one for a way to reach whoever runs them, so the first run needs
`--contact`. It is remembered after that, and shared with the other tools.

Turn the pages on with `Settings > Library > Music > Liner Notes and Biographies > Enabled`.
New pages show after `Settings > Library > Maintenance > Rebuild File Index`, or the next time
the iPod is connected to a computer.

## What it writes

- **An album** gets `/Notes/Albums/<Artist> - <Album>.md`: the opening of its Wikipedia
  article and its Background, Recording or Production section, then its personnel, one line
  a person, with the tracks they are on where that is not the whole album. The player fills
  in the track list and the pages that mention the album.
- **An artist** gets `/Notes/People/<Artist>.md`: when and where they were born or formed,
  the opening of their article, and a band's members. The player fills in their albums and
  the pages that mention them.
- With `--depth 1`, **the key people** the albums credit get pages too: producers,
  songwriters, band members, featured artists, conductors and orchestras credited on a whole
  album or at least two of its tracks. Engineers, arrangers and session players get a page when
  three or more of your albums credit them. Everyone else credited is still a link, and the
  player lists the pages that name them.

- With `--tracks`, **each album's tracks** get a page of their own,
  `/Notes/Albums/<Artist> - <Album> - Tracks.md`, which the album's page links to and lists
  in its menu. A track with its own Wikipedia article gets a section with the article's
  opening, and a track with credits of its own lists them. Finding the songs' articles takes
  one more request a track, about doubling the run.

Each page stays short: about 8 KB of an album's article, 5 KB of a person's. Where the
article goes on, the page says so. Reception, charts, awards and track listings are left out.

Every page ends with where its text came from, as Wikipedia's licence asks. The pages are
for your own player. A page you pass on to others has to keep that line.

## Your own pages are left alone

A page you wrote, or one of its pages you have edited since, is never changed, and
`notes.md` or `bio.md` beside the music counts as yours. Run it again and it writes only
pages that are new. `--refresh` rewrites its own unedited pages too.

## Options

| Option | |
| --- | --- |
| `--depth 1` | Pages for the key people the albums credit, as above |
| `--tracks` | A page of each album's tracks, as above |
| `--save-templates` | Put the built-in layouts in `.rockbox\fetch\templates` on the iPod, to edit (below) |
| `--only "Artist"` | Only that artist's albums |
| `--lang ja,en` | Wikipedia's languages, in order. A page is written in the first that has an article |
| `--templates DIR` | Take the layouts from DIR instead (below) |
| `--max-people-per-album 8` | At most this many key people from one album |
| `--max-pages 1500` | Stop after this many pages |
| `--compilations` | Include albums by Various Artists |
| `--auto-accept 0.85` | How closely MusicBrainz's album must match yours to be used (0-1) |
| `--dry-run` | Say what would be written, and write nothing |
| `--refresh` | Rewrite its own pages that you have not edited |
| `--undo` | Delete the pages it wrote that you have not edited |
| `--notes DIR` | The Notes folder to write into, when the library is a folder on your computer |

## Layouts

The pages are made from three templates: `Album.md`, `Person.md` and `Tracks.md`. To change
them:

```bash
.\fetch_pb notes E:\ --save-templates
```

puts the built-in ones in `.rockbox\fetch\templates` on the iPod. Edit them there, and every
run after that uses them, and says so when it starts. A template you delete goes back to the
built-in one. `--refresh` rewrites the pages already written by the new layout. To keep
templates somewhere else, give that folder as `--templates` both times.

The built-in album layout:

```markdown
<<lead>>

<<section:Background|Recording|Production>>

<<personnel>>

## Tracks
{{tracks|group=disc}}

<<tracks_page>>

## Mentioned In
{{mentions}}

<<source>>
```

The person layout:

```markdown
<<facts>>

<<lead>>

<<members>>

## Albums
{{albums|guest=true}}

## Mentioned In
{{mentions}}

<<source>>
```

And the tracks page. The front matter makes it one of the album's pages:

```markdown
---
album: <<album>>
---
<<track_sections>>
```

| Placeholder | Becomes |
| --- | --- |
| `<<lead>>` | The opening of the article |
| `<<section:Background\|Recording>>` | The first of these sections the article has, with its heading. Name the ones you want, in order |
| `<<personnel>>` | The credits, under a Personnel heading |
| `<<tracks_page>>` | A link to the album's tracks page, when there is one |
| `<<facts>>` | When and where a person was born, or a band formed |
| `<<members>>` | A band's members |
| `<<album>>` | The album's page name, `Artist - Album` |
| `<<track_sections>>` | A section a track, as above |
| `<<source>>` | The line saying where the text came from. Keep it: Wikipedia's licence needs it |

A line whose placeholder has nothing to say is left out. A run warns about a placeholder it
does not know. `<<section:...>>` works only on a line of its own. The player's own pieces, such as
`{{tracks}}` and `{{mentions}}`, pass through to the page, and the player fills them in; one
with nothing to list is left out with the heading above it. The
[Liner Notes guide](https://github.com/anthonyfletcher/podbox/blob/master/docs/podbox/notes-guide.md) lists them all.

## Time

A first run over 300 albums takes about half an hour, most of it MusicBrainz, which allows
one request a second. `--depth 1` adds another 15 to 30 minutes. What it fetched is kept in
`.rockbox/fetch/notes-cache/` for six months, so a run with `--refresh` after a template
change takes seconds.
