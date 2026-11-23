# PodBox library tools

`podbox_fetch` fills your library with what PodBox can show but cannot fetch itself. It is
one program with three commands:

| Command | Does |
| --- | --- |
| [`podbox_fetch art`](art_fetch.md) | Pictures of artists and authors, and covers for albums, audiobooks, podcasts and ebooks: all of them, or the kinds you name |
| [`podbox_fetch lyrics`](lyric_fetch.md) | Synchronised lyrics |
| [`podbox_fetch notes`](notes_fetch.md) | Liner notes and biographies for the player's wiki pages |

Download it from the [Tools release](https://github.com/anthonyfletcher/podbox/releases/tag/Tools):
`podbox-tools-windows.zip`, `podbox-tools-macos-arm64.zip` for an Apple Silicon Mac, or
`podbox-tools-macos-intel.zip` for an older one. Unzip it and run it from a terminal in
that folder (PowerShell or Command Prompt on Windows, Terminal on a Mac):

```bash
.\podbox_fetch lyrics E:\                # Windows, the iPod mounted as E:
./podbox_fetch lyrics /Volumes/IPOD      # Mac
.\podbox_fetch notes --help              # a command's options
```

**On a Mac**, the program is not signed, so macOS refuses to open it the first time. Run this
once in Terminal, in the unzipped folder:

```bash
xattr -dr com.apple.quarantine .
```

Point `lyrics` and `notes` at your mounted iPod, or at a music folder on your computer. Point
`art` at the music folder itself, such as `E:\Music`: it finds audiobooks, podcasts and ebooks
anywhere on the iPod from there. None of them changes your audio files, and none overwrites a
file it did not write unless you tell it to.

Settings such as API keys are kept in `%APPDATA%\PodBox\fetch\` on Windows and
`~/Library/Application Support/PodBox/fetch/` on a Mac.

## Running from source

Python 3.11 or later. Each command is also a script of its own:

```bash
pip install -r requirements.txt
python podbox_fetch.py lyrics E:\
python lyric_fetch.py E:\                # the same
```

To build the program yourself, add `pip install pyinstaller` and run
`pyinstaller --onefile podbox_fetch.py`. The result goes in `dist/`, and runs only on the kind
of computer it was built on.
