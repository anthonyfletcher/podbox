# fetch_pb art

Creates JPEG artwork for a music library organised as artist folders containing album
folders, and for the audiobooks, podcasts and ebooks on an iPod. It can create:

- artist artwork at `Artist/folder.jpg`
- album artwork at `Artist/Album/folder.jpg`
- covers of audiobooks, podcasts and ebooks, and pictures of their authors, in the iPod's
  `Artwork` folder
- if you ask, pictures of artists with no folder of their own, in the same place
- baseline / non-progressive JPEG files, which the player decodes fastest

---

## What the tool does

For each artist or album folder, it:

1. checks folder names against audio metadata
2. collects artwork candidates from local and online sources
3. removes duplicate candidates
4. optionally caps candidates per provider
5. downloads candidate images
6. rejects images that are too small or obvious placeholders
7. scores each valid image using actual downloaded properties
8. picks the highest-scoring image
9. crops/resizes to a square JPEG
10. writes the output image
11. updates the relevant cache

The important point is that provider metadata is not trusted blindly. The image is actually downloaded and opened before final scoring.

---

## What to fetch

Name the kinds you want before the folder, or none for all five:

| Kind | Writes |
|---|---|
| `artists` | a picture of each artist, in the artist's folder |
| `albums` | each album's cover, in the album's folder |
| `audiobooks` | covers of audiobooks, and pictures of their authors |
| `podcasts` | covers of podcasts |
| `ebooks` | covers of ebooks, and pictures of their authors |

```bash
.\fetch_pb art E:\Music                    # everything
.\fetch_pb art albums E:\Music             # album covers only
.\fetch_pb art podcasts audiobooks E:\     # podcasts and audiobooks
```

On a Mac, `./fetch_pb art /Volumes/IPOD/Music`. From source, `python art_fetch.py` takes
the same options.

Give the music folder: artists and albums are found in the folders below it. Audiobooks,
podcasts and ebooks are found anywhere on the iPod, and their covers go in its `Artwork`
folder (see [below](#audiobooks-podcasts-and-ebooks)). When the folder is not on an iPod,
those three are skipped unless you name them and give `--artwork-root`.

---

## Library layout

Expected layout:

```text
D:\Music
├── Artist One
│   ├── Album One
│   │   ├── 01 Track.flac
│   │   ├── 02 Track.flac
│   │   └── ...
│   └── Album Two
│       └── ...
├── Artist Two
│   └── Album One
│       └── ...
└── Artist Three
    └── Album One
        └── ...
```

Artist artwork is written to:

```text
D:\Music\Artist\folder.jpg
```

Album artwork is written to:

```text
D:\Music\Artist\Album\folder.jpg
```

The output filename can be changed to `cover.jpg`:

```bash
--output-name cover.jpg
```

PodBox also reads art from an `Artwork` folder at the root of the player: artist pictures as
`Artwork/People/<Artist>.jpg` and album covers as `Artwork/Covers/<Artist> - <Album>.jpg`.
These take precedence over the images beside the music. To keep the images out of your music
folders, move the files this tool writes there and rename them to match.

Folders of audiobooks or podcasts (going by genre, as the player decides) are skipped by
`artists` and `albums`; the `audiobooks` and `podcasts` kinds cover them.

---

## Your own artwork is left alone

art_fetch keeps a list of the images it wrote, and never replaces anything else. An image you
put there yourself, or one an older art_fetch wrote, is reported as `your artwork` and
skipped.

- `--overwrite` fetches again the images art_fetch wrote.
- `--overwrite-all` replaces any artist picture or album cover, yours included. Covers and
  pictures of authors you put in `Artwork` are never replaced.

The list is `fetch-manifest.json`: in `.rockbox/fetch/` on an iPod, or at the top of a music
folder on your computer.

---

## Audiobooks, podcasts and ebooks

```bash
.\fetch_pb art audiobooks podcasts ebooks E:\
```

Point it at the mounted iPod, or a folder on it. It finds the audiobooks and podcasts by genre, and the EPUB,
FB2 and MOBI books, and writes into the iPod's `Artwork` folder:

- a book's cover as `Artwork/Covers/<Author> - <Title>.jpg`, and a show's as
  `Artwork/Covers/<Show>.jpg`
- a picture of each author as `Artwork/People/<Author>.jpg` (`--no-authors` skips them)

Nothing is fetched for a book the iPod can already show a cover for: one in `Artwork/Covers`,
a cover inside the book or its files, an image named after it, or an image in a folder it
has to itself.

Covers come from Audible (through Audnexus, when the folder or tags carry the book's ASIN,
such as `Dune [B002V1OF70]`), Apple Books, Open Library and, with `--google-books-key`,
Google Books. A podcast's comes from Apple and the show's own feed. An ISBN in the tags,
the file name or an ebook's details finds the book exactly. Without one, it is matched by
title and author.

A printed book's cover is taller than it is wide. It is saved whole, and PodBox crops it
where it needs a square.

On a music folder on your computer, give `--artwork-root` the folder to write into.

---

## Artists without a folder

`artists` pictures the artists that have folders. Three options add people who have none,
each for a PodBox setting you may not use:

| Option | Adds a picture of | For |
|---|---|---|
| `--split-artists` | each artist in an Album Artist such as `Madonna; Charli XCX` | **Split Album Artists** |
| `--track-artists` | each name in the Artist tag that is never an album artist | Music > Artist |
| `--featured-artists` | each guest a title or Artist tag credits, such as `Song (feat. Drake)` | **Featured Artists** |

```bash
.\fetch_pb art artists E:\Music --split-artists --featured-artists
```

Give the music folder on a mounted iPod: the pictures go in its `Artwork/People` folder, as
`Artwork/People/<Artist>.jpg`. On a music folder on your computer, give `--artwork-root`.

Every track's tags are read to find the names, which takes a few minutes on a large library.
Names are split and guests found as the iPod does it, including the names in its
`known_artists.txt`. Nobody is looked up who the iPod can already show a picture of: an
album artist, or anyone with a picture in `Artwork/People` or in a folder of their name.

On a library full of compilations, `--track-artists` can find thousands of names, and the run
takes as long.

---

## Close matches only

An album found online is used only when its title and artist agree with yours to 85%
(`--auto-accept`), or 90% for an artist's name (`--auto-accept-artist`). Edition words such as
"Deluxe" or "Remastered" and "feat." guests are ignored. When nothing is close enough but
something was near, the album is listed at the end as **Unsure**, with the nearest match and
its image's address, for you to check.

---

## Installation

Download it for Windows or macOS from the
[Tools release](https://github.com/anthonyfletcher/podbox/releases/tag/Tools); the
[README](README.md) says how to run it.

---

## Credentials

Credentials are stored in the settings file the PodBox tools share:

```text
%APPDATA%\PodBox\fetch\fetch.json                              (Windows)
~/Library/Application Support/PodBox/fetch/fetch.json          (macOS)
```

Keys saved by an older art_fetch, in `%APPDATA%\RockboxArtistArt\`, are copied across the
first time it runs.

Install or update credentials interactively:

```bash
.\fetch_pb art --install-credentials --prompt-credentials
```

Install credentials non-interactively:

```bash
.\fetch_pb art --install-credentials --lastfm-api-key YOUR_KEY --fanart-api-key YOUR_KEY
```

Check configured provider status:

```bash
.\fetch_pb art --show-credentials-status
```

Last.fm and fanart.tv require configured API keys. MusicBrainz, TheAudioDB and Deezer need no key.

---

## Artist artwork workflow

Artist artwork uses these providers:

1. Last.fm API
2. Last.fm public artist/gallery pages
3. fanart.tv
4. TheAudioDB
5. Deezer, used only when one of its albums matches one of the artist's album folders, or, for an artist folder with no album folders, when exactly one Deezer artist has that name

The process is:

1. verify the artist folder against audio metadata
2. query each enabled provider
3. collect all usable artist image candidates
4. remove duplicates
5. sort each provider's candidates by metadata score
6. keep only `--max-candidates-per-provider` candidates from each provider
7. download the retained candidates
8. reject images that are too small or placeholder-like
9. score the valid images
10. save the highest-scoring result

Example log shape:

```text
[Artist Name]
  folder check: OK, matched 'Artist Name' at 100%
  candidate: last.fm returned 8 usable artist image(s), keeping best 3
  candidate: fanart.tv returned 3 usable artist image(s)
  candidate: TheAudioDB returned 7 usable artist image(s), keeping best 3
  candidate pool: 9 unique artist image(s) to download/compare
  validating: last.fm-web image
    candidate score: actual 770x770, square=1.000, resolution=64.2, source=30.0, provider=10.0, score=204.2
  validating: fanart.tv image
    candidate score: actual 1000x1000, square=1.000, resolution=75.0, source=10.0, provider=15.0, score=200.0
  selected: last.fm-web (...)
  saved: D:\Music\Artist Name\folder.jpg (last.fm-web)
```

---

## Album artwork workflow

Album artwork uses these sources:

1. embedded artwork from local audio files
2. Last.fm album lookup
3. Cover Art Archive
4. Deezer
5. Apple Music
6. TheAudioDB album lookup
7. Discogs, with a personal access token (`--install-credentials --discogs-token TOKEN`)

Embedded artwork is checked first. If embedded artwork is found, it is added as a candidate and usually wins because it receives a strong source reputation bonus.

`--ignore-embedded` leaves embedded artwork out, for an album whose files carry a small or
poor cover: a better one is fetched online instead. It also fetches covers for books and shows
whose files, or ebooks, already carry one.

The album workflow is:

1. verify artist and album folder names against audio metadata
2. inspect a limited number of audio files for embedded artwork
3. if embedded artwork exists, validate and score it
4. if no existing or cached candidate is available, query online providers
5. download and score available candidates
6. save the highest-scoring image

Album provider lookups normally return only one candidate per source, so `--max-candidates-per-provider` is applied to artist candidate collection rather than album candidate collection.

---

## Image validation

An image is rejected if:

- it cannot be downloaded
- it cannot be opened as an image
- it is smaller than `--min-source-size` in either dimension
- it matches a known placeholder URL marker
- it appears to be a simple Last.fm placeholder-style image

Default minimum source size:

```bash
--min-source-size 300
```

For stricter quality control:

```bash
--min-source-size 500
```

---

## Image scoring

The final score is calculated after the image has been downloaded and opened.

Conceptually:

```text
final score = square score + resolution score + source reputation + provider tie-break
```

### Square score

Square images are strongly preferred because the final output is square. A perfectly square image receives the maximum square score.

Examples:

```text
1000x1000 -> square ratio 1.000
1280x720  -> square ratio 0.562
800x310   -> square ratio 0.388
```

### Resolution score

Resolution score uses the shortest edge of the actual downloaded image. This avoids treating wide banners as high-quality square sources.

For example:

```text
1200x1200 -> strong resolution score
1280x720  -> based on 720, not 1280
800x310   -> based on 310
```

### Source reputation

Source reputation nudges close decisions without completely overriding actual image quality.

Current source reputation values:

```text
embedded            40.0
Cover Art Archive   35.0
audnexus            35.0
podcast feed        35.0
last.fm             30.0
apple               30.0
deezer              25.0
TheAudioDB          20.0
openlibrary         20.0
discogs             15.0
fanart.tv           10.0
google books        10.0
cache                0.0
```

This means a fanart.tv image can still win if it is significantly better, but Last.fm and embedded artwork are preferred in close comparisons.

### Provider tie-break

Each provider may supply metadata such as image size, type, likes or image rank. That metadata is used as a smaller tie-breaker, capped so it cannot dominate actual image quality.

---

## Provider candidate cap

The provider cap controls how many artist candidates are retained from each provider before downloading and scoring.

Default:

```bash
--max-candidates-per-provider 3
```

Custom value:

```bash
--max-candidates-per-provider 5
```

Unlimited:

```bash
--max-candidates-per-provider 0
```

Why this exists:

- Last.fm gallery pages can expose many image hashes
- fanart.tv can return several thumbnails/backgrounds
- TheAudioDB can return artist thumbnails, logos, clearart and fanart images
- downloading every candidate can be slow
- the first few provider-ranked candidates are usually enough

The cap only limits non-rejected candidates returned by each provider before download. The final selector still compares candidates across all providers.

---

## Caching

The tool writes two cache files into the music root.

Artist cache:

```text
D:\Music\artist-art-cache.json
```

Album cache:

```text
D:\Music\album-art-cache.json
```

The cache records:

- successful lookups
- selected source
- selected image URL
- provider artist/album names
- MBID where available
- not-found results

Use the cache normally for repeat runs.

Ignore cache entirely:

```bash
--ignore-cache
```

Retry previously cached not-found entries:

```bash
--retry-not-found
```

A full retest normally uses both:

```bash
.\fetch_pb art "D:\Music" --overwrite --ignore-cache --retry-not-found
```

---

## Folder verification

It does not blindly trust folder names. It reads a sample of audio files and compares folder names against metadata.

Artist folders are checked against:

- album artist
- artist

Album folders are checked against:

- parent folder versus artist / album artist tag
- album folder versus album tag

Default threshold:

```bash
--match-threshold 0.78
```

If matching fails and `--interactive` is enabled, it can prompt for an alternative search term.

Lower threshold example:

```bash
--match-threshold 0.70
```

Higher threshold example:

```bash
--match-threshold 0.90
```

---

## Artists and albums in one pass

Fetching both artists and albums, it walks the library once.

For each artist branch it:

1. processes artist artwork
2. processes album folders inside that artist branch
3. moves to the next artist branch
Example output:

```text
=== Combined artist + album artwork ===
Artist cache   : D:\Music\artist-art-cache.json
Album cache    : D:\Music\album-art-cache.json
Traversal      : single pass over artist folders

=== Artist branch 1: Artist Name ===
...
--- Album in Artist Name (1) ---
...
```

---

## Important command-line options

### What to fetch

```bash
artists albums audiobooks podcasts ebooks   # any of them, before the folder; none for all
```

### Artists without a folder

```bash
--split-artists              # each artist of a ';' Album Artist
--track-artists              # each Artist-tag name that is never an album artist
--featured-artists           # each guest a credit names
```

### Output filename

```bash
--output-name folder.jpg
--output-name cover.jpg
```

### Output size

```bash
--max-size 300
```

### Minimum source size

```bash
--min-source-size 300
```

### Matching

```bash
--match-threshold 0.78
```

### Files inspected

```bash
--max-files-per-artist 25
--max-files-per-album 20
```

### Candidate cap

```bash
--max-candidates-per-provider 3
```

### Behaviour

```bash
--interactive
--overwrite                  # fetch again what art_fetch wrote
--overwrite-all              # replace your own artwork too
--dry-run
--ignore-cache
--retry-not-found
--no-theaudiodb
--no-deezer
--no-apple
--ignore-embedded            # don't use art inside the files: fetch a cover anyway
--update-player-cache        # on a mounted iPod (Windows), run its artcache_pb.exe afterwards
```

### Matching

```bash
--auto-accept 0.85
--auto-accept-artist 0.90
--min-match 0.45             # nearer than this, but not close enough, is listed as unsure
```

### Audiobooks, podcasts and ebooks

```bash
--artwork-root D:\Artwork    # where to write, when not on a mounted iPod
--no-authors
--no-audnexus
--google-books-key KEY       # remembered
```

### Rate limits

```bash
--lastfm-rate-limit 1.0
--fanart-rate-limit 1.0
--musicbrainz-rate-limit 1.1
--coverartarchive-rate-limit 1.0
--theaudiodb-rate-limit 2.5
--deezer-rate-limit 0.3
--apple-rate-limit 3.0
--discogs-rate-limit 1.1
--openlibrary-rate-limit 1.0
--image-rate-limit 0.2
```

### Identifying yourself

```bash
--contact you@example.com
```

Puts your address in the tool's requests, so a service can reach you about them instead of
blocking them. It is remembered, and shared with `lyrics` and `notes`.

---

## Common command examples

Everything:

```bash
.\fetch_pb art "D:\Music"
```

Interactive run:

```bash
.\fetch_pb art "D:\Music" --interactive
```

Artist artwork only:

```bash
.\fetch_pb art artists "D:\Music"
```

Album artwork only:

```bash
.\fetch_pb art albums "D:\Music"
```

Overwrite existing generated files:

```bash
.\fetch_pb art "D:\Music" --overwrite
```

Dry run:

```bash
.\fetch_pb art "D:\Music" --dry-run
```

Full retest:

```bash
.\fetch_pb art "D:\Music" --overwrite --ignore-cache --retry-not-found
```

Higher quality source requirement:

```bash
.\fetch_pb art "D:\Music" --min-source-size 500
```

Try more candidates per provider:

```bash
.\fetch_pb art "D:\Music" --max-candidates-per-provider 5
```

Unlimited artist provider candidates:

```bash
.\fetch_pb art "D:\Music" --max-candidates-per-provider 0
```

Disable TheAudioDB:

```bash
.\fetch_pb art "D:\Music" --no-theaudiodb
```

---

## Troubleshooting

### Nothing is written

Check whether existing artwork is being skipped:

```text
skip: artwork already exists: folder.jpg
skip: your artwork: folder.jpg
```

The first is an image art_fetch wrote, which `--overwrite` fetches again. The second is one
it did not write, which only `--overwrite-all` replaces.

An album listed as **Unsure** at the end had no match close enough to use; see
[Close matches only](#close-matches-only).

### Cached not-found results are being reused

Use:

```bash
--retry-not-found
```

or ignore cache completely:

```bash
--ignore-cache
```

### Folder names do not match tags

Use interactive mode:

```bash
--interactive
```

or lower the threshold:

```bash
--match-threshold 0.70
```

### Too many images are being downloaded

Keep the default cap or lower it:

```bash
--max-candidates-per-provider 2
```

### You want every candidate tried

Use:

```bash
--max-candidates-per-provider 0
```

### Source images are too low quality

Increase the minimum source size:

```bash
--min-source-size 500
```