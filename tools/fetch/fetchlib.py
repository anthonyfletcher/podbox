"""
The core the PodBox fetch tools share: where their settings live, how they
pace and identify their requests, how they tell music from books and
podcasts, how names are matched, and the manifest that keeps them from
overwriting anything the owner made.

Parts, in order:
  - settings and credentials
  - requests: user agent, pacing, retries
  - files: atomic writes, JSON
  - names: remote similarity, and the player's own name keys
  - the library: tags, kinds, walking a root
  - the manifest
"""
from __future__ import annotations

import difflib
import hashlib
import json
import os
import re
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable, Optional

import requests

try:
    from mutagen import File as MutagenFile
except ModuleNotFoundError:
    MutagenFile = None

# Certificates are checked against the operating system's store rather than
# Python's own list, which lacks the roots a company network or filtering
# proxy installs: without this, every HTTPS request fails there.
try:
    import truststore
    truststore.inject_into_ssl()
except ImportError:
    pass

PROJECT_URL = "https://github.com/anthonyfletcher/podbox"

AUDIO_EXTS = {".mp3", ".mp2", ".mp1", ".m4a", ".m4b", ".mp4", ".aac", ".flac",
              ".ogg", ".oga", ".opus", ".wav", ".wma", ".aiff", ".aif",
              ".ape", ".wv", ".mpc"}

# ---------------------------------------------------------------------
# Settings and credentials
# ---------------------------------------------------------------------
CONFIG_FILENAME = "fetch.json"
LEGACY_CREDENTIALS = "artist_art_credentials.json"


def config_dir() -> Path:
    if os.name == "nt":
        base = os.environ.get("APPDATA") or str(Path.home() / "AppData" / "Roaming")
        return Path(base) / "PodBox" / "fetch"
    if sys.platform == "darwin":
        return Path.home() / "Library" / "Application Support" / "PodBox" / "fetch"
    base = os.environ.get("XDG_CONFIG_HOME") or str(Path.home() / ".config")
    return Path(base) / "podbox" / "fetch"


def legacy_credentials_path() -> Path:
    if os.name == "nt":
        base = os.environ.get("APPDATA") or str(Path.home() / "AppData" / "Roaming")
        return Path(base) / "RockboxArtistArt" / LEGACY_CREDENTIALS
    base = os.environ.get("XDG_CONFIG_HOME") or str(Path.home() / ".config")
    return Path(base) / "rockbox-artist-art" / LEGACY_CREDENTIALS


def config_path() -> Path:
    return config_dir() / CONFIG_FILENAME


def load_config() -> dict[str, Any]:
    """The shared settings: API keys and the contact address. art_fetch's
    old credentials file is copied in the first time, and left in place."""
    path = config_path()
    data = load_json(path, {})
    if not path.exists():
        old = load_json(legacy_credentials_path(), {})
        if old:
            data.update({k: v for k, v in old.items() if v})
            save_config(data)
    return data


def save_config(data: dict[str, Any]) -> Path:
    path = config_path()
    save_json(path, data)
    try:
        os.chmod(path, 0o600)
    except OSError:
        pass
    return path


def contact_from(args_contact: Optional[str], config: dict[str, Any]) -> str:
    """The contact address for the user agent: the one given now, which is
    remembered, else the remembered one."""
    if args_contact:
        if config.get("contact") != args_contact:
            config["contact"] = args_contact
            save_config(config)
        return args_contact
    return str(config.get("contact") or "")

# ---------------------------------------------------------------------
# Requests
# ---------------------------------------------------------------------
def user_agent(tool: str, version: str, contact: str = "") -> str:
    """PodBox-<tool>/<version> (+<project>; <contact>). Wikimedia and LRCLIB
    both give an identified client more room than an anonymous one."""
    extra = f"; {contact}" if contact else ""
    return f"PodBox-{tool}/{version} (+{PROJECT_URL}{extra})"


class RateLimiter:
    """The least time between two requests to one service, shared by every
    caller in the process."""

    def __init__(self, delays: dict[str, float]) -> None:
        self.delays = dict(delays)
        self.last: dict[str, float] = {}

    def wait(self, service: str) -> None:
        delay = max(0.0, float(self.delays.get(service, 0.0)))
        elapsed = time.time() - self.last.get(service, 0.0)
        if elapsed < delay:
            time.sleep(delay - elapsed)
        self.last[service] = time.time()


def http_get(session: requests.Session, url: str, retries: int = 3,
             limit_status: tuple[int, ...] = (429,), **kwargs) -> requests.Response:
    """GET that waits and tries again when a site says "too many requests"
    or is briefly unavailable (5xx), instead of failing straight away.
    'limit_status' names the codes a site uses for its rate limit: Open
    Library's covers send 403."""
    kwargs.setdefault("timeout", 30)
    r = session.get(url, **kwargs)
    for attempt in range(retries):
        if r.status_code not in limit_status and r.status_code < 500:
            break
        wait = 5.0 * (attempt + 1)
        try:
            wait = max(wait, min(60.0, float(r.headers.get("Retry-After", 0))))
        except ValueError:
            pass
        print(f"    (server said HTTP {r.status_code}; waiting {wait:.0f}s and trying again)")
        time.sleep(wait)
        r = session.get(url, **kwargs)
    return r

# ---------------------------------------------------------------------
# Files
# ---------------------------------------------------------------------
def write_atomic(path: Path, data: bytes) -> None:
    """Written beside the target and renamed over it, so an interrupted run
    leaves the old file or none, never a truncated one."""
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + ".tmp")
    try:
        tmp.write_bytes(data)
        os.replace(tmp, path)
    except BaseException:
        tmp.unlink(missing_ok=True)
        raise


def load_json(path: Path, default: Any) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return default


def save_json(path: Path, data: Any) -> None:
    write_atomic(path, json.dumps(data, indent=2, ensure_ascii=False).encode("utf-8"))


_UNSAFE = re.compile(r'[/\\:*?"<>|\x00-\x1f]')


def safe_filename(name: str) -> str:
    """A name FAT accepts. The player matches names loosely, so a replaced
    character costs nothing. FAT holds 255 characters a name; 180 leaves
    room for a suffix and the extension."""
    name = re.sub(r"\s+", " ", _UNSAFE.sub(" ", name)).strip()[:180].rstrip(" .")
    return name or "_"

# ---------------------------------------------------------------------
# Names
# ---------------------------------------------------------------------
def normalise_name(value: str) -> str:
    """For comparing a library name with a remote one."""
    value = (value or "").casefold().strip().replace("&", " and ")
    value = re.sub(r"\bthe\b", " ", value)
    value = re.sub(r"\b(feat|ft|featuring)\.?\b.*$", " ", value)
    value = re.sub(r"\([^)]*\)|\[[^]]*\]", " ", value)
    value = re.sub(r"\b(cd|disc|disk)\s*\d+\b", " ", value)
    value = re.sub(r"[^\w\s]+", " ", value, flags=re.UNICODE)
    return re.sub(r"\s+", " ", value).strip()


def similarity(a: str, b: str) -> float:
    na, nb = normalise_name(a), normalise_name(b)
    if not na or not nb:
        return 0.0
    if na == nb:
        return 1.0
    if set(na.split()) == set(nb.split()):
        return 0.98
    return difflib.SequenceMatcher(None, na, nb).ratio()


# The player's name key (apps-ipod/metadata/art_name.c), reproduced exactly:
# what it hashes, these return as a string, so two names the player takes
# for one person give equal strings here. U+00C0-U+017F fold to a base
# letter; upper case stands for two letters and '-' for none.
_LATIN_FOLD = (
    "aaaaaaAceeeeiiiidnooooo-ouuuuyTS"
    "aaaaaaAceeeeiiiidnooooo-ouuuuyTy"
    "aaaaaaccccccccddddeeeeeeeeeegggg"
    "gggghhhhiiiiiiiiiiIIjjkkklllllll"
    "lllnnnnnnnnnooooooOOrrrrrrssssss"
    "ssttttttuuuuuuuuuuuuwwyyyzzzzzzs")
_PAIRS = {"A": "ae", "O": "oe", "S": "ss", "T": "th", "I": "ij"}


def _fold_char(c: int) -> str:
    if 0xFF01 <= c <= 0xFF5E:
        c -= 0xFEE0
    if c < 0x80:
        ch = chr(c)
        if "A" <= ch <= "Z":
            return ch.lower()
        if "a" <= ch <= "z" or "0" <= ch <= "9":
            return ch
        return "and" if ch == "&" else ""
    if (c < 0xC0 or 0x0300 <= c <= 0x036F or 0x2000 <= c <= 0x2BFF
            or 0x3000 <= c <= 0x303F or 0xFE00 <= c <= 0xFE0F):
        return ""
    if c < 0x180:
        f = _LATIN_FOLD[c - 0xC0]
        return "" if f == "-" else _PAIRS.get(f, f)
    return chr(c)


def _fold_part(s: str, first: bool) -> str:
    if first and len(s.encode("utf-8")) > 4 and s[:4].casefold() == "the ":
        s = s[4:]
    return "".join(_fold_char(ord(ch)) for ch in s)


def _symbol_char(c: int) -> str:
    if 0xFF01 <= c <= 0xFF5E:
        c -= 0xFEE0
    if c <= 0x20 or c in (0xA0, 0x3000):
        return ""
    ch = chr(c)
    return ch.lower() if "A" <= ch <= "Z" else ch


def name_key(name: str) -> str:
    """The player's key for a person: "" for a name with nothing left. A name
    of nothing but symbols ("¥$") is kept as written, less spaces and case."""
    if not name:
        return ""
    if len(name.encode("utf-8")) > 5 and name[-5:].casefold() == ", the":
        name = name[:-5]
    key = _fold_part(name, True)
    if not key:
        if len(name.encode("utf-8")) > 4 and name[:4].casefold() == "the ":
            name = name[4:]
        key = "".join(_symbol_char(ord(ch)) for ch in name)
    return key


def name_alt_key(name: str) -> str:
    """"Pratchett, Terry" read as "Terry Pratchett"; "" without exactly one
    comma."""
    if not name or name.count(",") != 1:
        return ""
    before, after = name.split(",")
    if after.startswith(" "):
        after = after[1:]
    return _fold_part(after, True) + _fold_part(before, False)


def work_key(artist: str, title: str) -> str:
    """The player's key for a work, as a file in /Artwork/Covers named
    "<artist> - <title>" (or "<title>") gives it."""
    if not title:
        return ""
    a = _fold_part(artist, True) if artist else ""
    return a + _fold_part(title, not a)


def name_keys(name: str) -> set[str]:
    """Both keys; two names match when the sets meet."""
    return {k for k in (name_key(name), name_alt_key(name)) if k}


def same_person(a: str, b: str) -> bool:
    return bool(name_keys(a) & name_keys(b))

# ---------------------------------------------------------------------
# The library
# ---------------------------------------------------------------------
# What the player counts as spoken word (apps-ipod/database/db_spoken.c):
# a genre containing one of the first list, or exactly one of the second.
# The podcast names are spoken word that is a show, not a book.
_SPOKEN_CONTAINED = ("audiobook", "audio book", "spoken word")
_SPOKEN_EXACT = {"book", "books", "spoken", "speech", "non-music", "nonmusic",
                 "podcast", "podcasts"}
_PODCAST_EXACT = {"podcast", "podcasts"}
# The ebooks the player's Books shelf lists (apps-ipod/files/book_index.c)
EBOOK_EXTS = {".epub", ".fb2", ".mobi", ".prc", ".azw", ".azw3", ".pdb"}

MUSIC, AUDIOBOOK, PODCAST, EBOOK = "music", "audiobook", "podcast", "ebook"


def kind_of_genre(genre: str) -> str:
    g = (genre or "").strip().casefold()
    if g in _PODCAST_EXACT:
        return PODCAST
    if g in _SPOKEN_EXACT or any(n in g for n in _SPOKEN_CONTAINED):
        return AUDIOBOOK
    return MUSIC


@dataclass
class TrackTags:
    path: Path
    artist: str = ""
    album_artist: str = ""
    album: str = ""
    title: str = ""
    genre: str = ""
    duration: float = 0.0
    error: str = ""

    @property
    def kind(self) -> str:
        return kind_of_genre(self.genre)


def first_value(v: Any) -> str:
    if v is None:
        return ""
    if isinstance(v, list):
        return str(v[0]) if v else ""
    return str(v)


def read_tags(path: Path) -> TrackTags:
    try:
        audio = MutagenFile(str(path), easy=True)
        if audio is None:
            return TrackTags(path=path, error="Unsupported metadata")
        tags = audio.tags or {}

        def get(*keys: str) -> str:
            for k in keys:
                try:
                    v = first_value(tags.get(k))
                except (KeyError, ValueError):
                    v = ""
                if v:
                    return v.strip()
            return ""
        length = float(getattr(audio.info, "length", 0.0) or 0.0)
        return TrackTags(path, get("artist"),
                         get("albumartist", "album artist", "performer"),
                         get("album"), get("title"), get("genre"), length)
    except Exception as exc:
        return TrackTags(path=path, error=str(exc))


def iter_audio_files(path: Path) -> Iterable[Path]:
    for p in sorted(path.rglob("*"), key=lambda x: str(x).casefold()):
        if p.is_file() and p.suffix.casefold() in AUDIO_EXTS and not any(part.startswith(".") for part in p.parts):
            yield p


# Top-level folders that hold no library: on any drive, and on a player
_SYSTEM_SKIP = {"$recycle.bin", "system volume information"}
_PLAYER_SKIP = {"artwork", "notes"}


def is_player_root(root: Path) -> bool:
    return (root / ".rockbox").is_dir()


def walk_files(root: Path, exts: set[str]) -> Iterable[Path]:
    """Every file under 'root' with one of 'exts', in name order, past
    hidden folders. On a player, its own folders and the iPod's games are
    passed over."""
    player = is_player_root(root)
    for dirpath, dirnames, filenames in os.walk(root):
        here = Path(dirpath)
        rel = here.relative_to(root)
        keep = []
        for d in sorted(dirnames, key=str.casefold):
            low = d.casefold()
            if d.startswith("."):
                continue
            if not rel.parts and (low in _SYSTEM_SKIP or (player and low in _PLAYER_SKIP)):
                continue
            if player and rel.parts and rel.parts[0].casefold() == "ipod_control" \
                    and len(rel.parts) == 1 and low == "games_ro":
                continue
            keep.append(d)
        dirnames[:] = keep
        for f in sorted(filenames, key=str.casefold):
            if not f.startswith(".") and Path(f).suffix.casefold() in exts:
                yield here / f


def player_root_of(path: Path) -> Optional[Path]:
    """The player 'path' is on (the folder holding .rockbox), or None."""
    path = path.resolve()
    for p in (path, *path.parents):
        if is_player_root(p):
            return p
    return None


def data_dir(root: Path) -> Path:
    """Where a run keeps its cache and manifest: inside .rockbox on a player,
    even when the run was given a folder on it, and the root itself for a
    music folder on a computer."""
    player = player_root_of(root)
    return player / ".rockbox" / "fetch" if player else root

# ---------------------------------------------------------------------
# The manifest
# ---------------------------------------------------------------------
MANIFEST_NAME = "fetch-manifest.json"


def sha1_of(path: Path) -> str:
    h = hashlib.sha1()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 16), b""):
            h.update(block)
    return h.hexdigest()


class Manifest:
    """Every file a tool wrote under a root, with its hash. A file is the
    tool's to rewrite or remove only while it is listed and unchanged;
    anything else is the owner's."""

    def __init__(self, root: Path) -> None:
        self.root = player_root_of(root) or root.resolve()
        self.path = data_dir(root) / MANIFEST_NAME
        self.files: dict[str, dict[str, str]] = load_json(self.path, {}).get("files", {})
        self.unsaved = 0

    def _rel(self, path: Path) -> str:
        """Relative to the root, so a player's list survives a new drive
        letter; a file outside it (--artwork-root) is kept by full path."""
        path = path.resolve()
        try:
            return path.relative_to(self.root.resolve()).as_posix()
        except ValueError:
            return path.as_posix()

    def owns(self, path: Path) -> bool:
        entry = self.files.get(self._rel(path))
        try:
            return bool(entry) and path.is_file() and sha1_of(path) == entry["sha1"]
        except OSError:
            return False

    def may_write(self, path: Path) -> bool:
        return not path.exists() or self.owns(path)

    def write(self, path: Path, data: bytes, tool: str) -> None:
        rel = self._rel(path)
        write_atomic(path, data)
        self.files[rel] = {"tool": tool, "sha1": hashlib.sha1(data).hexdigest()}
        # Saved as it goes, so a run that is killed still knows its files
        self.unsaved += 1
        if self.unsaved >= 20:
            self.save()

    def undo(self, tool: str, dry_run: bool = False) -> tuple[int, int]:
        """Deletes the tool's unedited files; returns (deleted, kept)."""
        deleted = kept = 0
        for rel, entry in list(self.files.items()):
            if entry.get("tool") != tool:
                continue
            path = self.root / rel
            if not path.exists():
                del self.files[rel]
            elif self.owns(path):
                print(f"  {'would delete' if dry_run else 'deleted'}: {rel}")
                if not dry_run:
                    path.unlink()
                    del self.files[rel]
                deleted += 1
            else:
                print(f"  kept, edited since: {rel}")
                kept += 1
        return deleted, kept

    def save(self) -> None:
        save_json(self.path, {"version": 1, "files": self.files})
        self.unsaved = 0
