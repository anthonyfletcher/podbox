# fetch_pb lyrics

Saves synchronised lyrics for your music, from [LRCLIB](https://lrclib.net), as `.lrc`
files beside the tracks, where PodBox's Lyrics screen finds them.

```bash
.\fetch_pb lyrics E:\                # a mounted iPod (Windows)
./fetch_pb lyrics /Volumes/IPOD     # a mounted iPod (macOS)
.\fetch_pb lyrics D:\Music           # or a music folder on your computer
```

From source, `python lyric_fetch.py` takes the same options.

Run it again whenever you add music: it asks only about tracks it has not seen. A first run
over 5,000 tracks takes about half an hour.

## What it does

- Each track gets `<track name>.lrc` beside it, in UTF-8. Lyrics timed word by word keep
  their word timing.
- Only **timed** lyrics are saved, because the Lyrics screen needs them. Tracks LRCLIB has
  only plain lyrics for are counted in the summary, and no file is written.
- It skips a track that already has lyrics: a `.lrc`, `.lrc8` or `.snc` named after it, or
  after its title, in its folder or any folder above it, or lyrics inside an MP3.
- It skips audiobooks and podcasts, going by genre in the same way the player does, and
  tracks with no artist or title tag.
- It never changes your audio files, and never overwrites a lyrics file it did not write.

Tracks LRCLIB has nothing for are asked about again after 30 days, or at once with
`--retry-not-found`.

## Options

| Option | |
| --- | --- |
| `--dry-run` | List the tracks it would look up, and write nothing |
| `--only "Artist"` | Only that artist's tracks |
| `--retry-not-found` | Ask again about tracks LRCLIB had nothing for |
| `--undo` | Delete the `.lrc` files it wrote. Any you have edited since are kept |
| `--delay 0.3` | Seconds between requests |
| `--contact you@example.com` | Put your address in its requests, so LRCLIB can reach you about them. It is remembered, and LRCLIB allows a client that names itself more requests a minute |

## Files it keeps

On an iPod, `.rockbox/fetch/` holds `lyrics-cache.json` (what LRCLIB said about each track)
and `fetch-manifest.json` (the files the tools wrote, which is how `--undo` knows them). On a
music folder they sit at the top of it.

Lyrics are copyright. Keep the files for your own listening.
