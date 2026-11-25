"""
art_fetch's people: pictures in the player's Artwork/People of artists who
have no folder of their own, each group behind its option:

  --split-artists     each artist of an Album Artist joined with ';', as
                      the player's Split Album Artists lists them
  --track-artists     each name in the Artist tag that is never an album
                      artist, as Music > Artist lists them
  --featured-artists  each guest a title or Artist tag credits, as the
                      player's Featured Artists lists them

The names come from every music track's tags, read as the player reads
them (fetchlib). Nobody the player can already picture is looked up: an
album artist, whom the artists kind pictures in their folder, anyone with
a file in Artwork/People, and anyone with an image in a folder of their
name.

Parts, in order:
  - the names the library holds
  - the run
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import requests

from art_books import Central, artwork_root, folder_image
from fetchlib import (AUDIO_EXTS, MUSIC, RateLimiter, credit_key,
                      credits_anyone, data_dir, featured_names, load_json,
                      name_key, name_keys, player_root_of, read_tags,
                      safe_filename, save_json, split_album_artist,
                      walk_files)

CACHE_NAME = "people-art-cache.json"
GROUPS = (("split", "split_artists", "split artists"),
          ("track", "track_artists", "track artists"),
          ("featured", "featured_artists", "featured artists"))

# ---------------------------------------------------------------------
# The names the library holds
# ---------------------------------------------------------------------
@dataclass
class Person:
    name: str
    group: str                      # the first group that named them
    albums: set[str] = field(default_factory=set)


def known_artists(root: Path) -> set[str]:
    """The names the owner told the player about, which decide where a
    credit comes apart as the album artists do."""
    player = player_root_of(root)
    path = player / ".rockbox" / "library" / "user" / "known_artists.txt" if player else None
    out: set[str] = set()
    try:
        text = path.read_text(encoding="utf-8-sig") if path else ""
    except OSError:
        return out
    for line in text.splitlines():
        line = line.strip(" \t")
        if line and not line.startswith("#"):
            out.add(credit_key(line))
    return out


def library_people(root: Path, groups: set[str]) -> list[Person]:
    """Everyone in 'groups' who is not an album artist, once each by the
    player's name key, in name order."""
    tracks = []
    album_artists: set[str] = set()
    for path in walk_files(root, AUDIO_EXTS):
        t = read_tags(path)
        if t.error or t.kind != MUSIC:
            continue
        # The player files a track without an Album Artist under its Artist
        aa = t.album_artist or t.artist
        tracks.append((aa, t.artist or aa, t.title, t.album))
        if aa:
            album_artists.add(aa)
    own = {name_key(a) for a in album_artists}
    known = {credit_key(a) for a in album_artists} | known_artists(root)

    people: dict[str, Person] = {}

    def add(name: str, group: str, album: str) -> None:
        key = name_key(name)
        if not key or key in own:
            return
        p = people.setdefault(key, Person(name, group))
        if album:
            p.albums.add(album)

    for aa, artist, title, album in tracks:
        if "split" in groups:
            for part in split_album_artist(aa):
                add(part, "split", album)
        # A credit or a joint name is no one person to look up
        if "track" in groups and artist and ";" not in artist and not credits_anyone(artist):
            add(artist, "track", album)
        if "featured" in groups:
            for s in (title, artist):
                for guest in featured_names(s, known):
                    add(guest, "featured", album)
    return sorted(people.values(), key=lambda p: p.name.casefold())

# ---------------------------------------------------------------------
# The run
# ---------------------------------------------------------------------
def wanted_groups(args: argparse.Namespace) -> set[str]:
    return {g for g, attr, _label in GROUPS if getattr(args, attr, False)}


def folders_pictured(root: Path) -> set[str]:
    """The name keys of the top-level folders holding an image."""
    out: set[str] = set()
    for d in root.iterdir():
        if d.is_dir() and not d.name.startswith(".") and folder_image(d):
            out |= name_keys(d.name)
    return out


def run_people(root: Path, args: argparse.Namespace, creds: Any, session: requests.Session,
               limiter: RateLimiter, hooks: Any) -> int:
    """'hooks' is art_fetch itself, for its artist sources, scoring and
    JPEG writing."""
    groups = wanted_groups(args)
    art = artwork_root(root, args)
    if art is None:
        print("\nPeople: skipped. Their pictures go in the iPod's Artwork folder: "
              "point it at a mounted iPod, or give --artwork-root", flush=True)
        return 2
    people_dir = art / "People"
    pictured = Central(people_dir, people=True)
    in_folders = folders_pictured(root)
    manifest = args.manifest
    cache_path = data_dir(root) / CACHE_NAME
    cache: dict[str, Any] = load_json(cache_path, {}).get("people", {})

    print("\n=== People: " + ", ".join(label for g, _a, label in GROUPS if g in groups) + " ===")
    print(f"Artwork folder : {art}")
    print(f"Looking in     : {root}")
    people = library_people(root, groups)
    print("Found          : " + ", ".join(
        f"{sum(p.group == g for p in people)} {label}" for g, _a, label in GROUPS if g in groups))
    counts: dict[str, int] = {}

    def count(what: str) -> None:
        counts[what] = counts.get(what, 0) + 1

    try:
        for p in people:
            target = people_dir / (safe_filename(p.name) + ".jpg")
            keys = name_keys(p.name)
            if not manifest.owns(target) and (pictured.has(*keys) or keys & in_folders):
                count("had a picture"); continue
            if target.exists() and not (args.overwrite and manifest.owns(target)):
                count("had a picture"); continue
            key = name_key(p.name)
            entry = cache.get(key, {})
            if entry.get("status") == "not_found" and not args.retry_not_found and not args.ignore_cache:
                count("not found"); continue
            print(f"\n[{p.group}: {p.name}]")
            if args.dry_run:
                print(f"  dry-run: would look for a picture -> {target.relative_to(art.parent)}")
                count("would look up"); continue
            candidates = hooks.artist_candidates(p.name, creds, args, session, limiter,
                                                 entry.get("mbid") or "", sorted(p.albums))
            ok, source, notes = hooks.try_candidates(candidates, target, args, session, limiter)
            if ok:
                pictured.add(p.name)
                c = next((x for x in candidates if x.source == source), None)
                cache[key] = {"status": "found", "source": source,
                              "mbid": c.mbid if c else ""}
                count("pictures saved")
            else:
                cache[key] = {"status": "not_found", "reason": notes or "No artwork found"}
                print(f"  artwork: {notes or 'No artwork found'}")
                count("not found")
    finally:
        if not args.dry_run:
            save_json(cache_path, {"version": 1, "people": cache})

    print("\nPeople summary\n--------------")
    for what in ("pictures saved", "would look up", "had a picture", "not found"):
        if counts.get(what):
            print(f"  {what:<15}: {counts[what]}")
    return 0
