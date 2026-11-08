- **Long comments and other tags in FLAC, Ogg, Opus, APE and WavPack files
  are now kept to 500 characters,** as in MP3 and M4A files; until now they
  were cut at about 260, or 127 in APE and WavPack. Files already in the
  library keep the short text until you run **Rebuild Database** (Settings >
  Library > Maintenance); an Update leaves them be.
- **A cover drawn only partly, its top showing and the rest flat grey,** is
  not mended by this update on its own: the art cache keeps a picture until
  its image file changes. Run **Rebuild Art Cache**, or **Check a File's Art**
  on one track of the album, to make it again. Covers are no longer kept like
  that when a read from the disk fails.
- **Select Action and Select + Play now offer the same list:** Select can run
  any of the now-playing menu's actions, Select + Play can go to Music,
  Album Covers, Files or the previous browser, and both can open Theme
  Options. Your settings are kept. Like Select + Play, Select Action is no
  longer found by Search or listed in Changed Settings, and has no Explain
  entry.
- **Menu on the now-playing screen now goes back to the list you started the
  music from** -- a Music list, Files, Playlists, Podcasts, the queue, Album
  Covers or Artist Portraits -- in the same place, and a second Menu reaches
  the main menu.
  The end of the music returns there too. To have Menu go straight to
  the main menu as before, set **Menu Action** (Settings > Playback > Now
  Playing Screen) to Main Menu. Select Action and Select + Play Action
  (Select + Play until now) offer this Previous Screen too; the choice they
  called Previous Screen before, back to the browser last used, is now
  **Previous Browser** and is still Select's default. If you had set Select +
  Play to Previous Screen, it now goes back the new way; pick Previous
  Browser to keep the old.
- **Album covers are now kept per album, not per folder,** so a library whose
  folders hold several albums each -- one copied onto the player by iTunes,
  under `iPod_Control/Music` -- shows each album its own cover in Music and
  Album Covers. The first art pass after updating moves your existing covers
  across without making them again, except where a folder held more than one
  album; those albums' covers are made afresh. The Report rebuilds its figures
  from the log once, which takes a few seconds the first time it opens.
- **The sounds of iPod click-wheel games are left out of the database,** under
  the new **Skip iPod Games** setting (Settings > Library > Database, on by
  default). On a player iTunes has synced, the next Update removes them from
  your Music lists; turn the setting off before updating to keep them.
- **Spike's Audio Offset setting is gone.** It never moved where a press was
  judged, so play is unchanged; the line it leaves in `config.cfg` is
  ignored.
