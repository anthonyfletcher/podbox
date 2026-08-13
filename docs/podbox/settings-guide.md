# Settings guide

Every setting PodBox has, in the order you meet it, with what it does and
whether it is hidden by default.

Descriptions here are the same text the player shows: **Explain**, in a
setting's context menu, reads them from `/.rockbox/docs/settings-help.txt`, and
the tables below are generated from that file. If the two ever disagree, the
file is right and this document has gone stale.

---

## Standard and Everything

The settings tree has one shape, and **Settings Mode** at the foot of it decides
how much you see.

- **Standard** hides the rows marked **Adv** below. It is what a new install
  shows.
- **Everything** shows all of them.

A setting is marked advanced when the default is already the right answer for
someone who has not thought about it — because you would need to know how the
feature works to set it, because it shapes something whose on/off row is
elsewhere, because it exists for one piece of hardware, or because it is there
for theme authors.

Two things are never hidden, whatever the mode:

- **destructive actions**, because hiding Update Database would hide the fix for
  *my new album isn't showing up*;
- **the way back from a visible problem** — Dynamic Colors stays in Standard
  even though its tuning does not.

A screen whose rows are all advanced disappears in Standard rather than opening
empty. Database, Dialogs, Peak Meter and Artwork Filter are the four that do.

Nothing is unreachable in Standard: **Search**, at the top of the tree, finds
every setting whatever the mode, and opens it directly.

---

## Where things are

```
Settings
├─ Search…                     find a setting by name, or by what it is about
├─ Sound                       levels, tone, and the DSP effects
├─ Playback                    what happens as music plays, the now-playing
│                              screen, and playlists, bookmarks and resume
├─ Library                     the browsers, the database, artwork, the viewers,
│                              and Maintenance
├─ Appearance                  themes, fonts, colours, scrolling and the menus
├─ Battery & Power             backlight, brightness, sleep, disk and charging
├─ System                      USB, accessories, language, time, settings files
├─ Settings Mode               Standard or Everything
└─ Changed Settings…           everything no longer at its default
```

One screen appears in more than one place on purpose, as the same screen rather
than a copy: **Viewers** is under both Appearance and Library, because
the viewers are opened from the browsers and look the way the theme tells them
to.

---

## Library — Maintenance

Not settings but actions, gathered into one screen so the difference between
them can be read rather than remembered. Ordered cheapest first, and each asks
before it starts. All of them queue work for the background and return at once.

| Action | What it does |
|---|---|
| Update Database | Finds music added since the last scan. The fix for *my new album isn't showing up*. |
| Update Index | Refreshes the album and artist lists the carousels and charts read. |
| Update Cache | Fills in thumbnails that are missing, for artwork added to folders already scanned. |
| Rescan Documents & Images | Rebuilds the flat Documents and Images lists. Normally reruns itself after a USB session. |
| Rebuild Database | Discards the database and reads every file again. Slow. |
| Rebuild Index | Discards the album and artist lists and derives them again. |
| Rebuild Cache | Purges every thumbnail and regenerates from the original artwork. |

---

## Every setting

Rows marked **Adv** are hidden unless Settings Mode is Everything. Defaults are
what a Reset Settings produces, read from the player itself rather than typed
out here.

### Sound Settings

| Setting | What it does | Default | |
|---|---|---|---|
| Volume | Output level, and the same value the volume buttons move. Shown in decibels below maximum, so -25 dB is quieter than -10 dB. | -25 |  |
| Maximum Volume Limit | A ceiling the volume control cannot pass. Useful with sensitive earphones, where the usable range is squeezed into the bottom of the scale. | 6 |  |
| Bass | Lifts or cuts the low end. Bass Cutoff decides how far up it reaches. | 0 |  |
| Bass Cutoff | The frequency Bass works below. A lower number lifts only the deepest notes; a higher one reaches up into the lower mids and can muddy voices. | 1 | **Adv** |
| Treble | Lifts or cuts the high end. Treble Cutoff decides how far down it reaches. | 0 |  |
| Treble Cutoff | The frequency Treble works above. Lower reaches down towards voices, higher affects only the very top. | 1 | **Adv** |
| Balance | Shifts the output towards one ear. Meant for correcting a hearing difference or a worn earphone, not for effect. | 0 |  |
| Channel Configuration | What ends up in each ear: normal stereo, both channels mixed to mono, one channel in both ears, the two swapped, or karaoke, which cancels whatever is common to both and so removes most centred vocals. | stereo |  |
| Stereo Width | Widens or narrows the stereo image. Below 100% pulls it towards mono; above pushes it apart, which can sound impressive briefly and tiring for long. | 100 |  |
| Fade on Stop/Pause | Ramps the volume down and up on stop and pause instead of cutting, which avoids the click a hard stop can produce. | on |  |
| Dithering | Adds a very quiet noise when reducing bit depth, which trades a little hiss for the absence of a particular kind of distortion on very quiet passages. | off | **Adv** |
| Auditory Fatigue Reduction | Tames the harshness that makes long listening tiring, mostly in the upper mids. Subtle by design; if you can hear it working it is set too high. | off |  |

### Sound Settings — Crossfeed

| Setting | What it does | Default | |
|---|---|---|---|
| Crossfeed | Bleeds a little of each channel into the other, with a delay, so headphones sound less like two separate sources bolted to your head. | off |  |
| Direct Gain | The level of the sound that reaches the ear it was mixed for. Lowering it makes the effect stronger by making the crossfed path relatively louder. | -15 | **Adv** |
| Cross Gain | The level of the sound crossing to the other ear. The main strength control: closer to the direct gain means a narrower, more speaker-like image. | -60 | **Adv** |
| High-Frequency Attenuation | How much the crossed-over sound is dulled. A real head blocks high frequencies far more than low ones, so this is what makes the effect sound natural rather than like an echo. | -160 | **Adv** |
| High-Frequency Cutoff | The frequency above which the crossfed signal -- the part bled into the opposite ear -- starts being dulled. Lower values roll it off earlier and give a warmer, more distant effect. | 700 | **Adv** |

### Sound Settings — Equalizer

| Setting | What it does | Default | |
|---|---|---|---|
| Enable EQ | Master switch for the ten-band equaliser. Load EQ is usually a better starting point than setting the bands by hand. | off |  |
| Precut | Attenuates everything before the equaliser runs, to leave room for bands you have boosted. | 0 | **Adv** |

### Sound Settings — Haas Surround

| Setting | What it does | Default | |
|---|---|---|---|
| Haas Surround | Delay in milliseconds for the Haas effect, which widens the image by delaying part of the signal rather than by changing its level. Zero is off. | off |  |
| Balance | How the widened signal is weighted between left and right. | 35 | **Adv** |
| f(x1) | The upper frequency limit of the Haas effect. | 3400 | **Adv** |
| f(x2) | The lower frequency limit of the Haas effect. | 320 | **Adv** |
| Side Only | Applies the effect only to the difference between the channels, leaving anything mixed to the centre -- usually the voice -- untouched. | off | **Adv** |
| Dry / Wet Mix | How much of the effect is blended with the untouched signal. Lower values keep the original intact and add a suggestion of width. | 50 | **Adv** |

### Sound Settings — Perceptual Bass Enhancement

| Setting | What it does | Default | |
|---|---|---|---|
| Perceptual Bass Enhancement | Adds harmonics above notes that are too low for small drivers to reproduce, so the ear infers the bass that is not physically there. Strength is a percentage; it distorts if pushed. | 0 |  |
| Precut | Attenuates the signal so the harmonics the effect adds do not push it into clipping. | -25 | **Adv** |

### Sound Settings — Compressor

| Setting | What it does | Default | |
|---|---|---|---|
| Threshold | The level above which compression starts, and the switch for the whole effect -- at 0 dB nothing is compressed. | 0 | **Adv** |
| Makeup Gain | Compression makes loud parts quieter, so the whole track ends up quieter. This puts the level back. | auto | **Adv** |
| Ratio | How hard the signal is held down once past the threshold. 2:1 is gentle, Limit allows essentially nothing above it. | 4:1 | **Adv** |
| Knee | How abruptly compression starts once the signal passes the threshold. | soft knee | **Adv** |
| Attack Time | How quickly compression clamps down after a loud passage begins. Short catches transients and can dull drums; longer lets the initial hit through. | 5 | **Adv** |
| Release Time | How quickly it lets go again. Too short pumps audibly on bass notes; too long leaves quiet material compressed after the loud part has passed. | 500 | **Adv** |

### Sound Settings — Replaygain

| Setting | What it does | Default | |
|---|---|---|---|
| Replaygain Type | Uses the loudness information many rippers write into files, so albums recorded at different levels play at a similar volume. | track shuffle |  |
| Prevent Clipping | Backs the gain off when applying it would push the track into clipping, rather than clipping it. | off | **Adv** |
| Pre-amp | A fixed adjustment applied on top of whatever Replaygain works out. | 0 | **Adv** |

### Playback

| Setting | What it does | Default | |
|---|---|---|---|
| Shuffle | Plays the current list in a random order. Changing it reshuffles what is already playing rather than waiting for the next list. | off |  |
| Repeat | What happens at the end of the list: stop, start again, repeat the one track, reshuffle and go again, or loop between two marks you set. | off |  |
| Play Selected First | Choosing a track starts from that track rather than from the top of the folder, with the rest of the folder queued behind it. | on |  |
| Single Mode | Stops after the current track, album, artist, genre or playlist instead of carrying on. Useful for falling asleep to one record. | off |  |
| Party Mode | Anything selected is added to the end of the queue instead of replacing it, so a second person cannot wipe out what is already lined up. | off |  |
| Cuesheet Support | Reads .cue files, so an album ripped as one long file still shows track names and can be skipped through. | off | **Adv** |
| Auto-Change Directory | At the end of a folder, move to the next one rather than stopping. Random picks one at random instead. | off |  |
| Constrain Auto-Change | When playback moves on to the next folder by itself, keeps that move inside the folder you started in -- so an album that runs on goes to its sibling rather than wandering off into the rest of the library. | off | **Adv** |
| Skip Length | Makes skip jump a fixed amount of time instead of a whole track. For long podcasts and mixes, where a track is an hour. | track | **Adv** |
| Prevent Track Skipping | Disables skipping entirely. For handing the player to someone else, or a pocket that presses buttons. | off | **Adv** |
| Rewind Across Tracks | Rewinding past the start of a track continues into the previous one instead of stopping at the beginning. | off | **Adv** |
| Rewind Before Resume | Backs up a few seconds when resuming, so you hear a little of what came before rather than restarting mid-word. | 0 | **Adv** |
| Rewind on Pause | Backs up a few seconds every time you unpause, so you hear a little of what came before rather than restarting mid-word. | 0 | **Adv** |
| Anti-Skip Buffer | How much audio is read ahead and held in memory before the disk is allowed to stop. | 5 | **Adv** |
| Track Skip Beep | A short beep when skipping tracks, as feedback that the press registered. | off | **Adv** |
| Frequency | The sample rate the output runs at. Auto follows the file and is right almost always; forcing a rate resamples everything. | auto | **Adv** |
| Logging | Records what was played and when. On feeds the listening statistics; the Last.fm setting writes a scrobble log a computer can upload. | on | **Adv** |
| Album Art | Where cover art comes from: the thumbnail cache, files beside the music, or tags embedded in the files. Prefer Cache is fastest. | prefer cache |  |

### Playback — Fast-Forward/Rewind

| Setting | What it does | Default | |
|---|---|---|---|
| FF/RW Min Step | How far the first press of fast forward or rewind moves. Smaller is better for finding a moment in a song, larger for crossing a long file. | 1 | **Adv** |
| FF/RW Accel | How quickly held seeking speeds up. Faster crosses an hour-long file in a moment but overshoots a three-minute song. | normal | **Adv** |

### Playback — Crossfade

| Setting | What it does | Default | |
|---|---|---|---|
| Enable Crossfade | Overlaps the end of one track with the start of the next. | off |  |
| Fade-In Delay | How long to wait after the incoming track starts before its volume begins to rise. | 0 | **Adv** |
| Fade-In Duration | How long the incoming track takes to reach full volume. | 2 | **Adv** |
| Fade-Out Delay | How long the outgoing track holds its level before starting to fall. | 0 | **Adv** |
| Fade-Out Duration | How long the outgoing track takes to fall silent. Longer overlaps blur the join more. | 2 | **Adv** |
| Fade-Out Mode | Crossfade dips both tracks through the join. Mix holds the total level roughly constant, which sounds fuller but muddier where both tracks are busy. | crossfade | **Adv** |

### Playback — Pause on Headphone Unplug

| Setting | What it does | Default | |
|---|---|---|---|
| Pause on Headphone Unplug | Pauses when the headphones are pulled out, so the music does not carry on without you. Can also resume automatically when they go back in. | off |  |
| Disable resume on startup if phones unplugged | Stops the player resuming at power-on when nothing is plugged in, which otherwise plays into a dock or an empty socket. | off | **Adv** |

### Playback — Playlists

| Setting | What it does | Default | |
|---|---|---|---|
| Sort Playlists | The order saved playlists are listed in: by name, or by when they were made. | alpha |  |
| Recursively Insert Directories | Adding a folder to a playlist also adds everything in its subfolders. | on |  |
| Warn When Erasing Dynamic Playlist | Asks before replacing a queue you have built up by hand, which is otherwise easy to lose with one Select. | on |  |
| Keep Current Track When Replacing Playlist | Leaves the playing track in place when a new selection replaces the queue, so the music does not stop mid-song. | on | **Adv** |
| Show Shuffled Adding Options | Adds "insert shuffled" entries to the playlist context menus. | on | **Adv** |
| Show Queue Options | Adds the queueing entries -- which play once and are then dropped -- to the context menus. | off | **Adv** |
| Show Icons | Draws icons beside the entries in the playing queue. | on |  |
| Show Indices | Numbers the entries in the playing queue. | on |  |
| Track Display | Whether the queue shows filenames or the artist and title from the tags. | track name |  |

### Playback — Bookmarks

| Setting | What it does | Default | |
|---|---|---|---|
| Bookmark on Stop | Saves your position when you stop, so you can come back to it. Ask prompts each time; the recent-only settings save without adding a bookmark file to the folder. | off | **Adv** |
| Update on Stop | Overwrites the existing bookmark for a folder instead of adding another, so the list stays one entry per book rather than one per session. | off | **Adv** |
| Load Last Bookmark | Offers the saved position when you next open a folder that has one. | off | **Adv** |
| Maintain a List of Recent Bookmarks? | Keeps a list of the last places you stopped, reachable from the main menu, as well as bookmarks stored beside the music. | off | **Adv** |

### Playback — Automatic Resume

| Setting | What it does | Default | |
|---|---|---|---|
| Automatic Resume | Remembers how far through each track you were, and returns there rather than starting from the beginning. | off |  |
| Resume on Automatic Track Change | Whether an automatic position is also remembered when a track changes on its own. Custom limits it to folders you nominate, which is how to keep it for podcasts without applying it to albums. | never | **Adv** |

### Playback — What's Playing Screen

| Setting | What it does | Default | |
|---|---|---|---|
| Artwork | Which picture the now-playing screen shows. Auto uses the artist photograph if you arrived through the artist menu, and the album cover otherwise. | album |  |
| Default Browser | Which browser the root and the now-playing screen return to: the database or the files. | database |  |
| Select Action | Where Select goes from the now-playing screen. | default | **Adv** |

### Library — Files

| Setting | What it does | Default | |
|---|---|---|---|
| Sort Case Sensitive | Whether uppercase sorts separately from lowercase, so that "Zoo" can come before "apple". | off |  |
| Sort Directories | The order folders are listed in: by name, or by when they were last written. | alpha |  |
| Sort Files | The order files are listed in, including grouping by type. | alpha |  |
| Interpret Numbers When Sorting | Whether "track2" comes before "track10" by reading the digits as a number, or after it by comparing them one character at a time. | numbers |  |
| Show Files | Which files the browser shows: everything, only what can be opened, only music, or only playlists. The quickest way to make a cluttered folder readable. | supported |  |
| Show Filename Extensions | Whether names are shown with their extension. Hiding it is tidier; showing it helps when several files share a name. | view_all |  |
| Follow Playlist | Opens the browser at the folder the playing track came from, rather than where you last were. | off |  |
| Show Path | Puts the current folder, or the whole path to it, in the title bar. | current directory |  |
| Hotkey | What the hotkey button does in the file browser. | Off, | **Adv** |

### Library — Music

| Setting | What it does | Default | |
|---|---|---|---|
| Sort Albums By | The order album lists appear in throughout the Music menu: by name, or by release year with either end first. | name |  |
| Maximum Results | How many results a search keeps. More results take longer to scroll than to find; another letter is usually quicker. | 50 |  |
| Minimum Letters | How many letters must be typed before searching starts. Raise it if one-letter searches return more than they are worth. | 1 |  |
| Result Order | Which of tracks, albums and artists is listed first in search results. | track album artist |  |

### Library — Carousel

| Setting | What it does | Default | |
|---|---|---|---|
| On Album Select | What Select does on a cover: open the album's track list, or start playing it. | show tracks |  |
| Show Album Title | Whether the album name is drawn over the covers, and where. | both bottom |  |
| Show Year in Album Title | Adds the release year to the caption. | off |  |
| Background | Which theme colour fills the screen behind the covers. | background |  |
| Status Bar | Draws the status bar over the carousel. Off gives the covers the whole screen. | off |  |
| Year Sort Order | Whether sorting by year puts the oldest or the newest first. | ascending | **Adv** |
| Sort Albums By | The order covers appear in: by artist, by year, or by album name. | artist+name |  |
| Sort Artists By | The order artist portraits appear in: by name, or most played first. | name |  |
| View Mode | 3D angles the covers away on both sides. Flat lays them face-on and the same size, in two piles either side of the current one. | 3d |  |
| 3D Centre Margin | The gap between the front cover and its neighbours in the 3D view. | 0 | **Adv** |
| 3D Slide Tuck | How far the side covers stack back behind the front one in the 3D view. | 32 | **Adv** |
| 3D Parallel Slides | Draws the side covers face-on rather than angled, which reads as a row of covers rather than a tunnel. | on | **Adv** |
| 3D Transition Speed | How quickly the 3D view settles after scrolling. No effect in Flat, which times itself off the wheel instead. | 325 | **Adv** |
| Flat Pile Fade | How far the two piles in the Flat view are blended towards the background, so the cover in the middle stands out. Zero leaves them solid. | 0 | **Adv** |
| Flat Pile Offset | How far below the middle cover the Flat view's piles sit. A cover eases down onto its pile as it leaves and back up as it arrives. | 0 | **Adv** |
| Scroll Speed | How far a flick of the wheel carries. Affects both view modes. | 175 |  |

### Library — Database

| Setting | What it does | Default | |
|---|---|---|---|
| Load to RAM | Keeps the database in memory instead of reading it from disk as you browse. | quick | **Adv** |
| Scan on Startup | Rescans the library at every boot. Mostly redundant if Scan on Eject is on, since that is when the music can actually have changed, and it costs time at every start. | off | **Adv** |
| Scan on Eject | Rescans after a USB session, which is the moment new music normally arrives. | on | **Adv** |
| Autocommit on Startup | Finishes a scan that was cut short by a flat battery or an unplug. Off asks first instead. | on | **Adv** |
| Gather Runtime Data | Records play counts, ratings and when each track was last played. What the listening statistics are built from. | on | **Adv** |
| Select Directories to Scan | Restricts scanning to chosen folders, so spoken-word or sample libraries stay out of the music database. | / | **Adv** |
| Write Debug Log | Writes scan progress to a log file. For working out why a track is missing from the database. | off | **Adv** |

### Library — Art Cache

| Setting | What it does | Default | |
|---|---|---|---|
| Fast Build | Decodes each image once for the largest thumbnail and derives the smaller sizes from it, instead of decoding once per size. | off | **Adv** |
| Write Debug Log | Writes thumbnail generation to a log file. For working out why a particular album has no art. | off | **Adv** |

### Appearance

| Setting | What it does | Default | |
|---|---|---|---|
| Font | The typeface used throughout the interface. A larger font is easier to read and fits fewer rows on screen. | none |  |

### Appearance — Skins

| Setting | What it does | Default | |
|---|---|---|---|
| While Playing Screen | The skin drawn while music is playing. Part of a theme; loading a theme sets it. | none |  |
| Base Skin | The base skin -- the frame drawn behind lists and menus. Part of a theme. | none |  |
| Backdrop | An image drawn behind everything. Loading a theme replaces it, and a theme that names none clears it rather than keeping the last one. | none |  |
| Bold Font | An optional bold companion to the interface font, used where a screen wants emphasis. Unset means the regular font is used for both. | none | **Adv** |
| Iconset | The image file the list icons are taken from. A theme-author setting. | /.rockbox/icons/tango_icons.16x16.bmp | **Adv** |
| Viewers Iconset | The icon set used for file types in the browser. A theme-author setting. | /.rockbox/icons/tango_icons_viewers.16x16.bmp | **Adv** |
| Filetype Colours | A file naming a colour per extension, so the browser can colour-code types. A theme-author setting. | none | **Adv** |
| UI Viewport | The rectangle a theme reserves for lists, so its own decoration is not drawn over. A theme-author setting. | none | **Adv** |
| Progress Bar Radius | Corner rounding of the progress bar, in pixels. A theme-author setting: most themes draw their own bar and ignore it. | 2 | **Adv** |

### Appearance — Colours

| Setting | What it does | Default | |
|---|---|---|---|
| Foreground Colour | The colour of text and lines. Reset by loading a theme. | e7f3ef |  |
| Background Colour | The colour behind them. Reset by loading a theme. | 000c21 |  |
| Line Selector Start Colour | The colour at the top of the graduated selector bar. | ffeb9c |  |
| Line Selector End Colour | The colour at the bottom of the graduated bar behind the highlighted row. Setting it the same as the start colour gives a flat bar instead of a fade. | b58e00 |  |
| Line Selector Text Colour | The colour of the text on the highlighted row, which has to read against the bar rather than against the background. | 000000 |  |
| Separator Colour | The colour of the rule drawn between rows in lists. Only visible where the separator has a height to draw. | 848284 |  |
| Dynamic Colors | Recolours the interface from the artwork of whatever is playing. A skin not written for it will look wrong, since it cannot know what its colours will become. | off |  |
| Dialog Colour Mode | How the confirmation and message boxes are coloured. Auto, the default, uses the theme's own two colours plus one accent on the selected button -- and the accent follows the album while Dynamic Colors is running. Off is the same two colours with no accent: the selected button is simply drawn inverted, which is what Rockbox has always done. On ignores both and uses the nine colours below, which appear only in that mode. | auto |  |
| Box Shadow Colour | The colour of the drop shadow behind confirmation and message boxes. Black by default rather than a theme colour, because its job is to contrast with the box whatever the theme is doing. | 000000 |  |
| Box Text | The text colour inside a dialog. Only used when Dialog Colour Mode is On. | e7f3ef |  |
| Box Background | The fill colour inside a dialog. Only used when Dialog Colour Mode is On. | 000c21 |  |
| Box Border | The colour of a dialog's own border. Only used when Dialog Colour Mode is On. | e7f3ef |  |
| Button Text | The text colour of an unselected dialog button. | e7f3ef |  |
| Button Background | The fill colour of an unselected dialog button. | 000c21 |  |
| Button Border | The border colour of an unselected dialog button. | e7f3ef |  |
| Selected Button Text | The text colour of the selected dialog button. | 000c21 |  |
| Selected Button Background | The fill colour of the selected dialog button, which is what marks it as chosen. | e7f3ef |  |
| Selected Button Border | The border colour of the selected dialog button. | e7f3ef |  |

Several rows here are listed only while something reads them, whatever Settings
Mode says. The nine palette rows -- everything from Box Text down -- need
Dialog Colour Mode on On. Line Selector Colours needs a selector type that
draws a coloured bar, and its Secondary Colour needs the gradient. Separator
Colour needs Line Separator set above zero, and Box Shadow Colour needs a
shadow to colour.

### Appearance — Elements

| Setting | What it does | Default | |
|---|---|---|---|
| Show Icons | Draws an icon beside each row in lists and menus. Turning them off gives the text more room. | on |  |
| Status Bar | Whether the clock and battery strip is drawn, and at which edge. | top |  |
| Scroll Bar | Whether a scroll bar is drawn beside lists, and on which side. | left |  |
| Scroll Bar Width | How wide the scroll bar beside lists is, in pixels. | 6 | **Adv** |
| Volume Display | Whether the status bar shows the volume as a bar or as a number. | graphic |  |
| Battery Display | Whether the status bar shows the battery as an icon or as a percentage. | graphic |  |
| Line Selector Type | How the highlighted row is marked: a pointer beside it, the row inverted, or a bar behind it in a flat or graduated colour. | bar (gradient) |  |
| Line Separator | The thickness of the rule between rows, in pixels. Auto follows the font, and off draws none. | off | **Adv** |
| Album Art Rows | Draws album thumbnails beside the rows in the database browser. Needs a theme that supports artwork in lists, or the rows are tall and empty. | off |  |
| Artist Art Rows | Draws artist photographs beside the rows in the database browser. Needs a theme that supports artwork in lists, or the rows are tall and empty. | off |  |
| Album Art Row Height | Row height in the database browser when album or artist art is shown beside rows. A theme-author setting -- it has to match the artwork the theme draws. | 52 | **Adv** |
| Filter 1 | First of three image adjustments applied to artwork before it is drawn. A theme-author setting: a theme that wants a treatment names it, and one that does not should have none. | off | **Adv** |
| Filter 2 | Second image adjustment in the chain, applied after the first. | off | **Adv** |
| Filter 3 | Third and last image adjustment in the chain. | off | **Adv** |

### Appearance — Dialogs

| Setting | What it does | Default | |
|---|---|---|---|
| Box Border Width | The thickness of that border, in pixels. Zero draws none. | 2 | **Adv** |
| Box Margin | How far the dialog is inset from the edges of the screen. | 10 | **Adv** |
| Box Shadow | A solid drop shadow offset down and to the right, which lifts the box off whatever is behind it. Zero turns it off. | 4 | **Adv** |
| Button Border Width | The thickness of a dialog button's border, in pixels. | 2 | **Adv** |
| Button Corner Radius | How rounded the corners of a dialog button are. Zero is square. | 0 | **Adv** |

### Appearance — Scrolling

| Setting | What it does | Default | |
|---|---|---|---|
| Scroll Speed | How quickly text too long for its line moves. | 9 |  |
| Scroll Start Delay | How long text waits before it starts moving, so a row can be read before it slides. | 1000 |  |
| Scroll Step Size | How many pixels each step of scrolling text moves. One is smooth and costs more work; larger steps are jerkier and cheaper. | 6 | **Adv** |
| Bidirectional Scroll Limit | Text narrower than this share of the line bounces back and forth instead of scrolling off one side and round again. | 50 | **Adv** |
| Screen Scroll Step Size | How many pixels each step moves when a whole screen slides, rather than when text scrolls within one line. Smaller is smoother and costs more work. | 16 | **Adv** |
| Paged Scrolling | Lists move a screenful at a time instead of a row at a time. | off |  |
| List Wraparound | Passing the last item returns to the first, rather than stopping. | on |  |
| List Order | Whether lists are traversed from the top down or the bottom up. | ascending | **Adv** |
| Screen Scrolls Out of View | Lets a line of scrolling text carry on past the edge of its viewport instead of stopping at it. Looks better on a theme whose list has room to spare, and clips awkwardly on one that does not. | No | **Adv** |
| Disable Main Menu Scrolling | Stops long entries scrolling on the main menu, leaving them truncated. The main menu is glanced at rather than read, and text moving under the cursor there is more distracting than helpful. | No | **Adv** |
| Hold Left/Right to Scroll a List | Holding left or right scrolls a list rather than repeating whatever those buttons do on the current screen. | on | **Adv** |

### Appearance — Peak Meter

| Setting | What it does | Default | |
|---|---|---|---|
| Peak Release | How quickly the level meter falls back after a peak. Slower is easier to read, faster is more honest about the signal. | 8 | **Adv** |
| Peak Hold Time | How long the meter holds at a peak before starting to fall. | 500 | **Adv** |
| Clip Hold Time | How long a clipping indication stays on screen once triggered. | 60 | **Adv** |
| Logarithmic (dB) | Whether the meter's scale is in decibels, which matches how loudness is perceived, or a straight percentage. | on | **Adv** |
| Minimum of Range | The quiet end of the meter's range. | 60 | **Adv** |
| Maximum of Range | The loud end of the meter's range. | 0 | **Adv** |

### Appearance / Library — Viewers

| Setting | What it does | Default | |
|---|---|---|---|
| Colour Mode | Whether the text viewer follows the theme, inverts it, or uses plain black on white or white on black. | white on black |  |
| Margin | Insets the text from the edges of the screen, which is easier to read at the cost of a few characters per line. | on |  |
| Line Spacing | Extra pixels between lines. A little space makes a wall of text much easier to follow. | 0 | **Adv** |
| Page Number | Shows a page counter at the foot. | off |  |
| Text Viewer Font | A font used only by the text viewer, so reading can use a different typeface from the menus. Unset uses the interface font. | /.rockbox/fonts/22-Literata.fnt |  |
| Colour Mode | Whether the lyrics screen follows the theme, inverts it, or uses plain black on white or white on black. | theme |  |
| Alignment | Whether lyric lines are aligned left, centred or right. | centre |  |
| Line Spacing | Extra pixels between lyric lines. | 2 | **Adv** |
| Previous Line | How far lines that have already been sung fade back. Lower makes the current line stand out more. | 30 | **Adv** |
| Next Line | How far lines still to come fade back. | 55 | **Adv** |
| Animation | How the display moves from one line to the next: instantly, or scrolling at one of three speeds. | normal |  |
| Highlight Sung Words | Highlights individual words as they are sung, where the file carries word timings. Files with only line timings are unaffected. | on |  |
| Keep Backlight On | Holds the backlight on while lyrics are showing, since a screen that keeps going dark defeats the point. Costs battery. | on |  |
| Lyrics Font | A font used only by the lyrics screen. Unset uses the interface font. | none |  |

### Battery & Power

| Setting | What it does | Default | |
|---|---|---|---|
| Brightness | Panel brightness. Second only to the backlight timeout as a drain on the battery. | 32 |  |
| Idle Poweroff | How long the player sits idle, not playing, before switching itself off. Zero never does. | 10 |  |
| Disk Spindown | How long the drive sits idle before it is allowed to stop. | 5 | **Adv** |
| Storage Mode | Tells power management what kind of drive is fitted, which decides whether it is worth spinning down and how aggressively. | auto | **Adv** |
| Charge During USB Connection | Whether the player charges from a USB connection. Force charges even from a port that does not advertise enough current, which not every port tolerates. | force |  |
| Battery Capacity | The capacity of the cell actually fitted, in mAh. | 400 | **Adv** |

### Battery & Power — Backlight

| Setting | What it does | Default | |
|---|---|---|---|
| Backlight | How long the backlight stays on after the last button press. The single biggest lever on battery life. | on |  |
| Backlight (While Plugged In) | How long the backlight stays on after the last button press while the player is charging. Kept separate from the battery figure because there is less reason to be frugal on the mains. | on |  |
| Backlight on Hold | What the backlight does while the hold switch is on: behave normally, stay off, or stay on. | off | **Adv** |
| Caption Backlight | Wakes the backlight briefly at each track change, so you can see what started without touching anything. Costs battery on a long album. | off | **Adv** |
| Backlight Fade In | How long the backlight takes to come up rather than snapping on. | 300 ms | **Adv** |
| Backlight Fade Out | How long it takes to go down. A slow fade is gentler in the dark. | 2000 ms | **Adv** |
| First Buttonpress Enables Backlight Only | The press that wakes the screen does nothing else, so you cannot change a setting you could not see. Costs one extra press each time. | on | **Adv** |
| Sleep (After Backlight Off) | Powers the panel down entirely a while after the backlight goes out. Saves more than the backlight alone, and costs a moment to wake. | 5 | **Adv** |

### Battery & Power — Sleep Timer

| Setting | What it does | Default | |
|---|---|---|---|
| Default Sleep Timer Duration | How long the sleep timer runs for, after which the player switches itself off. Sets the length only; starting the timer is a separate entry on the same screen. | 30 |  |
| Start Sleep Timer on Boot | Starts the sleep timer automatically at power-on, for a player used mostly to fall asleep to. | off | **Adv** |
| Restart Sleep Timer on Keypress | Any button press resets the countdown, so the timer only fires once you have actually stopped touching it. | off | **Adv** |

### Battery & Power — Car Adapter

| Setting | What it does | Default | |
|---|---|---|---|
| Car Adapter Mode | Pauses when external power is cut and resumes when it returns, so the player follows the car's ignition. | off | **Adv** |
| Delay Before Resume | How long to wait after power returns before resuming, so a stall or a restart does not start the music. | 5 | **Adv** |

### System

| Setting | What it does | Default | |
|---|---|---|---|
| Quick Screen | Whether a long press opens the quick screen or the shortcuts menu. | off |  |
| Directory Cache | Keeps the layout of the disk in memory so the file browser does not have to read it each time. | on |  |
| Volume Adjustment Mode | Direct moves the volume in fixed decibel steps. Perceptual divides the range into steps that sound evenly spaced, which suits the bottom of the scale where a decibel is a large change. | direct | **Adv** |
| Number of Volume Steps | How many steps Perceptual mode divides the range into. More steps mean finer control and more presses to cross the range. Direct mode never reads it, which is why the row is listed only in Perceptual. | 50 | **Adv** |
| Start Screen | Which screen opens at power-on. | root |  |
| Show Shutdown Message | Shows a message while shutting down, rather than the screen simply going dark. | on | **Adv** |
| Clear Settings on Reset-Button Hold | Holding a button during startup clears the settings. A way back from a configuration that makes the player unusable. | off | **Adv** |
| Settings Mode | How much of the settings tree is shown. Standard hides the advanced rows; Everything shows all of them. | standard |  |
| Show Debug Menu | Reveals the debug screens under System. They read hardware and internal state; nothing there is needed in normal use. | off | **Adv** |

### System — USB

| Setting | What it does | Default | |
|---|---|---|---|
| USB Mode | What a USB connection does: present the disk to the computer, or charge only. Charge only is useful with a car or a plug that would otherwise interrupt playback. | mass storage |  |
| USB HID | Presents the player as a keyboard or remote control to the computer, so its buttons can drive playback there. | off | **Adv** |
| USB Keypad Mode | What the buttons send while acting as a USB device: media keys, a mouse, or presentation controls. | multimedia | **Adv** |
| USB-DAC | Lets the player act as a USB sound card for a computer. | Never | **Adv** |

### System — Accessories

| Setting | What it does | Default | |
|---|---|---|---|
| Serial Bitrate | The speed of the dock connector's serial line. Auto suits every accessory that follows the standard. | auto | **Adv** |
| Accessory Power Supply | Powers the accessory pin on the dock connector. Needed by some adapters, and a constant drain if nothing is attached. | on | **Adv** |
| Line Out | Enables the dock's line output, which bypasses the volume control and feeds an amplifier at a fixed level. | on | **Adv** |

### System — Keyclick

| Setting | What it does | Default | |
|---|---|---|---|
| Headphone Keyclick | A click through the headphones on each button press. Feedback in a pocket, irritating on a quiet passage. | off | **Adv** |
| Speaker Keyclick | A click from the player own speaker on each button press, rather than through the headphones. Audible without anything plugged in. | on | **Adv** |
| Keyclick Repeats | Whether auto-repeat -- a held button -- clicks each time or only once. | off | **Adv** |

### System — Limits

| Setting | What it does | Default | |
|---|---|---|---|
| Max Entries in File Browser | The largest number of entries loaded from one folder. A folder with more is truncated rather than refused. | 5000 | **Adv** |
| Max Playlist Size | The largest number of tracks a playlist may hold. Building very large lists is slow on this hardware, which is why the limit is lower here than on a computer. | 10000 | **Adv** |
| Glyphs to Cache | How many characters of the font are kept in memory at once. | 250 | **Adv** |

### System — Language & Text

| Setting | What it does | Default | |
|---|---|---|---|
| Language | The language of the interface. Loading one replaces the built-in English. |  |  |
| Default Codepage | The character set assumed for tags that do not say which they use. Wrong guesses show accented characters as nonsense; Unicode is right for anything tagged recently. | utf-8 | **Adv** |
| Time Format | Whether times are shown on a 12- or 24-hour clock. | 12hour |  |

### System — Voice

| Setting | What it does | Default | |
|---|---|---|---|
| Voice Menus | Speaks menu entries aloud. Needs a voice file on the player. | on |  |
| Voice Directories | Whether folder names are spoken, and whether by number or spelled out. | off | **Adv** |
| Voice Filenames | Whether filenames are spoken, and how. | off | **Adv** |
| Use Directory .talk Clips | Plays a recorded .talk clip for a folder where one exists, instead of speaking the name. | off | **Adv** |
| Use File .talk Clips | Plays a recorded .talk clip for an individual file where one exists, instead of speaking its name. | off | **Adv** |
| Say File Type | Speaks the file's type along with its name. | off | **Adv** |
| Voice Prompt Volume | How loud speech is relative to the music underneath it. | 100 | **Adv** |

---

## Settings a theme resets

Loading a theme returns every setting that describes the look to its default
before reading the theme's own `.cfg`, so a theme that says nothing about a
setting cannot inherit the last theme's answer. Without that, the same theme
renders differently depending on what was loaded before it.

That covers everything in Appearance, plus the artwork rows, the carousel's
geometry, the scrolling settings and the dialog chrome — every setting carrying
`F_THEMESETTING` or `F_THEMERESET` in `settings_list.c`.

**What you set by hand is not lost.** Changes made through the settings screens
while a theme is loaded are kept in `/.rockbox/themes/<name>.usercfg`, read
straight after the theme, so they survive the reset. *Forget My Changes*, at the
foot of Appearance, throws them away and reloads the theme as its author
shipped it.

A `.cfg` counts as a theme, and so triggers the reset, only if it names a font.
One that does not is considered a patch, and is applied on top of what is already 
there.

See [`theme-guide.md`](theme-guide.md) §2 for this from the theme author's side.
