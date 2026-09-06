# Playlist Engine

Playlists built from how the music sounds rather than from what the tags say.

Ask for tracks like the one playing, pick a mood, or set off on a journey from
one mood to another. None of it uses genres, ratings or play counts — it
listens to the music and matches on what it heard.

Off until you turn it on: **Settings → Library → Playlist Engine → Enabled**.
Switching it on starts the analysis, which asks before it begins.

---

## The analysis

Every track is measured once and the result kept in an index - each record holds:

- **Tempo**, and how steady it was. A tempo the tracker never settled on is
  marked untrusted and left out of anything that needs one.
- **Loudness** and **crest factor** — how loud, and how compressed.
- **Tonal balance** in three bands, low, middle and high.
- **Onset density** — how much is happening, per band.
- **Peakiness**, how sharply the sound attacks, and **stereo width**.
- **Key and mode**, major or minor, with a confidence margin. Below the margin
  the record says so rather than guessing.
- **Tonal clarity** and **harmonic change** — how clearly pitched it is, and
  how much the harmony moves.

Measuring takes 40 seconds of audio from each track, starting after the intro.
On the player that is hours for a full library; the desktop tool does the same
work much faster. Either way it happens once, and adding music later
only measures what is new.

**Analysis Depth** offers a quicker pass that stops as soon as the tempo has
settled, around 11 seconds. It is roughly three and a half times faster and
noticeably less accurate — the tempo settling also ends the measurement of
loudness, tone and key, and against a full analysis the quick one picked a
different closest track two times in three. Use it if a full analysis on the
player is too long to sit through.

## What a match is

Every record becomes eleven numbers on a fixed 0–1000 scale, and two tracks
are compared by weighted distance across them. Level and tonal balance carry
the most weight.   Same genre pulls two tracks together a little; a wide gap in
year pushes them apart a little; disagreeing about major versus minor costs
more. None of those decide a match on their own.

Two rules shape the result rather than the matching:

- No more than **two tracks by one artist**, and at least **three tracks
  between them** — so an artist is spread through the list instead of clumped
  in it, and a playlist is not a reshuffle of the album it started from.
- Tracks under 90 seconds are left out. They are intros, interludes and
  segues; they measure as real tracks and would arrive as real matches.

## The three ways in

**Play Similar** is on the context menu of any track — in the file browser, in
the database, or on the playing screen. The track you chose plays first and
the rest follow in order of how near they are to it.

**Moods** and **Journeys** are at the top of the **Playlists** screen, above
your saved playlists.

A mood is a place in the same space a track match uses, described in the same
numbers: *Calm* is quiet, sparse, unpeaked and dynamic. Sixteen are offered —
Calm, Energetic, Dark, Bright, Warm, Raw, Lush, Punchy, Smooth, Sparse, Dense,
Hypnotic, Slow, Fast, Melancholy and Uplifting. *Slow*, *Fast* and *Hypnotic*
consider only tracks whose tempo the analysis trusts.

A journey travels from one mood to another across the playlist. Each position
in the run is filled from the tracks nearest that point along the way, so the
change is heard gradually rather than as a join in the middle. Slow → Fast
climbs steadily from around 78 BPM to around 133.

## Settings

All under **Settings → Library → Playlist Engine**.

| Setting | What it does |
|---|---|
| Enabled | Turns the engine on, and starts the analysis. Off hides the feature and keeps what has been measured, so turning it back on costs nothing. |
| Analysis Depth | Thorough (40 seconds a track) or Quick (about 11). See above. |
| Playlist Length | How many tracks a generated playlist holds, 5 to 100. |
| Track Playlist | How much Play Similar varies between runs. |
| Mood Playlist | The same, for moods and journeys. |
| Continue Playing | Keeps the music going when any playlist runs out. |

**Predictable**, **Weekly** and **Variable** mean the same thing throughout.
Predictable gives the same playlist every time until the library changes;
Weekly gives the same one all week and a new one next week; Variable picks
between the nearest few at each step, so every run differs. None of them reach
further out for the difference — the playlist is drawn from the same
neighbourhood either way.

## Continue Playing

With this on, a playlist that runs out is extended rather than stopping. It
works for any playlist: an album, a saved one, a folder, or one the engine
built.

A playlist the engine built is continued on its own terms — a mood keeps
aiming at that mood rather than drifting away from it one extension at a time.
Anything else is continued from the track that just finished. A journey
continues in the mood it *ended* in, because it has arrived; walking its path
again would send you back to the beginning.

Nothing already in the playlist is added again.

## Keeping it current

**Settings → Library → Maintenance** gains two rows once the engine is on.

**Update Sound Analysis** measures tracks not covered yet, and retries any it
previously could not read — a file that defeated one machine's codecs often
decodes on another, so a retry is worth the seconds.

**Rebuild Sound Analysis** throws the lot away and measures everything again, which
is only worth it if the measurements themselves look wrong.

Both need the charger and hold the player while they run. They stop if the
charger is removed and carry on from where they stopped next time.

---

## The desktop tool

Measuring a few thousand tracks on the player is a night's work. The same
analysis on a PC is roughly an hour, and produces an identical index.

It ships inside the firmware, at `.rockbox/tools/soundscan.exe`, with the
codecs it needs beside it in `.rockbox/tools/codecs/`.

Connect the player, open a command prompt in that folder and run:

```
soundscan.exe
```

With no arguments it looks at the connected drives, finds the one with a
`.rockbox` folder on it, and asks you to confirm before touching anything. If
you have more than one player attached, or you would rather be explicit, name
the drive:

```
soundscan.exe E:\
```

It writes `.rockbox/db_sound.dat` on the player, which is the same file the
player writes and reads. Eject normally afterwards.

| Option | What it does |
|---|---|
| `-v` | One line per track, with its tempo, loudness and key. |
| `-n` | Measure but write nothing. |

Tracks already measured are left alone, so running it again after adding music
only measures what is new. A file it cannot decode is recorded as a failure so
the next run does not spend the same seconds discovering the same thing —
except that an update retries failures, because the reason for one is often
the machine rather than the file.

