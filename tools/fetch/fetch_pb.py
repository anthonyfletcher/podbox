#!/usr/bin/env python3
"""
fetch_pb: the three library tools as one program, so the download is
one executable rather than three that each carry Python. Each tool is also
its own script and runs alone (python lyric_fetch.py ...).

    fetch_pb art [artists] [albums] [audiobooks] [podcasts] [ebooks] ROOT [options]
    fetch_pb lyrics ROOT [options]
    fetch_pb notes ROOT [options]
"""
from __future__ import annotations

import sys
from typing import Optional

VERSION = "1.0"
COMMANDS = {
    "art": ("art_fetch", "covers and pictures for artists, albums, audiobooks, podcasts and ebooks"),
    "lyrics": ("lyric_fetch", "synchronised lyrics from LRCLIB, as .lrc files beside the tracks"),
    "notes": ("notes_fetch", "liner notes and biographies from Wikipedia and MusicBrainz, for the player's /Notes"),
}


def usage() -> str:
    lines = [f"fetch_pb {VERSION}: fill a PodBox library from the web.", "",
             "Usage: fetch_pb COMMAND ROOT [options]", "", "Commands:"]
    lines += [f"  {name:<8}{what}" for name, (_m, what) in COMMANDS.items()]
    lines += ["", "ROOT is a mounted iPod, such as E:\\ or /Volumes/IPOD, or a music folder.",
              "fetch_pb COMMAND --help lists that command's options."]
    return "\n".join(lines)


def main(argv: Optional[list[str]] = None) -> int:
    argv = sys.argv[1:] if argv is None else argv
    # Redirected to a file, Windows output is in the ANSI code page, where a
    # Japanese or Korean name would stop the run
    for stream in (sys.stdout, sys.stderr):
        stream.reconfigure(errors="replace")
    if not argv or argv[0] in ("-h", "--help", "help"):
        print(usage())
        return 0 if argv else 2
    if argv[0] == "--version":
        print(f"fetch_pb {VERSION}")
        return 0
    command = COMMANDS.get(argv[0])
    if command is None:
        print(f"fetch_pb: no command '{argv[0]}'.\n\n{usage()}", file=sys.stderr)
        return 2
    # Imported by name, here: PyInstaller bundles only the imports it can
    # see, and a tool's start-up then costs only that tool
    if command[0] == "art_fetch":
        import art_fetch as module
    elif command[0] == "lyric_fetch":
        import lyric_fetch as module
    else:
        import notes_fetch as module
    sys.argv[0] = f"fetch_pb {argv[0]}"          # what its --help calls it
    return module.main(argv[1:])


if __name__ == "__main__":
    raise SystemExit(main())
