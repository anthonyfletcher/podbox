#!/usr/bin/env python3
"""
Rockbox Music Artwork Fetcher
Single-file, single-parser artist + album art tool.
"""
from __future__ import annotations

import argparse
import base64
import difflib
import getpass
import html
import json
import os
import re
import sys
import time
from dataclasses import dataclass
from io import BytesIO
from pathlib import Path
from typing import Any, Iterable, Optional
from urllib.parse import quote, quote_plus, urljoin

import requests
from PIL import Image, ImageStat

try:
    from mutagen import File as MutagenFile
    from mutagen.flac import FLAC, Picture
    from mutagen.id3 import APIC, ID3
    from mutagen.mp4 import MP4, MP4Cover
except ModuleNotFoundError:
    MutagenFile = None
    FLAC = Picture = APIC = ID3 = MP4 = MP4Cover = None

SCRIPT_VERSION = "3.8"

AUDIO_EXTS = {".mp3", ".mp2", ".mp1", ".m4a", ".mp4", ".aac", ".flac", ".ogg", ".opus", ".wav", ".wma", ".aiff", ".aif"}
CONFIG_FILENAME = "artist_art_credentials.json"
ARTIST_CACHE_FILENAME = "artist-art-cache.json"
ALBUM_CACHE_FILENAME = "album-art-cache.json"

LASTFM_API = "https://ws.audioscrobbler.com/2.0/"
LASTFM_SITE = "https://www.last.fm/music"
FANART_API = "https://webservice.fanart.tv/v3/music"
MUSICBRAINZ_ARTIST_API = "https://musicbrainz.org/ws/2/artist"
MUSICBRAINZ_RELEASE_API = "https://musicbrainz.org/ws/2/release"
CAA_RELEASE_FRONT = "https://coverartarchive.org/release/{mbid}/front"
THEAUDIODB_API = "https://www.theaudiodb.com/api/v1/json"
THEAUDIODB_PUBLIC_KEY = "123"

PLACEHOLDER_MARKERS = {
    "2a96cbd8b46e442fc41c2b86b821562f", "c6f59c1e5e7240a4c0d427abd71f3dbb",
    "default_artist", "default_album", "default_cover", "placeholder", "noimage", "no_image",
}

# Reputational weighting used after an image has been downloaded and measured.
# These are deliberately modest bonuses: they influence close calls, but a much
# larger / squarer image from a lower-ranked provider can still win.
SOURCE_REPUTATION_WEIGHTS = {
    "embedded": 40.0,
    "Cover Art Archive": 35.0,
    "last.fm-api": 30.0,
    "last.fm-web": 30.0,
    "last.fm": 30.0,
    "TheAudioDB": 20.0,
    "fanart.tv": 10.0,
    "cache": 0.0,
}

@dataclass
class Credentials:
    lastfm_api_key: str = ""
    lastfm_api_secret: str = ""
    fanart_api_key: str = ""
    @property
    def has_lastfm(self) -> bool: return bool(self.lastfm_api_key.strip())
    @property
    def has_fanart(self) -> bool: return bool(self.fanart_api_key.strip())

@dataclass
class TrackTags:
    path: Path
    artist: str = ""
    album_artist: str = ""
    album: str = ""
    error: str = ""

@dataclass
class ImageCandidate:
    source: str
    artist: str
    album: str = ""
    image_url: str = ""
    image_bytes: bytes = b""
    mbid: str = ""
    # Optional provider metadata. These are useful for logging/tie-breaking,
    # but actual image choice is now based on images we really download/open.
    source_width: int = 0
    source_height: int = 0
    source_score: float = 0.0

class RejectedImage(Exception): pass

class RateLimiter:
    def __init__(self, args: argparse.Namespace) -> None:
        self.delays = {
            "lastfm": args.lastfm_rate_limit,
            "fanart": args.fanart_rate_limit,
            "musicbrainz": args.musicbrainz_rate_limit,
            "coverartarchive": args.coverartarchive_rate_limit,
            "theaudiodb": args.theaudiodb_rate_limit,
            "image": args.image_rate_limit,
        }
        self.last: dict[str, float] = {}
    def wait(self, service: str) -> None:
        delay = max(0.0, float(self.delays.get(service, 0.0)))
        elapsed = time.time() - self.last.get(service, 0.0)
        if elapsed < delay:
            time.sleep(delay - elapsed)
        self.last[service] = time.time()

# ---------------------------------------------------------------------
# Credentials and cache
# ---------------------------------------------------------------------
def get_config_path() -> Path:
    if os.name == "nt":
        base = os.environ.get("APPDATA") or str(Path.home() / "AppData" / "Roaming")
        return Path(base) / "RockboxArtistArt" / CONFIG_FILENAME
    base = Path(os.environ.get("XDG_CONFIG_HOME") or Path.home() / ".config")
    return base / "rockbox-artist-art" / CONFIG_FILENAME

def load_credentials() -> Credentials:
    path = get_config_path()
    if not path.exists(): return Credentials()
    try: data = json.loads(path.read_text(encoding="utf-8"))
    except Exception: return Credentials()
    return Credentials(str(data.get("lastfm_api_key") or ""), str(data.get("lastfm_api_secret") or ""), str(data.get("fanart_api_key") or ""))

def save_credentials(creds: Credentials) -> Path:
    path = get_config_path(); path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps({"lastfm_api_key": creds.lastfm_api_key.strip(), "lastfm_api_secret": creds.lastfm_api_secret.strip(), "fanart_api_key": creds.fanart_api_key.strip()}, indent=2), encoding="utf-8")
    try: os.chmod(path, 0o600)
    except Exception: pass
    return path

def install_credentials(args: argparse.Namespace) -> int:
    existing = load_credentials()
    lastfm_key = args.lastfm_api_key if args.lastfm_api_key is not None else existing.lastfm_api_key
    lastfm_secret = args.lastfm_api_secret if args.lastfm_api_secret is not None else existing.lastfm_api_secret
    fanart_key = args.fanart_api_key if args.fanart_api_key is not None else existing.fanart_api_key
    if args.prompt_credentials:
        print("Install API credentials. Leave blank to keep existing value or skip.")
        v = input("Last.fm API key: ").strip()
        if v: lastfm_key = v
        v = getpass.getpass("Last.fm API secret: ").strip()
        if v: lastfm_secret = v
        v = input("fanart.tv API key: ").strip()
        if v: fanart_key = v
    creds = Credentials(lastfm_key or "", lastfm_secret or "", fanart_key or "")
    path = save_credentials(creds)
    print(f"Saved credentials to: {path}")
    print(f"Last.fm enabled  : {creds.has_lastfm}")
    print(f"fanart.tv enabled: {creds.has_fanart}")
    return 0

def load_cache(path: Path, key: str) -> dict[str, Any]:
    if not path.exists(): return {key: {}}
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
        if not isinstance(data.get(key), dict): data[key] = {}
        return data
    except Exception:
        return {key: {}}

def save_cache(path: Path, cache: dict[str, Any]) -> None:
    path.write_text(json.dumps(cache, indent=2, ensure_ascii=False), encoding="utf-8")

# ---------------------------------------------------------------------
# General helpers
# ---------------------------------------------------------------------
def ensure_dependencies() -> bool:
    if MutagenFile is None:
        print("ERROR: missing dependency 'mutagen'. Install with: pip install mutagen pillow requests", file=sys.stderr)
        return False
    return True

def normalise_name(value: str) -> str:
    value = (value or "").casefold().strip().replace("&", " and ")
    value = re.sub(r"\bthe\b", " ", value)
    value = re.sub(r"\b(feat|ft|featuring)\.?\b.*$", " ", value)
    value = re.sub(r"\([^)]*\)|\[[^]]*\]", " ", value)
    value = re.sub(r"\b(cd|disc|disk)\s*\d+\b", " ", value)
    value = re.sub(r"[^\w\s]+", " ", value, flags=re.UNICODE)
    return re.sub(r"\s+", " ", value).strip()

def similarity(a: str, b: str) -> float:
    na, nb = normalise_name(a), normalise_name(b)
    if not na or not nb: return 0.0
    if na == nb: return 1.0
    if set(na.split()) == set(nb.split()): return 0.98
    return difflib.SequenceMatcher(None, na, nb).ratio()

def first_value(v: Any) -> str:
    if v is None: return ""
    if isinstance(v, list): return str(v[0]) if v else ""
    return str(v)

def read_tags(path: Path) -> TrackTags:
    try:
        audio = MutagenFile(str(path), easy=True)
        if audio is None: return TrackTags(path=path, error="Unsupported metadata")
        return TrackTags(path, first_value(audio.get("artist")), first_value(audio.get("albumartist") or audio.get("album artist") or audio.get("performer")), first_value(audio.get("album")))
    except Exception as exc:
        return TrackTags(path=path, error=str(exc))

def iter_audio_files(path: Path) -> Iterable[Path]:
    for p in sorted(path.rglob("*"), key=lambda x: str(x).casefold()):
        if p.is_file() and p.suffix.casefold() in AUDIO_EXTS and not any(part.startswith(".") for part in p.parts):
            yield p

def contains_placeholder_marker(url: str) -> bool:
    low = (url or "").casefold()
    return any(m in low for m in PLACEHOLDER_MARKERS)

def cache_key(*parts: str) -> str:
    return "||".join(normalise_name(p) for p in parts)

def centre_crop_square(img: Image.Image) -> Image.Image:
    if img.width == img.height: return img
    side = min(img.width, img.height)
    return img.crop(((img.width-side)//2, (img.height-side)//2, (img.width-side)//2 + side, (img.height-side)//2 + side))

def image_seems_bad(img: Image.Image, min_source_size: int, source: str = "") -> Optional[str]:
    if img.width < min_source_size or img.height < min_source_size:
        return f"image too small ({img.width}x{img.height}, minimum {min_source_size}x{min_source_size})"
    if source.casefold().startswith("last.fm"):
        sample = img.convert("RGB").resize((64, 64))
        colours = sample.getcolors(maxcolors=4096) or []
        stat = ImageStat.Stat(sample)
        if len(colours) <= 16 and max(stat.stddev) < 55:
            return "suspected Last.fm placeholder/simple graphic"
    return None

def image_from_candidate(c: ImageCandidate, args: argparse.Namespace, session: requests.Session, limiter: RateLimiter) -> Image.Image:
    if c.image_bytes:
        img = Image.open(BytesIO(c.image_bytes)).convert("RGB")
    else:
        if not c.image_url: raise RejectedImage("empty image URL")
        if contains_placeholder_marker(c.image_url): raise RejectedImage(f"known placeholder URL: {c.image_url}")
        limiter.wait("image")
        r = session.get(c.image_url, timeout=60, headers={"User-Agent": "RockboxMusicArt/3.2"})
        r.raise_for_status()
        img = Image.open(BytesIO(r.content)).convert("RGB")
    reason = image_seems_bad(img, args.min_source_size, c.source)
    if reason: raise RejectedImage(reason)
    return img


def unique_candidates(candidates: list[ImageCandidate]) -> list[ImageCandidate]:
    """Remove duplicate candidate URLs while preserving order.

    A few providers return several size URLs for the same underlying image, and
    the Last.fm page parser can discover the same URL repeatedly. We still keep
    embedded artwork, because it has image_bytes rather than a URL.
    """
    out: list[ImageCandidate] = []
    seen: set[str] = set()
    for c in candidates:
        key = c.image_url.strip() if c.image_url else f"bytes:{len(c.image_bytes)}:{c.source}:{c.artist}:{c.album}"
        if not key or key in seen:
            continue
        seen.add(key)
        out.append(c)
    return out


def actual_square_score(width: int, height: int) -> float:
    """Return 1.0 for square, approaching 0.0 as aspect ratio worsens."""
    if width <= 0 or height <= 0:
        return 0.0
    return min(width, height) / max(width, height)


def source_reputation_score(source: str) -> float:
    return SOURCE_REPUTATION_WEIGHTS.get(source, 0.0)

def resolution_score(width: int, height: int, min_source_size: int) -> float:
    """Reward actual downloaded resolution, capped so huge files do not dominate.

    This uses the shortest edge, because artwork ultimately gets square-cropped.
    A 1200x300 banner should not score like a useful 1200x1200 source.
    """
    if width <= 0 or height <= 0:
        return 0.0
    useful_edge = min(width, height)
    baseline = max(1, min_source_size)
    return min(75.0, (useful_edge / baseline) * 25.0)

def score_downloaded_candidate(
    img: Image.Image,
    c: ImageCandidate,
    args: argparse.Namespace,
) -> tuple[float, str]:
    """Score a downloaded candidate using actual dimensions and source quality.

    Selection now considers:
      - actual squareness, so less cropping is preferred;
      - actual useful resolution, based on the downloaded image;
      - source reputation, e.g. Last.fm above fanart.tv;
      - provider metadata/likes as a smaller tie-breaker via c.source_score.
    """
    square = actual_square_score(img.width, img.height)
    square_score = square * 100.0
    res_score = resolution_score(img.width, img.height, args.min_source_size)
    reputation = source_reputation_score(c.source)
    provider_tiebreak = min(15.0, max(0.0, float(c.source_score)))
    score = square_score + res_score + reputation + provider_tiebreak
    meta = ""
    if c.source_width and c.source_height:
        meta = f", provider said {c.source_width}x{c.source_height}"
    reason = (
        f"actual {img.width}x{img.height}, square={square:.3f}, "
        f"resolution={res_score:.1f}, source={reputation:.1f}, "
        f"provider={provider_tiebreak:.1f}{meta}, score={score:.1f}"
    )
    return score, reason

def save_square_jpeg(
    img: Image.Image,
    output_path: Path,
    size: int,
) -> None:
    img = centre_crop_square(img)
    if img.width != size or img.height != size:
        img = img.resize((size, size), Image.Resampling.LANCZOS)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    img.save(output_path, "JPEG", quality=92, optimize=True, progressive=False)

# ---------------------------------------------------------------------
# Last.fm helpers, including robust Rêve-style photo gallery parsing
# ---------------------------------------------------------------------
def lastfm_image_candidates(images: list[dict[str, Any]], source: str, artist_name: str, mbid: str = "") -> list[ImageCandidate]:
    rank = {"mega": 6, "extralarge": 5, "large": 4, "medium": 3, "small": 2, "": 1}
    out: list[ImageCandidate] = []
    seen: set[str] = set()
    for item in images or []:
        url = str(item.get("#text") or item.get("text") or "").strip()
        if not url or contains_placeholder_marker(url) or url in seen:
            continue
        seen.add(url)
        size_name = str(item.get("size") or "").casefold()
        # This is only a source preference. The real choice happens later after
        # downloading/opening the image and measuring the real dimensions.
        out.append(ImageCandidate(source, artist_name, image_url=url, mbid=mbid, source_score=float(rank.get(size_name, 0))))
    return out


def choose_best_lastfm_image(images: list[dict[str, Any]]) -> str:
    # Kept for backward compatibility with any external imports. Artist selection
    # no longer uses this single-URL shortcut.
    candidates = lastfm_image_candidates(images, "last.fm", "")
    return sorted(candidates, key=lambda c: c.source_score, reverse=True)[0].image_url if candidates else ""

def normalise_lastfm_html(text: str) -> str:
    text = html.unescape(text or "")
    text = text.replace("\\/", "/")
    text = text.replace("\\u002B", "+").replace("\\u002b", "+")
    text = text.replace("%2B", "+").replace("%2b", "+")
    return text

def extract_lastfm_image_urls(text: str) -> list[str]:
    text = normalise_lastfm_html(text)
    urls: list[str] = []
    for pattern in (r'https?://lastfm\.freetls\.fastly\.net/i/u/[^"\'<>\s]+', r'https?://[^"\'<>\s]*lastfm[^"\'<>\s]+/i/u/[^"\'<>\s]+'):
        for m in re.findall(pattern, text, flags=re.IGNORECASE):
            u = m.split("?")[0]
            if u not in urls and not contains_placeholder_marker(u): urls.append(u)
    return urls

def extract_lastfm_photo_hashes(text: str) -> list[str]:
    text = normalise_lastfm_html(text)
    hashes: list[str] = []
    for h in re.findall(r'/\+images/([0-9a-fA-F]{32})', text):
        h = h.lower()
        if h not in hashes and not contains_placeholder_marker(h): hashes.append(h)
    return hashes

def lastfm_cdn_urls_from_hash(h: str) -> list[str]:
    h = h.lower().strip()
    if not re.fullmatch(r"[0-9a-f]{32}", h): return []
    return [
        f"https://lastfm.freetls.fastly.net/i/u/500x500/{h}.jpg",
        f"https://lastfm.freetls.fastly.net/i/u/300x300/{h}.jpg",
        f"https://lastfm.freetls.fastly.net/i/u/770x0/{h}.jpg",
        f"https://lastfm.freetls.fastly.net/i/u/{h}.jpg",
    ]

def lookup_lastfm_artist_web_candidates(name: str, session: requests.Session, limiter: RateLimiter, mbid: str = "") -> list[ImageCandidate]:
    """Return Last.fm artist image candidates discovered from public web pages.

    Last.fm's API image list can be sparse or placeholder-heavy for some artists.
    The gallery pages often contain direct CDN image URLs or image hashes, so this
    scraper collects those URLs and lets the normal download/score pipeline decide
    whether they are actually usable.

    This function must be deliberately non-fatal: a layout change, 404, or scrape
    failure should not discard valid Last.fm API candidates.
    """
    artist_name = (name or "").strip()
    if not artist_name:
        return []

    encoded_name = quote(artist_name, safe="")
    pages = [
        f"{LASTFM_SITE}/{encoded_name}",
        f"{LASTFM_SITE}/{encoded_name}/+images",
    ]

    urls: list[str] = []
    seen: set[str] = set()

    for page_url in pages:
        try:
            limiter.wait("lastfm")
            r = session.get(
                page_url,
                timeout=30,
                headers={"User-Agent": "RockboxMusicArt/3.7"},
            )
            if r.status_code == 404:
                continue
            r.raise_for_status()

            text = r.text or ""

            for url in extract_lastfm_image_urls(text):
                if url and url not in seen and not contains_placeholder_marker(url):
                    seen.add(url)
                    urls.append(url)

            for image_hash in extract_lastfm_photo_hashes(text):
                for url in lastfm_cdn_urls_from_hash(image_hash):
                    if url and url not in seen and not contains_placeholder_marker(url):
                        seen.add(url)
                        urls.append(url)

        except Exception:
            # Keep this silent so the caller can decide how much to log. The
            # lookup_lastfm_artist_candidates() wrapper catches/logs web scrape
            # failures without losing Last.fm API results.
            continue

    return [
        ImageCandidate(
            "last.fm-web",
            artist_name,
            image_url=url,
            mbid=mbid,
            source_score=10.0,
        )
        for url in urls
    ]


def lookup_lastfm_artist_candidates(name: str, creds: Credentials, session: requests.Session, limiter: RateLimiter) -> list[ImageCandidate]:
    if not creds.has_lastfm:
        return []
    params = {"method": "artist.getinfo", "artist": name, "api_key": creds.lastfm_api_key, "format": "json", "autocorrect": "1"}
    limiter.wait("lastfm")
    r = session.get(LASTFM_API, params=params, timeout=30); r.raise_for_status()
    artist = r.json().get("artist") or {}
    artist_name = artist.get("name") or name
    mbid = artist.get("mbid") or ""
    out = lastfm_image_candidates(artist.get("image") or [], "last.fm-api", artist_name, mbid)
    try:
        out.extend(lookup_lastfm_artist_web_candidates(artist_name, session, limiter, mbid))
    except Exception as exc:
        # Do not let a Last.fm page-layout/network issue discard API candidates.
        print(f"  candidate: last.fm web scrape error: {exc}")
    return unique_candidates(out)


def lookup_lastfm_artist(name: str, creds: Credentials, session: requests.Session, limiter: RateLimiter) -> Optional[ImageCandidate]:
    # Backward-compatible single-candidate wrapper.
    candidates = lookup_lastfm_artist_candidates(name, creds, session, limiter)
    return candidates[0] if candidates else None

def lookup_musicbrainz_artist_mbid(name: str, args: argparse.Namespace, session: requests.Session, limiter: RateLimiter) -> str:
    headers = {"User-Agent": args.musicbrainz_user_agent, "Accept": "application/json"}
    params = {"query": f'artist:"{name}"', "fmt": "json", "limit": "5"}
    limiter.wait("musicbrainz")
    r = session.get(MUSICBRAINZ_ARTIST_API, params=params, headers=headers, timeout=30); r.raise_for_status()
    artists = r.json().get("artists") or []
    if not artists: return ""
    best = max(artists, key=lambda a: similarity(name, a.get("name", "")) * 100 + float(a.get("score") or 0))
    if similarity(name, best.get("name", "")) < 0.70 and float(best.get("score") or 0) < 80: return ""
    return best.get("id") or ""

def fanart_artist_candidates_from_data(data: dict[str, Any], name: str, mbid: str) -> list[ImageCandidate]:
    out: list[ImageCandidate] = []
    for field, weight in (("artistthumb",40),("artistthumbnail",40),("musicthumb",30),("artistbackground",10)):
        images = data.get(field)
        if not isinstance(images, list):
            continue
        for img in images:
            url = img.get("url")
            if not url:
                continue
            likes = int(img.get("likes") or 0)
            w, h = int(img.get("width") or 0), int(img.get("height") or 0)
            # Provider dimensions are now only a tie-breaker. The final selector
            # downloads candidates and scores actual dimensions.
            provider_square = actual_square_score(w, h) if w and h else 0.5
            source_score = float(weight) + min(10, likes) + (provider_square * 5.0)
            out.append(ImageCandidate("fanart.tv", data.get("name") or name, image_url=url, mbid=mbid, source_width=w, source_height=h, source_score=source_score))
    return unique_candidates(out)


def choose_fanart_image(data: dict[str, Any]) -> str:
    # Kept for backward compatibility. Artist processing now uses all fanart.tv
    # candidates and ranks them after download.
    candidates = fanart_artist_candidates_from_data(data, data.get("name") or "", "")
    return sorted(candidates, key=lambda c: c.source_score, reverse=True)[0].image_url if candidates else ""

def lookup_fanart_artist_candidates(name: str, mbid: str, creds: Credentials, args: argparse.Namespace, session: requests.Session, limiter: RateLimiter) -> list[ImageCandidate]:
    if not creds.has_fanart:
        return []
    if not mbid:
        mbid = lookup_musicbrainz_artist_mbid(name, args, session, limiter)
    if not mbid:
        return []
    limiter.wait("fanart")
    r = session.get(f"{FANART_API}/{mbid}", params={"api_key": creds.fanart_api_key}, timeout=30)
    if r.status_code == 404:
        return []
    r.raise_for_status()
    return fanart_artist_candidates_from_data(r.json(), name, mbid)


def lookup_fanart_artist(name: str, mbid: str, creds: Credentials, args: argparse.Namespace, session: requests.Session, limiter: RateLimiter) -> Optional[ImageCandidate]:
    # Backward-compatible single-candidate wrapper.
    candidates = lookup_fanart_artist_candidates(name, mbid, creds, args, session, limiter)
    return candidates[0] if candidates else None

def lookup_theaudiodb_artist_candidates(name: str, session: requests.Session, limiter: RateLimiter) -> list[ImageCandidate]:
    if normalise_name(name) in {"", "unknown artist", "various artists"}:
        return []
    limiter.wait("theaudiodb")
    r = session.get(f"{THEAUDIODB_API}/{THEAUDIODB_PUBLIC_KEY}/search.php?s={quote_plus(name)}", timeout=30); r.raise_for_status()
    artists = r.json().get("artists") or []
    if not artists:
        return []
    best = max(artists, key=lambda a: similarity(name, a.get("strArtist", "")))
    out: list[ImageCandidate] = []
    fields = (
        ("strArtistThumb", 35.0),
        ("strArtistLogo", 5.0),
        ("strArtistClearart", 5.0),
        ("strArtistFanart", 15.0),
        ("strArtistFanart2", 15.0),
        ("strArtistFanart3", 15.0),
        ("strArtistFanart4", 15.0),
    )
    for field, score in fields:
        url = best.get(field)
        if url:
            out.append(ImageCandidate("TheAudioDB", best.get("strArtist") or name, image_url=url, mbid=best.get("strMusicBrainzID") or "", source_score=score))
    return unique_candidates(out)


def lookup_theaudiodb_artist(name: str, session: requests.Session, limiter: RateLimiter) -> Optional[ImageCandidate]:
    # Backward-compatible single-candidate wrapper.
    candidates = lookup_theaudiodb_artist_candidates(name, session, limiter)
    return candidates[0] if candidates else None

# ---------------------------------------------------------------------
# Album lookups
# ---------------------------------------------------------------------
def extract_embedded_art_bytes(path: Path) -> bytes:
    suffix = path.suffix.casefold()
    try:
        if suffix == ".mp3":
            pics = [f for f in ID3(str(path)).values() if isinstance(f, APIC)]
            if pics:
                pics.sort(key=lambda f:(1 if getattr(f,"type",None)==3 else 0, len(f.data)), reverse=True)
                return pics[0].data
        if suffix == ".flac":
            audio = FLAC(str(path))
            if audio.pictures:
                pics = sorted(audio.pictures, key=lambda p:(1 if p.type==3 else 0, len(p.data)), reverse=True)
                return pics[0].data
        if suffix in {".m4a", ".mp4", ".aac"}:
            audio = MP4(str(path)); covers = audio.tags.get("covr", []) if audio.tags else []
            if covers: return bytes(covers[0])
        audio = MutagenFile(str(path))
        if audio and audio.tags:
            for key in ("metadata_block_picture", "METADATA_BLOCK_PICTURE"):
                values = audio.tags.get(key)
                if values:
                    raw = values[0] if isinstance(values, list) else values
                    return Picture(base64.b64decode(raw)).data
    except Exception:
        return b""
    return b""

def find_embedded_album_art(folder: Path, max_files: int) -> Optional[ImageCandidate]:
    best: tuple[int, bytes] | None = None
    for i, f in enumerate(iter_audio_files(folder), 1):
        data = extract_embedded_art_bytes(f)
        if data and (best is None or len(data) > best[0]): best = (len(data), data)
        if i >= max_files: break
    return ImageCandidate("embedded", folder.parent.name, folder.name, image_bytes=best[1]) if best else None

def lookup_lastfm_album(artist: str, album: str, creds: Credentials, session: requests.Session, limiter: RateLimiter) -> Optional[ImageCandidate]:
    if not creds.has_lastfm: return None
    params = {"method":"album.getinfo", "artist":artist, "album":album, "api_key":creds.lastfm_api_key, "format":"json", "autocorrect":"1"}
    limiter.wait("lastfm")
    r = session.get(LASTFM_API, params=params, timeout=30); r.raise_for_status()
    data = r.json(); obj = data.get("album") or {}
    if not obj or data.get("error"): return None
    url = choose_best_lastfm_image(obj.get("image") or [])
    return ImageCandidate("last.fm", obj.get("artist") or artist, obj.get("name") or album, image_url=url, mbid=obj.get("mbid") or "") if url else None

def lookup_musicbrainz_release_mbid(artist: str, album: str, args: argparse.Namespace, session: requests.Session, limiter: RateLimiter) -> str:
    headers = {"User-Agent": args.musicbrainz_user_agent, "Accept": "application/json"}
    params = {"query": f'artist:"{artist}" AND release:"{album}"', "fmt":"json", "limit":"10"}
    limiter.wait("musicbrainz")
    r = session.get(MUSICBRAINZ_RELEASE_API, params=params, headers=headers, timeout=30); r.raise_for_status()
    releases = r.json().get("releases") or []
    if not releases: return ""
    def score(rel: dict[str, Any]) -> float:
        title = rel.get("title") or ""; ac = " ".join((a.get("name") or "") for a in rel.get("artist-credit", []) if isinstance(a, dict))
        cover = 25 if (rel.get("cover-art-archive") or {}).get("front") else 0
        return similarity(album,title)*100 + similarity(artist,ac)*100 + float(rel.get("score") or 0) + cover
    best = max(releases, key=score)
    return best.get("id") or "" if similarity(album, best.get("title", "")) >= 0.65 else ""

def lookup_cover_art_archive(artist: str, album: str, mbid: str, args: argparse.Namespace, session: requests.Session, limiter: RateLimiter) -> Optional[ImageCandidate]:
    if not mbid: mbid = lookup_musicbrainz_release_mbid(artist, album, args, session, limiter)
    if not mbid: return None
    url = CAA_RELEASE_FRONT.format(mbid=mbid)
    limiter.wait("coverartarchive")
    head = session.head(url, timeout=20, allow_redirects=False, headers={"User-Agent":"RockboxMusicArt/3.2"})
    return ImageCandidate("Cover Art Archive", artist, album, image_url=url, mbid=mbid) if head.status_code in (200,301,302,307) else None

def lookup_theaudiodb_album(artist: str, album: str, session: requests.Session, limiter: RateLimiter) -> Optional[ImageCandidate]:
    limiter.wait("theaudiodb")
    r = session.get(f"{THEAUDIODB_API}/{THEAUDIODB_PUBLIC_KEY}/searchalbum.php?s={quote_plus(artist)}&a={quote_plus(album)}", timeout=30); r.raise_for_status()
    albums = r.json().get("album") or []
    if not albums: return None
    best = max(albums, key=lambda a: similarity(album, a.get("strAlbum", "")) + similarity(artist, a.get("strArtist", "")))
    url = best.get("strAlbumThumb") or ""
    return ImageCandidate("TheAudioDB", best.get("strArtist") or artist, best.get("strAlbum") or album, image_url=url, mbid=best.get("strMusicBrainzID") or "") if url else None

# ---------------------------------------------------------------------
# Scanning and processing
# ---------------------------------------------------------------------
def get_artist_folders(root: Path) -> list[Path]:
    out=[]
    for c in sorted((p for p in root.iterdir() if p.is_dir()), key=lambda p:p.name.casefold()):
        if c.name.casefold() in {"$recycle.bin", "system volume information"}: continue
        try: next(iter_audio_files(c)); out.append(c)
        except StopIteration: pass
    return out

def get_album_folders(root: Path) -> list[Path]:
    out=[]
    for ad in sorted((p for p in root.iterdir() if p.is_dir()), key=lambda p:p.name.casefold()):
        if ad.name.casefold() in {"$recycle.bin", "system volume information"}: continue
        for al in sorted((p for p in ad.iterdir() if p.is_dir()), key=lambda p:p.name.casefold()):
            try: next(iter_audio_files(al)); out.append(al)
            except StopIteration: pass
    return out

def verify_artist_folder(folder: Path, args: argparse.Namespace) -> tuple[bool,str,str]:
    best_score=0.0; best_tag=""
    for i,f in enumerate(iter_audio_files(folder),1):
        try: rel=f.relative_to(folder)
        except ValueError: continue
        if len(rel.parts) < 2: continue
        t=read_tags(f)
        for tv in (t.album_artist,t.artist):
            score=similarity(folder.name,tv)
            if score>best_score: best_score=score; best_tag=tv
            if score>=args.match_threshold: return True, tv.strip() or folder.name, f"matched '{tv}' at {score:.0%}"
        if i>=args.max_files_per_artist: break
    return False, folder.name, f"best tag match {best_score:.0%}: '{best_tag}'"

def verify_album_folder(folder: Path, args: argparse.Namespace) -> tuple[bool,str,str,str]:
    best_as=best_ls=0.0; best_a=best_l=""
    for i,f in enumerate(iter_audio_files(folder),1):
        t=read_tags(f); ta=t.album_artist or t.artist
        a_s=similarity(folder.parent.name,ta); l_s=similarity(folder.name,t.album)
        if a_s>best_as: best_as=a_s; best_a=ta
        if l_s>best_ls: best_ls=l_s; best_l=t.album
        if a_s>=args.match_threshold and l_s>=args.match_threshold:
            return True, ta.strip() or folder.parent.name, t.album.strip() or folder.name, f"matched artist '{ta}' at {a_s:.0%}, album '{t.album}' at {l_s:.0%}"
        if i>=args.max_files_per_album: break
    return False, folder.parent.name, folder.name, f"best artist match {best_as:.0%}: '{best_a}', best album match {best_ls:.0%}: '{best_l}'"

def prompt_artist(name: str, reason: str) -> Optional[str]:
    print(f"\nCould not fetch artist artwork for: {name}\nReason: {reason}\nEnter a different search name, press Enter to skip, or type !ignore to skip:")
    v=input("> ").strip()
    return None if not v or v.casefold()=="!ignore" else v

def prompt_album(artist: str, album: str, reason: str) -> Optional[tuple[str,str]]:
    print(f"\nCould not fetch album artwork for: {artist} / {album}\nReason: {reason}\nEnter an alternative artist name, press Enter to skip, or type !ignore to skip:")
    a=input("Artist> ").strip()
    if not a or a.casefold()=="!ignore": return None
    print("Enter an alternative album name, or press Enter to keep the current album name:")
    return a, input("Album> ").strip() or album

def try_candidates(candidates: list[ImageCandidate], output_path: Path, args: argparse.Namespace, session: requests.Session, limiter: RateLimiter) -> tuple[bool,str,str]:
    notes=[]
    validated: list[tuple[float, ImageCandidate, Image.Image, str]] = []
    candidates = unique_candidates(candidates)
    for c in candidates:
        try:
            print(f"  validating: {c.source} image")
            img = image_from_candidate(c,args,session,limiter)
            score, reason = score_downloaded_candidate(img, c, args)
            print(f"    candidate score: {reason}")
            validated.append((score, c, img, reason))
        except Exception as exc:
            note=f"{c.source} failed/rejected: {exc}"
            notes.append(note)
            print(f"  {note}; trying next source")
    if not validated:
        return False,"","; ".join(notes) or "No artwork found"
    validated.sort(key=lambda x: x[0], reverse=True)
    score, c, img, reason = validated[0]
    try:
        save_square_jpeg(img, output_path, args.max_size)
        print(f"  selected: {c.source} ({reason})")
        print(f"  saved: {output_path} ({c.source})")
        return True,c.source,""
    except Exception as exc:
        note=f"{c.source} failed/rejected while saving: {exc}"
        notes.append(note)
        print(f"  {note}")
        return False,"","; ".join(notes) or "No artwork found"

def cap_provider_candidates(label: str, candidates: list[ImageCandidate], args: argparse.Namespace) -> list[ImageCandidate]:
    """Sort and cap candidate images retained from one provider.

    The cap is applied before download/scoring, so it limits network work while
    still letting the final selector compare the retained images across all
    providers. A value of 0 means unlimited.
    """
    candidates = unique_candidates(candidates)
    original_count = len(candidates)
    candidates = sorted(candidates, key=lambda c: float(c.source_score), reverse=True)

    limit = max(0, int(getattr(args, "max_candidates_per_provider", 3)))
    if limit and original_count > limit:
        candidates = candidates[:limit]
        print(f"  candidate: {label} returned {original_count} usable artist image(s), keeping best {limit}")
    elif original_count:
        print(f"  candidate: {label} returned {original_count} usable artist image(s)")
    else:
        print(f"  candidate: {label} returned no usable artist image")

    return candidates


def artist_candidates(name: str, creds: Credentials, args: argparse.Namespace, session: requests.Session, limiter: RateLimiter, mbid: str="") -> list[ImageCandidate]:
    out=[]
    calls=[
        ("last.fm", lambda: lookup_lastfm_artist_candidates(name,creds,session,limiter)),
        ("fanart.tv", lambda: lookup_fanart_artist_candidates(name,mbid,creds,args,session,limiter)),
        ("TheAudioDB", lambda: [] if args.no_theaudiodb else lookup_theaudiodb_artist_candidates(name,session,limiter)),
    ]
    for label,fn in calls:
        try:
            cs=cap_provider_candidates(label, fn() or [], args)
            if cs:
                out.extend(cs)
                if cs[-1].mbid:
                    mbid=cs[-1].mbid
        except Exception as exc:
            print(f"  candidate: {label} error: {exc}")
    out = unique_candidates(out)
    print(f"  candidate pool: {len(out)} unique artist image(s) to download/compare")
    return out

def album_candidates(artist: str, album: str, creds: Credentials, args: argparse.Namespace, session: requests.Session, limiter: RateLimiter, mbid: str="") -> list[ImageCandidate]:
    out=[]
    calls=[("last.fm", lambda: lookup_lastfm_album(artist,album,creds,session,limiter)), ("Cover Art Archive", lambda: lookup_cover_art_archive(artist,album,mbid,args,session,limiter)), ("TheAudioDB", lambda: None if args.no_theaudiodb else lookup_theaudiodb_album(artist,album,session,limiter))]
    for label,fn in calls:
        try:
            c=fn()
            if c: out.append(c); print(f"  candidate: {c.source} -> {c.artist} / {c.album}")
            else: print(f"  candidate: {label} returned no usable album image")
        except Exception as exc: print(f"  candidate: {label} error: {exc}")
        if out and out[-1].mbid: mbid=out[-1].mbid
    return out

def process_artist_folder(folder: Path, args: argparse.Namespace, creds: Credentials, session: requests.Session, limiter: RateLimiter, cache: dict[str,Any]) -> tuple[bool,str]:
    print(f"\n[{folder.name}]")
    output=folder/args.output_name
    if output.exists() and not args.overwrite: print(f"  skip: artwork already exists: {output.name}"); return False,"exists"
    ok,name,reason=verify_artist_folder(folder,args)
    if not ok:
        print(f"  folder check: no confident tag match ({reason})")
        if not args.interactive: return False,"unmatched"
        override=prompt_artist(folder.name,reason)
        if not override: return False,"unmatched"
        name=override
    else: print(f"  folder check: OK, {reason}")
    if args.dry_run: print(f"  dry-run: would search artist artwork for '{name}'"); return False,"dry-run"
    key=cache_key(name); entry=cache.get("artists",{}).get(key,{})
    if entry.get("status")=="found" and entry.get("image_url") and not args.ignore_cache:
        candidates=[ImageCandidate(entry.get("source") or "cache", entry.get("artist_name") or name, image_url=entry.get("image_url") or "", mbid=entry.get("mbid") or "")]
    elif entry.get("status")=="not_found" and not args.retry_not_found and not args.ignore_cache:
        print(f"  cache: {entry.get('reason') or 'cached as not found'}"); return False,"not-found-cache"
    else:
        candidates=artist_candidates(name, creds, args, session, limiter, entry.get("mbid") or "")
    ok,source,notes=try_candidates(candidates, output, args, session, limiter)
    if ok:
        c=next((x for x in candidates if x.source==source), candidates[0])
        cache["artists"][key]={"status":"found","source":c.source,"artist_name":c.artist,"image_url":c.image_url,"mbid":c.mbid}
        return True,source
    if args.interactive:
        override=prompt_artist(name, notes or "No source returned artwork")
        if override:
            candidates=artist_candidates(override, creds, args, session, limiter)
            ok,source,notes=try_candidates(candidates, output, args, session, limiter)
            if ok:
                c=next((x for x in candidates if x.source==source), candidates[0])
                cache["artists"][cache_key(override)]={"status":"found","source":c.source,"artist_name":c.artist,"image_url":c.image_url,"mbid":c.mbid}
                return True,source
    cache["artists"][key]={"status":"not_found","artist_name":name,"reason":notes or "No artwork found"}
    print(f"  artwork: {notes or 'No artwork found'}")
    return False,"not-found"

def process_album_folder(folder: Path, args: argparse.Namespace, creds: Credentials, session: requests.Session, limiter: RateLimiter, cache: dict[str,Any]) -> tuple[bool,str]:
    print(f"\n[{folder.parent.name} / {folder.name}]")
    output=folder/args.output_name
    if output.exists() and not args.overwrite: print(f"  skip: artwork already exists: {output.name}"); return False,"exists"
    ok,artist,album,reason=verify_album_folder(folder,args)
    if not ok:
        print(f"  folder check: no confident tag match ({reason})")
        if not args.interactive: return False,"unmatched"
        override=prompt_album(folder.parent.name,folder.name,reason)
        if not override: return False,"unmatched"
        artist,album=override
    else: print(f"  folder check: OK, {reason}")
    if args.dry_run: print(f"  dry-run: would try embedded artwork first, then online lookup for '{artist} / {album}'"); return False,"dry-run"
    candidates=[]; embedded=find_embedded_album_art(folder,args.max_files_per_album)
    if embedded: embedded.artist=artist; embedded.album=album; candidates.append(embedded); print("  candidate: embedded artwork found in audio file")
    else: print("  candidate: no embedded artwork found in checked audio files")
    key=cache_key(artist,album); entry=cache.get("albums",{}).get(key,{})
    if not candidates:
        if entry.get("status")=="found" and entry.get("image_url") and not args.ignore_cache:
            candidates=[ImageCandidate(entry.get("source") or "cache", entry.get("artist_name") or artist, entry.get("album_name") or album, image_url=entry.get("image_url") or "", mbid=entry.get("mbid") or "")]
        elif entry.get("status")=="not_found" and not args.retry_not_found and not args.ignore_cache:
            print(f"  cache: {entry.get('reason') or 'cached as not found'}"); return False,"not-found-cache"
        else:
            candidates=album_candidates(artist, album, creds, args, session, limiter, entry.get("mbid") or "")
    ok,source,notes=try_candidates(candidates, output, args, session, limiter)
    if ok:
        if source != "embedded":
            c=next((x for x in candidates if x.source==source), candidates[0])
            cache["albums"][key]={"status":"found","source":c.source,"artist_name":c.artist,"album_name":c.album,"image_url":c.image_url,"mbid":c.mbid}
        return True,source
    if args.interactive:
        override=prompt_album(artist, album, notes or "No source returned artwork")
        if override:
            oa,ob=override; candidates=album_candidates(oa,ob,creds,args,session,limiter)
            ok,source,notes=try_candidates(candidates, output, args, session, limiter)
            if ok:
                c=next((x for x in candidates if x.source==source), candidates[0])
                cache["albums"][cache_key(oa,ob)]={"status":"found","source":c.source,"artist_name":c.artist,"album_name":c.album,"image_url":c.image_url,"mbid":c.mbid}
                return True,source
    cache["albums"][key]={"status":"not_found","artist_name":artist,"album_name":album,"reason":notes or "No artwork found"}
    print(f"  artwork: {notes or 'No artwork found'}")
    return False,"not-found"

def run_artist(root: Path, args: argparse.Namespace, creds: Credentials, session: requests.Session, limiter: RateLimiter) -> int:
    cache_path=root/ARTIST_CACHE_FILENAME; cache=load_cache(cache_path,"artists"); folders=get_artist_folders(root)
    print("\n=== Artist artwork ==="); print(f"Artist folders : {len(folders)}"); print(f"Cache file     : {cache_path}")
    saved=0; by={}
    try:
        for i,folder in enumerate(folders,1):
            print(f"\n--- Artist {i}/{len(folders)} ---")
            ok,src=process_artist_folder(folder,args,creds,session,limiter,cache)
            if ok: saved+=1; by[src]=by.get(src,0)+1
    finally: save_cache(cache_path,cache)
    print("\nArtist summary\n--------------"); print(f"Artist folders scanned : {len(folders)}"); print(f"Artist artwork saved   : {saved}")
    for src,count in sorted(by.items()): print(f"  {src:<18}: {count}")
    return 0

def run_album(root: Path, args: argparse.Namespace, creds: Credentials, session: requests.Session, limiter: RateLimiter) -> int:
    cache_path=root/ALBUM_CACHE_FILENAME; cache=load_cache(cache_path,"albums"); folders=get_album_folders(root)
    print("\n=== Album artwork ==="); print(f"Album folders  : {len(folders)}"); print(f"Cache file     : {cache_path}")
    saved=0; by={}
    try:
        for i,folder in enumerate(folders,1):
            print(f"\n--- Album {i}/{len(folders)} ---")
            ok,src=process_album_folder(folder,args,creds,session,limiter,cache)
            if ok: saved+=1; by[src]=by.get(src,0)+1
    finally: save_cache(cache_path,cache)
    print("\nAlbum summary\n-------------"); print(f"Album folders scanned : {len(folders)}"); print(f"Album artwork saved   : {saved}")
    for src,count in sorted(by.items()): print(f"  {src:<18}: {count}")
    return 0

def run_both_single_pass(root: Path, args: argparse.Namespace, creds: Credentials, session: requests.Session, limiter: RateLimiter) -> int:
    """Process artist and album artwork in a single top-level library traversal.

    The earlier clean single-parser build called run_album() and then run_artist(), which
    caused two independent passes over the library. This function walks each artist folder
    once, processes the artist and then its album folders while that branch is already in
    hand, and writes both caches at the end.
    """
    artist_cache_path = root / ARTIST_CACHE_FILENAME
    album_cache_path = root / ALBUM_CACHE_FILENAME
    artist_cache = load_cache(artist_cache_path, "artists")
    album_cache = load_cache(album_cache_path, "albums")

    artist_saved = 0
    album_saved = 0
    artist_by_source: dict[str, int] = {}
    album_by_source: dict[str, int] = {}
    artist_count = 0
    album_count = 0

    print("\n=== Combined artist + album artwork ===")
    print(f"Artist cache   : {artist_cache_path}")
    print(f"Album cache    : {album_cache_path}")
    print("Traversal      : single pass over artist folders")

    try:
        artist_dirs = sorted((p for p in root.iterdir() if p.is_dir()), key=lambda p: p.name.casefold())
        for artist_dir in artist_dirs:
            if artist_dir.name.casefold() in {"$recycle.bin", "system volume information"}:
                continue

            # Work out the album folders while we are already in this artist branch.
            album_dirs: list[Path] = []
            for album_dir in sorted((p for p in artist_dir.iterdir() if p.is_dir()), key=lambda p: p.name.casefold()):
                try:
                    next(iter_audio_files(album_dir))
                    album_dirs.append(album_dir)
                except StopIteration:
                    continue

            # Treat it as an artist only if it has at least one album folder containing audio,
            # or any audio somewhere under it.
            has_artist_audio = bool(album_dirs)
            if not has_artist_audio:
                try:
                    next(iter_audio_files(artist_dir))
                    has_artist_audio = True
                except StopIteration:
                    has_artist_audio = False
            if not has_artist_audio:
                continue

            artist_count += 1
            print(f"\n=== Artist branch {artist_count}: {artist_dir.name} ===")

            ok, source = process_artist_folder(artist_dir, args, creds, session, limiter, artist_cache)
            if ok:
                artist_saved += 1
                artist_by_source[source] = artist_by_source.get(source, 0) + 1

            for album_dir in album_dirs:
                album_count += 1
                print(f"\n--- Album in {artist_dir.name} ({album_count}) ---")
                ok, source = process_album_folder(album_dir, args, creds, session, limiter, album_cache)
                if ok:
                    album_saved += 1
                    album_by_source[source] = album_by_source.get(source, 0) + 1
    finally:
        save_cache(artist_cache_path, artist_cache)
        save_cache(album_cache_path, album_cache)

    print("\nCombined summary")
    print("----------------")
    print(f"Artist folders scanned : {artist_count}")
    print(f"Artist artwork saved   : {artist_saved}")
    for source, count in sorted(artist_by_source.items()):
        print(f"  artist {source:<14}: {count}")
    print(f"Album folders scanned  : {album_count}")
    print(f"Album artwork saved    : {album_saved}")
    for source, count in sorted(album_by_source.items()):
        print(f"  album  {source:<14}: {count}")
    return 0

# ---------------------------------------------------------------------
# Single parser
# ---------------------------------------------------------------------
def build_parser() -> argparse.ArgumentParser:
    p=argparse.ArgumentParser(description="Single-file Rockbox artwork fetcher with one parser for artist and album artwork.")
    p.add_argument("mode", nargs="?", choices=("artist","album","both"), default="both", help="What to process. Default: both")
    p.add_argument("music_root", nargs="?", help="Root folder containing <artist>/<album>/<music files>")
    p.add_argument("--install-credentials", action="store_true", help="Install/update Last.fm and fanart.tv credentials, then exit")
    p.add_argument("--prompt-credentials", action="store_true", help="Prompt interactively for credentials")
    p.add_argument("--lastfm-api-key", default=None, help="Last.fm API key to install")
    p.add_argument("--lastfm-api-secret", default=None, help="Last.fm shared secret to install")
    p.add_argument("--fanart-api-key", default=None, help="fanart.tv API key to install")
    p.add_argument("--show-credentials-status", action="store_true", help="Show source/credential status, then exit")
    p.add_argument("--output-name", default="folder.jpg", choices=("folder.jpg","cover.jpg"), help="Image filename to create")
    p.add_argument("--max-size", type=int, default=300, help="Final square output size in pixels")
    p.add_argument("--min-source-size", type=int, default=300, help="Reject source images smaller than this width/height")
    p.add_argument("--match-threshold", type=float, default=0.78, help="Fuzzy folder/tag match threshold")
    p.add_argument("--max-files-per-artist", type=int, default=25, help="Files to inspect per artist folder")
    p.add_argument("--max-files-per-album", type=int, default=20, help="Files to inspect per album folder")
    p.add_argument("--interactive", action="store_true", help="Prompt for alternative search names")
    p.add_argument("--overwrite", action="store_true", help="Replace existing artwork")
    p.add_argument("--dry-run", action="store_true", help="Show what would happen without writing images")
    p.add_argument("--retry-not-found", action="store_true", help="Retry cached not-found items")
    p.add_argument("--ignore-cache", action="store_true", help="Ignore cached decisions")
    p.add_argument("--no-theaudiodb", action="store_true", help="Disable TheAudioDB fallback")
    p.add_argument("--max-candidates-per-provider", type=int, default=3, help="Maximum artist candidate images retained from each provider before download/scoring; 0 = unlimited")
    p.add_argument("--lastfm-rate-limit", type=float, default=1.0, help="Sets the rate limit for api calls")
    p.add_argument("--fanart-rate-limit", type=float, default=1.0, help="Sets the rate limit for api calls")
    p.add_argument("--musicbrainz-rate-limit", type=float, default=1.1, help="Sets the rate limit for api calls")
    p.add_argument("--coverartarchive-rate-limit", type=float, default=1.0, help="Sets the rate limit for api calls")
    p.add_argument("--theaudiodb-rate-limit", type=float, default=2.5, help="Sets the rate limit for api calls")
    p.add_argument("--image-rate-limit", type=float, default=0.2, help="Sets the rate limit for api calls")
    p.add_argument("--musicbrainz-user-agent", default="RockboxMusicArt/3.2 (local library artwork tool)", help="Specifies the user agent for requests")
    return p

def main(argv: Optional[list[str]]=None) -> int:
    args=build_parser().parse_args(argv)
    if args.install_credentials:
        if not any([args.lastfm_api_key,args.lastfm_api_secret,args.fanart_api_key,args.prompt_credentials]): args.prompt_credentials=True
        return install_credentials(args)
    creds=load_credentials()
    if args.show_credentials_status:
        print(f"Script version  : {SCRIPT_VERSION}"); print(f"Credential file : {get_config_path()}")
        print(f"Last.fm enabled : {creds.has_lastfm}"); print(f"fanart.tv enabled: {creds.has_fanart}")
        print("MusicBrainz API : no key used"); print(f"TheAudioDB enabled: {not args.no_theaudiodb}")
        return 0
    if not ensure_dependencies(): return 2
    if not args.music_root:
        print("ERROR: music_root is required unless using --install-credentials or --show-credentials-status", file=sys.stderr); return 2
    root=Path(args.music_root).expanduser().resolve()
    if not root.exists() or not root.is_dir(): print(f"ERROR: music_root is not a folder: {root}", file=sys.stderr); return 2
    print(f"Script version  : {SCRIPT_VERSION}"); print("JPEG output     : baseline/non-progressive"); print(f"Mode            : {args.mode}")
    print(f"Music root      : {root}"); print(f"Output name     : {args.output_name}"); print(f"Output size     : {args.max_size}x{args.max_size}")
    print(f"Min source size : {args.min_source_size}x{args.min_source_size}"); print(f"Dry run         : {args.dry_run}")
    session=requests.Session(); limiter=RateLimiter(args); rc=0
    if args.mode == "both":
        rc = run_both_single_pass(root,args,creds,session,limiter)
    elif args.mode == "album":
        rc = run_album(root,args,creds,session,limiter)
    elif args.mode == "artist":
        rc = run_artist(root,args,creds,session,limiter)
    return rc

if __name__ == "__main__":
    raise SystemExit(main())
