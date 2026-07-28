# Rockbox Music Artwork Fetcher

Single-file, single-parser artwork generation for Rockbox-style music libraries.

Current build:

```text
3.8-provider-candidate-cap-no-face-detection
```

This tool creates square JPEG artwork for a music library organised as artist folders containing album folders. It can create:

- artist artwork at `Artist/folder.jpg`
- album artwork at `Artist/Album/folder.jpg`
- baseline / non-progressive JPEG files suitable for older devices and Rockbox use cases

It is designed for local music libraries where artwork quality, square cropping, repeatability and performance matter more than simply accepting the first image returned by an API.

---

## What the tool does

For each artist or album folder, the script:

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

## Supported modes

The script supports three modes:

```bash
python art_fetch.py both "D:\Music"
```

```bash
python art_fetch.py artist "D:\Music"
```

```bash
python art_fetch.py album "D:\Music"
```

If no mode is provided, the default mode is `both`.

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

---

## Installation

Install the required Python dependencies:

```bash
pip install mutagen pillow requests
```

The script uses:

- `mutagen` for reading audio metadata and embedded artwork
- `Pillow` for image validation, cropping, resizing and JPEG writing
- `requests` for online provider lookups and image downloads

---

## Credentials

Credentials are stored in a shared config file.

On Windows:

```text
%APPDATA%\RockboxArtistArt\artist_art_credentials.json
```

Install or update credentials interactively:

```bash
python art_fetch.py --install-credentials --prompt-credentials
```

Install credentials non-interactively:

```bash
python art_fetch.py --install-credentials --lastfm-api-key YOUR_KEY --fanart-api-key YOUR_KEY
```

Check configured provider status:

```bash
python art_fetch.py --show-credentials-status
```

Last.fm and fanart.tv require configured API keys. MusicBrainz and TheAudioDB use public endpoints in this script.

---

## Artist artwork workflow

Artist artwork uses these providers:

1. Last.fm API
2. Last.fm public artist/gallery pages
3. fanart.tv
4. TheAudioDB

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
4. TheAudioDB album lookup

Embedded artwork is checked first. If embedded artwork is found, it is added as a candidate and usually wins because it receives a strong source reputation bonus.

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
last.fm-api         30.0
last.fm-web         30.0
last.fm             30.0
TheAudioDB          20.0
fanart.tv           10.0
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
python art_fetch.py both "D:\Music" --overwrite --ignore-cache --retry-not-found
```

---

## Folder verification

The script does not blindly trust folder names. It reads a sample of audio files and compares folder names against metadata.

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

If matching fails and `--interactive` is enabled, the script can prompt for an alternative search term.

Lower threshold example:

```bash
--match-threshold 0.70
```

Higher threshold example:

```bash
--match-threshold 0.90
```

---

## Single-pass `both` mode

In `both` mode, the script performs a single top-level library traversal.

For each artist branch it:

1. processes artist artwork
2. processes album folders inside that artist branch
3. moves to the next artist branch

This avoids the older behaviour of running one album pass and then one artist pass over the whole library.

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

### Processing mode

```bash
artist
album
both
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
--overwrite
--dry-run
--ignore-cache
--retry-not-found
--no-theaudiodb
```

### Rate limits

```bash
--lastfm-rate-limit 1.0
--fanart-rate-limit 1.0
--musicbrainz-rate-limit 1.1
--coverartarchive-rate-limit 1.0
--theaudiodb-rate-limit 2.5
--image-rate-limit 0.2
```

### MusicBrainz user agent

```bash
--musicbrainz-user-agent "RockboxMusicArt/3.8 (local library artwork tool)"
```

---

## Common command examples

Basic combined run:

```bash
python art_fetch.py both "D:\Music"
```

Interactive combined run:

```bash
python art_fetch.py both "D:\Music" --interactive
```

Artist artwork only:

```bash
python art_fetch.py artist "D:\Music"
```

Album artwork only:

```bash
python art_fetch.py album "D:\Music"
```

Overwrite existing generated files:

```bash
python art_fetch.py both "D:\Music" --overwrite
```

Dry run:

```bash
python art_fetch.py both "D:\Music" --dry-run
```

Full retest:

```bash
python art_fetch.py both "D:\Music" --overwrite --ignore-cache --retry-not-found
```

Higher quality source requirement:

```bash
python art_fetch.py both "D:\Music" --min-source-size 500
```

Try more candidates per provider:

```bash
python art_fetch.py both "D:\Music" --max-candidates-per-provider 5
```

Unlimited artist provider candidates:

```bash
python art_fetch.py both "D:\Music" --max-candidates-per-provider 0
```

Disable TheAudioDB:

```bash
python art_fetch.py both "D:\Music" --no-theaudiodb
```

---

## Troubleshooting

### Nothing is written

Check whether existing artwork is being skipped:

```text
skip: artwork already exists: folder.jpg
```

Use:

```bash
--overwrite
```

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

### You want the old unlimited behaviour

Use:

```bash
--max-candidates-per-provider 0
```

### Source images are too low quality

Increase the minimum source size:

```bash
--min-source-size 500
```

---

## Building a standalone Windows executable

Install PyInstaller:

```bash
pip install pyinstaller
```

Build a single executable:

```bash
pyinstaller --onefile art_fetch.py
```

The executable will be created under:

```text
dist\art_fetch.exe
```

To run the commands explained in this manual with the executable, replace:

```text
python art_fetch.py
```

with:

```text
.\art_fetch.exe
```