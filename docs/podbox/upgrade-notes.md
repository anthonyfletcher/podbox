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
- **Rebuild Database** keeps your old database until the new one is
  finished, so a rebuild cut short by USB, a restart or a flat battery leaves
  the library as it was. While it runs, the old database takes its space a
  second time. A rebuild that finds no music at all now keeps the old one.
- **Liner Notes** reads `/Notes` again, in the background, the first time the
  iPod starts; until it finishes, notes pages and **Biography** rows are
  missing. A library of more than 2,048 pages now keeps all of them.
- The **Playback Report** leaves out audiobooks and podcasts while
  **Segregate Audiobooks** and **Segregate Podcasts** keep them out of the
  music, so its figures, badges included, can go down. It reads the whole
  log again the first time it opens.
- An audiobook series lists its books in series order when they are tagged
  with a series number (`SERIES-PART`, or the movement number some audiobook
  tools write), and by year as before when they are not. Numbers already in
  your tags are read at the next **Rebuild Database**; a book you tag from
  now on is picked up by an ordinary update.
- **Books** reads every ebook again, in the background, the first time the
  iPod starts, so an EPUB whose cover is only the picture on its first page
  now shows that cover.
