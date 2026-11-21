- The **Playback Report** reads only the iPod's own listening log now, so its
  album and song pictures show under **On + Last.fm** too.
- **Bony, Jive and Obsede' 2** are no longer on the Themes release. They are
  at https://github.com/anthonyfletcher/podbox-themes; a copy already on the
  iPod keeps working.
- The **Playlist Engine** measures your Opus, Speex, ADX and game-music
  tracks again on its next update, as they were measured wrongly; the rest of
  the library is kept.
- The library no longer reads hidden folders and files (the `._` files a Mac
  copies beside each song, and names that start with a dot and a letter,
  such as `.Trashes`), `__MACOSX` folders, or the Windows recycle bin; Images,
  Books, Documents and Sound Scan pass them by too. Music kept in a folder
  such as `.music` leaves the library, with its play counts, at the next
  update: rename the folder first to keep it. Names such as
  `...And Justice for All` and `.38 Special` are read as before, and Images,
  Books and Sound Scan now find what is in them.
