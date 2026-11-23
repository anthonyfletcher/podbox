# Liner notes and biographies

The player has a page for every artist and album in your library, with
links between them, like a small wiki of your music. Pages you write -- an
album's liner notes, an artist's biography, anything else -- are text files
with a little Markdown; for everything you haven't written about, the player
makes a page of its own from the library.

Turn them on first: `Settings > Library > Music > Liner Notes and
Biographies > Enabled`. Off, as it starts, the player doesn't read `/Notes`
at all and no menu offers them.

## Where the files go

```
/Notes/People/Steely Dan.md          a person: an artist, a producer, anyone
/Notes/People/Steely Dan.jpg         a picture for the top of the page (optional)
/Notes/Albums/Steely Dan - Aja.md    an album's liner notes
/Notes/Albums/Steely Dan - Aja.jpg   a picture for the top of the notes (optional)
/Notes/Studios/Electric Lady.md      anything else, in a folder of your choosing
```

A page's name is its file's name. `.txt` works too, read as Markdown; where
both exist, `.md` is used.

[`podbox_fetch notes`](../../tools/fetch/notes_fetch.md), a tool you run on a computer
with the iPod connected, writes pages for your albums and artists from Wikipedia and
MusicBrainz, and with `--tracks` a page about each album's tracks. It leaves the pages you
wrote alone.

The iPod's own software reads its Notes from `/Notes` too, so your player
may have the folder already, perhaps with old notes in it. Those show as
pages of their own.

Files in `/Notes` don't appear in `Books` or the document lists while the
setting is on.

A change to a page shows the next time you open it. A new, renamed or moved
page, and a link you add or take out, is picked up after you next connect
the player to a computer, or straight away with `Settings > Library >
Maintenance > Rebuild File Index`.

The player reads up to 65,535 pages, and each takes about 50 bytes of
memory from the music buffer, so 25,000 pages take about 1.2 MB. Past the
limit the rest are left out, and the background task log says so.

### Beside the music

You can keep pages with the music instead, as you might a `cover.jpg`:

```
/Music/Steely Dan/bio.md             the page "Steely Dan"
/Music/Steely Dan/Aja/notes.md       the page "Steely Dan - Aja"
```

They are named by their folders, exactly as though they were in `/Notes`,
so links reach them the same way. Where `/Notes` has a page of the same
name, that one is used.

## Links

Put a name in square brackets -- `[Steve Gadd]`, `[Aja]`, `[Electric
Lady]` -- and it becomes a link. It finds its page however it's spelt:
capitals, accents and punctuation don't matter, `[Beatles, The]` finds The
Beatles, and `Gadd, Steve` is the same person as `Steve Gadd`. A link looks
for, in order:

1. A page of exactly that name: `[Steely Dan - Aja]`
2. A person's page: `[Steve Gadd]`
3. An album's page by its title alone: `[Aja]`. Two albums of one title go
   to the one by the artist the page is about, else the older
4. A page in any other folder of `/Notes`
5. Your library: an artist, else an album of that name, whose page the
   player makes up
6. A name two or more pages give, whose page lists where

A name none of these finds stays plain text. So does a name longer than
192 bytes, and every name past the 512th on a page, counting the links
pieces make.

Two people with the same name are told apart as Wikipedia does it: add a
word in brackets to the less known one, `[Michael McDonald (producer)]`,
and name their page to match, `Michael McDonald (producer).md`. The page
shows "Michael McDonald". Leave the name the albums are filed under as it
is, so Now Playing still finds that page. A name that finds nothing is shown
whole, its words in brackets too: `[Ring Ring (Swedish version)]`.

Four words in brackets say what kind of thing you mean instead:

| You write | It links to |
|---|---|
| `[Aja (album)]` | An album, not a person of the same name |
| `[Prince (person)]` | A person, not an album |
| `[Peg (track)]` | A track: on the page's own album first, then the artist's, then any. `[Steely Dan - Peg (track)]` names the artist |
| `[Recording (section)]` | A heading of this page. Left comes back. A heading the page doesn't have leaves the name plain text |

A track has no page of its own: its link opens its album's page at its
section of the notes, or at its row in the track list. Notes with sections
for some tracks have no track list, so a link to a track without a section
opens the page at the top.

With `Trim Titles` on, album and track names from your library are shown
trimmed, and `[Aja]` still finds `Aja (Remastered)`.

## The pages nobody wrote

An artist's page lists their albums and where your pages name them. An
album's page has a line linking its artist, with its year and genre, then
its tracks and where it is named.

A page you write gets some of this too: an album's page starts with the
line linking its artist unless your page has `{{artist}}` or a link to
them, and ends with its tracks unless it has `{{tracks}}` or sections for
them; an artist's ends with their albums unless it has `{{albums}}` or a
link to one of them. Use the pieces below to put them somewhere else.

## Pieces

A piece in double braces is filled in when the page opens. On a line of its
own, these become lists, each item a link; one that comes out empty is
left out along with the heading just above it. Among other words, a list is
written on their line, its items joined by ` · `.

| Piece | Becomes |
|---|---|
| `{{albums}}` | The page's artist's albums, oldest first |
| `{{tracks}}` | The page's album's tracks, numbered |
| `{{mentions}}` | The pages that name this page |
| `{{pages: Studios}}` | Every page in a folder of `/Notes`, by name |
| `{{played}}` | The tracks of the page's album or artist played last, newest first: five unless `limit` says |
| `{{contents}}` | The page's own headings, each a link to its place: `{{contents\|style=inline}}` at the top of long notes |

A piece takes properties after a `|`: `{{albums|sort=-plays|limit=5}}`.
The first can follow a colon instead: `{{albums: Steely Dan}}` is
`{{albums|artist=Steely Dan}}`. Every list takes these:

| Property | Does |
|---|---|
| `limit=5` | Only the first five. No list shows more than 256, the 256 that sort first |
| `sort=year`, `sort=-year` | The order; a `-` turns it round |
| `show=year,tracks` | What is shown dimmed after each item |
| `style=inline` | All on one line: `Aja · Gaucho · Pretzel Logic` |
| `style=commas` | All on one line, parted by commas: `Aja, Gaucho, Pretzel Logic` |
| `style=bullets` | A bullet before each |
| `heading=Albums` | A heading above it, left out with it when it is empty |
| `empty=Nothing yet` | A line to show when there is nothing to list |

And each its own:

| Piece | Properties |
|---|---|
| `{{albums}}` | `artist=Name`; `genre=Jazz`; `year=1970-1979`; `guest=true` adds the albums they only feature on (once Featured Artists has read your library); `sort=year` (the order it starts in), `name`, `plays` (most played first), `played` (last played first) or `added` (newest first); `group=decade` or `genre` for a heading over each; `show=year,tracks,length,plays` |
| `{{tracks}}` | `album=Name`, with `artist=Name` for whose album it is; or `artist=Name` alone for all their tracks, or `composer=Name` for every track whose composer tag names them (names in the tag parted by `,` `;` `/` `&` or `and`); `number=1,2,4` for only those tracks; `disc=2`; `group=disc` for a heading over each disc; `sort=plays`, `played` or `title`; `numbers=false`, or `numbers=true` for a list on one line or in bullets, which have none; `show=length,plays,artist` |
| `{{mentions}}` | `name=Name`; `filter=artists`, `albums`, `people` or `other`, for the kind of page that does the naming; `headings=true` to show the section of the page each is named in; `group=kind` for a heading over albums, people and other pages |
| `{{pages}}` | `folder=People`, `Albums` or the name of any other folder of `/Notes` |

For example, an artist's ten most played tracks: `{{tracks|artist=Steely
Dan|sort=plays|limit=10|show=plays}}`; the 1970s in your library, newest
first: `{{albums|year=1970-1979|sort=-year}}`; the tracks a session player
is on, among the words of a credit: `Bass on
{{tracks|album=Aja|artist=Steely Dan|number=1,2,5|style=commas}}`.

These become the album's details wherever you put them, so `[{{artist}}]`
is a link to the artist:

`{{artist}}`, `{{album}}`, `{{year}}`, `{{genre}}`, `{{composer}}`,
`{{grouping}}`, `{{comment}}`, `{{tracks.count}}`, `{{discs}}`, `{{length}}`,
`{{plays}}` (an album's plays, or on an artist's page all of theirs)

One the album doesn't have is left out, along with a ` · ` beside it.
`{{length|format=minutes}}` gives `42 min`, and `{{year|format=decade}}` `2010s`.
`{{track|number=5}}` is a link to the album's fifth track, so notes can
name a track without spelling it: `The single, {{track|number=2}}, ...`
(`disc=2` for a second disc). For another album, `album=` takes
`Artist - Album` or the title alone, as `{{track|number=1|album=ABBA - Ring
Ring}}` or `{{track|number=1|album=Ring Ring|artist=ABBA}}`. A title alone,
with no `artist=`, is the page's artist's album of that name, else the
oldest.

### Changing the made-up pages

The pages the player makes up follow three templates, which you can replace
with files of your own made of the same pieces:

```
/.rockbox/notes/Album.md     every album page nobody wrote
/.rockbox/notes/Artist.md    every artist page
/.rockbox/notes/Name.md      a name your pages give that isn't in the library
```

The built-in album template is:

```markdown
[{{artist}}] · {{year}} · {{genre}}

## Tracks
{{tracks|group=disc}}

## Mentioned In
{{mentions}}
```

and the artist template:

```markdown
## Albums
{{albums|guest=true}}

## Mentioned In
{{mentions}}
```

A page you wrote yourself gets the same track list, or the same albums,
added at the end when it has no list of its own.

## Opening them

While music plays, hold `Select` on the Now Playing screen and choose
`Liner Notes`: the playing album's page, opened at the playing track's
section of the notes, or at its row in the track list. To open it with one press, choose `Liner
Notes` for one of the Now Playing buttons: `Settings > Playback Settings >
Now Playing Screen`, then `Select Action`, `Select + Play Action` or `Long
Press Play Action`.

In the library, an album's menu -- in `Music` or `Album Covers` -- has
`Liner Notes` when you have written notes for it. An artist's menu -- in
`Music`, or on their portrait in `Artist Portraits` -- has `Biography` when
you have written their page, or `Mentions` when your pages name them.

Any of these rows can be hidden, as other menu rows can: hold `Select` on
it and choose `Hide This Row`, or use `Settings > Appearance > Menus >
Context Menus`, where it can be shown again.

## Moving about

Links are drawn in the accent colour. One of them is the current link,
drawn highlighted: the one a third of the way down the screen, or at the top
of a page the first. Turn the wheel and the text scrolls, the current link
moving with it. With key clicks on, the wheel clicks only as a new link
becomes current, so you can hear where the links are.

| Button | Does |
|---|---|
| Wheel | Scrolls. Two links on one line take a click each before the text moves on; at the end of a page, and on a page too short to scroll, the clicks move the current link on instead |
| `Select`, `>>\|` | Opens the current link. A track of the page's own album plays the album from that track |
| `Select` held | The current link's menu |
| `\|<<` | Back to the page before, where you left it. On the first page, leaves the screen |
| `Play` | On an album's page, plays the album when it isn't the one playing; elsewhere, pauses and plays |
| `Menu` | Leaves the screen |
| `Menu` held | The page's menu |

A link's menu, for an album or a track, has `Play`, `Play Next` and `Play
Last`; an album's has `Go to Album` too, which opens it in `Music`, and an
artist in your library has `Go to Artist`. A link to anything else has no
menu, and holding `Select` on it does nothing.

The page's menu has the same rows for the album or artist the page is
about, `Move Banner` (below), and the `Colours` and `Banners` settings,
which change the page as soon as you leave the menu. On an album's page,
the album's other pages (below) come first, each opening that page; `|<<`
comes back.

The page doesn't change with the music: it stays where you are until you
leave, and the next time you open it from Now Playing it is the album then
playing.

## Colours

Each page is tinted from its own picture -- the album's cover, the artist's
picture -- with its links in the picture's most vivid colour, or the
theme's when the picture has no colour in it, and a page with no picture
keeps the colours of the last one. `Settings > Library > Music > Liner
Notes and Biographies > Colours` changes that: `Theme` follows your theme,
and with dynamic colours on, the album that's playing; `Text Viewer's` uses
the text viewer's colours, your own custom pair among them.

`Custom` sets each part apart: the text, the links, the page, dimmed text,
headings and the line beside a pull line. Each takes the picture's colour,
the theme's, white, black, or one you choose with the colour picker
(`Chosen Colour`): white text on black with links in the picture's colour,
say, or grey text with amber links. Dimmed text, headings and pull lines
are `Auto` until you set them, made from the text and link colours.

`Progress Line` off leaves out the line along the foot of the page that
shows how far the playing track has got.

## The picture at the top

`Settings > Library > Music > Liner Notes and Biographies > Banners` off
leaves the picture out: every page starts with its name.

A page in `/Notes/People` or `/Notes/Albums` shows its own picture -- `Steely
Dan.jpg` beside `Steely Dan.md` -- if you give it one, scaled and cropped to
the banner, which is 320 by 132. A wide picture suits it best. Without one,
an artist's page uses the artist's picture from the player's artwork, and an
album's its cover. Other pages have no picture: their name is set at the top.

A new picture is made ready in the background, so it shows the second time
you open the page, or after you next connect to a computer.

The part of the artist's picture or the cover that shows can be moved: hold
`Menu`, choose `Move Banner`, turn the wheel, and press `Select` to keep it
(`Menu` leaves it as it was). `Reset Banner` puts it back. A banner from a
picture of your own is shown as it is, and can't be moved.

## The marks

| You write | You see |
|---|---|
| `# Heading` | A bold heading, underlined when the theme has no bold font |
| `## Heading` | A lesser heading, underlined. `###` and below look the same |
| `## 2. Aja` | In album notes, a heading named for a track starts that track's section |
| `*text*` | Dimmer text, for dates, credits and asides |
| `**text**` | Bold, when the theme has a bold font; ordinary text when it has none |
| `> A line` | A pull line, set apart with a line of the accent colour down its side |
| `[Steve Gadd]` | A link |
| `- item` | A bullet, the item's lines indented beside it. `*` and `+` work too; two spaces in front put it a level in |
| `1. item` | A numbered item, its lines indented under its first word |
| `<!-- a note -->` | Left out, over as many lines as it runs |
| `---` | On the first line, starts front matter, left out to the next `---` line. Its lines are `key: value` lines, lists and indented lines; at any other line it ends there and the page starts |

Everything else Markdown has is shown as plain text:

- A web link's label shows, and its address is left out
- Code keeps its words

The screen shows the page's name at the top in large type, so start with
the text itself rather than a title: a first line of `# Steely Dan` would
say it twice.

Each line you write is a line on the player, so write a paragraph as one
long line and let your editor wrap it. That way a list of credits, one per
line, stays a list.

A track's section is found by its heading: the track's title, with or
without its number in front (`## 2. Aja` or `## Aja`). A title that starts
with a number, `## 99 Luftballons`, is found by its title too.

## A person's page

```markdown
*Formed 1972, Los Angeles*

> Seven albums in nine years, then twenty before the next.

[Walter Becker] and [Donald Fagen] met at Bard College in 1967 and wrote for other acts before forming the band.

## The studio years

From 1974 they stopped touring and made records with session players, among them [Wayne Shorter] and [Steve Gadd].
```

## Album notes, a section a track

```markdown
Recorded across 1977 with dozens of session players.

## 1. Black Cow
*Tenor saxophone* [Tom Scott]
*Bass* [Chuck Rainey]

## 2. Aja
Nearly eight minutes, with solos for Gadd and Shorter.
*Drums* [Steve Gadd]
*Tenor saxophone* [Wayne Shorter]
```

Notes don't have to be split by track. A single essay for the whole album
works just as well; leave out the `##` sections.

## More than one disc

Put a heading naming the disc -- `# Disc 2`, `# Disk 2` or `# CD 2` --
above that disc's tracks. A track's section is then looked for only under
its own disc, so the second disc's track 1 doesn't open at the first
disc's.

```markdown
# Disc 1
## 1. Black Cow
## 2. Aja

# Disc 2
## 1. Peg
```

The disc comes from the track's disc number tag; a track without one is on
disc 1. Notes with no disc headings are read as one disc.

## An album's other pages

A page is read only up to its first 32 KB, and shows no more than 40 KB
once its pieces are filled in. Notes longer than that, or notes you would
rather read in parts, can go in pages of their own that belong to the
album. Start each with front matter naming the album:

```markdown
---
album: Steely Dan - Aja
---
*Recorded at Village Recorder, Los Angeles, 1977*

{{tracks}}
```

Name the file for the album and the part:
`/Notes/Albums/Steely Dan - Aja - Credits.md` is the page "Aja - Credits",
and `[Steely Dan - Aja - Credits]` links to it. Such a page
has the album's banner and colours, its menu plays the album and has `Go to
Album`, pieces such as `{{tracks}}` and `{{year}}` are the album's, and
`[Peg (track)]` finds the album's track first. It gets no line linking the
artist and no track list of its own; write `{{artist}}` and `{{tracks}}`
where you want them.

The album's page and its other pages list one another at the top of the
page's menu (hold `Menu`), up to eight of them. A new page is listed after
you next connect to a computer, or after `Rebuild File Index`. `album:`
also takes the title alone, `album: Aja`, which is the oldest album of that
name in your library.

## Markdown files in the text viewer

The text viewer reads the same marks in any `.md` file: headings are
underlined and listed under `Browse Chapters`, `*text*` is dimmer,
`> lines` are set apart, lists have their bullets, and comments and front
matter are left out. `**bold**` shows as ordinary text there, and a
`[Name]` as plain text.
