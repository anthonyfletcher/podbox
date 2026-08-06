# Settings guide

This guide is provided to help you navigate the settings available in PodBox.

The settings come in two shapes — a short **Basic** page, which is what a fresh
install shows, and the full **Advanced** tree. The Basic page's last entry opens 
the full tree.

Defaults below are what a **Reset Settings** produces. They are not always what
your player is showing, for two reasons worth knowing before you go looking for
a bug:

- The shipped `config.cfg` sets some values at first boot, so the device starts
  from that rather than from the compiled defaults.
- Loading a theme resets number of settings — see [§9](#9-settings-a-theme-resets).

Where the two players differ, the entry says so: **(5G)** is the iPod Video,
**(6G)** the iPod Classic.

---

## 1. Basic and Advanced

The Basic page, in order:

| Setting |
|---|
| Volume |
| Bass |
| Treble |
| Shuffle |
| Repeat |
| Brightness |
| Backlight |
| Load EQ |
| Enable EQ |
| Load Theme |
| Font |
| Idle Poweroff |
| Main Menu Settings |
| **Advanced** |

Each of these is the *same* setting as its counterpart in the full tree, not a
copy — change Bass here and Sound Settings shows the new value. Entries marked
**●** in the sections below are the ones that appear on this page.

---

## 2. Sound Settings

| Setting | What it does | Default |
|---|---|---|
| Volume ● | Output level. The same value the volume buttons move. | −25 dB |
| Maximum Volume Limit | Ceiling the volume control cannot pass. | +6 dB **(5G)**, +12 dB **(6G)** |
| Bass ● | Tone control, low shelf. | 0 dB |
| Bass Cutoff | Corner frequency Bass acts below. | 1 **(5G)**, 2 **(6G)** |
| Treble ● | Tone control, high shelf. | 0 dB |
| Treble Cutoff | Corner frequency Treble acts above. | 1 |
| Balance | Left/right level offset. | 0 |
| Channel Configuration | Stereo, mono, mono left/right, karaoke, swap. | Stereo |
| Stereo Width | Widens or narrows the stereo image. | 100% |
| **Crossfeed** ▸ | Bleeds each channel into the other so headphones sound less "in-head". | |
| ├ Crossfeed | Off / Simple (Meier) / Custom. | Off |
| ├ Direct Gain | Level of the direct (same-ear) path. | −1.5 dB |
| ├ Cross Gain | Level of the crossfed path. | −6.0 dB |
| ├ High-Frequency Attenuation | How far the crossfed path is rolled off. | −16.0 dB |
| └ High-Frequency Cutoff | Where that roll-off starts. | 700 Hz |
| **Equalizer** ▸ | 10-band parametric EQ. | |
| ├ Load EQ ● | Load one of the bundled presets. Cheaper than learning the bands. | *(action)* |
| ├ Enable EQ ● | Master on/off. | Off |
| ├ Graphical EQ | Draggable band-gain editor. | *(screen)* |
| ├ Simple EQ | Gain only, one row per band. | all bands 0 dB |
| ├ Advanced EQ | Gain, centre/cutoff frequency and Q per band. | 32–16000 Hz, Q 0.7–1.0 |
| ├ Precut | Global attenuation, to leave headroom for boosted bands. | 0 dB |
| ├ Save EQ Preset | Write the current curve to a `.cfg`. | *(action)* |
| └ Reset EQ | Restore flat defaults. | *(action)* |
| Dithering | Adds dither noise on bit-depth reduction. Inaudible in practice. | Off |
| **Haas Surround** ▸ | Pseudo-surround by delaying one channel. | |
| ├ Haas Surround | Delay in ms; 0 is off. | Off |
| ├ Balance | Wet-signal L/R weighting. | 35% |
| ├ f(x1) / f(x2) | Upper and lower band limits of the effect. | 3400 Hz / 320 Hz |
| ├ Side Only | Apply to the side (difference) signal only. | Off |
| └ Dry / Wet Mix | Effect blend. | 50% |
| **Perceptual Bass Enhancement** ▸ | Adds harmonics so small drivers imply bass they cannot produce. | |
| ├ Perceptual Bass Enhancement | Strength, 0–100%. | 0% |
| └ Precut | Attenuation to stop the effect clipping. | −2.5 dB |
| Auditory Fatigue Reduction | Tames harshness on long listens. | Off |
| **Compressor** ▸ | Dynamic range compression — quiet passages stay audible in noise. | |
| ├ Threshold | Level above which compression starts; 0 is off. | Off |
| ├ Makeup Gain | Restores level lost to compression. | Auto |
| ├ Ratio | 2:1 … 10:1, or Limit. | 4:1 |
| ├ Knee | Hard or soft transition at the threshold. | Soft Knee |
| ├ Attack Time | How fast it clamps down. | 5 ms |
| └ Release Time | How fast it lets go. | 500 ms |

---

## 3. Playback Settings

| Setting | What it does | Default |
|---|---|---|
| Shuffle ● | Randomise the playlist. Re-shuffles the live playlist immediately. | Off |
| Repeat ● | Off / All / One / Shuffle / A-B. | Off |
| Play Selected First | Selecting a track starts from it rather than the top of the folder. | Yes |
| **Fast-Forward/Rewind** ▸ | | |
| ├ FF/RW Min Step | Seek distance for the first press. | 1s |
| └ FF/RW Accel | How fast held seeking speeds up. | Normal |
| Anti-Skip Buffer | Audio kept buffered before the disk may spin down. Raise it if playback stutters when moving. | 5s |
| Fade on Stop/Pause | Ramp the volume instead of cutting. | Yes |
| Single Mode | Stop after one track / album / artist / genre / playlist. | Off |
| Party Mode | New selections queue instead of replacing the playlist. | Off |
| **Crossfade** ▸ | Overlap the end of one track with the start of the next. | |
| ├ Enable Crossfade | Off / auto skip / manual skip / shuffle / always. | Off |
| ├ Fade-In Delay / Duration | Shape of the incoming track. | 0s / 2s |
| ├ Fade-Out Delay / Duration | Shape of the outgoing track. | 0s / 2s |
| └ Fade-Out Mode | Crossfade (dip) or Mix (constant power). | Crossfade |
| **Replaygain** ▸ | Applies the loudness tags many rippers write, so albums match in level. | |
| ├ Replaygain Type | Track / Album / Track-if-shuffling / Off. | Track Gain If Shuffling |
| ├ Prevent Clipping | Back the gain off rather than clip. | No |
| └ Pre-amp | Fixed offset applied on top. | 0.0 dB |
| Track Skip Beep | Beep on skip. | Off |
| Auto-Change Directory | At the end of a folder, move to the next (or a random) one. | No |
| Constrain Auto-Change | Keep that move inside the starting folder's parent. | No |
| Cuesheet Support | Read `.cue` files so a single-file album shows track boundaries. | Off |
| **Pause on Headphone Unplug** ▸ | | |
| ├ Pause on Headphone Unplug | Off / pause / pause and resume on replug. | Off |
| └ Disable resume on startup if phones unplugged | Do not auto-resume into a bare speaker or line-out. | No |
| Skip Length | Make skip jump a fixed time instead of a whole track. Useful for long podcasts. | Skip Track |
| Prevent Track Skipping | Locks out skip entirely — kiosk or party use. | No |
| Rewind Across Tracks | Rewinding past the start of a track enters the previous one. | No |
| Rewind Before Resume | Back up N seconds when resuming, to re-establish context. | Off |
| Rewind on Pause | The same, on every unpause. | Off |
| Frequency | Output sample rate. Auto is right almost always. | Auto |
| Album Art | Where cover art comes from. | Prefer Cache |
| Logging | Off / on / Last.fm scrobble log. | Off |

---

## 4. General Settings

### 4.1 What's Playing Screen

| Setting | What it does | Default |
|---|---|---|
| Default Browser | Which browser the now-playing screen and root default to. | Music |
| Select Action | Where Select from the now-playing screen goes. | Previous Screen |
| Hotkey | Action bound to the hotkey button in the now-playing screen. | Lyrics |
| Set Context Item 1–4 | The four configurable rows at the bottom of the now-playing context menu. | Track Info, Delete, Show in Files, Album Art |
| Reset Settings | Restore the hotkey and the four rows to defaults. | *(action)* |

### 4.2 Playlists

| Setting | What it does | Default |
|---|---|---|
| Sort Playlists | Alphabetical / oldest / newest. | Alphabetical |
| **Playlist Viewer Settings** ▸ Show Icons / Show Indices / Track Display | Chrome of the playlist viewer; Track Display picks filename vs. tags. | On / On / Track Name Only |
| Recursively Insert Directories | Inserting a folder pulls in its subfolders. | On |
| **Current Playlist** ▸ Warn When Erasing / Keep Current Track When Replacing / Show Shuffled Adding Options | Confirmations and which add-to-playlist options appear. | On / On / On |
| **Current Playlist** ▸ Show Queue Options | Whether queue entries appear in context menus. | No |

### 4.3 File Browser

| Setting | What it does | Default |
|---|---|---|
| Sort Case Sensitive | Uppercase sorts separately from lowercase. | No |
| Sort Directories | Alphabetical / by date / by newest date. | Alphabetical |
| Sort Files | As above, plus by type. | Alphabetical |
| Interpret Numbers When Sorting | `track2` before `track10` (whole numbers) vs. after (digits). | As Whole Numbers |
| Show Files | All / Supported / Music / Playlists. Hides clutter. | Supported |
| Show Filename Extensions | Off / on / unknown types only / only when viewing all types. | Only When Viewing All Types |
| Follow Playlist | Browser opens at the playing track's folder. | No |
| Show Path | Off / current directory / full path in the title. | Current Directory Only |
| Start File Browser at / | Clears the remembered start directory. | *(action)* |
| Hotkey | Action bound to the hotkey button in the browser. | Off |
| Rescan Documents & Images | One-shot rebuild of the flat Documents and Images lists. Normally reruns itself after USB. | *(action)* |

### 4.4 Music

| Setting | What it does                                                                                                                                                         | Default |
|---|----------------------------------------------------------------------------------------------------------------------------------------------------------------------|---|
| Sort Albums By | Name, or release year with the oldest or the newest first. Applies to every album list in the Music menu, not just the top one.  Follows the same logic as Carousel. | Name |
| Music Menu Settings | Opens a list of the Music menu's rows, each toggling on or off.                                                                                                      | *(action)* |

### 4.5 Search

| Setting | What it does | Default |
|---|---|---|
| Maximum Results | Total results kept, 25–200 in steps of 25. | 50 |
| Minimum Letters | How many letters must be typed before a search runs, 1–3. Raise it if one-letter searches return more than they are worth. | 1 |
| Result Order | Which of tracks, albums and artists is listed first, second and third. Six permutations. | Tracks, Albums, Artists |

### 4.6 Database

| Setting | What it does                                                                                                  | Default                          |
|---|---------------------------------------------------------------------------------------------------------------|----------------------------------|
| Load to RAM | Keep the database in memory: off / on / quick (ignore dircache). Faster browsing, more RAM.                   | Off ("quick" via shipped config) |
| Scan on Startup | Rescan at every boot. Usually redundant if Scan on Eject is on.                                               | On  ("off" via shipped config)   |
| Scan on Eject | Rescan after a USB session — the moment the library can actually have changed.                                | On                               |
| Autocommit on Startup | Finish a commit cut short by a flat battery or a mid-scan USB session. Off asks first.                        | On                               |
| Gather Runtime Data | Record play counts and ratings.                                                                               | On                               |
| Select Directories to Scan | Restrict the scan to chosen folders. Offers a rebuild afterwards without which you may see duplicate entries. | `/`                              |
| Rebuild Database | Full rebuild from scratch. Slow.                                                                              | *(action)*                       |
| Update Database | Incremental rescan. The fix for "my new album isn't showing up".                                              | *(action)*                       |
| Rebuild Index | Discard and re-derive the album/artist list the carousels and charts read.                                    | *(action)*                       |
| Update Index | The same, keeping what still applies.                                                                         | *(action)*                       |
| Export / Import Modifications | Move runtime data and edits in and out of a file.                                                             | *(action)*                       |
| Write Debug Log | Append scan progress to `.rockbox/tagcache.log`.                                                              | Off                              |

### 4.7 Carousel

Settings for the Album Covers and Artist Profiles screens.

| Setting | What it does                                                                                                  | Default                             |
|---|---------------------------------------------------------------------------------------------------------------|-------------------------------------|
| Show Album Title | Only applies to Album Covers. Hide / album at bottom / album at top / album+artist top / album+artist bottom. | Show Album and Artist at the Bottom |
| Show Year in Album Title | Only applies to Album Covers. Appends the year to the caption.                                                                              | No                                  |
| Background | Which theme colour fills the screen behind the covers.                                                        | Background Colour                   |
| Status Bar | Draw the status bar over the carousel.                                                                        | Off                                 |
| Year Sort Order | Only applies to Album Covers. Ascending or descending, when sorting by year.                                                                | Ascending                           |
| Sort Albums By | Artist+name / artist+year / year / name.                                                                      | Artist + Name                       |
| Sort Artists By | Name or most played.                                                                                          | Name                                |
| Centre Margin | Gap between the front cover and its neighbours.                                                               | 0 ("20" via shipped config          |
| Slide Tuck | How far back covers stack behind the front one.                                                               | 32                                  |
| Parallel Slides | Flat side covers instead of angled.                                                                           | On                                  |
| Scroll Speed | Flick speed.                                                                                                  | 200% ("175%" via shipped config)    |
| Transition Speed | Settle animation speed.                                                                                       | 400% ("325%" via shipped config)    |

### 4.8 Art Cache

| Setting | What it does | Default |
|---|---|---|
| Fast Build | Decode each source image once for the largest thumbnail and derive the rest. Applies to new thumbnails only. | Off |
| Missing Album Artwork / Missing Artist Portraits | The folders the last pass could find no art for. | *(screens)* |
| Rebuild Cache | Purge every thumbnail and regenerate. | *(action)* |
| Update Cache | Fill in what is missing — the fix for art added to already-indexed folders. | *(action)* |
| Write Debug Log | Append to `.rockbox/artcache.log`. | Off |

### 4.9 Text Viewer

| Setting | What it does | Default                       |
|---|---|-------------------------------|
| Colour Mode | Theme / theme inverted / black on white / white on black. | Theme                         |
| Margin | Inset the text from the screen edge. | Off ("on" via shipped config) |
| Line Spacing | Extra pixels between lines, 0–8. | 0                             |
| Page Number | Show a page counter. | Off                           |
| Font | Pick a `.fnt` for the viewer only. | *(none — uses the UI font)*  ("22-Literata" via shipped config) |
| Use UI Font | Drop back to the theme's font. | *(action)*                    |

### 4.10 Lyrics Viewer

| Setting | What it does | Default |
|---|---|---|
| Colour Mode | As the text viewer. | Theme |
| Alignment | Left / centre / right. | Centre |
| Line Spacing | Extra pixels between lines, 0–10. | 2 |
| Previous Line Opacity | How far past lines fade back. | 30% |
| Next Line Opacity | How far upcoming lines fade back. | 55% |
| Animation | Off / fast / normal / slow scroll between lines. | Normal |
| Highlight Sung Words | Word-level highlighting where the file supports it. | On |
| Keep Backlight On | Hold the backlight up while lyrics are showing. | On |
| Font / Use UI Font | A `.fnt` for the viewer only, and the way back. | *(none)* |

### 4.11 Display

#### LCD Settings

| Setting | What it does | Default |
|---|---|---|
| Backlight ● | Backlight timeout on battery. The single biggest battery lever. | 15s |
| Backlight (While Plugged In) | Separate timeout while charging. | 15s |
| Backlight on Lock | Normal / off / on while hold is engaged. | Off |
| Caption Backlight | Wake the backlight briefly at each track change. | Off |
| Backlight Fade In / Fade Out | Ramp the backlight instead of switching. **(5G only)** | 300 ms / 2000 ms |
| First Buttonpress Enables Backlight Only | The press that wakes the screen does nothing else. | Yes |
| **Backlight Exemptions** ▸ Enabled / Settings | Actions that do *not* wake the backlight. | Off / none |
| Sleep (After Backlight Off) | Power the LCD panel down N seconds after the backlight. Extra battery. | 5s |
| Brightness ● | Panel brightness. | 16 of 32 **(5G)**, 32 of 63 **(6G)** |

#### Peak Meter

The level meter some themes draw.

| Setting | What it does | Default |
|---|---|---|
| Peak Release | Fall-back rate. | 8 |
| Peak Hold Time | How long a peak is held. | 500 ms |
| Clip Hold Time | How long a clip indication is held. | 60s |
| Scale | Logarithmic (dB) or linear (%). | Logarithmic |
| Minimum / Maximum of Range | Ends of the displayed range; Scale decides how they read. | 60 / 0 |

#### Default Codepage

| Setting | What it does | Default |
|---|---|---|
| Default Codepage | Character set assumed for non-Unicode tags. | Unicode (UTF-8) |

### 4.12 System

| Setting | What it does | Default                                 |
|---|---|-----------------------------------------|
| **Battery** ▸ Battery Capacity | mAh of the fitted cell, so the runtime estimate is right after a replacement. | 400 mAh **(5G)**, 550 mAh **(6G)**      |
| **Battery** ▸ Charge During USB Connection | Off / on / force. | On                                      |
| **Disk** ▸ Disk Spindown | Idle seconds before the drive parks. Irrelevant on an SSD. | 5s                                      |
| **Disk** ▸ Storage Mode | Auto / HDD / SSD. Tells power management what is fitted. | Auto                                    |
| **Disk** ▸ Directory Cache | Keep the directory tree in RAM. Much faster browsing; needs a reboot. | On                                      |
| **Limits** ▸ Max Entries in File Browser | Cap on entries loaded per folder. | 5000                                    |
| **Limits** ▸ Max Playlist Size | Cap on playlist length. Lower on the 5G — building large playlists is slow there. | 2000 **(5G)**, 10000 **(6G)**           |
| **Limits** ▸ Glyphs to Cache | Font glyph cache size. Raise for CJK. | 250                                     |
| Volume Adjustment Mode | Direct (raw dB steps) or Perceptual (even-sounding steps). | Direct                                  |
| Number of Volume Steps | How many steps Perceptual mode divides the range into. | 50                                      |
| **Car Adapter Mode** ▸ Car Adapter Mode / Delay Before Resume | Auto-pause when the car's power cuts, resume when it returns. | Off / 5s                                |
| Serial Bitrate | Accessory serial rate. | Auto                                    |
| Accessory Power Supply | Power the dock accessory pin. | On                                      |
| Line Out | Enable the dock line-out. | On                                      |
| **Keyclick** ▸ Headphone Keyclick / Speaker Keyclick / Keyclick Repeats | Audible click on keypress, and whether auto-repeat clicks too. | Off / Off ("On" in shipped config) / No |
| USB HID | Present as a USB keyboard or remote when plugged in. | Off                                     |
| USB Keypad Mode | What the buttons send in HID mode. | Multimedia                              |
| USB-DAC | Act as a USB audio device. **(5G; never tested on hardware)** | Never                                   |
| USB Mode | Mass Storage or Charge Only when a host connects. | Mass Storage                            |
| Show Debug Menu | Reveals the Debug entry under root → System. | Off                                     |

### 4.13 Startup/Shutdown

| Setting | What it does | Default |
|---|---|---|
| Show Shutdown Message | Splash on power-off. | Yes |
| Start Screen | What opens at boot. | Main Menu |
| Idle Poweroff ● | Minutes of inactivity before shutting down; 0 is never. | 10 min |
| Sleep Timer | Start/stop the sleep timer now. The label shows time remaining. | *(action)* |
| Default Sleep Timer Duration | Length the timer starts at. | 30 min |
| Start Sleep Timer on Boot | Arm it automatically at power-on. | No |
| Restart Sleep Timer on Keypress | Any button resets the countdown. | No |
| Clear settings when reset button is held during startup | Recovery escape hatch for a bad config. | No |

### 4.14 Bookmarking

| Setting | What it does | Default |
|---|---|---|
| Bookmark on Stop | No / Yes / Ask / recent-only yes / recent-only ask. | No |
| Update on Stop | Overwrite an existing bookmark rather than adding one. | No |
| Load Last Bookmark | No / Yes / Ask, on entering a bookmarked folder. | No |
| Maintain a List of Recent Bookmarks? | Feeds the root menu's Recent Bookmarks. | No |

### 4.15 Automatic Resume

| Setting | What it does | Default |
|---|---|---|
| Automatic Resume | Remember a per-track position and return to it. Needs a usable database, and offers to build one. | No |
| Resume on Automatic Track Change | Never / always / in custom directories only. | Never |

### 4.16 Language

| Setting | What it does | Default |
|---|---|---|
| Language | Browse and load a `.lng` file. | English |

### 4.17 Voice

| Setting | What it does | Default |
|---|---|---|
| Voice Menus | Speak menu entries. Needs a voice file present. | On |
| Voice Directories / Voice Filenames | Off / numbers / spell. | Off / Off |
| Use Directory .talk Clips / Use File .talk Clips | Play per-item recorded clips where they exist. | Off / Off |
| Say File Type | Announce the extension. | Off |
| Announce Battery Level | Speak the battery level. | Off |
| Voice Prompt Volume | Level of speech relative to music. | 100% |

Voice builds are unverified in this fork.

---

## 5. UI Settings

| Setting | What it does | Default                                 |
|---|---|-----------------------------------------|
| Load Theme ● | Load a whole theme `.cfg` — skins, font, colours, backdrop at once. | Themify_2                               |
| Font ● | UI font. | 22-LeagueSpartan-Regular                |
| **Theme Settings** ▸ | The pieces a theme is built from — for adjusting one after loading a theme. |                                         |
| ├ While Playing Screen | Pick the `.wps` skin. | Themify_2                               |
| ├ Base Skin | Pick the `.sbs` — the frame drawn behind lists and menus. | Themify_2                               |
| ├ Show Icons | Draw list icons. | On ("Off" in  shipped config)           |
| ├ Clear Backdrop | Drop the background image. | *(action)*                              |
| ├ **Status-/Scrollbar** ▸ Scroll Bar / Scroll Bar Width | Off / left / right, and its width. | Left ("Right" in shipped config) / 6 px |
| ├ **Status-/Scrollbar** ▸ Status Bar | Off / top / bottom. | Top                                     |
| ├ **Status-/Scrollbar** ▸ Volume Display / Battery Display | Graphic or numeric in the status bar. | Graphic / Graphic                       |
| ├ Line Selector Type | Pointer / inverse bar / solid colour bar / gradient bar. | Bar (Gradient Colour)                   |
| ├ Line Separator | Height of the rule between list rows. Auto / off / 1–30 px. | Off                                     |
| ├ **Colours** ▸ | List, selector and separator colours, and Reset Colours. | Themify_2 palette                       |
| ├ **Dialogs** ▸ | Modal dialog chrome — see below. |                                         |
| ├ Album Art Rows | Draw album thumbnails beside database rows. | Off ("On" in shipped config)            |
| └ Artist Art Rows | The same for artist portraits. | Off  ("On" in shipped config)                                    |
| **Scrolling** ▸ | How text and lists move — see below. |                                         |
| Dynamic Colors | Recolour the UI from the current album's artwork. A skin not written for it will look wrong. | Off ("On" in shipped config)                                     |
| Artwork | Whether the now-playing screen shows Album Art or Artist Art. Re-buffers immediately. | Album Art                               |
| Quick Screen | What the long press opens: the quick screen (**On**) or the shortcuts menu. | On                                      |

Art rows force taller list rows, so a theme whose list layout does not draw the
cover gets tall rows with nothing in them. Turn them on only for a theme that
asks for them.

### Dialogs

Chrome for modal dialogs (the yes/no and confirmation boxes).

| Setting | What it does | Default                      |
|---|---|------------------------------|
| Box Border Width | Width of the dialog's own border. | 2 px                         |
| Box Margin | Inset of the box from the screen edge. | 10 px                        |
| Box Shadow | Solid drop shadow, offset right and down; 0 turns it off. | 4 px                         |
| Box Shadow Colour | Colour of that shadow. | Black                        |
| Button Border Width | Width of a button's border. | 2 px                         |
| Button Corner Radius | Corner rounding; 0 is square. | 0 px ("4" in shipped config) |
| Dialog Colours | **Off** inherits every colour from the theme, flat. **On** uses the nine below. **Auto** derives them from the theme's colours, or from the album's while Dynamic Colors is running. | Auto                         |
| **Colours** ▸ | Box Text / Background / Border, Button Text / Background / Border, and the same three for the selected button. | Themify_2 palette            |

The nine colours only apply when Dialog Colours is **On** — Auto derives its own
and ignores them.

The shadow is the exception: **Box Shadow** and **Box Shadow Colour** apply
whatever Dialog Colours is set to, so a theme can style the shadow without
having to take over all nine colours as well. It is black by default rather than
a theme colour because its job is to lift the box off whatever is behind it, and
a colour taken from the theme's own pair is the one guaranteed not to contrast
with the box.

Every setting in this group is reset by loading a theme
([§9](#9-settings-a-theme-resets)), including anything you set here by hand.

### Scrolling

The first four govern text too long for its line; the rest govern how lists move
under the wheel.

| Setting | What it does | Default                             |
|---|---|-------------------------------------|
| Scroll Speed | Speed of scrolling text. | 9 ("14" via shipped config)         |
| Scroll Start Delay | Pause before long text starts moving. | 1000 ms ("1500" via shipped config) |
| Scroll Step Size | Pixels per scroll step. | 6 px ("1" via shipped config)       |
| Bidirectional Scroll Limit | Text under this width bounces instead of wrapping. | 50%                                 |
| Screen Scrolls Out of View | Allow lines to scroll past the viewport edge. | No                                  |
| Disable Main Menu Scrolling | Stop the root menu scrolling long entries. | No                                  |
| Screen Scroll Step Size | Pixels per step for the above. | 16 px                               |
| Paged Scrolling | Lists move a page at a time rather than a line. | No                                  |
| List Wraparound | Past the last item, return to the first. | Yes                                 |
| List Order | Ascending or descending list traversal. | Ascending                           |

---

## 6. Time & Date Settings

This screen draws a live clock above the menu.

| Setting | What it does | Default |
|---|---|---|
| Set Time/Date | Clock editor. | *(screen)* |
| Time Format | 24- or 12-hour. | 24-hour |

---

## 7. Main Menu Settings

Not a settings list — a screen that reorders and hides root menu entries. It is
also on the Basic page, because a root menu with the entries you actually use is
worth more than most individual settings.

---

## 8. Manage Settings

| Entry | What it does | Default |
|---|---|---|
| Settings Mode | Basic or Advanced — see [§1](#1-basic-and-advanced). | Basic |
| Browse .cfg Files | Load a saved configuration. | *(action)* |
| Reset Settings | Restore every default in this document. Confirms first. | *(action)* |
| Save .cfg File | Write all settings to a file. | *(action)* |
| Save Sound Settings | Write only the sound and DSP settings. | *(action)* |
| Save Theme Settings | Write only the theme settings. | *(action)* |

---

## 9. Settings a theme resets

Loading a theme resets one group of settings to the defaults in this document
*before* reading the theme's `.cfg`. That is deliberate: a theme which says
nothing about a setting should get the shipped default, not whatever the last
theme left behind. A backdrop is the clearest case — without the reset, a theme
naming no backdrop would show the previous theme's image through everything it
draws.

It also means these do not stay put if you set them by hand and later load a
theme:

- Backdrop, and the bold font
- Dynamic Colors
- Every setting under **Dialogs**, including the shadow and all nine colours
- Carousel Background and Status Bar
- Album Art Rows and Artist Art Rows

See [`theme-guide.md`](theme-guide.md) - §2 covers this from the theme author's side, including the full
list and how to opt into it.

---

## Appendix: settings with no menu entry

These exist only in a `.cfg` file. Most are for theme authors; the rest are
remembered state rather than preferences.

| Name in `.cfg` | What it does | Default                                                                                   |
|---|---|-------------------------------------------------------------------------------------------|
| `progress bar radius` | Corner rounding of the progress bar. | 2                                                                                         |
| `database art row height` | Row height used when art rows are on. | 52                                                                                        |
| `font bold` | Bold UI font. Unset means "match the regular UI font". | *(unset)*                                                                                 |
| `backdrop` | Background image. | *(none)*                                                                                  |
| `iconset` / `viewers iconset` | List icons. | tango_icons.16x16                                                                         |
| `filetype colours` | A `.colours` file for the file browser. | *(none)*                                                                                  |
| `ui viewport` | The list viewport a theme reserves. | *(none)*                                                                                  |
| `start directory` | Remembered file-browser start point. | `/`                                                                                       |
| `playlist catalog directory` | Where new playlists are written. | `/Playlists`                                                                              |
| `autoresume next track paths` | Folders Automatic Resume treats as custom. | `/podcast:/podcasts`                                                                      |
| `database path` | Where the database files live. | `/.rockbox`                                                                               |
| `qs top` / `qs left` / `qs right` / `qs bottom` | Which settings the quick screen shows. | — / Shuffle / Repeat / — ("Brightness / Shuffle / Repeat / Brightness" in default config) |
| `root menu order` | Written by Main Menu Settings. | *(stock order)*                                                                           |
| `music menu hidden` | Which Music menu rows are turned off. Written by Music Menu Settings. | *(none hidden)*                                                                           |
| `music menu signature` | Identifies the row set the above was chosen against; a mismatch discards it. | 0                                                                                         |
| `hold_lr_for_scroll_in_list` | Left/right holds scroll a list. | On                                                                                        |
