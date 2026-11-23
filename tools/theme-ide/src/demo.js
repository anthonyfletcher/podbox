/* The player's music: an invented library, so a theme's Now Playing, lists
 * and album art have something to show. Nothing in it is anyone's -- the
 * names are made up and the covers are drawn by demo/covers.py.
 *
 * The tracks are tagged IMA ADPCM .wav files at their full length, which the
 * firmware plays with its own WAV codec. None is stored: each file's reads
 * are answered as they come, with the header and then a few seconds of
 * encoded audio over and over. IMA ADPCM blocks stand alone, so the repeat is
 * seamless, and a five-minute track costs about 30 KB of memory.
 *
 * install(base) writes /Music/<artist>/<album>/ with each album's
 * cover.jpg, fetched from base + "demo/". */
/* global FS, MEMFS */

const T = (title, secs) => ({ title, secs });
export const LIBRARY = [
  { id: "paper_lanterns", title: "Paper Lanterns", artist: "Juniper Vale", year: 2019, genre: "Folk",
    tracks: [T("Lantern Light", 214), T("Harbour Road (feat. Marisol Kade)", 241), T("Smaller Hours", 198),
             T("Copper Kettle", 225), T("The Long Field", 262), T("Paper Boats", 187), T("Kindling", 233),
             T("Home by Nine", 251)] },
  { id: "neon_tides", title: "Neon Tides", artist: "The Night Ferries", year: 2021, genre: "Synthwave",
    tracks: [T("Overdrive", 246), T("Coastline 1986", 274), T("Midnight Arcade", 231),
             T("Tidal Lock (feat. Kit Arbour)", 289), T("Pink Horizon", 255), T("After the Rain", 302),
             T("Last Ferry Out", 318)] },
  { id: "quiet_machines", title: "Quiet Machines", artist: "Lumen & Rook", year: 2016, genre: "Ambient",
    tracks: [T("Idle", 344), T("Low Hum", 402), T("Standby", 297), T("Red Light", 368), T("Cooling Fan", 425)] },
  { id: "wildflower_static", title: "Wildflower Static", artist: "Marisol Kade", year: 2023, genre: "Dream Pop",
    tracks: [T("Petals", 203), T("Static Bloom", 229), T("Meadowlight", 244),
             T("Pollen Count (feat. Juniper Vale)", 192), T("Soft Focus", 236), T("Hayfever", 211)] },
  { id: "northern_lines", title: "Northern Lines", artist: "Fjell", year: 2018, genre: "Electronic",
    tracks: [T("Polar Night", 312), T("Aurora", 287), T("Tromso", 265), T("Glacier Drift", 341),
             T("Magnetic North", 298), T("Thaw", 276)] },
  { id: "saltwater_hymns", title: "Saltwater Hymns", artist: "Harbour Choir", year: 2014, genre: "Folk",
    tracks: [T("Tidewater", 222), T("Ropes and Rigging", 248), T("Low Tide Psalm", 271), T("Gulls", 176),
             T("Lighthouse Keeper", 293), T("Salt", 205), T("The Crossing", 318)] },
  { id: "concrete_bloom", title: "Concrete Bloom", artist: "Okapi", year: 2022, genre: "Hip Hop",
    tracks: [T("Block Party", 198), T("Yellow Line", 214), T("Brutal", 187),
             T("Rooftop Garden (feat. Tilde)", 236), T("Grid (feat. Sol Ivers)", 202), T("Bloom", 245)] },
  { id: "velvet_hours", title: "Velvet Hours", artist: "Ada Moreau", year: 2017, genre: "Jazz",
    tracks: [T("Gold Leaf", 284), T("After Midnight (feat. Sol Ivers)", 312), T("Velvet", 268),
             T("Smoke Rings", 297), T("Last Call", 333)] },
  { id: "glass_orchard", title: "Glass Orchard", artist: "Tilde", year: 2020, genre: "Indie Pop",
    tracks: [T("Coral", 192), T("Orchard", 218), T("Sugar Glass", 201), T("Peach Fuzz", 186),
             T("Windfall", 224), T("Glasshouse", 239)] },
  { id: "ember_season", title: "Ember Season", artist: "The Low Fires", year: 2015, genre: "Rock",
    tracks: [T("Kindle", 231), T("Ember Season", 257), T("Ash", 203), T("Wildfire", 276),
             T("Smoulder", 248), T("Afterglow", 312)] },
  { id: "satellite_hearts", title: "Satellite Hearts", artist: "Kit Arbour", year: 2024, genre: "Indie Pop",
    tracks: [T("Orbit", 207), T("Satellite Hearts", 223), T("Ground Control", 241),
             T("Signal (feat. Okapi)", 198), T("Static Sky", 232)] },
  { id: "moss_and_mineral", title: "Moss & Mineral", artist: "Fern Collective", year: 2012, genre: "Ambient",
    tracks: [T("Lichen", 356), T("Riverbed", 382), T("Slate", 311), T("Undergrowth", 404), T("Quartz", 337)] },
];

const RATE = 16000;
const BLOCK = 256;                          /* bytes per IMA ADPCM block, mono */
const PER_BLOCK = (BLOCK - 4) * 2 + 1;      /* samples in each */
const PHRASE = 32;                          /* blocks before the audio repeats */

/* ---- IMA ADPCM ---- */
const STEPS = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
  50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
  337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552,
  1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484,
  7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385,
  24623, 27086, 29794, 32767];
const INDEX = [-1, -1, -1, -1, 2, 4, 6, 8];

function encodeBlock(pcm, out, at, state) {
  out[at] = pcm[0] & 255;
  out[at + 1] = (pcm[0] >> 8) & 255;
  out[at + 2] = state.index;
  out[at + 3] = 0;
  let pred = pcm[0], index = state.index;
  for (let i = 1; i < PER_BLOCK; i++) {
    const step = STEPS[index];
    let diff = pcm[i] - pred, code = 0;
    if (diff < 0) { code = 8; diff = -diff; }
    let delta = step >> 3;
    if (diff >= step) { code |= 4; diff -= step; delta += step; }
    if (diff >= step >> 1) { code |= 2; diff -= step >> 1; delta += step >> 1; }
    if (diff >= step >> 2) { code |= 1; delta += step >> 2; }
    pred += code & 8 ? -delta : delta;
    pred = Math.max(-32768, Math.min(32767, pred));
    index = Math.max(0, Math.min(88, index + INDEX[code & 7]));
    const byte = at + 4 + ((i - 1) >> 1);
    if ((i - 1) & 1) out[byte] |= code << 4; else out[byte] = code;
  }
  state.index = index;
}

/* ---- the music ---- */
/* A few seconds per track: a bass note and a melody over it, in a key and
   a speed taken from the track's place in the library. Not music anyone
   would choose, but it plays as audio and loops without a seam. */
function phrase(seed) {
  let s = seed * 2654435761 >>> 0;
  const rnd = () => ((s = (s * 1664525 + 1013904223) >>> 0) / 4294967296);
  const scale = [0, 2, 4, 7, 9, 12, 14, 16];
  const root = 196 * Math.pow(2, Math.floor(rnd() * 7) / 12);
  const total = PHRASE * PER_BLOCK, notes = 16, len = total / notes;
  const melody = Array.from({ length: notes }, () => scale[Math.floor(rnd() * scale.length)]);
  const pcm = new Int16Array(total);
  for (let i = 0; i < total; i++) {
    const n = Math.floor(i / len), t = (i - n * len) / RATE;
    const f = root * Math.pow(2, melody[n] / 12);
    const env = Math.exp(-t * 3);
    const bass = Math.sin(2 * Math.PI * (root / 2) * i / RATE) * 0.25;
    const lead = Math.sin(2 * Math.PI * f * t) * env * 0.45;
    pcm[i] = Math.round((bass + lead) * 12000);
  }
  const out = new Uint8Array(PHRASE * BLOCK), state = { index: 0 };
  for (let b = 0; b < PHRASE; b++)
    encodeBlock(pcm.subarray(b * PER_BLOCK, (b + 1) * PER_BLOCK), out, b * BLOCK, state);
  return out;
}

/* ---- the file ---- */
function header(album, track, number, blocks) {
  const enc = new TextEncoder();
  const info = [
    ["INAM", track.title], ["IART", album.artist], ["ISBJ", album.artist],
    ["IPRD", album.title], ["IGNR", album.genre], ["ICRD", String(album.year)],
    ["IPRT", number + "/" + album.tracks.length],
  ].map(([id, text]) => {
    const b = enc.encode(text + "\0");
    const pad = b.length & 1;
    const c = new Uint8Array(8 + b.length + pad);
    c.set(enc.encode(id), 0);
    new DataView(c.buffer).setUint32(4, b.length, true);
    c.set(b, 8);
    return c;
  });
  const listLen = 4 + info.reduce((n, c) => n + c.length, 0);
  const dataLen = blocks * BLOCK;
  const size = 12 + 28 + 12 + 8 + listLen + 8;
  const h = new Uint8Array(size), v = new DataView(h.buffer);
  let p = 0;
  const tag = (s) => { h.set(enc.encode(s), p); p += 4; };
  const u32 = (n) => { v.setUint32(p, n, true); p += 4; };
  const u16 = (n) => { v.setUint16(p, n, true); p += 2; };
  tag("RIFF"); u32(size - 8 + dataLen); tag("WAVE");
  tag("fmt "); u32(20); u16(0x11); u16(1); u32(RATE);
  u32(Math.round(RATE * BLOCK / PER_BLOCK)); u16(BLOCK); u16(4); u16(2); u16(PER_BLOCK);
  tag("fact"); u32(4); u32(blocks * PER_BLOCK);
  tag("LIST"); u32(listLen); tag("INFO");
  for (const c of info) { h.set(c, p); p += c.length; }
  tag("data"); u32(dataLen);
  return h;
}

/* A file in Emscripten's MEMFS whose bytes are made as they are read */
function virtualFile(dir, name, head, blocks, seed) {
  let audio = null;
  const size = head.length + blocks * BLOCK;
  const node = FS.createFile(dir, name, {}, true, false);
  node.usedBytes = size;
  node.contents = null;
  node.stream_ops = Object.assign({}, node.stream_ops, {
    read(stream, buffer, offset, length, position) {
      const end = Math.min(size, position + length);
      for (let p = position; p < end; p++) {
        if (p < head.length) { buffer[offset + p - position] = head[p]; continue; }
        if (!audio) audio = phrase(seed);
        buffer[offset + p - position] = audio[(p - head.length) % audio.length];
      }
      return Math.max(0, end - position);
    },
  });
  /* MEMFS reads a file's size from these */
  node.node_ops = Object.assign({}, node.node_ops, {
    getattr(n) {
      const a = MEMFS.node_ops.getattr(n);
      a.size = size;
      return a;
    },
  });
}

const safe = s => s.replace(/[\\/:*?"<>|]/g, "_");

function mkdirs(path) {
  let at = "";
  for (const d of path.split("/").filter(Boolean)) {
    at += "/" + d;
    try { FS.mkdir(at); } catch (e) { /* already there */ }
  }
}

/* an album's folder on the player's disk */
export const albumDir = (album) => '/Music/' + safe(album.artist) + '/' + safe(album.title);

export async function install(base) {
  let seed = 1;
  await Promise.all(LIBRARY.map(async album => {
    const dir = "/simdisk" + albumDir(album);
    mkdirs(dir);
    album.tracks.forEach((track, i) => {
      const blocks = Math.ceil(track.secs * RATE / PER_BLOCK);
      const name = String(i + 1).padStart(2, "0") + " " + safe(track.title) + ".wav";
      virtualFile(dir, name, header(album, track, i + 1, blocks), blocks, seed++);
    });
    try {
      const r = await fetch(base + "demo/" + album.id + ".jpg");
      if (r.ok) FS.writeFile(dir + "/cover.jpg", new Uint8Array(await r.arrayBuffer()));
    } catch (e) { /* the album shows without art */ }
  }));
}

