# Playlist Engine

Playlists built from how the music sounds rather than from what the tags say.
Ask for tracks like one you choose, pick a mood, or travel from one mood to
another.

Turn it on at **Settings → Library → Playlist Engine → Enabled**. That starts
the analysis, which asks before it begins.

---

## The analysis

Each track is measured once: its tempo, loudness, tonal balance, how busy and
how punchy it is, stereo width, and its key. After adding music, only the new
tracks need measuring — see [Keeping it current](#keeping-it-current).

On the player a full library takes hours; the [desktop tool](#the-desktop-tool)
does it in about an hour. **Analysis Depth → Quick** is about three and a half
times faster on the player, but noticeably less accurate.

## Making a playlist

- **Play Similar** — hold `Select` on a track, anywhere, and choose Play
  Similar. That track plays first and the rest follow in order of how close
  they sound. On an album or a folder the playlist is built around the album
  as a whole, with no more than a couple of its own tracks.
- **Moods** — at the top of **Playlists**. Sixteen: Calm, Energetic, Dark,
  Bright, Warm, Raw, Lush, Punchy, Smooth, Sparse, Dense, Hypnotic, Slow,
  Fast, Melancholy and Uplifting.
- **Journeys** — below Moods. A playlist that moves gradually from one mood to
  another, such as Slow → Fast.
- **Wind Down** — hold `Select` on a track. It plays first, and each track
  after it is calmer. With a sleep timer running, the playlist lasts as long
  as the time left on it.
- **Order by Sound** — hold `Select` in the playlist viewer. Puts the playlist
  you already have in an order where each track leads into the next. The
  track playing stays first, tracks not yet measured go to the end, and a
  playlist of more than 100 tracks is left as it is.

Every playlist the engine builds follows the same rules:

- No more than two tracks by one artist, with at least three tracks between
  them.
- Nothing under 90 seconds.
- A playlist holds only tracks that are genuinely close, so it can be shorter
  than Playlist Length. Measuring more of your library makes them longer.

## Continue Playing

With **Continue Playing** on, any playlist that runs out is extended instead of
stopping — an album, a folder, a saved playlist, or one the engine built.

A mood keeps to that mood, and a journey carries on in the mood it ended in.
Anything else carries on from the track that just finished. Nothing already in
the playlist is added again, and a track you skip in its first twenty seconds
is not offered again.

## What things sound like

- **A track** — hold `Select`, choose **Track Info** and open the **Sound**
  row. It lists the moods the track fits, then energy, pace, tone, key and
  more, in words.
- **An album or a folder** — hold `Select` on it and choose **Album Sound**.
  The same read-out, averaged over its tracks.
- **Your library** — **Settings → Library → Playlist Engine → Library Sound**.
  How much has been measured, how many tracks could not be read, and how many
  have a steady enough tempo and a clear enough key to use.

The words compare the music with the rest of your library: *Bright* means
brighter than three quarters of what you own. Library Sound's **Compared To**
row says whether that comparison is with your library yet or still with the
built-in defaults.

Two of the words affect playlists:

- **Pace** reads *unsteady* where the beat wanders, which is most live
  recordings and nearly all jazz. A track whose tempo wanders further has no
  speed word, and Slow, Fast and Hypnotic leave it out.
- **Key** reads *Unclear* on about two fifths of a typical library. That is
  normal.

## Settings

All under **Settings → Library → Playlist Engine**.

| Setting | What it does |
|---|---|
| Enabled | Turns the engine on and starts the analysis. Turning it off keeps what has been measured. |
| Analysis Depth | Thorough (40 seconds of each track) or Quick (about 11). |
| Playlist Length | How many tracks a playlist holds, 5 to 100. |
| Track Playlist | How much Play Similar varies between runs. |
| Mood Playlist | The same, for moods and journeys. |
| Continue Playing | Keeps the music going when a playlist runs out. |
| Library Sound | What the analysis found across your library. |

**Predictable** gives the same playlist every time until the library changes,
**Weekly** a new one each week, and **Variable** a different one every time.
All three draw from the same close matches.

## Keeping it current

**Settings → Library → Maintenance** has two rows once the engine is on:

- **Update Sound Analysis** measures new tracks, and retries any that could
  not be read before.
- **Rebuild Sound Analysis** measures everything again from scratch.

Both need the charger connected. They stop if it is removed, and carry on
from where they stopped next time.

---

## The desktop tool

`soundscan.exe` does the same analysis on a Windows PC in about an hour, and
writes the same file the player uses. It ships at `.rockbox/tools/`.

Connect the player, open a command prompt in that folder and run:

```
soundscan.exe
```

It finds the player's drive and asks before writing anything. With more than
one player attached, name the drive:

```
soundscan.exe E:\
```

| Option | What it does |
|---|---|
| `-v` | One line per track, with its tempo, loudness and key. |
| `-n` | Measure, but write nothing. |

Running it again only measures what is new. Eject the player normally
afterwards.
