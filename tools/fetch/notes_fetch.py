#!/usr/bin/env python3
"""
notes_fetch: liner notes and biographies for a PodBox library, as the
player's wiki pages in /Notes, from MusicBrainz's credits and Wikipedia's
prose.

An album gets /Notes/Albums/<Artist> - <Album>.md: its article's lead and
one background section, and its personnel by role. An artist, and at
--depth 1 the key people its albums credit, gets /Notes/People/<Name>.md:
a facts line, the article's lead, a band's members. The player adds the
track lists and album lists itself. Every page ends with where its text
came from, as Wikipedia's licence asks. A page the owner wrote or edited
is never touched.

Parts, in order:
  - roles: which credits count
  - fetching, cached
  - the library
  - MusicBrainz: the album, its credits, people
  - Wikidata and Wikipedia: the article, its prose
  - pages: templates and writing
  - the run
"""
from __future__ import annotations

import argparse
import gzip
import hashlib
import html
import json
import re
import sys
import time
from dataclasses import dataclass, field
from html.parser import HTMLParser
from pathlib import Path
from typing import Any, Optional
from urllib.parse import quote, unquote

import requests

from fetchlib import (AUDIO_EXTS, MUSIC, Manifest, RateLimiter, config_path,
                      contact_from, data_dir, http_get, load_config, name_key,
                      player_root_of, read_tags, safe_filename, same_person,
                      similarity, user_agent, walk_files)

TOOL = "notes_fetch"
VERSION = "1.0"
MB = "https://musicbrainz.org/ws/2"
WIKIDATA = "https://www.wikidata.org/w/api.php"
CACHE_DAYS = 180
NOT_FOUND_DAYS = 30
ALBUM_BUDGET = 8 * 1024         # bytes of prose on an album page
PERSON_BUDGET = 5 * 1024        # and on a person's
PAGE_MAX = 30 * 1024            # a whole page; the player reads 32 KB of one

# ---------------------------------------------------------------------
# Roles
# ---------------------------------------------------------------------
# MusicBrainz relationship types, by tier. Tier 1 people get pages of their
# own at --depth 1; tier 2 are listed, and get a page only when credited on
# MIN_ALBUMS_FOR_TIER2 of the library's albums; anything not here (design,
# photography, A&R, liner notes, legal, "misc") is left out.
ROLES = {
    "producer": (1, "Producer"),
    "composer": (1, "Composer"), "lyricist": (1, "Lyricist"),
    "writer": (1, "Songwriter"), "librettist": (1, "Librettist"),
    "conductor": (1, "Conductor"), "performing orchestra": (1, "Orchestra"),
    "featured": (1, "Featuring"),
    "mix": (2, "Mixing"), "mastering": (2, "Mastering"),
    "recording": (2, "Recording"), "engineer": (2, "Engineer"),
    "sound": (2, "Engineer"), "audio": (2, "Engineer"), "balance": (2, "Engineer"),
    "arranger": (2, "Arranger"), "orchestrator": (2, "Orchestrator"),
    "instrument arranger": (2, "Arranger"), "vocal arranger": (2, "Vocal arranger"),
    "instrument": (2, None), "vocal": (2, None), "performer": (2, "Performer"),
    "chorus master": (2, "Chorus master"),
}
# A credit with one of these attributes is an assistant's, and left out
SKIP_ATTRIBUTES = {"assistant", "associate", "executive"}
# Personnel is listed in this order of groups
GROUPS = (("Producers", {"Producer"}),
          ("Songwriters", {"Composer", "Lyricist", "Songwriter", "Librettist"}),
          ("Musicians", None),
          ("Engineers", {"Mixing", "Mastering", "Recording", "Engineer"}))
MIN_ALBUMS_FOR_TIER2 = 3


@dataclass
class Credit:
    mbid: str
    name: str
    role: str
    tier: int
    tracks: set[int] = field(default_factory=set)   # empty: the whole album


def role_of(rel: dict[str, Any]) -> Optional[tuple[int, str]]:
    kind = rel.get("type") or ""
    if kind not in ROLES:
        return None
    attrs = [a.casefold() for a in rel.get("attributes") or []]
    if any(a in SKIP_ATTRIBUTES for a in attrs):
        return None
    tier, label = ROLES[kind]
    if label is None:                    # an instrument or voice: its name
        named = [re.sub(r"\s*\([^)]*\)", "", a) for a in rel.get("attributes") or []
                 if a.casefold() not in ("guest", "additional", "solo")]
        label = ", ".join(n for n in named if n) or ("vocals" if kind == "vocal" else "instruments")
    return tier, label[0].upper() + label[1:]

# ---------------------------------------------------------------------
# Fetching, cached
# ---------------------------------------------------------------------
class Fetcher:
    """GETs, kept on disk for CACHE_DAYS (a miss for NOT_FOUND_DAYS), so a
    second run, or one with new templates, works almost offline."""

    def __init__(self, cache_dir: Path, agent: str, limiter: RateLimiter) -> None:
        self.dir = cache_dir
        self.dir.mkdir(parents=True, exist_ok=True)
        self.s = requests.Session()
        self.s.headers["User-Agent"] = agent
        self.lim = limiter
        self.requests = 0

    def get(self, service: str, url: str, params: Optional[dict[str, Any]] = None, as_json: bool = True) -> Any:
        key = hashlib.sha1((url + "?" + json.dumps(params or {}, sort_keys=True)).encode()).hexdigest()
        # Compressed, as a whole article's HTML runs to a megabyte; a plain
        # .json is read too
        path = self.dir / f"{key}.json.gz"
        for p, opener in ((path, gzip.open), (self.dir / f"{key}.json", open)):
            if not p.exists():
                continue
            try:
                with opener(p, "rt", encoding="utf-8") as fh:
                    entry = json.load(fh)
                age = time.time() - entry["time"]
                if age < (NOT_FOUND_DAYS if self.empty(entry["body"]) else CACHE_DAYS) * 86400:
                    return entry["body"]
            except (OSError, EOFError, ValueError, KeyError):
                pass
            break
        headers = {"Accept": "application/json"} if as_json else {}
        for attempt in range(8):
            self.lim.wait(service)
            self.requests += 1
            r = http_get(self.s, url, params=params, headers=headers, retries=0)
            if r.status_code not in (429, 503):
                break
            self.slow_down(service, r, attempt)
        if r.status_code == 404:
            body = None
        else:
            r.raise_for_status()
            body = r.json() if as_json else r.text
        with gzip.open(path, "wt", encoding="utf-8") as fh:
            json.dump({"time": time.time(), "body": body}, fh)
        return body

    @staticmethod
    def empty(body: Any) -> bool:
        """A miss, asked about again sooner: a 404, or a search that found
        nothing."""
        return body is None or (isinstance(body, dict) and body.get("count") == 0)

    def slow_down(self, service: str, r: requests.Response, attempt: int) -> None:
        """A refusal for going too fast: wait as long as the server asks, and
        go slower for the rest of the run. MusicBrainz allows one request a
        second from an address, so another program, or another computer on
        the same network, using it at the same time is enough to set it off."""
        try:
            wait = max(1.0, float(r.headers.get("Retry-After") or 0))
        except ValueError:
            wait = 2.0
        wait = min(60.0, wait * (attempt + 1))
        old = self.lim.delays.get(service, 1.0)
        new = min(4.0, old + 0.5)
        self.lim.delays[service] = new
        if new > old and attempt == 0:
            who = "MusicBrainz" if service == "musicbrainz" else "Wikipedia"
            print(f"\n  ({who} asked for fewer requests; slowing to one every {new:.1f} s)\n  ", end="", flush=True)
        time.sleep(wait)

    def mb(self, path: str, **params: Any) -> Any:
        return self.get("musicbrainz", f"{MB}/{path}", {"fmt": "json", **params})

    def wikidata(self, ids: list[str]) -> dict[str, Any]:
        out: dict[str, Any] = {}
        for i in range(0, len(ids), 50):
            j = self.get("wikimedia", WIKIDATA, {"action": "wbgetentities", "ids": "|".join(ids[i:i + 50]),
                                                 "props": "labels|claims|sitelinks", "languages": "en",
                                                 "format": "json"}) or {}
            out.update(j.get("entities") or {})
        return out

# ---------------------------------------------------------------------
# The library
# ---------------------------------------------------------------------
@dataclass
class Album:
    artist: str
    title: str
    folder: Path
    tracks: int = 0
    year: str = ""


def library_albums(root: Path) -> list[Album]:
    albums: dict[tuple[str, str], Album] = {}
    for path in walk_files(root, AUDIO_EXTS):
        t = read_tags(path)
        if t.error or t.kind != MUSIC or not t.album:
            continue
        artist = t.album_artist or t.artist
        a = albums.setdefault((name_key(artist), name_key(t.album)), Album(artist, t.album, path.parent))
        a.tracks += 1
    return list(albums.values())

# ---------------------------------------------------------------------
# MusicBrainz
# ---------------------------------------------------------------------
def plain_title(title: str) -> str:
    t = re.sub(r"\([^)]*\)|\[[^]]*\]", " ", title or "")
    t = re.sub(r"\s[-–]\s*(ep|single|deluxe.*|remaster.*|expanded.*)\s*$", " ", t, flags=re.IGNORECASE)
    return " ".join(t.split()) or title


def credit_name(credits: list[dict[str, Any]]) -> str:
    return "".join((c.get("name") or "") + (c.get("joinphrase") or "") for c in credits or [])


def album_match(a: Album, artist: str, title: str) -> float:
    return 0.6 * similarity(plain_title(a.title), plain_title(title)) + 0.4 * similarity(a.artist, artist)


@dataclass
class AlbumInfo:
    rg: str = ""
    wikidata: str = ""
    artists: list[tuple[str, str]] = field(default_factory=list)     # (mbid, name)
    credits: list[Credit] = field(default_factory=list)
    year: str = ""
    tracks: list["Track"] = field(default_factory=list)


@dataclass
class Track:
    number: int                 # counted across the discs, as Credit.tracks is
    disc: int
    position: int               # on its disc
    title: str
    works: list[str]            # the MusicBrainz works it records


def release_tracks(rel: dict[str, Any]) -> list[Track]:
    out, number = [], 0
    for disc, medium in enumerate(rel.get("media") or [], 1):
        for pos, t in enumerate(medium.get("tracks") or [], 1):
            number += 1
            rec = t.get("recording") or {}
            works = [(r.get("work") or {}).get("id") for r in rec.get("relations") or []
                     if r.get("target-type") == "work" and (r.get("work") or {}).get("id")]
            out.append(Track(number, disc, pos, t.get("title") or rec.get("title") or "", works))
    return out


def find_album(f: Fetcher, a: Album, threshold: float) -> Optional[AlbumInfo]:
    def phrase(s: str) -> str:
        return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'
    q = f"releasegroup:{phrase(plain_title(a.title))} AND artist:{phrase(a.artist)}"
    groups = (f.mb("release-group", query=q, limit=10) or {}).get("release-groups") or []
    scored = [(album_match(a, credit_name(g.get("artist-credit")), g.get("title") or ""), g) for g in groups]
    if not scored:
        return None
    score, g = max(scored, key=lambda x: x[0])
    if score < threshold:
        print(f"nearest is '{credit_name(g.get('artist-credit'))} - {g.get('title')}' ({score:.0%}), too far off; ", end="")
        return None
    info = AlbumInfo(rg=g["id"], year=(g.get("first-release-date") or "")[:4])
    info.artists = [(c["artist"]["id"], c["artist"]["name"]) for c in g.get("artist-credit") or [] if c.get("artist")]
    rg = f.mb(f"release-group/{g['id']}", inc="url-rels") or {}
    info.wikidata = wikidata_id(rg.get("relations") or [])

    # The release of the group whose track count is the library's, else
    # the earliest official one
    releases = (f.mb("release", **{"release-group": g["id"], "inc": "media", "limit": 100}) or {}).get("releases") or []
    if not releases:
        return info
    def rank(r: dict[str, Any]) -> tuple[int, int, str]:
        count = sum(m.get("track-count") or 0 for m in r.get("media") or [])
        return (count != a.tracks, (r.get("status") or "") != "Official", r.get("date") or "9999")
    rel = min(releases, key=rank)
    full = f.mb(f"release/{rel['id']}", inc="recordings+artist-credits+artist-rels+recording-level-rels"
                                          "+work-rels+work-level-rels") or {}
    info.credits = release_credits(full)
    info.tracks = release_tracks(full)
    return info


def wikidata_id(relations: list[dict[str, Any]]) -> str:
    for r in relations:
        url = ((r.get("url") or {}).get("resource")) or ""
        m = re.search(r"wikidata\.org/wiki/(Q\d+)", url)
        if m:
            return m.group(1)
    return ""


def release_credits(rel: dict[str, Any]) -> list[Credit]:
    """The release's credits by person and role, each with the tracks it is
    for (none: all of them). Songwriters come from the recordings' works."""
    found: dict[tuple[str, str], Credit] = {}
    album_artists = {c["artist"]["id"] for c in rel.get("artist-credit") or [] if c.get("artist")}

    def add(r: dict[str, Any], track: Optional[int]) -> None:
        artist = r.get("artist") or {}
        role = role_of(r)
        if not role or not artist.get("id"):
            return
        c = found.setdefault((artist["id"], role[1]), Credit(artist["id"], artist.get("name") or "", role[1], role[0]))
        if track is None:
            c.tracks = set()
            c.tracks.add(0)                 # 0 marks a whole-album credit
        elif 0 not in c.tracks:
            c.tracks.add(track)

    for r in rel.get("relations") or []:
        add(r, None)
    number = 0
    for medium in rel.get("media") or []:
        for t in medium.get("tracks") or []:
            number += 1
            rec = t.get("recording") or {}
            for r in rec.get("relations") or []:
                if r.get("target-type") == "work":
                    for wr in (r.get("work") or {}).get("relations") or []:
                        add(wr, number)
                else:
                    add(r, number)
            for ac in rec.get("artist-credit") or []:
                a = ac.get("artist") or {}
                if a.get("id") and a["id"] not in album_artists:
                    add({"type": "featured", "artist": a}, number)
    total = number
    out = []
    for c in found.values():
        if 0 in c.tracks or len(c.tracks) >= total:
            c.tracks = set()
        out.append(c)
    return out


def artist_details(f: Fetcher, mbid: str) -> dict[str, Any]:
    return f.mb(f"artist/{mbid}", inc="url-rels+artist-rels") or {}


def band_members(details: dict[str, Any]) -> list[str]:
    members = []
    for r in details.get("relations") or []:
        if r.get("type") == "member of band" and r.get("direction") == "backward" and r.get("artist"):
            name = r["artist"].get("name") or ""
            if name and name not in members:
                members.append(name)
    return members

# ---------------------------------------------------------------------
# Wikidata and Wikipedia
# ---------------------------------------------------------------------
def claim_ids(entity: dict[str, Any], prop: str) -> list[str]:
    out = []
    for c in (entity.get("claims") or {}).get(prop) or []:
        v = ((c.get("mainsnak") or {}).get("datavalue") or {}).get("value")
        if isinstance(v, dict) and v.get("id"):
            out.append(v["id"])
    return out


def claim_year(entity: dict[str, Any], prop: str) -> str:
    for c in (entity.get("claims") or {}).get(prop) or []:
        v = ((c.get("mainsnak") or {}).get("datavalue") or {}).get("value")
        if isinstance(v, dict) and v.get("time"):
            m = re.match(r"[+-]?0*(\d+)-", v["time"])
            if m:
                return m.group(1)
    return ""


def label(entity: dict[str, Any]) -> str:
    return ((entity.get("labels") or {}).get("en") or {}).get("value") or ""


def sitelink(entity: dict[str, Any], langs: list[str]) -> tuple[str, str]:
    for lang in langs:
        s = (entity.get("sitelinks") or {}).get(f"{lang}wiki")
        if s and s.get("title"):
            return lang, s["title"]
    return "", ""


def wiki_lookup_album(f: Fetcher, a: Album, langs: list[str]) -> str:
    """A Wikidata id for an album MusicBrainz did not link, by its likely
    article titles; accepted only when its performer is the library's artist."""
    lang = langs[0]
    titles = [f"{a.title} ({a.artist} album)", f"{a.title} (album)", a.title]
    j = f.get("wikimedia", f"https://{lang}.wikipedia.org/w/api.php",
              {"action": "query", "titles": "|".join(titles), "redirects": 1, "prop": "pageprops",
               "ppprop": "wikibase_item|disambiguation", "format": "json"}) or {}
    pages = ((j.get("query") or {}).get("pages") or {}).values()
    by_title = {p.get("title"): p for p in pages if "missing" not in p}
    redirects = {r["from"]: r["to"] for r in (j.get("query") or {}).get("redirects") or []}
    for t in titles:
        p = by_title.get(redirects.get(t, t))
        props = (p or {}).get("pageprops") or {}
        if not p or "disambiguation" in props or not props.get("wikibase_item"):
            continue
        qid = props["wikibase_item"]
        ent = f.wikidata([qid]).get(qid) or {}
        performers = f.wikidata(claim_ids(ent, "P175")) if claim_ids(ent, "P175") else {}
        if any(similarity(label(e), a.artist) >= 0.85 for e in performers.values()):
            return qid
    return ""


def wiki_lookup_artist(f: Fetcher, name: str, langs: list[str]) -> str:
    lang = langs[0]
    titles = [name, f"{name} (band)", f"{name} (musician)", f"{name} (singer)", f"{name} (rapper)"]
    j = f.get("wikimedia", f"https://{lang}.wikipedia.org/w/api.php",
              {"action": "query", "titles": "|".join(titles), "redirects": 1, "prop": "pageprops",
               "ppprop": "wikibase_item|disambiguation", "format": "json"}) or {}
    for p in ((j.get("query") or {}).get("pages") or {}).values():
        props = p.get("pageprops") or {}
        if "missing" in p or "disambiguation" in props or not props.get("wikibase_item"):
            continue
        ent = f.wikidata([props["wikibase_item"]]).get(props["wikibase_item"]) or {}
        # A person or a group of musicians, not a film or a place of that name
        is_artist = claim_ids(ent, "P106") or claim_ids(ent, "P527") or claim_ids(ent, "P1303") \
            or "Q5" in claim_ids(ent, "P31")
        if is_artist and same_person(label(ent) or p.get("title", ""), name):
            return props["wikibase_item"]
    return ""


class Prose(HTMLParser):
    """Parsoid HTML to blocks of ("h2"|"h3"|"p"|"li", text, links), with
    references, infoboxes, tables, figures, navboxes and hatnotes left out.
    A link is (start, end, target title) within the block's text."""

    SKIP_TAGS = {"table", "figure", "style", "script", "sup", "math", "link", "meta"}
    VOID_TAGS = {"br", "img", "link", "meta", "wbr", "hr", "input", "source"}
    SKIP_CLASSES = ("infobox", "navbox", "hatnote", "reflist", "references", "mw-ref", "thumb",
                    "metadata", "sistersitebox", "mw-empty-elt", "noprint", "gallery", "toc")

    def __init__(self) -> None:
        super().__init__(convert_charrefs=True)
        self.blocks: list[tuple[str, str, list[tuple[int, int, str]]]] = []
        self.skip = 0
        self.stack: list[tuple[str, bool]] = []
        self.kind: Optional[str] = None
        self.segs: list[tuple[str, str]] = []      # (text, link target or "")
        self.target = ""

    def handle_starttag(self, tag: str, attrs: list[tuple[str, Optional[str]]]) -> None:
        if tag in self.VOID_TAGS:
            return
        a = dict(attrs)
        cls = a.get("class") or ""
        skipping = tag in self.SKIP_TAGS or any(c in cls for c in self.SKIP_CLASSES)
        self.stack.append((tag, skipping))
        if skipping:
            self.skip += 1
            return
        if self.skip:
            return
        if tag in ("p", "h2", "h3", "li") and self.kind is None:
            self.kind, self.segs = tag, []
        elif tag == "a" and self.kind and "mw:WikiLink" in (a.get("rel") or ""):
            self.target = unquote((a.get("href") or "").removeprefix("./")).replace("_", " ").split("#")[0]

    def handle_endtag(self, tag: str) -> None:
        if tag in self.VOID_TAGS:
            return
        while self.stack:
            t, skipping = self.stack.pop()
            if skipping:
                self.skip -= 1
            if t == tag:
                break
        if self.skip:
            return
        if tag == "a":
            self.target = ""
        elif tag == self.kind:
            self.blocks.append((tag, *self._flatten()))
            if not self.blocks[-1][1]:
                self.blocks.pop()
            self.kind = None

    def handle_data(self, data: str) -> None:
        if not self.skip and self.kind:
            self.segs.append((data, self.target))

    def _flatten(self) -> tuple[str, list[tuple[int, int, str]]]:
        """The block's text with its whitespace collapsed, and where its
        links fall in it."""
        text, links = "", []
        for data, target in self.segs:
            data = re.sub(r"\s+", " ", data)
            if text.endswith(" ") or not text:
                data = data.lstrip(" ")
            if not data:
                continue
            if target:
                start = len(text) + (len(data) - len(data.lstrip(" ")))
                end = len(text) + len(data.rstrip(" "))
                if links and links[-1][2] == target and links[-1][1] >= start - 1:
                    links[-1] = (links[-1][0], end, target)
                elif end > start:
                    links.append((start, end, target))
            text += data
        stripped = text.rstrip()
        return stripped, [(s, min(e, len(stripped)), t) for s, e, t in links if s < len(stripped)]


def article_blocks(f: Fetcher, lang: str, title: str) -> list[tuple[str, str, list[tuple[int, int, str]]]]:
    body = f.get("wikimedia", f"https://{lang}.wikipedia.org/w/rest.php/v1/page/{quote(title.replace(' ', '_'), safe='')}/html",
                 as_json=False)
    if not body:
        return []
    p = Prose()
    p.feed(body)
    return p.blocks


def lead_and_section(blocks: list[tuple[str, str, list[Any]]], wanted: tuple[str, ...]) -> tuple[list[Any], str, list[Any]]:
    """The paragraphs before the first heading, and the first of 'wanted'
    sections (matched on how its heading starts) with its heading."""
    lead, section, heading = [], [], ""
    i = 0
    while i < len(blocks) and blocks[i][0] not in ("h2", "h3"):
        if blocks[i][0] == "p":
            lead.append(blocks[i])
        i += 1
    for name in wanted:
        for j in range(i, len(blocks)):
            if blocks[j][0] == "h2" and blocks[j][1].casefold().startswith(name.casefold()):
                heading = blocks[j][1]
                for k in range(j + 1, len(blocks)):
                    if blocks[k][0] == "h2":
                        break
                    if blocks[k][0] in ("p", "li"):
                        section.append(blocks[k])
                return lead, heading, section
    return lead, heading, section

# ---------------------------------------------------------------------
# Pages
# ---------------------------------------------------------------------
ALBUM_TEMPLATE = """<<lead>>

<<section:Background|Recording|Production>>

<<personnel>>

<<tracks_page>>

<<source>>
"""
PERSON_TEMPLATE = """<<facts>>

<<lead>>

<<members>>

<<source>>
"""
# The album's own page lists this one in its menu: front matter naming the
# album makes it one of the album's pages
TRACKS_TEMPLATE = """---
album: <<album>>
---
<<track_sections>>
"""
TEMPLATES = (("album", "Album.md", ALBUM_TEMPLATE), ("person", "Person.md", PERSON_TEMPLATE),
             ("tracks", "Tracks.md", TRACKS_TEMPLATE))
TRACK_BUDGET = 1200             # bytes of a song's article in its section


def track_sections(f: Fetcher, info: AlbumInfo, linker: "Linker", langs: list[str]) -> str:
    """A section a track that has something to say: the opening of the
    song's own article, and the credits that are that track's alone. A
    track with neither has no section; with no section at all, no page."""
    work_qid: dict[str, str] = {}
    for t in info.tracks:
        for w in t.works[:1]:
            work_qid[w] = wikidata_id((f.mb(f"work/{w}", inc="url-rels") or {}).get("relations") or [])
    entities = f.wikidata(sorted({q for q in work_qid.values() if q}))
    discs = len({t.disc for t in info.tracks}) > 1
    out, disc = [], 0
    for t in info.tracks:
        qid = next((work_qid.get(w) for w in t.works if work_qid.get(w)), "")
        lang, title = sitelink(entities.get(qid) or {}, langs) if qid else ("", "")
        prose: list[str] = []
        if title:
            lead, _h, _s = lead_and_section(article_blocks(f, lang, title), ())
            prose, cut = cut_to([linker.prose(b) for b in lead], TRACK_BUDGET)
            if cut:
                prose.append("*Continued on Wikipedia.*")
        own = [c for c in info.credits if t.number in c.tracks]
        if not prose and not own:
            continue
        if discs and t.disc != disc:
            disc = t.disc
            out.append(f"# Disc {disc}")
        out.append(f"## {t.position}. {escape(t.title)}")
        for p in prose:
            out += [p, ""]
        people: dict[str, list[str]] = {}
        for c in sorted(own, key=lambda c: (group_of(c.role), c.role)):
            people.setdefault(c.name, []).append(c.role)
        for name, roles in people.items():
            out.append(f"*{escape(', '.join([roles[0]] + [r[0].lower() + r[1:] for r in roles[1:]]))}* {linker.person_link(name)}")
        if title:
            out.append("*" + source_line(lang, title, "").strip("*") + "*")
        out.append("")
    return "\n".join(out).strip()


def escape(text: str) -> str:
    """Square brackets the article itself has, such as an editor's insertion
    in a quote, would be links on the player: they become round ones."""
    return text.replace("[", "(").replace("]", ")")


class Linker:
    """Which names become [links]: the pages this run writes or the
    player makes up (library artists and albums)."""

    def __init__(self) -> None:
        self.people: dict[str, str] = {}        # name_key(wiki title or name) -> page name
        self.albums: dict[str, str] = {}        # name_key(album title) -> link text
        self.credited: dict[str, str] = {}      # name_key -> the name the credits give

    def person_link(self, name: str) -> str:
        """Always a link: to the person's page, or by their name, which the
        player resolves, lists the mentions of, or shows plain."""
        page = self.people.get(name_key(name)) or self.people.get(name_key(re.sub(r"\s*\([^)]*\)$", "", name)))
        return f"[{escape(page or name)}]"

    def prose(self, block: tuple[str, str, list[tuple[int, int, str]]]) -> str:
        kind, text, links = block
        out, pos = [], 0
        for s, e, target in sorted(links):
            if s < pos:
                continue
            shown = text[s:e]
            base = re.sub(r"\s*\([^)]*\)$", "", target)
            link = None
            if name_key(target) in self.people or name_key(base) in self.people:
                page = self.people.get(name_key(target)) or self.people[name_key(base)]
                if name_key(shown) in (name_key(page), name_key(re.sub(r"\s*\([^)]*\)$", "", page))):
                    link = f"[{escape(page)}]"
            elif name_key(base) in self.albums and name_key(shown) == name_key(base):
                link = f"[{escape(shown)}]"
            elif name_key(shown) in self.credited and name_key(shown) in (name_key(target), name_key(base)):
                link = f"[{escape(self.credited[name_key(shown)])}]"
            out.append(escape(text[pos:s]))
            out.append(link or escape(shown))
            pos = e
        out.append(escape(text[pos:]))
        line = "".join(out)
        return f"- {line}" if kind == "li" else line


def cut_to(paragraphs: list[str], budget: int) -> tuple[list[str], bool]:
    """Whole paragraphs within 'budget' bytes, and whether any were left
    out. A first paragraph over it alone is cut at a sentence; a heading
    left with nothing under it goes."""
    out, used = [], 0
    for p in paragraphs:
        size = len(p.encode("utf-8")) + 2
        if used + size > budget:
            if not out:
                head = p.encode("utf-8")[:budget].decode("utf-8", "ignore")
                out.append(head[:head.rfind(". ") + 1] if ". " in head else head)
            while out and out[-1].startswith("#"):
                out.pop()
            return out, True
        out.append(p)
        used += size
    return out, False


def cap_page(page: str) -> str:
    """A page within what the player reads, cut at a line. A source line at
    the end is kept."""
    if len(page.encode("utf-8")) <= PAGE_MAX:
        return page
    lines = page.rstrip("\n").split("\n")
    last = lines.pop() if lines[-1].startswith("*Adapted from Wikipedia") else ""
    room = PAGE_MAX - len(last.encode("utf-8")) - 64
    out, used = [], 0
    for line in lines:
        used += len(line.encode("utf-8")) + 1
        if used > room:
            break
        out.append(line)
    while out and (not out[-1].strip() or out[-1].startswith("#")):
        out.pop()
    return "\n".join(out + ["", "*Continued on Wikipedia.*"] + (["", last] if last else [])) + "\n"


def group_of(role: str) -> int:
    for n, (_name, roles) in enumerate(GROUPS):
        if roles is not None and role in roles:
            return n
    return 2                                    # Musicians: every instrument and voice


def personnel(credits: list[Credit], linker: Linker) -> str:
    """One line a person in each group, producers first:
    *Guitar, keyboard (tracks 2, 5)* [Jonny Greenwood]. Track numbers only
    where a role is not on the whole album."""
    if not credits:
        return ""
    rows: dict[tuple[int, str], list[Credit]] = {}
    for c in credits:
        rows.setdefault((group_of(c.role), c.mbid), []).append(c)
    lines = ["## Personnel"]
    for (_g, _m), cs in sorted(rows.items(), key=lambda kv: (kv[0][0], -len(kv[1]), kv[1][0].name.casefold())):
        roles = []
        for c in sorted(cs, key=lambda c: (bool(c.tracks), c.role)):
            text = c.role if not roles else c.role[0].lower() + c.role[1:]
            if c.tracks:
                nums = sorted(c.tracks)
                text += f" (track{'s' if len(nums) > 1 else ''} {', '.join(map(str, nums))})"
            roles.append(escape(text))
        lines.append(f"*{', '.join(roles)}* {linker.person_link(cs[0].name)}")
    return "\n".join(lines)


def fill(template: str, values: dict[str, str]) -> str:
    """The template with its <<placeholders>> filled; a line whose
    placeholder comes out empty goes, and runs of blank lines close up.
    The player's own {{pieces}} pass through."""
    out = []
    for line in template.splitlines():
        m = re.fullmatch(r"\s*<<([^>]+)>>\s*", line)
        if m:
            v = values.get(m.group(1).split(":")[0], "") if not m.group(1).startswith("section") \
                else values.get("section", "")
            if v:
                out.append(v)
            continue
        out.append(re.sub(r"<<([^>]+)>>", lambda mm: values.get(mm.group(1), ""), line))
    text = re.sub(r"\n{3,}", "\n\n", "\n".join(out)).strip()
    return text + "\n"


def source_line(lang: str, title: str, extra: str) -> str:
    if not title:
        return f"*{extra}*" if extra else ""
    url = f"https://{lang}.wikipedia.org/wiki/{quote(title.replace(' ', '_'))}"
    return (f'*Adapted from Wikipedia, "{title}", {url}, CC BY-SA 4.0.'
            + (f" {extra}" if extra else "") + "*")

# ---------------------------------------------------------------------
# The run
# ---------------------------------------------------------------------
def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description="Write liner notes and biographies for your library into the player's /Notes, from MusicBrainz and Wikipedia.")
    p.add_argument("root", nargs="?", help="a mounted player, or a music folder (then give --notes)")
    p.add_argument("--notes", help="the Notes folder to write into, when not a mounted player's")
    p.add_argument("--depth", type=int, choices=(0, 1), default=0,
                   help="0: albums and their artists (default); 1: also the producers, songwriters and others they credit")
    p.add_argument("--lang", default="en", help="Wikipedia languages to use, in order: en, or ja,en")
    p.add_argument("--tracks", action="store_true",
                   help="also a page of each album's tracks: songs' own articles and each track's credits (about doubles the time)")
    p.add_argument("--templates", help="a folder holding Album.md, Person.md and Tracks.md "
                   "(default: .rockbox/fetch/templates on the player, when it has them)")
    p.add_argument("--save-templates", action="store_true",
                   help="write the built-in templates into the templates folder for you to edit, and stop")
    p.add_argument("--only", metavar="ARTIST", help="only this artist's albums")
    p.add_argument("--max-people-per-album", type=int, default=8)
    p.add_argument("--max-pages", type=int, default=1500)
    p.add_argument("--compilations", action="store_true", help="include albums by Various Artists")
    p.add_argument("--auto-accept", type=float, default=0.85, help="how closely MusicBrainz's album must match yours (0-1)")
    p.add_argument("--dry-run", action="store_true", help="say what would be written, and write nothing")
    p.add_argument("--refresh", action="store_true", help="rewrite the pages this tool wrote and you have not edited, in the current layout")
    p.add_argument("--undo", action="store_true", help="delete the pages this tool wrote and you have not edited")
    p.add_argument("--contact", metavar="EMAIL", help="required by Wikipedia for a tool like this; remembered")
    p.add_argument("--version", action="version", version=f"notes_fetch {VERSION}")
    return p


def main(argv: Optional[list[str]] = None) -> int:
    args = build_parser().parse_args(argv)
    if not args.root:
        build_parser().print_help(); return 2
    root = Path(args.root).expanduser().resolve()
    if not root.is_dir():
        print(f"ERROR: not a folder: {root}", file=sys.stderr); return 2
    player = player_root_of(root)
    notes = Path(args.notes).expanduser().resolve() if args.notes else (player / "Notes" if player else None)
    if notes is None:
        print("ERROR: give --notes, the Notes folder to write into, or point it at a mounted player", file=sys.stderr)
        return 2
    manifest = Manifest(root)
    if args.undo:
        deleted, kept = manifest.undo(TOOL, args.dry_run)
        if not args.dry_run:
            manifest.save()
        print(f"\n{deleted} page(s) {'would be ' if args.dry_run else ''}deleted, {kept} kept because you edited them")
        return 0

    if args.save_templates:
        folder = Path(args.templates).expanduser() if args.templates else data_dir(root) / "templates"
        folder.mkdir(parents=True, exist_ok=True)
        for _kind, name, text in TEMPLATES:
            p = folder / name
            if p.exists():
                print(f"kept, already there: {p}")
            else:
                p.write_text(text, encoding="utf-8")
                print(f"wrote: {p}")
        print("\nEdit them, and the next run lays its pages out by them.")
        return 0

    config = load_config()
    contact = contact_from(args.contact, config)
    if not contact:
        print("ERROR: Wikipedia asks tools like this one for a way to reach whoever runs them.\n"
              "Run it once with --contact you@example.com; it is remembered.", file=sys.stderr)
        return 2
    langs = [x.strip() for x in args.lang.split(",") if x.strip()] or ["en"]
    templates = {kind: text for kind, _name, text in TEMPLATES}
    template_dir = Path(args.templates).expanduser() if args.templates else data_dir(root) / "templates"
    used = []
    for kind, name, _text in TEMPLATES:
        p = template_dir / name
        if p.is_file():
            templates[kind] = p.read_text(encoding="utf-8")
            used.append(name)
    known = {"lead", "section", "personnel", "tracks_page", "source", "facts", "members", "album", "track_sections"}
    for kind, name, _text in TEMPLATES:
        for line in templates[kind].splitlines():
            for ph in re.findall(r"<<([^>]+)>>", line):
                if ph.split(":")[0] not in known:
                    print(f"Warning: {name}: <<{ph}>> is not a placeholder, and comes out empty")
                elif ph.startswith("section") and line.strip() != f"<<{ph}>>":
                    print(f"Warning: {name}: <<{ph}>> works only on a line of its own")
    m = re.search(r"<<section:([^>]+)>>", templates["album"])
    album_sections = tuple(s.strip() for s in m.group(1).split("|")) if m else ()

    f = Fetcher(data_dir(root) / "notes-cache", user_agent(TOOL, VERSION, contact),
                RateLimiter({"musicbrainz": 1.1, "wikimedia": 1.0}))
    print(f"notes_fetch {VERSION}")
    print(f"Library  : {root}")
    print(f"Notes    : {notes}")
    print(f"Depth    : {args.depth}" + ("; with a page of each album's tracks" if args.tracks else ""))
    print(f"Templates: {', '.join(used) + ' from ' + str(template_dir) if used else 'built in'}")
    print(f"Settings : {config_path()}")

    albums = library_albums(root)
    if args.only:
        albums = [a for a in albums if similarity(args.only, a.artist) >= 0.9]
    if not args.compilations:
        albums = [a for a in albums if name_key(a.artist) not in ("variousartists", "various", "va")]
    print(f"Albums   : {len(albums)}")
    counts: dict[str, int] = {}

    def phase(n: int, what: str, items: int, per_item: float) -> None:
        """A phase's heading, with how long a first run of it takes:
        MusicBrainz allows a request a second. Cached items take none."""
        minutes = items * per_item / 60
        when = f"up to {minutes:.0f} min on a first run" if minutes >= 1.5 else "under two minutes"
        print(f"\n=== {n} of 3: {what} ({items}; {when}) ===", flush=True)

    def count(what: str) -> None:
        counts[what] = counts.get(what, 0) + 1

    def owner_has(path: Path, beside: Optional[Path]) -> bool:
        for p in (path, path.with_suffix(".txt")):
            if p.exists() and not manifest.owns(p):
                return True
        return bool(beside and beside.exists())

    # Phase 1: what each album is, and who it credits
    found: list[tuple[Album, AlbumInfo]] = []
    library_artists: dict[str, tuple[str, str]] = {}      # name_key -> (mbid, library name)
    try:
        phase(1, "finding your albums and their credits on MusicBrainz", len(albums), 4.5)
        for i, a in enumerate(sorted(albums, key=lambda a: (a.artist.casefold(), a.title.casefold())), 1):
            print(f"[{i}/{len(albums)}] {a.artist} - {a.title}: ", end="", flush=True)
            try:
                info = find_album(f, a, args.auto_accept)
            except requests.RequestException as exc:
                print(f"error: {exc}"); count("errors"); continue
            if not info:
                print("not found"); count("albums not found"); continue
            found.append((a, info))
            for mbid, name in info.artists:
                if same_person(name, a.artist) or len(info.artists) == 1:
                    library_artists.setdefault(name_key(a.artist), (mbid, a.artist))
            print(f"{len(info.credits)} credit(s)" + (", a Wikipedia article" if info.wikidata else ""))

        # Who gets a page of their own
        credited: dict[str, list[Credit]] = {}            # mbid -> their credits
        on_albums: dict[str, set[int]] = {}               # mbid -> the albums crediting them
        for n, (_a, info) in enumerate(found):
            for c in info.credits:
                credited.setdefault(c.mbid, []).append(c)
                on_albums.setdefault(c.mbid, set()).add(n)
        people: dict[str, str] = {}                        # mbid -> name
        for _key, (mbid, name) in library_artists.items():
            people[mbid] = name
        if args.depth >= 1:
            for a, info in found:
                ranked = []
                for c in info.credits:
                    if c.mbid in people or any(c.mbid == m for m, _ in info.artists):
                        continue
                    albums_credited = len(on_albums[c.mbid])
                    mine = [x for x in credited[c.mbid] if x in info.credits]
                    best_tier = min(x.tier for x in credited[c.mbid])
                    # A key role on one track of one album -- the writers
                    # of a cover, say -- earns a link, not a page
                    whole = any(not x.tracks for x in mine) or len(set().union(*(x.tracks for x in mine))) >= 2
                    if (best_tier == 1 and (whole or albums_credited >= 2)) or albums_credited >= MIN_ALBUMS_FOR_TIER2:
                        ranked.append((best_tier, -albums_credited, c.name, c.mbid))
                for _t, _n, name, mbid in sorted(set(ranked))[:args.max_people_per_album]:
                    people.setdefault(mbid, name)
        people = dict(list(people.items())[:args.max_pages])

        # Phase 2: each person's article
        person_pages: dict[str, dict[str, Any]] = {}
        linker = Linker()
        for key, (_m, name) in library_artists.items():
            linker.people[key] = name
        key_people = len(people) - len(library_artists)
        phase(2, "finding the artists" + (f" and {key_people} key people they credit" if key_people > 0 else "")
              + " on Wikipedia", len(people), 2.5)
        for i, (mbid, name) in enumerate(people.items(), 1):
            is_library = name_key(name) in library_artists
            print(f"[{i}/{len(people)}] {name}: ", end="", flush=True)
            try:
                details = artist_details(f, mbid)
                qid = wikidata_id(details.get("relations") or [])
                if not qid and is_library:
                    qid = wiki_lookup_artist(f, name, langs)
                ent = f.wikidata([qid]).get(qid) if qid else None
            except requests.RequestException as exc:
                print(f"error: {exc}"); count("errors"); continue
            lang, title = sitelink(ent or {}, langs)
            # One article, one page: Richard D. James is Aphex Twin's
            same = next((p["name"] for p in person_pages.values() if title and p["title"] == title), None)
            if same:
                print(f'Wikipedia, "{title}", which is {same}\'s page; linked there')
                linker.people.setdefault(name_key(name), same)
                continue
            print(f'Wikipedia, "{title}"' if title else "no Wikipedia article" + ("" if is_library else "; a link only"))
            if not title and not is_library:
                continue                     # a credited person with no article: a link only
            page = name if is_library else title
            person_pages[mbid] = {"name": page, "details": details, "entity": ent, "lang": lang, "title": title}
            linker.people[name_key(page)] = page
            linker.people.setdefault(name_key(name), page)
            if title:
                linker.people.setdefault(name_key(title), page)
        for a, info in found:
            linker.albums[name_key(a.title)] = a.title
            for c in info.credits:
                linker.credited.setdefault(name_key(c.name), c.name)

        # Phase 3: write
        written = 0
        total = len(found) * (2 if args.tracks else 1) + len(person_pages)
        phase(3, "writing the pages", total, 1.5 + (12 if args.tracks else 0) * len(found) / max(1, total))
        step = 0

        def start(folder: str, target: Path) -> None:
            nonlocal step
            step += 1
            print(f"[{step}/{total}] {folder}/{target.name}: ", end="", flush=True)

        def done(page: str, target: Path, what: str) -> bool:
            nonlocal written
            page = cap_page(page)
            size = f"{len(page.encode()) / 1024:.1f} KB"
            if args.dry_run:
                print(f"would write, {size}"); count("would write"); return True
            try:
                manifest.write(target, page.encode("utf-8"), TOOL)
            except OSError as exc:
                print(f"could not write: {exc}"); count("errors"); return False
            written += 1; count(what)
            print(f"written, {size}")
            return True

        # Each album's tracks page first, so its album page can link to it
        tracks_pages: dict[int, str] = {}                 # id(album) -> page name
        for a, info in found if args.tracks else ():
            name = f"{a.artist} - {a.title} - Tracks"
            target = notes / "Albums" / (safe_filename(name) + ".md")
            start("Albums", target)
            if owner_has(target, None):
                print("yours, left alone"); count("pages you wrote"); tracks_pages[id(a)] = name; continue
            if target.exists() and not args.refresh:
                print("written before (--refresh rewrites it)"); count("pages already written")
                tracks_pages[id(a)] = name; continue
            try:
                sections = track_sections(f, info, linker, langs)
            except requests.RequestException as exc:
                print(f"error: {exc}"); count("errors"); continue
            if not sections:
                print("no track has an article or credits of its own; not written"); count("track pages with nothing to say"); continue
            page = fill(templates["tracks"], {"album": f"{a.artist} - {a.title}", "track_sections": sections})
            if done(page, target, "track pages written"):
                tracks_pages[id(a)] = name

        for a, info in found:
            if written >= args.max_pages:
                break
            target = notes / "Albums" / (safe_filename(f"{a.artist} - {a.title}") + ".md")
            start("Albums", target)
            if owner_has(target, a.folder / "notes.md"):
                print("yours, left alone"); count("pages you wrote"); continue
            if target.exists() and not args.refresh:
                print("written before (--refresh rewrites it)"); count("pages already written"); continue
            try:
                qid = info.wikidata or wiki_lookup_album(f, a, langs)
                ent = f.wikidata([qid]).get(qid) if qid else None
                lang, title = sitelink(ent or {}, langs)
                blocks = article_blocks(f, lang, title) if title else []
            except requests.RequestException as exc:
                print(f"error: {exc}"); count("errors"); continue
            lead, heading, section = lead_and_section(blocks, album_sections)
            if not lead and not info.credits:
                print("no article and no credits; not written"); count("albums with nothing to say"); continue
            if not lead:
                print("no article, credits only; ", end="")
            prose = [linker.prose(b) for b in lead]
            sec = [linker.prose(b) for b in section]
            kept, cut = cut_to(prose + ([f"## {heading}"] + sec if sec else []), ALBUM_BUDGET)
            lead_text = "\n\n".join(kept[:len(prose)])
            sec_kept = kept[len(prose):]
            section_text = "\n\n".join(sec_kept) + ("\n\n*Continued on Wikipedia.*" if cut else "") if sec_kept else \
                ("*Continued on Wikipedia.*" if cut else "")
            page = fill(templates["album"], {
                "lead": lead_text, "section": section_text,
                "personnel": personnel(info.credits, linker),
                "tracks_page": f"*More about each track:* [{escape(tracks_pages[id(a)])}]" if id(a) in tracks_pages else "",
                "source": source_line(lang, title, "Credits: MusicBrainz (CC0)." if info.credits else ""),
            })
            done(page, target, "album pages written")

        for mbid, pp in person_pages.items():
            if written >= args.max_pages:
                break
            target = notes / "People" / (safe_filename(pp["name"]) + ".md")
            start("People", target)
            beside = None
            lib = library_artists.get(name_key(pp["name"]))
            if lib:
                folders = {a.folder.parent for a, _ in found if name_key(a.artist) == name_key(pp["name"])}
                beside = next((d / "bio.md" for d in folders if (d / "bio.md").exists()), None)
            if owner_has(target, beside):
                print("yours, left alone"); count("pages you wrote"); continue
            if target.exists() and not args.refresh:
                print("written before (--refresh rewrites it)"); count("pages already written"); continue
            ent = pp["entity"] or {}
            try:
                blocks = article_blocks(f, pp["lang"], pp["title"]) if pp["title"] else []
                facts = facts_line(f, ent)
            except requests.RequestException as exc:
                print(f"error: {exc}"); count("errors"); continue
            lead, _h, _s = lead_and_section(blocks, ())
            if not lead:
                print("no article; not written"); count("people with nothing to say"); continue
            kept, cut = cut_to([linker.prose(b) for b in lead], PERSON_BUDGET)
            members = band_members(pp["details"])
            page = fill(templates["person"], {
                "facts": facts,
                "lead": "\n\n".join(kept) + ("\n\n*Continued on Wikipedia.*" if cut else ""),
                "members": ("## Members\n" + "\n".join(f"- {linker.person_link(m)}" for m in members)) if members else "",
                "source": source_line(pp["lang"], pp["title"], "Facts: Wikidata (CC0)." if facts else ""),
            })
            done(page, target, "person pages written")
    except KeyboardInterrupt:
        print("\nStopped; the pages written so far are kept.")
    finally:
        if not args.dry_run:
            manifest.save()

    print(f"\nSummary ({f.requests} request(s))\n-------")
    for what in ("album pages written", "track pages written", "person pages written", "would write", "pages already written",
                 "track pages with nothing to say",
                 "pages you wrote", "albums not found", "albums with nothing to say",
                 "people with nothing to say", "errors"):
        if counts.get(what):
            print(f"  {what:<26}: {counts[what]}")
    if counts.get("album pages written") or counts.get("person pages written"):
        print("\nOn the player, the new pages show after Settings > Library > Maintenance > Rebuild File Index,"
              "\nor the next time it is connected to a computer.")
    return 0


def facts_line(f: Fetcher, ent: dict[str, Any]) -> str:
    """*Born 1949, Passaic* or *Formed 1972, Los Angeles*, and a death or
    a band's end after it."""
    if not ent:
        return ""
    if claim_year(ent, "P569"):
        start, place_prop, word = claim_year(ent, "P569"), "P19", "Born"
        end, end_word = claim_year(ent, "P570"), "died"
    elif claim_year(ent, "P571"):
        start, place_prop, word = claim_year(ent, "P571"), "P740", "Formed"
        end, end_word = claim_year(ent, "P576"), "disbanded"
    else:
        return ""
    places = claim_ids(ent, place_prop)
    place = label(f.wikidata(places[:1]).get(places[0]) or {}) if places else ""
    line = f"{word} {start}" + (f", {place}" if place else "") + (f"; {end_word} {end}" if end else "")
    return f"*{html.unescape(line)}*"


if __name__ == "__main__":
    raise SystemExit(main())
