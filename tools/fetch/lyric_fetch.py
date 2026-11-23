#!/usr/bin/env python3
"""
lyric_fetch: synchronised lyrics from LRCLIB for a music library.

The player's lyrics screen reads only timed lyrics: a .lrc, .lrc8 or .snc
named after the track (or its title) in the track's folder or one above,
or an MP3's SYLT frame. So this writes "<track>.lrc", UTF-8 with a BOM,
beside each track LRCLIB has timed lyrics for, and nothing else. Tracks
with only plain lyrics are counted, not written. Audio files are never
changed.

Parts, in order: the library side, LRCLIB, writing, the run.
"""
from __future__ import annotations

import argparse
import re
import sys
import time
from pathlib import Path
from typing import Any, Optional

import requests

from fetchlib import (AUDIO_EXTS, MUSIC, Manifest, RateLimiter, TrackTags,
                      config_path, contact_from, data_dir, http_get,
                      load_config, load_json, name_key, read_tags, save_json,
                      similarity, user_agent, walk_files)

TOOL = "lyric_fetch"
VERSION = "1.0"
LRCLIB = "https://lrclib.net/api"
LYRICS_EXTS = (".lrc", ".lrc8", ".snc")
CACHE_NAME = "lyrics-cache.json"
RETRY_AFTER_DAYS = 30
# LRCLIB matches a duration only to within about 2 s
DURATION_SLACK = 2.0
# Lookups failing one after another stop the run: the service is down
MAX_FAILURES = 5

# ---------------------------------------------------------------------
# The library side
# ---------------------------------------------------------------------
class LyricsFiles:
    """Which lyrics files exist, looked up the way the player looks: by the
    track's name or its title (with the characters a file name cannot hold
    replaced), in the track's folder and every folder above it to the top
    of the drive. Folder listings are kept, since every track of an album
    asks about the same folders."""

    def __init__(self) -> None:
        self.listings: dict[Path, set[str]] = {}

    def _names(self, folder: Path) -> set[str]:
        if folder not in self.listings:
            try:
                self.listings[folder] = {p.name.casefold() for p in folder.iterdir()}
            except OSError:
                self.listings[folder] = set()
        return self.listings[folder]

    def found(self, track: Path, title: str) -> Optional[Path]:
        stems = [track.stem]
        if title and title != track.stem:
            stems.append(re.sub(r'[*/:<>?\\|]', "_", title).replace('"', "'"))
        folder = track.parent.resolve()
        while True:
            names = self._names(folder)
            for ext in LYRICS_EXTS:
                for stem in stems:
                    if (stem + ext).casefold() in names:
                        return folder / (stem + ext)
            if folder.parent == folder:
                return None
            folder = folder.parent

    def added(self, path: Path) -> None:
        self._names(path.parent.resolve()).add(path.name.casefold())


def has_sylt(path: Path) -> bool:
    if path.suffix.casefold() != ".mp3":
        return False
    try:
        from mutagen.id3 import ID3
        return bool(ID3(str(path)).getall("SYLT"))
    except Exception:
        return False


def cache_key(t: TrackTags) -> str:
    return f"{name_key(t.album_artist or t.artist)}|{name_key(t.title)}|{round(t.duration)}"

# ---------------------------------------------------------------------
# LRCLIB
# ---------------------------------------------------------------------
def lrclib_get(session: requests.Session, limiter: RateLimiter, path: str,
               params: dict[str, Any]) -> Any:
    limiter.wait("lrclib")
    r = http_get(session, f"{LRCLIB}/{path}", params=params)
    if r.status_code == 404:
        return None
    r.raise_for_status()
    return r.json()


def acceptable(rec: dict[str, Any], t: TrackTags, artist: str) -> bool:
    try:
        close = abs(float(rec.get("duration") or 0) - t.duration) <= DURATION_SLACK
    except (TypeError, ValueError):
        close = False
    return (close and bool(rec.get("syncedLyrics"))
            and similarity(t.title, rec.get("trackName") or "") >= 0.85
            and similarity(artist, rec.get("artistName") or "") >= 0.80)


def look_up(session: requests.Session, limiter: RateLimiter, t: TrackTags) -> tuple[str, Optional[dict[str, Any]]]:
    """One of found, instrumental, plain_only, not_found, with the record
    for found."""
    artist = t.artist or t.album_artist
    params = {"track_name": t.title, "artist_name": artist,
              "duration": round(t.duration)}
    if t.album:
        params["album_name"] = t.album
    rec = lrclib_get(session, limiter, "get", params)
    if rec and rec.get("instrumental"):
        return "instrumental", None
    if rec and rec.get("syncedLyrics"):
        return "found", rec
    plain = bool(rec and rec.get("plainLyrics"))

    # The exact lookup missed or has no timing: another record of the same
    # song may have it.
    results = lrclib_get(session, limiter, "search",
                         {"track_name": t.title, "artist_name": artist}) or []
    good = [r for r in results if acceptable(r, t, artist)]
    if good:
        good.sort(key=lambda r: (-(similarity(t.title, r.get("trackName") or "")
                                   + similarity(artist, r.get("artistName") or "")),
                                 abs(float(r.get("duration") or 0) - t.duration)))
        return "found", good[0]
    if plain or any(r.get("plainLyrics") and abs(float(r.get("duration") or 0) - t.duration) <= DURATION_SLACK
                    for r in results):
        return "plain_only", None
    return "not_found", None

# ---------------------------------------------------------------------
# Writing
# ---------------------------------------------------------------------
def header_value(s: str) -> str:
    return " ".join((s or "").replace("[", "(").replace("]", ")").split())


def stamp(ms: int) -> str:
    cs = max(0, int(ms)) // 10
    return f"{cs // 6000:02d}:{cs // 100 % 60:02d}.{cs % 100:02d}"


# A line ending this long before the next starts gets an empty timed line
# after it: the player shows the last sung line through a gap, and times the
# line before it by it.
GAP_MS = 1500


def word_synced_body(lyricsfile: Any) -> Optional[str]:
    """LRCLIB's word timing (YAML: lines, each with start_ms, end_ms and
    words with their own start_ms) as the enhanced LRC the player reads,
    [time]<time>word<time>word. None when it is absent or unreadable, and
    the line timing is used instead."""
    if not isinstance(lyricsfile, str) or not lyricsfile.strip():
        return None
    try:
        import yaml
        doc = yaml.safe_load(lyricsfile)
        out = []
        lines = doc["lines"]
        for i, line in enumerate(lines):
            start = int(line["start_ms"])
            words = line.get("words") or []
            if words:
                text = "".join(f"<{stamp(int(w['start_ms']))}>{w['text']}" for w in words)
            else:
                text = str(line.get("text") or "")
            out.append(f"[{stamp(start)}]{text.rstrip()}")
            end = line.get("end_ms")
            following = int(lines[i + 1]["start_ms"]) if i + 1 < len(lines) else None
            if end is not None and (following is None or following - int(end) >= GAP_MS):
                out.append(f"[{stamp(int(end))}]")
        return "\n".join(out) if out else None
    except Exception:
        return None


def lrc_bytes(t: TrackTags, rec: dict[str, Any]) -> bytes:
    lines = [f"[ti:{header_value(t.title)}]", f"[ar:{header_value(t.artist or t.album_artist)}]"]
    if t.album:
        lines.append(f"[al:{header_value(t.album)}]")
    lines.append("[by:LRCLIB]")
    body = (word_synced_body(rec.get("lyricsfile")) if rec.get("hasWordSync") else None) \
        or rec["syncedLyrics"].replace("\r\n", "\n").replace("\r", "\n").strip("\n")
    return ("﻿" + "\n".join(lines) + "\n" + body + "\n").encode("utf-8")

# ---------------------------------------------------------------------
# The run
# ---------------------------------------------------------------------
def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description="Fetch synchronised lyrics from LRCLIB and save them as .lrc files beside your tracks.")
    p.add_argument("root", nargs="?", help="a mounted player, or a music folder")
    p.add_argument("--dry-run", action="store_true", help="say what would be fetched, and write nothing")
    p.add_argument("--retry-not-found", action="store_true",
                   help=f"ask again about tracks LRCLIB had nothing for (otherwise after {RETRY_AFTER_DAYS} days)")
    p.add_argument("--only", metavar="ARTIST", help="only tracks by this artist")
    p.add_argument("--undo", action="store_true", help="delete the .lrc files this tool wrote and you have not edited")
    p.add_argument("--contact", metavar="EMAIL", help="put in the requests so LRCLIB can reach you; remembered")
    p.add_argument("--delay", type=float, default=0.3, help="seconds between requests (default 0.3)")
    p.add_argument("--version", action="version", version=f"lyric_fetch {VERSION}")
    return p


def main(argv: Optional[list[str]] = None) -> int:
    args = build_parser().parse_args(argv)
    if not args.root:
        build_parser().print_help()
        return 2
    root = Path(args.root).expanduser().resolve()
    if not root.is_dir():
        print(f"ERROR: not a folder: {root}", file=sys.stderr)
        return 2
    manifest = Manifest(root)

    if args.undo:
        deleted, kept = manifest.undo(TOOL, args.dry_run)
        if not args.dry_run:
            manifest.save()
        print(f"\n{deleted} file(s) {'would be ' if args.dry_run else ''}deleted, {kept} kept because you edited them")
        return 0

    config = load_config()
    contact = contact_from(args.contact, config)
    session = requests.Session()
    session.headers["User-Agent"] = user_agent(TOOL, VERSION, contact)
    limiter = RateLimiter({"lrclib": args.delay})
    cache_path = data_dir(root) / CACHE_NAME
    cache: dict[str, Any] = load_json(cache_path, {}).get("tracks", {})
    files = LyricsFiles()
    counts: dict[str, int] = {}
    now = time.time()
    failures = 0

    def count(what: str) -> None:
        counts[what] = counts.get(what, 0) + 1

    print(f"lyric_fetch {VERSION}")
    print(f"Library  : {root}")
    print(f"Cache    : {cache_path}")
    print(f"Settings : {config_path()}")
    if not contact:
        print("Contact  : none (--contact you@example.com lets LRCLIB reach you, and gets more room)")
    try:
        for path in walk_files(root, AUDIO_EXTS):
            t = read_tags(path)
            rel = path.relative_to(root)
            artist = t.artist or t.album_artist
            if t.error or not artist or not t.title or t.duration <= 0:
                count("untagged")
                continue
            if t.kind != MUSIC:
                count("spoken word")
                continue
            if args.only and similarity(args.only, artist) < 0.9 and similarity(args.only, t.album_artist) < 0.9:
                continue
            if files.found(path, t.title) or has_sylt(path):
                count("already had lyrics")
                continue
            target = path.with_suffix(".lrc")
            if not manifest.may_write(target):
                count("already had lyrics")
                continue

            key = cache_key(t)
            entry = cache.get(key, {})
            status = entry.get("status")
            if status in ("instrumental", "plain_only"):
                count(status.replace("_", " "))
                continue
            if status == "not_found" and not args.retry_not_found \
                    and now - float(entry.get("time", 0)) < RETRY_AFTER_DAYS * 86400:
                count("not found")
                continue
            if args.dry_run:
                print(f"would look up: {rel}")
                count("would look up")
                continue

            try:
                status, rec = look_up(session, limiter, t)
                failures = 0
            except requests.RequestException as exc:
                print(f"  error: {rel}: {exc}")
                count("errors")
                failures += 1
                if failures >= MAX_FAILURES:
                    print(f"\nStopped: {failures} lookups in a row failed, so LRCLIB looks to be down. Run it again later.")
                    break
                continue
            cache[key] = {"status": status, "time": now, "artist": artist, "title": t.title}
            if status == "found":
                try:
                    manifest.write(target, lrc_bytes(t, rec), TOOL)
                except OSError as exc:
                    print(f"  could not write {target.name}: {exc}")
                    count("errors")
                    continue
                files.added(target)
                print(f"saved: {target.relative_to(root)}")
                count("saved")
            else:
                print(f"{status.replace('_', ' ')}: {rel}")
                count(status.replace("_", " "))
    except KeyboardInterrupt:
        print("\nStopped; what was found so far is kept.")
    finally:
        if not args.dry_run:
            save_json(cache_path, {"version": 1, "tracks": cache})
            manifest.save()

    print("\nSummary\n-------")
    for what in ("saved", "would look up", "already had lyrics", "not found", "plain only",
                 "instrumental", "spoken word", "untagged", "errors"):
        if counts.get(what):
            print(f"  {what:<20}: {counts[what]}")
    if counts.get("plain only"):
        print("\nPlain-only lyrics have no timing, which the player needs, so none were saved for those.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
