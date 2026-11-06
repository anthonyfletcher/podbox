- **A cover drawn only partly, its top showing and the rest flat grey,** is
  not mended by this update on its own: the art cache keeps a picture until
  its image file changes. Run **Rebuild Art Cache**, or **Check a File's Art**
  on one track of the album, to make it again. Covers are no longer kept like
  that when a read from the disk fails.
- **Select Action and Select + Play now offer the same list:** Select can run
  any of the now-playing menu's actions, Select + Play can go to Music,
  Album Covers, Files or the previous screen, and both can open Theme
  Options. Your settings are kept. Like Select + Play, Select Action is no
  longer found by Search or listed in Changed Settings, and has no Explain
  entry.
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
