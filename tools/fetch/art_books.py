"""
art_fetch's spoken-word kinds: covers for audiobooks, podcasts and ebooks, and
pictures of authors, written into the player's /Artwork folder.

A book's or show's cover goes to Artwork/Covers/<Author> - <Title>.jpg (a
show's to <Show>.jpg) and an author's picture to Artwork/People/<Author>.jpg,
because a folder image belongs to a book only when it has the folder to
itself. Nothing is fetched for anything the player can already show a
picture for. Print covers are written whole, not cropped: the player crops.

Parts, in order:
  - what the library holds: audio books and shows, ebooks, identifiers
  - what the player already has
  - sources: Audnexus, Apple, Open Library, Google Books, podcast feeds
  - the run
"""
from __future__ import annotations

import argparse
import re
import xml.etree.ElementTree as ET
import zipfile
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Optional

import requests

from fetchlib import (AUDIO_EXTS, AUDIOBOOK, EBOOK, EBOOK_EXTS, MUSIC, PODCAST,
                      Manifest, RateLimiter, data_dir, http_get, load_json,
                      name_key, name_keys, player_root_of, read_tags,
                      safe_filename, same_person, save_json, similarity,
                      walk_files, work_key)

IMAGE_EXTS = {".jpg", ".jpeg", ".png", ".bmp", ".gif", ".webp"}
CACHE_NAME = "books-art-cache.json"
APPLE_SEARCH = "https://itunes.apple.com/search"
AUDNEXUS = "https://api.audnex.us/books"
OL_SEARCH = "https://openlibrary.org/search.json"
OL_COVERS = "https://covers.openlibrary.org"
GOOGLE_BOOKS = "https://www.googleapis.com/books/v1/volumes"

# ---------------------------------------------------------------------
# What the library holds
# ---------------------------------------------------------------------
@dataclass
class Work:
    kind: str                       # AUDIOBOOK, PODCAST or EBOOK
    author: str                     # the author, or a show's host
    title: str                      # the book, or the show
    files: list[Path] = field(default_factory=list)
    isbn: str = ""
    asin: str = ""
    has_cover: bool = False         # an ebook with a cover inside it

    @property
    def label(self) -> str:
        return f"{self.author} - {self.title}" if self.author else self.title


def isbn_valid(s: str) -> bool:
    d = re.sub(r"[^0-9Xx]", "", s)
    if len(d) == 13 and d.isdigit():
        return sum(int(c) * (1 if i % 2 == 0 else 3) for i, c in enumerate(d)) % 10 == 0
    if len(d) == 10 and d[:9].isdigit():
        total = sum((10 - i) * (10 if c in "Xx" else int(c)) for i, c in enumerate(d))
        return total % 11 == 0
    return False


def find_isbn(text: str) -> str:
    for m in re.finditer(r"(97[89][\d\- ]{10,16}|\b\d{9}[\dXx]\b)", text or ""):
        cand = re.sub(r"[^0-9Xx]", "", m.group(1))
        if isbn_valid(cand):
            return cand.upper()
    return ""


def find_asin(text: str) -> str:
    m = re.search(r"\b(B0[0-9A-Z]{8})\b", text or "")
    return m.group(1) if m else ""


def tag_ids(path: Path) -> tuple[str, str]:
    """An ISBN and an Audible ASIN from a file's own tags, where a tagger
    or ripper put them."""
    isbn = asin = ""
    try:
        from mutagen import File as MutagenFile
        audio = MutagenFile(str(path))
        tags = getattr(audio, "tags", None)
        if not tags:
            return "", ""
        values: dict[str, str] = {}
        if hasattr(tags, "getall"):                       # ID3
            for frame in tags.getall("TXXX"):
                values[frame.desc.casefold()] = str(frame.text[0]) if frame.text else ""
        else:                                             # MP4, Vorbis, FLAC
            for k, v in tags.items():
                k = k.casefold().replace("----:com.apple.itunes:", "")
                v = v[0] if isinstance(v, list) and v else v
                values[k] = v.decode("utf-8", "replace") if isinstance(v, bytes) else str(v)
        isbn = find_isbn(values.get("isbn", ""))
        asin = find_asin(values.get("asin", "") or values.get("audible_asin", ""))
    except Exception:
        pass
    return isbn, asin


def audio_works(root: Path) -> list[Work]:
    """The books and shows among the audio, grouped by author and album as
    the player groups them."""
    works: dict[tuple[str, str, str], Work] = {}
    for path in walk_files(root, AUDIO_EXTS):
        t = read_tags(path)
        if t.error or t.kind == MUSIC or not t.album:
            continue
        author = t.album_artist or t.artist
        key = (t.kind, name_key(author) if t.kind != PODCAST else "", name_key(t.album))
        w = works.setdefault(key, Work(t.kind, author, t.album))
        w.files.append(path)
    for w in works.values():
        for path in w.files[:5]:
            isbn, asin = tag_ids(path)
            w.isbn, w.asin = w.isbn or isbn, w.asin or asin
        names = " ".join({str(p.parent) for p in w.files} | {p.name for p in w.files[:5]})
        w.isbn = w.isbn or find_isbn(names)
        w.asin = w.asin or find_asin(names)
    return list(works.values())


def _xml_text(el: Optional[ET.Element]) -> str:
    return " ".join("".join(el.itertext()).split()) if el is not None else ""


def epub_work(path: Path) -> Optional[Work]:
    with zipfile.ZipFile(path) as z:
        container = ET.fromstring(z.read("META-INF/container.xml"))
        rootfile = next(e for e in container.iter() if e.tag.endswith("rootfile"))
        opf = ET.fromstring(z.read(rootfile.get("full-path")))
    title = author = isbn = ""
    cover = False
    for e in opf.iter():
        tag = e.tag.rsplit("}", 1)[-1]
        if tag == "title" and not title:
            title = _xml_text(e)
        elif tag == "creator" and not author:
            author = _xml_text(e)
        elif tag == "identifier" and not isbn:
            isbn = find_isbn(_xml_text(e))
        elif tag == "meta" and e.get("name") == "cover":
            cover = True
        elif tag == "item" and "cover-image" in (e.get("properties") or ""):
            cover = True
    return Work(EBOOK, author, title, [path], isbn=isbn, has_cover=cover) if title else None


def fb2_work(path: Path) -> Optional[Work]:
    root = ET.parse(path).getroot()
    def find(*names: str) -> Optional[ET.Element]:
        for e in root.iter():
            if e.tag.rsplit("}", 1)[-1] == names[0]:
                if len(names) == 1:
                    return e
                for c in e.iter():
                    if c.tag.rsplit("}", 1)[-1] == names[1]:
                        return c
        return None
    title = _xml_text(find("title-info", "book-title"))
    author_el = find("title-info", "author")
    author = ""
    if author_el is not None:
        parts = {c.tag.rsplit("}", 1)[-1]: _xml_text(c) for c in author_el}
        # As the player names the author: first, middle, last, else nickname
        author = (" ".join(p for p in (parts.get("first-name"), parts.get("middle-name"),
                                       parts.get("last-name")) if p) or parts.get("nickname", ""))
    isbn = find_isbn(_xml_text(find("publish-info", "isbn")))
    cover = find("title-info", "coverpage") is not None
    return Work(EBOOK, author, title, [path], isbn=isbn, has_cover=cover) if title else None


def mobi_work(path: Path) -> Optional[Work]:
    """A MOBI's EXTH header: 100 author, 104 ISBN, 503 title, 201 the
    cover's record."""
    data = path.read_bytes()[:1 << 20]
    rec0 = int.from_bytes(data[78:82], "big")
    exth = data.find(b"EXTH", rec0)
    if exth < 0:
        return None
    count = int.from_bytes(data[exth + 8:exth + 12], "big")
    pos, found = exth + 12, {}
    for _ in range(count):
        kind = int.from_bytes(data[pos:pos + 4], "big")
        size = int.from_bytes(data[pos + 4:pos + 8], "big")
        if size < 8:
            break
        found.setdefault(kind, data[pos + 8:pos + size])
        pos += size
    text = {k: v.decode("utf-8", "replace").strip() for k, v in found.items() if k in (100, 104, 503)}
    title = text.get(503) or path.stem
    return Work(EBOOK, text.get(100, ""), title, [path], isbn=find_isbn(text.get(104, "")),
                has_cover=201 in found)


def ebook_works(root: Path) -> list[Work]:
    out = []
    for path in walk_files(root, EBOOK_EXTS):
        ext = path.suffix.casefold()
        try:
            w = (epub_work(path) if ext == ".epub" else fb2_work(path) if ext == ".fb2"
                 else mobi_work(path))
        except Exception as exc:
            print(f"  could not read {path.name}: {exc}")
            w = None
        if w:
            out.append(w)
    return out

# ---------------------------------------------------------------------
# What the player already has
# ---------------------------------------------------------------------
class Central:
    """The files of Artwork/Covers or Artwork/People, by the player's keys:
    a person's name also by its "Last, First" form, a cover's by its main
    key alone."""

    def __init__(self, folder: Path, people: bool) -> None:
        self.people = people
        self.keys: set[str] = set()
        if folder.is_dir():
            for p in folder.iterdir():
                if p.suffix.casefold() in IMAGE_EXTS:
                    self.add(p.stem)

    def has(self, *keys: str) -> bool:
        return any(k and k in self.keys for k in keys)

    def add(self, name: str) -> None:
        self.keys |= name_keys(name) if self.people else {name_key(name)}


def folder_image(folder: Path) -> Optional[Path]:
    for name in ("cover", "folder"):
        for ext in IMAGE_EXTS:
            p = folder / (name + ext)
            if p.is_file():
                return p
    return None


def has_embedded_art(path: Path) -> bool:
    try:
        from mutagen import File as MutagenFile
        audio = MutagenFile(str(path))
        tags = getattr(audio, "tags", None)
        if not tags:
            return bool(getattr(audio, "pictures", None))
        if hasattr(tags, "getall"):
            return bool(tags.getall("APIC"))
        return bool(tags.get("covr") or tags.get("metadata_block_picture")
                    or getattr(audio, "pictures", None))
    except Exception:
        return False


def player_shows_cover(w: Work, covers: Central, ignore_embedded: bool = False) -> Optional[str]:
    """Why the player already has a cover for 'w', or None. The same steps
    the player takes (the Artwork guide). With 'ignore_embedded', a cover
    inside the book or its files does not count: one in Artwork/Covers
    comes before it on the player."""
    if covers.has(work_key(w.author, w.title), name_key(w.title)):
        return "Artwork/Covers has it"
    first = w.files[0]
    if ignore_embedded:
        pass
    elif w.kind == EBOOK:
        if w.has_cover:
            return "the ebook has a cover inside it"
    elif any(has_embedded_art(p) for p in w.files[:3]):
        return "its files carry a cover"
    for ext in IMAGE_EXTS:
        if first.with_suffix(ext).is_file():
            return f"an image named after it: {first.with_suffix(ext).name}"
    if w.kind != EBOOK:
        folder = first.parent
        alone = all(p.parent == folder for p in w.files) and \
            sum(1 for p in folder.iterdir() if p.suffix.casefold() in AUDIO_EXTS) == len(w.files)
        if alone and not same_person(folder.name, w.author) and folder_image(folder):
            return f"its folder has {folder_image(folder).name}"
    return None


def player_shows_person(author: str, works: list[Work], people: Central) -> bool:
    if people.has(*name_keys(author)):
        return True
    for w in works:
        for folder in list(w.files[0].parents)[:4]:
            if same_person(folder.name, author) and folder_image(folder):
                return True
    return False

# ---------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------
def plain_book_title(title: str) -> str:
    """'Guards! Guards! (Discworld #8) [Unabridged]' -> 'Guards! Guards!'"""
    t = re.sub(r"\([^)]*\)|\[[^]]*\]", " ", title or "")
    t = re.sub(r"\b(unabridged|abridged|a novel)\b", " ", t, flags=re.IGNORECASE)
    return " ".join(t.split()).strip(" -:") or (title or "")


def book_match(w: Work, r_title: str, r_author: str) -> float:
    """0.6 the title, its subtitle optional, and 0.4 the author."""
    ours, theirs = plain_book_title(w.title), plain_book_title(r_title)
    t = max(similarity(ours, theirs), similarity(ours.split(":")[0], theirs.split(":")[0]))
    if not w.author:
        return t
    a = 1.0 if any(same_person(w.author, x) for x in re.split(r"\s*(?:&|,| and )\s*", r_author or "") if x) \
        else similarity(w.author, r_author)
    return 0.6 * t + 0.4 * a


def bigger_apple_art(url: str) -> str:
    return re.sub(r"/\d+x\d+bb\.(jpg|png)$", r"/1200x1200bb.\1", url or "")


class Sources:
    def __init__(self, args: argparse.Namespace, session: requests.Session, limiter: RateLimiter, make_candidate: Any) -> None:
        self.args, self.s, self.lim, self.cand = args, session, limiter, make_candidate
        self.author_keys: dict[str, str] = {}     # name_key(author) -> OL author key

    def _json(self, service: str, url: str, **kw: Any) -> Any:
        self.lim.wait(service)
        r = http_get(self.s, url, headers={"User-Agent": self.args.user_agent}, **kw)
        if r.status_code == 404:
            return None
        r.raise_for_status()
        return r.json()

    def audnexus(self, w: Work) -> list[Any]:
        if not w.asin or self.args.no_audnexus:
            return []
        for region in ("us", "uk", "ca", "au", "de", "fr"):
            j = self._json("audnexus", f"{AUDNEXUS}/{w.asin}", params={"region": region})
            if j and j.get("image"):
                authors = ", ".join(a.get("name", "") for a in j.get("authors") or [])
                m = book_match(w, j.get("title") or "", authors)
                return [self.cand("audnexus", authors, j.get("title") or "", j["image"], m)]
        return []

    def apple(self, w: Work) -> list[Any]:
        if self.args.no_apple:
            return []
        if w.kind == PODCAST:
            j = self._json("apple", APPLE_SEARCH, params={"term": w.title, "media": "podcast", "entity": "podcast", "limit": 10})
        else:
            j = self._json("apple", APPLE_SEARCH, params={"term": f"{plain_book_title(w.title)} {w.author}".strip(),
                                                         "media": "audiobook", "entity": "audiobook", "limit": 10})
        out = []
        for r in (j or {}).get("results", []):
            title, artist = r.get("collectionName") or "", r.get("artistName") or ""
            if w.kind == PODCAST:
                # Shows' names are often generic, so the host counts too
                m = similarity(w.title, title)
                if w.author:
                    m = 0.6 * m + 0.4 * (1.0 if same_person(w.author, artist) else similarity(w.author, artist))
            else:
                m = book_match(w, title, artist)
            url = r.get("artworkUrl600") or bigger_apple_art(r.get("artworkUrl100") or "")
            if url:
                c = self.cand("apple", artist, title, bigger_apple_art(url), m)
                c.feed = r.get("feedUrl") or ""
                out.append(c)
        out.sort(key=lambda c: c.match, reverse=True)
        return out[:2]

    def podcast_feed(self, apple_hits: list[Any]) -> list[Any]:
        """The show's own art from its RSS feed: usually 1400 to 3000 px,
        larger than Apple's copy."""
        out = []
        for c in apple_hits:
            if not getattr(c, "feed", "") or c.match < self.args.auto_accept:
                continue
            try:
                self.lim.wait("feed")
                with self.s.get(c.feed, stream=True, timeout=30, headers={"User-Agent": self.args.user_agent}) as r:
                    head = r.raw.read(400_000, decode_content=True).decode("utf-8", "replace")
                m = re.search(r'<itunes:image[^>]*href=["\']([^"\']+)', head)
                if m:
                    out.append(self.cand("feed", c.artist, c.album, m.group(1).replace("&amp;", "&"), c.match))
            except requests.RequestException as exc:
                print(f"  candidate: feed error: {exc}")
            break
        return out

    def open_library(self, w: Work) -> list[Any]:
        """The work found by ISBN, and by title and author too: the ISBN's
        own edition may have only a small scan where another has a good
        one. Covers are fetched by cover id, which Open Library does not
        rate-limit; by ISBN, which it does, only when nothing else has one."""
        searches = []
        if w.isbn:
            searches.append({"isbn": w.isbn})
        searches.append({"title": plain_book_title(w.title), **({"author": w.author} if w.author else {})})
        out, seen = [], set()
        for query in searches:
            params = {"fields": "key,title,author_name,author_key,cover_i", "limit": 5, **query}
            j = self._json("openlibrary", OL_SEARCH, params=params) or {}
            for d in j.get("docs", []):
                authors = ", ".join(d.get("author_name") or [])
                m = 1.0 if "isbn" in query else book_match(w, d.get("title") or "", authors)
                for name, key in zip(d.get("author_name") or [], d.get("author_key") or []):
                    self.author_keys.setdefault(name_key(name), key)
                if d.get("cover_i") and d["cover_i"] not in seen:
                    seen.add(d["cover_i"])
                    url = f"{OL_COVERS}/b/id/{d['cover_i']}-L.jpg?default=false"
                    out.append(self.cand("openlibrary", authors, d.get("title") or "", url, m))
        if not out and w.isbn:
            url = f"{OL_COVERS}/b/isbn/{w.isbn}-L.jpg?default=false"
            out.append(self.cand("openlibrary", w.author, w.title, url, 1.0))
        out.sort(key=lambda c: c.match, reverse=True)
        return out[:3]

    def google_books(self, w: Work) -> list[Any]:
        key = self.args.google_books_key
        if not key:
            return []
        q = f"isbn:{w.isbn}" if w.isbn else f'intitle:"{plain_book_title(w.title)}"' + (f' inauthor:"{w.author}"' if w.author else "")
        j = self._json("google", GOOGLE_BOOKS, params={"q": q, "maxResults": 3, "key": key}) or {}
        for item in j.get("items", []):
            info = item.get("volumeInfo") or {}
            m = 1.0 if w.isbn else book_match(w, info.get("title") or "", ", ".join(info.get("authors") or []))
            # The search's thumbnails are tiny; the volume itself names larger ones
            vol = self._json("google", f"{GOOGLE_BOOKS}/{item['id']}", params={"key": key}) or {}
            links = (vol.get("volumeInfo") or {}).get("imageLinks") or {}
            url = next((links[k] for k in ("extraLarge", "large", "medium") if links.get(k)), "")
            if url:
                return [self.cand("google books", ", ".join(info.get("authors") or []), info.get("title") or "",
                                  url.replace("http://", "https://"), m)]
        return []

    def author_photo(self, author: str) -> list[Any]:
        key = self.author_keys.get(name_key(author))
        if not key:
            j = self._json("openlibrary", "https://openlibrary.org/search/authors.json", params={"q": author, "limit": 5}) or {}
            for d in j.get("docs", []):
                if same_person(d.get("name") or "", author):
                    key = d.get("key")
                    break
        if not key:
            return []
        return [self.cand("openlibrary", author, "", f"{OL_COVERS}/a/olid/{key}-L.jpg?default=false", 1.0)]

# ---------------------------------------------------------------------
# The run
# ---------------------------------------------------------------------
def artwork_root(root: Path, args: argparse.Namespace) -> Optional[Path]:
    if args.artwork_root:
        return Path(args.artwork_root).expanduser().resolve()
    player = player_root_of(root)
    return player / "Artwork" if player else None


def run_books(root: Path, args: argparse.Namespace, session: requests.Session, limiter: RateLimiter, hooks: Any) -> int:
    """'hooks' is art_fetch itself, for its candidate type, downloading,
    scoring and JPEG writing."""
    art = artwork_root(root, args)
    if art is None:
        print("ERROR: covers for audiobooks, podcasts and ebooks go in the player's Artwork folder: "
              "point it at a mounted iPod, or give --artwork-root", flush=True)
        return 2
    covers, people = Central(art / "Covers", people=False), Central(art / "People", people=True)
    manifest: Manifest = args.manifest
    cache_path = data_dir(root) / CACHE_NAME
    cache: dict[str, Any] = load_json(cache_path, {}).get("works", {})

    def cand(source: str, artist: str, title: str, url: str, match: float) -> Any:
        return hooks.ImageCandidate(source, artist, title, image_url=url, match=match)
    src = Sources(args, session, limiter, cand)

    # Books and shows are found anywhere on the player, not only under root
    scan = player_root_of(root) or root
    kinds: set[str] = args.spoken_kinds
    print("\n=== " + ", ".join(k.capitalize() + "s" for k in (AUDIOBOOK, PODCAST, EBOOK) if k in kinds) + " ===")
    print(f"Artwork folder : {art}")
    print(f"Looking in     : {scan}")
    works = audio_works(scan) if kinds & {AUDIOBOOK, PODCAST} else []
    works = [w for w in works if w.kind in kinds] + (ebook_works(scan) if EBOOK in kinds else [])
    print("Found          : " + ", ".join(f"{sum(w.kind == k for w in works)} {k}(s)"
                                          for k in (AUDIOBOOK, PODCAST, EBOOK) if k in kinds))
    counts: dict[str, int] = {}

    def count(what: str) -> None:
        counts[what] = counts.get(what, 0) + 1

    def choose(candidates: list[Any]) -> Optional[tuple[Any, Any]]:
        best = None
        for c in candidates:
            try:
                img = hooks.image_from_candidate(c, args, session, limiter)
            except Exception as exc:
                print(f"  {c.source} failed/rejected: {exc}")
                continue
            score, reason = hooks.score_downloaded_candidate(img, c, args)
            print(f"    {c.source}: {reason}")
            if best is None or score > best[0]:
                best = (score, c, img)
        return (best[1], best[2]) if best else None

    try:
        for w in sorted(works, key=lambda w: (w.kind, w.label.casefold())):
            print(f"\n[{w.kind}: {w.label}]")
            name = safe_filename(w.title if w.kind == PODCAST else w.label) + ".jpg"
            target = art / "Covers" / name
            why = player_shows_cover(w, covers, args.ignore_embedded)
            if why and not manifest.owns(target):
                print(f"  skip: the player has a cover ({why})"); count("had a cover")
            elif target.exists() and not manifest.may_write(target):
                print(f"  skip: your artwork: {name}"); count("had a cover")
            elif target.exists() and not args.overwrite:
                print(f"  skip: already fetched: {name}"); count("had a cover")
            else:
                key = f"{w.kind}|{work_key(w.author, w.title)}"
                entry = cache.get(key, {})
                if entry.get("status") == "not_found" and not args.retry_not_found and not args.ignore_cache:
                    print("  cache: not found before"); count("not found")
                elif args.dry_run:
                    ids = ", ".join(x for x in (w.isbn and f"ISBN {w.isbn}", w.asin and f"ASIN {w.asin}") if x)
                    print(f"  dry-run: would look for a cover{' (' + ids + ')' if ids else ''} -> {target.relative_to(art.parent)}")
                    count("would look up")
                else:
                    pool = []
                    calls = ([("apple", lambda: src.apple(w))] if w.kind == PODCAST else
                             [("audnexus", lambda: src.audnexus(w)), ("apple", lambda: src.apple(w) if w.kind == AUDIOBOOK else []),
                              ("open library", lambda: src.open_library(w)), ("google books", lambda: src.google_books(w))])
                    for label, fn in calls:
                        try:
                            hits = fn()
                            if label == "apple" and w.kind == PODCAST:
                                hits = hits + src.podcast_feed(hits)
                        except Exception as exc:
                            print(f"  candidate: {label} error: {exc}"); continue
                        for c in hits:
                            ok = c.match >= args.auto_accept
                            print(f"  candidate: {c.source} -> {c.artist} / {c.album} (match {c.match:.0%})"
                                  + ("" if ok else ": too far off, not used"))
                            if ok:
                                pool.append(c)
                    picked = choose(pool)
                    if picked:
                        c, img = picked
                        try:
                            manifest.write(target, hooks.jpeg_bytes(img, args.max_size, square=False), "art_fetch")
                        except OSError as exc:
                            print(f"  could not write {name}: {exc}"); count("not found"); continue
                        covers.add(target.stem)
                        cache[key] = {"status": "found", "source": c.source, "image_url": c.image_url}
                        print(f"  saved: {target.relative_to(art.parent)} ({c.source})"); count("covers saved")
                    else:
                        cache[key] = {"status": "not_found"}
                        print("  artwork: no cover found"); count("not found")

        if not args.no_authors:
            authors: dict[str, list[Work]] = {}
            for w in works:
                if w.kind != PODCAST and w.author:
                    authors.setdefault(name_key(w.author), []).append(w)
            for group in authors.values():
                author = group[0].author
                target = art / "People" / (safe_filename(author) + ".jpg")
                if not manifest.owns(target) and player_shows_person(author, group, people):
                    count("authors pictured"); continue
                if target.exists() and not (args.overwrite and manifest.owns(target)):
                    count("authors pictured"); continue
                key = f"author|{name_key(author)}"
                if cache.get(key, {}).get("status") == "not_found" and not args.retry_not_found and not args.ignore_cache:
                    continue
                print(f"\n[author: {author}]")
                if args.dry_run:
                    print(f"  dry-run: would look for a picture -> {target.relative_to(art.parent)}"); continue
                try:
                    picked = choose(src.author_photo(author))
                except requests.RequestException as exc:
                    print(f"  error: {exc}"); continue
                if picked:
                    try:
                        manifest.write(target, hooks.jpeg_bytes(picked[1], args.max_size, square=False), "art_fetch")
                    except OSError as exc:
                        print(f"  could not write {target.name}: {exc}"); continue
                    people.add(author)
                    cache[key] = {"status": "found"}
                    print(f"  saved: {target.relative_to(art.parent)}"); count("author pictures saved")
                else:
                    cache[key] = {"status": "not_found"}
                    print("  no picture found")
    finally:
        if not args.dry_run:
            save_json(cache_path, {"version": 1, "works": cache})

    print("\nBooks summary\n-------------")
    for what in ("covers saved", "would look up", "had a cover", "not found", "author pictures saved", "authors pictured"):
        if counts.get(what):
            print(f"  {what:<22}: {counts[what]}")
    return 0
